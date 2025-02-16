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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_PERF_PERF_DATA_TOKENIZER_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_PERF_PERF_DATA_TOKENIZER_H_

#include <stdint.h>
#include <cstdint>
#include <optional>
#include <vector>

#include "perfetto/base/flat_set.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/trace_processor/ref_counted.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "src/trace_processor/importers/common/chunked_trace_reader.h"
#include "src/trace_processor/importers/perf/perf_file.h"
#include "src/trace_processor/importers/perf/perf_session.h"
#include "src/trace_processor/util/trace_blob_view_reader.h"

namespace perfetto {
namespace trace_processor {
class TraceProcessorContext;

namespace perf_importer {

struct Record;

class PerfDataTokenizer : public ChunkedTraceReader {
 public:
  explicit PerfDataTokenizer(TraceProcessorContext*);
  ~PerfDataTokenizer() override;
  PerfDataTokenizer(const PerfDataTokenizer&) = delete;
  PerfDataTokenizer& operator=(const PerfDataTokenizer&) = delete;

  // ChunkedTraceReader implementation
  base::Status Parse(TraceBlobView) override;
  void NotifyEndOfFile() override;

 private:
  enum class ParsingState {
    kParseHeader,
    kParseAttrs,
    kSeekRecords,
    kParseRecords,
    kParseFeatureSections,
    kParseFeatures,
    kDone,
  };
  enum class ParsingResult { kMoreDataNeeded = 0, kSuccess = 1 };

  base::StatusOr<ParsingResult> ParseHeader();
  base::StatusOr<ParsingResult> ParseAttrs();
  base::StatusOr<ParsingResult> SeekRecords();
  base::StatusOr<ParsingResult> ParseRecords();
  base::StatusOr<ParsingResult> ParseFeatureSections();
  base::StatusOr<ParsingResult> ParseFeatures();

  base::StatusOr<PerfDataTokenizer::ParsingResult> ParseRecord(Record& record);
  bool PushRecord(Record record);
  base::Status ParseFeature(uint8_t feature_id, TraceBlobView payload);

  base::StatusOr<int64_t> ToTraceTimestamp(std::optional<uint64_t> time);

  TraceProcessorContext* context_;

  ParsingState parsing_state_ = ParsingState::kParseHeader;

  PerfFile::Header header_;
  base::FlatSet<uint8_t> feature_ids_;
  PerfFile::Section feature_headers_section_;
  // Sections for the features present in the perf file sorted by descending
  // section offset. This is done so that we can pop from the back as we process
  // the sections.
  std::vector<std::pair<uint8_t, PerfFile::Section>> feature_sections_;

  RefPtr<PerfSession> perf_session_;

  util::TraceBlobViewReader buffer_;

  int64_t latest_timestamp_ = 0;
};

}  // namespace perf_importer
}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PERF_PERF_DATA_TOKENIZER_H_
