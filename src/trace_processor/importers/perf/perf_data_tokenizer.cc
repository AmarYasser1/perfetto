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

#include "src/trace_processor/importers/perf/perf_data_tokenizer.h"

#include <algorithm>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "perfetto/base/flat_set.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/public/compiler.h"
#include "perfetto/trace_processor/ref_counted.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "protos/perfetto/trace/clock_snapshot.pbzero.h"
#include "protos/third_party/simpleperf/record_file.pbzero.h"
#include "src/trace_processor/importers/common/clock_tracker.h"
#include "src/trace_processor/importers/common/slice_tracker.h"
#include "src/trace_processor/importers/perf/attrs_section_reader.h"
#include "src/trace_processor/importers/perf/dso_tracker.h"
#include "src/trace_processor/importers/perf/features.h"
#include "src/trace_processor/importers/perf/perf_event.h"
#include "src/trace_processor/importers/perf/perf_event_attr.h"
#include "src/trace_processor/importers/perf/perf_file.h"
#include "src/trace_processor/importers/perf/perf_session.h"
#include "src/trace_processor/importers/perf/reader.h"
#include "src/trace_processor/importers/perf/record.h"
#include "src/trace_processor/importers/proto/perf_sample_tracker.h"
#include "src/trace_processor/sorter/trace_sorter.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/util/build_id.h"
#include "src/trace_processor/util/status_macros.h"

