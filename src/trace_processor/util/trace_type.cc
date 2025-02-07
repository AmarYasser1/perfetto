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

#include "src/trace_processor/util/trace_type.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/string_view.h"
#include "src/trace_processor/importers/android_bugreport/android_log_event.h"

namespace perfetto::trace_processor {
namespace {
// Fuchsia traces have a magic number as documented here:
// https://fuchsia.googlesource.com/fuchsia/+/HEAD/docs/development/tracing/trace-format/README.md#magic-number-record-trace-info-type-0
constexpr char kFuchsiaMagic[] = {'\x10', '\x00', '\x04', '\x46',
                                  '\x78', '\x54', '\x16', '\x00'};
constexpr char kPerfMagic[] = {'P', 'E', 'R', 'F', 'I', 'L', 'E', '2'};

constexpr char kZipMagic[] = {'P', 'K', '\x03', '\x04'};

constexpr char kGzipMagic[] = {'\x1f', '\x8b'};

inline bool isspace(unsigned char c) {
  return ::isspace(c);
}

std::string RemoveWhitespace(std::string str) {
  str.erase(std::remove_if(str.begin(), str.end(), isspace), str.end());
  return str;
}

template <size_t N>
bool MatchesMagic(const uint8_t* data, size_t size, const char (&magic)[N]) {
  if (size < N) {
    return false;
  }
  return memcmp(data, magic, N) == 0;
}

base::StringView FindLine(const uint8_t* data, size_t size) {
  for (size_t i = 0; i < size; ++i) {
    if (data[i] == '\n') {
      return base::StringView(reinterpret_cast<const char*>(data), i);
    }
  }
  return base::StringView();
}

}  // namespace

const char* ToString(TraceType trace_type) {
  switch (trace_type) {
    case kJsonTraceType:
      return "JSON trace";
    case kProtoTraceType:
      return "proto trace";
    case kNinjaLogTraceType:
      return "ninja log";
    case kFuchsiaTraceType:
      return "fuchsia trace";
    case kSystraceTraceType:
      return "systrace trace";
    case kGzipTraceType:
      return "gzip trace";
    case kCtraceTraceType:
      return "ctrace trace";
    case kZipFile:
      return "ZIP file";
    case kPerfDataTraceType:
      return "perf data";
    case kAndroidLogcatTraceType:
      return "Android logcat";
    case kUnknownTraceType:
      return "unknown trace";
  }
  PERFETTO_FATAL("For GCC");
}

TraceType GuessTraceType(const uint8_t* data, size_t size) {
  if (size == 0) {
    return kUnknownTraceType;
  }

  if (MatchesMagic(data, size, kFuchsiaMagic)) {
    return kFuchsiaTraceType;
  }

  if (MatchesMagic(data, size, kPerfMagic)) {
    return kPerfDataTraceType;
  }

  if (MatchesMagic(data, size, kZipMagic)) {
    return kZipFile;
  }

  if (MatchesMagic(data, size, kGzipMagic)) {
    return kGzipTraceType;
  }

  std::string start(reinterpret_cast<const char*>(data),
                    std::min<size_t>(size, kGuessTraceMaxLookahead));

  std::string start_minus_white_space = RemoveWhitespace(start);
  if (base::StartsWith(start_minus_white_space, "{\""))
    return kJsonTraceType;
  if (base::StartsWith(start_minus_white_space, "[{\""))
    return kJsonTraceType;

  // Systrace with header but no leading HTML.
  if (base::Contains(start, "# tracer"))
    return kSystraceTraceType;

  // Systrace with leading HTML.
  // Both: <!DOCTYPE html> and <!DOCTYPE HTML> have been observed.
  std::string lower_start = base::ToLower(start);
  if (base::StartsWith(lower_start, "<!doctype html>") ||
      base::StartsWith(lower_start, "<html>"))
    return kSystraceTraceType;

  // Traces obtained from atrace -z (compress).
  // They all have the string "TRACE:" followed by 78 9C which is a zlib header
  // for "deflate, default compression, window size=32K" (see b/208691037)
  if (base::Contains(start, "TRACE:\n\x78\x9c"))
    return kCtraceTraceType;

  // Traces obtained from atrace without -z (no compression).
  if (base::Contains(start, "TRACE:\n"))
    return kSystraceTraceType;

  // Ninja's build log (.ninja_log).
  if (base::StartsWith(start, "# ninja log"))
    return kNinjaLogTraceType;

  if (AndroidLogEvent::IsAndroidLogEvent(FindLine(data, size))) {
    return kAndroidLogcatTraceType;
  }

  // Systrace with no header or leading HTML.
  if (base::StartsWith(start, " "))
    return kSystraceTraceType;

  if (base::StartsWith(start, "\x0a"))
    return kProtoTraceType;

  return kUnknownTraceType;
}

}  // namespace perfetto::trace_processor
