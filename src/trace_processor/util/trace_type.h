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

#ifndef SRC_TRACE_PROCESSOR_UTIL_TRACE_TYPE_H_
#define SRC_TRACE_PROCESSOR_UTIL_TRACE_TYPE_H_

#include <cstddef>
#include <cstdint>

namespace perfetto::trace_processor {

enum TraceType {
  kUnknownTraceType,
  kProtoTraceType,
  kJsonTraceType,
  kFuchsiaTraceType,
  kSystraceTraceType,
  kGzipTraceType,
  kCtraceTraceType,
  kNinjaLogTraceType,
  kZipFile,
  kPerfDataTraceType,
  kAndroidLogcatTraceType,
};

constexpr size_t kGuessTraceMaxLookahead = 64;
TraceType GuessTraceType(const uint8_t* data, size_t size);
const char* ToString(TraceType type);

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_UTIL_TRACE_TYPE_H_