namespace perfetto::trace_processor::perf_importer {
namespace {

void AddIds(uint8_t id_offset,
            uint64_t flags,
            base::FlatSet<uint8_t>& feature_ids) {
  for (size_t i = 0; i < sizeof(flags) * 8; ++i) {
    if (flags & 1) {
      feature_ids.insert(id_offset);
    }
    flags >>= 1;
    ++id_offset;
  }
}

base::FlatSet<uint8_t> ExtractFeatureIds(const uint64_t& flags,
                                         const uint64_t (&flags1)[3]) {
  base::FlatSet<uint8_t> feature_ids;
  AddIds(0, flags, feature_ids);
  AddIds(64, flags1[0], feature_ids);
  AddIds(128, flags1[1], feature_ids);
  AddIds(192, flags1[2], feature_ids);
  return feature_ids;
}

bool ReadTime(const Record& record, std::optional<uint64_t>& time) {
  if (!record.attr) {
    time = std::nullopt;
    return true;
  }
  Reader reader(record.payload.copy());
  if (record.header.type != PERF_RECORD_SAMPLE) {
    std::optional<size_t> offset = record.attr->time_offset_from_end();
    if (!offset.has_value()) {
      time = std::nullopt;
      return true;
    }
    if (*offset > reader.size_left()) {
      return false;
    }
    return reader.Skip(reader.size_left() - *offset) &&
           reader.ReadOptional(time);
  }

  std::optional<size_t> offset = record.attr->time_offset_from_start();
  if (!offset.has_value()) {
    time = std::nullopt;
    return true;
  }
  return reader.Skip(*offset) && reader.ReadOptional(time);
}

}  // namespace

PerfDataTokenizer::PerfDataTokenizer(TraceProcessorContext* ctx)
    : context_(ctx) {}

PerfDataTokenizer::~PerfDataTokenizer() = default;

// A normal perf.data consts of:
// [ header ]
// [ attr section ]
// [ data section ]
// [ optional feature sections ]
//
// Where each "attr" describes one event type recorded in the file.
//
// Most file format documentation is outdated or misleading, instead see
// perf_session__do_write_header() in linux/tools/perf/util/header.c.
base::Status PerfDataTokenizer::Parse(TraceBlobView blob) {
  buffer_.PushBack(std::move(blob));

  base::StatusOr<ParsingResult> result = ParsingResult::kSuccess;
  while (result.ok() && result.value() == ParsingResult::kSuccess &&
         !buffer_.empty()) {
    switch (parsing_state_) {
      case ParsingState::kParseHeader:
        result = ParseHeader();
        break;

      case ParsingState::kParseAttrs:
        result = ParseAttrs();
        break;

      case ParsingState::kSeekRecords:
        result = SeekRecords();
        break;

      case ParsingState::kParseRecords:
        result = ParseRecords();
        break;

      case ParsingState::kParseFeatures:
        result = ParseFeatures();
        break;

      case ParsingState::kParseFeatureSections:
        result = ParseFeatureSections();
        break;

      case ParsingState::kDone:
        result = base::ErrStatus("Unexpected data");
    }
  }
  return result.status();
}

base::StatusOr<PerfDataTokenizer::ParsingResult>
PerfDataTokenizer::ParseHeader() {
  auto tbv = buffer_.SliceOff(0, sizeof(header_));
  if (!tbv) {
    return ParsingResult::kMoreDataNeeded;
  }
  PERFETTO_CHECK(Reader(std::move(*tbv)).Read(header_));

  // TODO: Check for endianess (big endian will have letters reversed);
  if (memcmp(header_.magic, PerfFile::kPerfMagic,
             sizeof(PerfFile::kPerfMagic)) != 0) {
    return base::ErrStatus("Invalid magic string");
  }

  if (header_.size != sizeof(PerfFile::Header)) {
    return base::ErrStatus(
        "Failed to perf file header size. Expected %zu"
        ", found %" PRIu64,
        sizeof(PerfFile::Header), header_.size);
  }

  feature_ids_ = ExtractFeatureIds(header_.flags, header_.flags1);
  feature_headers_section_ = {header_.data.end(),
                              feature_ids_.size() * sizeof(PerfFile::Section)};
  context_->clock_tracker->SetTraceTimeClock(
      protos::pbzero::ClockSnapshot::Clock::MONOTONIC);

  PERFETTO_CHECK(buffer_.PopFrontUntil(sizeof(PerfFile::Header)));
  parsing_state_ = ParsingState::kParseAttrs;
  return ParsingResult::kSuccess;
}

base::StatusOr<PerfDataTokenizer::ParsingResult>
PerfDataTokenizer::ParseAttrs() {
  std::optional<TraceBlobView> tbv =
      buffer_.SliceOff(header_.attrs.offset, header_.attrs.size);
  if (!tbv) {
    return ParsingResult::kMoreDataNeeded;
  }

  ASSIGN_OR_RETURN(AttrsSectionReader attr_reader,
                   AttrsSectionReader::Create(header_, std::move(*tbv)));

  PerfSession::Builder builder(context_);
  while (attr_reader.CanReadNext()) {
    PerfFile::AttrsEntry entry;
    RETURN_IF_ERROR(attr_reader.ReadNext(entry));

    if (entry.ids.size % sizeof(uint64_t) != 0) {
      return base::ErrStatus("Invalid id section size: %" PRIu64,
                             entry.ids.size);
    }

    tbv = buffer_.SliceOff(entry.ids.offset, entry.ids.size);
    if (!tbv) {
      return ParsingResult::kMoreDataNeeded;
    }

    std::vector<uint64_t> ids(entry.ids.size / sizeof(uint64_t));
    PERFETTO_CHECK(Reader(std::move(*tbv)).ReadVector(ids));
    builder.AddAttrAndIds(entry.attr, std::move(ids));
  }

  ASSIGN_OR_RETURN(perf_session_, builder.Build());
  parsing_state_ = ParsingState::kSeekRecords;
  return ParsingResult::kSuccess;
}

base::StatusOr<PerfDataTokenizer::ParsingResult>
PerfDataTokenizer::SeekRecords() {
  if (!buffer_.PopFrontUntil(header_.data.offset)) {
    return ParsingResult::kMoreDataNeeded;
  }
  parsing_state_ = ParsingState::kParseRecords;
  return ParsingResult::kSuccess;
}

base::StatusOr<PerfDataTokenizer::ParsingResult>
PerfDataTokenizer::ParseRecords() {
  while (buffer_.start_offset() < header_.data.end()) {
    Record record;

    if (auto res = ParseRecord(record);
        !res.ok() || *res != ParsingResult::kSuccess) {
      return res;
    }

    if (!PushRecord(std::move(record))) {
      context_->storage->IncrementStats(stats::perf_record_skipped);
    }
  }

  parsing_state_ = ParsingState::kParseFeatureSections;
  return ParsingResult::kSuccess;
}

base::StatusOr<PerfDataTokenizer::ParsingResult> PerfDataTokenizer::ParseRecord(
    Record& record) {
  record.session = perf_session_;
  std::optional<TraceBlobView> tbv =
      buffer_.SliceOff(buffer_.start_offset(), sizeof(record.header));
  if (!tbv) {
    return ParsingResult::kMoreDataNeeded;
  }
  PERFETTO_CHECK(Reader(std::move(*tbv)).Read(record.header));

  if (record.header.size < sizeof(record.header)) {
    return base::ErrStatus("Invalid record size: %" PRIu16, record.header.size);
  }

  tbv = buffer_.SliceOff(buffer_.start_offset() + sizeof(record.header),
                         record.header.size - sizeof(record.header));
  if (!tbv) {
    return ParsingResult::kMoreDataNeeded;
  }

  record.payload = std::move(*tbv);

  base::StatusOr<RefPtr<PerfEventAttr>> attr =
      perf_session_->FindAttrForRecord(record.header, record.payload);
  if (!attr.ok()) {
    return base::ErrStatus("Unable to determine perf_event_attr for record. %s",
                           attr.status().c_message());
  }
  record.attr = *attr;

  buffer_.PopFrontBytes(record.header.size);
  return ParsingResult::kSuccess;
}

base::StatusOr<int64_t> PerfDataTokenizer::ToTraceTimestamp(
    std::optional<uint64_t> time) {
  base::StatusOr<int64_t> trace_ts =
      time.has_value()
          ? context_->clock_tracker->ToTraceTime(
                protos::pbzero::ClockSnapshot::Clock::MONOTONIC,
                static_cast<int64_t>(*time))
          : std::max(latest_timestamp_, context_->sorter->max_timestamp());

  if (PERFETTO_LIKELY(trace_ts.ok())) {
    latest_timestamp_ = std::max(latest_timestamp_, *trace_ts);
  }

  return trace_ts;
}

bool PerfDataTokenizer::PushRecord(Record record) {
  std::optional<uint64_t> time;
  if (!ReadTime(record, time)) {
    return false;
  }

  base::StatusOr<int64_t> trace_ts = ToTraceTimestamp(time);
  if (!trace_ts.ok()) {
    return false;
  }

  switch (record.header.type) {
    case PERF_RECORD_AUXTRACE_INFO:
    case PERF_RECORD_AUXTRACE:
    case PERF_RECORD_AUX:
      break;
    default:
      context_->sorter->PushPerfRecord(*trace_ts, std::move(record));
      break;
  }

  return true;
}

base::StatusOr<PerfDataTokenizer::ParsingResult>
PerfDataTokenizer::ParseFeatureSections() {
  PERFETTO_CHECK(buffer_.start_offset() == header_.data.end());
  auto tbv = buffer_.SliceOff(feature_headers_section_.offset,
                              feature_headers_section_.size);
  if (!tbv) {
    return ParsingResult::kMoreDataNeeded;
  }

  Reader reader(std::move(*tbv));
  for (auto feature_id : feature_ids_) {
    feature_sections_.emplace_back(std::piecewise_construct,
                                   std::forward_as_tuple(feature_id),
                                   std::forward_as_tuple());
    PERFETTO_CHECK(reader.Read(feature_sections_.back().second));
  }

  std::sort(feature_sections_.begin(), feature_sections_.end(),
            [](const std::pair<uint8_t, PerfFile::Section>& lhs,
               const std::pair<uint8_t, PerfFile::Section>& rhs) {
              return lhs.second.offset > rhs.second.offset;
            });

  buffer_.PopFrontUntil(feature_headers_section_.end());
  parsing_state_ = feature_sections_.empty() ? ParsingState::kDone
                                             : ParsingState::kParseFeatures;
  return ParsingResult::kSuccess;
}

base::StatusOr<PerfDataTokenizer::ParsingResult>
PerfDataTokenizer::ParseFeatures() {
  while (!feature_sections_.empty()) {
    const auto feature_id = feature_sections_.back().first;
    const auto& section = feature_sections_.back().second;
    auto tbv = buffer_.SliceOff(section.offset, section.size);
    if (!tbv) {
      return ParsingResult::kMoreDataNeeded;
    }

    RETURN_IF_ERROR(ParseFeature(feature_id, std::move(*tbv)));
    buffer_.PopFrontUntil(section.end());
    feature_sections_.pop_back();
  }

  parsing_state_ = ParsingState::kDone;
  return ParsingResult::kSuccess;
}

base::Status PerfDataTokenizer::ParseFeature(uint8_t feature_id,
                                             TraceBlobView data) {
  switch (feature_id) {
    case feature::ID_CMD_LINE: {
      ASSIGN_OR_RETURN(std::vector<std::string> args,
                       feature::ParseCmdline(std::move(data)));
      perf_session_->SetCmdline(args);
      return base::OkStatus();
    }

    case feature::ID_EVENT_DESC:
      return feature::EventDescription::Parse(
          std::move(data), [&](feature::EventDescription desc) {
            for (auto id : desc.ids) {
              perf_session_->SetEventName(id, std::move(desc.event_string));
            }
            return base::OkStatus();
          });

    case feature::ID_BUILD_ID:
      return feature::BuildId::Parse(
          std::move(data), [&](feature::BuildId build_id) {
            perf_session_->AddBuildId(
                build_id.pid, std::move(build_id.filename),
                BuildId::FromRaw(std::move(build_id.build_id)));
            return base::OkStatus();
          });

    case feature::ID_GROUP_DESC: {
      feature::HeaderGroupDesc group_desc;
      RETURN_IF_ERROR(
          feature::HeaderGroupDesc::Parse(std::move(data), group_desc));
      // TODO(carlscab): Do someting
      break;
    }

    case feature::ID_SIMPLEPERF_META_INFO: {
      feature::SimpleperfMetaInfo meta_info;
      RETURN_IF_ERROR(feature::SimpleperfMetaInfo::Parse(data, meta_info));
      for (auto it = meta_info.event_type_info.GetIterator(); it; ++it) {
        perf_session_->SetEventName(it.key().type, it.key().config, it.value());
      }
      break;
    }
    case feature::ID_SIMPLEPERF_FILE2: {
      RETURN_IF_ERROR(feature::ParseSimpleperfFile2(
          std::move(data), [&](TraceBlobView blob) {
            third_party::simpleperf::proto::pbzero::FileFeature::Decoder file(
                blob.data(), blob.length());
            DsoTracker::GetOrCreate(context_).AddSimpleperfFile2(file);
          }));

      break;
    }
    default:
      context_->storage->IncrementIndexedStats(stats::perf_features_skipped,
                                               feature_id);
  }

  return base::OkStatus();
}

void PerfDataTokenizer::NotifyEndOfFile() {}

}  // namespace perfetto::trace_processor::perf_importer
