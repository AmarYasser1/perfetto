/*
 * Copyright (C) 2024 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_ZIP_ZIP_TRACE_READER_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_ZIP_ZIP_TRACE_READER_H_

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "src/trace_processor/importers/common/chunked_trace_reader.h"
#include "src/trace_processor/util/trace_type.h"
#include "src/trace_processor/util/zip_reader.h"

namespace perfetto::trace_processor {

class ForwardingTraceParser;
class TraceProcessorContext;

// Forwards files contained in a ZIP to the appropiate ChunkedTraceReader. It is
// guaranteed that proto traces will be parsed first.
class ZipTraceReader : public ChunkedTraceReader {
 public:
  explicit ZipTraceReader(TraceProcessorContext* context);
  ~ZipTraceReader() override;

  // ChunkedTraceReader implementation
  base::Status Parse(TraceBlobView) override;
  void NotifyEndOfFile() override;

 private:
  // Represents a file in the ZIP file. Used to sort them before sending the
  // files one by one to a `ForwardingTraceParser` instance.
  struct Entry {
    // File name. Used to break ties.
    std::string name;
    // Position in the zip file. Used to break ties.
    size_t index;
    // Trace type. This is the main attribute traces are ordered by. Proto
    // traces are always parsed first as they might contains clock sync
    // data needed to correctly parse other traces.
    TraceType trace_type;
    TraceBlobView uncompressed_data;
    // True for proto trace_types whose fist message is a ModuleSymbols packet
    bool has_symbols = false;
    // Comparator used to determine the order in which files in the ZIP will be
    // read.
    bool operator<(const Entry& rhs) const;
  };

  base::Status NotifyEndOfFileImpl();
  static base::StatusOr<std::vector<Entry>> ExtractEntries(
      std::vector<util::ZipFile> files);
  base::Status ParseEntry(Entry entry);

  TraceProcessorContext* const context_;
  util::ZipReader zip_reader_;
  // For every file in the ZIP we will create a `ForwardingTraceParser`instance
  // and send that file to it for tokenization. The instances are kept around
  // here as some tokenizers might keep state that is later needed after
  // sorting.
  std::vector<std::unique_ptr<ForwardingTraceParser>> parsers_;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_ZIP_ZIP_TRACE_READER_H_
