/*
 * Copyright (C) 2023 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "src/trace_processor/importers/perf/record_parser.h"

#include <cinttypes>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/public/compiler.h"
#include "perfetto/trace_processor/ref_counted.h"
#include "src/trace_processor/importers/common/address_range.h"
#include "src/trace_processor/importers/common/create_mapping_params.h"
#include "src/trace_processor/importers/common/mapping_tracker.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/common/stack_profile_tracker.h"
#include "src/trace_processor/importers/common/virtual_memory_mapping.h"
#include "src/trace_processor/importers/perf/mmap_record.h"
#include "src/trace_processor/importers/perf/perf_counter.h"
#include "src/trace_processor/importers/perf/perf_event.h"
#include "src/trace_processor/importers/perf/perf_event_attr.h"
#include "src/trace_processor/importers/perf/reader.h"
#include "src/trace_processor/importers/perf/record.h"
#include "src/trace_processor/importers/perf/sample.h"
#include "src/trace_processor/importers/proto/perf_sample_tracker.h"
#include "src/trace_processor/importers/proto/profile_packet_utils.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/metadata_tables_py.h"
#include "src/trace_processor/tables/profiler_tables_py.h"
#include "src/trace_processor/util/build_id.h"
#include "src/trace_processor/util/status_macros.h"

#include "protos/perfetto/trace/profiling/profile_packet.pbzero.h"

namespace perfetto::trace_processor::perf_importer {
namespace {

CreateMappingParams BuildCreateMappingParams(
    const CommonMmapRecordFields& fields,
    std::string filename,
    std::optional<BuildId> build_id) {
  return {AddressRange::FromStartAndSize(fields.addr, fields.len), fields.pgoff,
          // start_offset: This is the offset into the file where the ELF header
          // starts. We assume all file mappings are ELF files an thus this
          // offset is 0.
          0,
          // load_bias: This can only be read out of the actual ELF file, which
          // we do not have here, so we set it to 0. When symbolizing we will
          // hopefully have the real load bias and we can compensate there for a
          // possible mismatch.
          0, std::move(filename), std::move(build_id)};
}

bool IsInKernel(protos::pbzero::Profiling::CpuMode cpu_mode) {
  switch (cpu_mode) {
    case protos::pbzero::Profiling::MODE_UNKNOWN:
      PERFETTO_FATAL("Unknown CPU mode");
    case protos::pbzero::Profiling::MODE_GUEST_KERNEL:
    case protos::pbzero::Profiling::MODE_KERNEL:
      return true;
    case protos::pbzero::Profiling::MODE_USER:
    case protos::pbzero::Profiling::MODE_HYPERVISOR:
    case protos::pbzero::Profiling::MODE_GUEST_USER:
      return false;
  }
  PERFETTO_FATAL("For GCC.");
}

}  // namespace

using FramesTable = tables::StackProfileFrameTable;
using CallsitesTable = tables::StackProfileCallsiteTable;

RecordParser::RecordParser(TraceProcessorContext* context)
    : context_(context) {}

RecordParser::~RecordParser() = default;

void RecordParser::ParsePerfRecord(int64_t ts, Record record) {
  if (base::Status status = ParseRecord(ts, std::move(record)); !status.ok()) {
    context_->storage->IncrementStats(record.header.type == PERF_RECORD_SAMPLE
                                          ? stats::perf_samples_skipped
                                          : stats::perf_record_skipped);
  }
}

base::Status RecordParser::ParseRecord(int64_t ts, Record record) {
  switch (record.header.type) {
    case PERF_RECORD_COMM:
      return ParseComm(std::move(record));

    case PERF_RECORD_SAMPLE:
      return ParseSample(ts, std::move(record));

    case PERF_RECORD_MMAP:
      return ParseMmap(std::move(record));

    case PERF_RECORD_MMAP2:
      return ParseMmap2(std::move(record));

    case PERF_RECORD_AUX:
    case PERF_RECORD_AUXTRACE:
    case PERF_RECORD_AUXTRACE_INFO:
      // These should be dealt with at tokenization time
      PERFETTO_FATAL("Unexpected record type at parsing time: %" PRIu32,
                     record.header.type);

    default:
      context_->storage->IncrementIndexedStats(
          stats::perf_unknown_record_type,
          static_cast<int>(record.header.type));
      return base::ErrStatus("Unknown PERF_RECORD with type %" PRIu32,
                             record.header.type);
  }
}

base::Status RecordParser::ParseSample(int64_t ts, Record record) {
  Sample sample;
  RETURN_IF_ERROR(sample.Parse(ts, record));

  if (!sample.period.has_value() && record.attr != nullptr) {
    sample.period = record.attr->sample_period();
  }

  return InternSample(std::move(sample));
}

base::Status RecordParser::InternSample(Sample sample) {
  if (!sample.time.has_value()) {
    // We do not really use this TS as this is using the perf clock, but we need
    // it to be present so that we can compute the trace_ts done during
    // tokenization. (Actually at tokenization time we do estimate a trace_ts if
    // no perf ts is present, but for samples we want this to be as accurate as
    // possible)
    return base::ErrStatus(
        "Can not parse samples with no PERF_SAMPLE_TIME field");
  }

  if (!sample.pid_tid.has_value()) {
    return base::ErrStatus(
        "Can not parse samples with no PERF_SAMPLE_TID field");
  }

  if (!sample.cpu.has_value()) {
    return base::ErrStatus(
        "Can not parse samples with no PERF_SAMPLE_CPU field");
  }

  UniqueTid utid = context_->process_tracker->UpdateThread(sample.pid_tid->tid,
                                                           sample.pid_tid->pid);
  const auto upid = *context_->storage->thread_table()
                         .FindById(tables::ThreadTable::Id(utid))
                         ->upid();

  if (sample.callchain.empty() && sample.ip.has_value()) {
    sample.callchain.push_back(Sample::Frame{sample.cpu_mode, *sample.ip});
  }
  std::optional<CallsiteId> callsite_id =
      InternCallchain(upid, sample.callchain);

  context_->storage->mutable_perf_sample_table()->Insert(
      {sample.trace_ts, utid, *sample.cpu,
       context_->storage->InternString(
           ProfilePacketUtils::StringifyCpuMode(sample.cpu_mode)),
       callsite_id, std::nullopt, sample.perf_session->perf_session_id()});

  return UpdateCounters(sample);
}

std::optional<CallsiteId> RecordParser::InternCallchain(
    UniquePid upid,
    const std::vector<Sample::Frame>& callchain) {
  if (callchain.empty()) {
    return std::nullopt;
  }

  auto& stack_profile_tracker = *context_->stack_profile_tracker;
  auto& mapping_tracker = *context_->mapping_tracker;

  std::optional<CallsiteId> parent;
  uint32_t depth = 0;
  for (auto it = callchain.rbegin(); it != callchain.rend(); ++it) {
    VirtualMemoryMapping* mapping;
    if (IsInKernel(it->cpu_mode)) {
      mapping = mapping_tracker.FindKernelMappingForAddress(it->ip);
    } else {
      mapping = mapping_tracker.FindUserMappingForAddress(upid, it->ip);
    }

    if (!mapping) {
      context_->storage->IncrementStats(stats::perf_dummy_mapping_used);
      // Simpleperf will not create mappings for anonymous executable mappings
      // which are used by JITted code (e.g. V8 JavaScript).
      mapping = mapping_tracker.GetDummyMapping();
    }

    const FrameId frame_id =
        mapping->InternFrame(mapping->ToRelativePc(it->ip), "");

    parent = stack_profile_tracker.InternCallsite(parent, frame_id, depth);
    depth++;
  }
  return parent;
}

base::Status RecordParser::ParseComm(Record record) {
  Reader reader(record.payload.copy());
  uint32_t pid;
  uint32_t tid;
  std::string comm;
  if (!reader.Read(pid) || !reader.Read(tid) || !reader.ReadCString(comm)) {
    return base::ErrStatus("Failed to parse PERF_RECORD_COMM");
  }

  context_->process_tracker->UpdateThread(tid, pid);
  context_->process_tracker->UpdateThreadName(
      tid, context_->storage->InternString(base::StringView(comm)),
      ThreadNamePriority::kFtrace);

  return base::OkStatus();
}

base::Status RecordParser::ParseMmap(Record record) {
  MmapRecord mmap;
  RETURN_IF_ERROR(mmap.Parse(record));
  std::optional<BuildId> build_id =
      record.session->LookupBuildId(mmap.pid, mmap.filename);
  if (IsInKernel(record.GetCpuMode())) {
    context_->mapping_tracker->CreateKernelMemoryMapping(
        BuildCreateMappingParams(mmap, std::move(mmap.filename),
                                 std::move(build_id)));
    return base::OkStatus();
  }

  context_->mapping_tracker->CreateUserMemoryMapping(
      GetUpid(mmap), BuildCreateMappingParams(mmap, std::move(mmap.filename),
                                              std::move(build_id)));

  return base::OkStatus();
}

base::Status RecordParser::ParseMmap2(Record record) {
  Mmap2Record mmap2;
  RETURN_IF_ERROR(mmap2.Parse(record));
  std::optional<BuildId> build_id = mmap2.GetBuildId();
  if (!build_id.has_value()) {
    build_id = record.session->LookupBuildId(mmap2.pid, mmap2.filename);
  }
  if (IsInKernel(record.GetCpuMode())) {
    context_->mapping_tracker->CreateKernelMemoryMapping(
        BuildCreateMappingParams(mmap2, std::move(mmap2.filename),
                                 std::move(build_id)));
    return base::OkStatus();
  }

  context_->mapping_tracker->CreateUserMemoryMapping(
      GetUpid(mmap2), BuildCreateMappingParams(mmap2, std::move(mmap2.filename),
                                               std::move(build_id)));

  return base::OkStatus();
}

UniquePid RecordParser::GetUpid(const CommonMmapRecordFields& fields) const {
  UniqueTid utid =
      context_->process_tracker->UpdateThread(fields.tid, fields.pid);
  auto upid = context_->storage->thread_table()
                  .FindById(tables::ThreadTable::Id(utid))
                  ->upid();
  PERFETTO_CHECK(upid.has_value());
  return *upid;
}

base::Status RecordParser::UpdateCounters(const Sample& sample) {
  if (!sample.read_groups.empty()) {
    return UpdateCountersInReadGroups(sample);
  }

  if (!sample.period.has_value() && !sample.attr->sample_period().has_value()) {
    return base::ErrStatus("No period for sample");
  }

  uint64_t period = sample.period.has_value() ? *sample.period
                                              : *sample.attr->sample_period();
  if (!sample.cpu.has_value()) {
    return base::ErrStatus("No cpu for sample");
  }
  sample.attr->GetOrCreateCounter(*sample.cpu)
      .AddDelta(sample.trace_ts, static_cast<double>(period));
  return base::OkStatus();
}

base::Status RecordParser::UpdateCountersInReadGroups(const Sample& sample) {
  if (!sample.cpu.has_value()) {
    return base::ErrStatus("No cpu for sample");
  }

  for (const auto& entry : sample.read_groups) {
    RefPtr<PerfEventAttr> attr =
        sample.perf_session->FindAttrForEventId(*entry.event_id);
    if (PERFETTO_UNLIKELY(!attr)) {
      return base::ErrStatus("No perf_event_attr for id %" PRIu64,
                             *entry.event_id);
    }
    attr->GetOrCreateCounter(*sample.cpu)
        .AddCount(sample.trace_ts, static_cast<double>(entry.value));
  }
  return base::OkStatus();
}

}  // namespace perfetto::trace_processor::perf_importer
