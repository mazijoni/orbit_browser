#include "core/log.h"

#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>

namespace orbit::log {
namespace {

void DebugSink(Level level, const char* file, int line, const char* message) {
  static constexpr const char* kNames[] = {"VERBOSE", "INFO", "WARNING", "ERROR"};
  char buffer[1200];
  std::snprintf(buffer, sizeof(buffer), "[orbit %s %s:%d] %s\n",
                kNames[static_cast<int>(level)], file, line, message);
  ::OutputDebugStringA(buffer);
}

std::atomic<Sink> g_sink{&DebugSink};
std::atomic<Level> g_min_level{Level::kInfo};

}  // namespace

void SetSink(Sink sink) { g_sink.store(sink ? sink : &DebugSink); }
void SetMinLevel(Level level) { g_min_level.store(level); }

void Write(Level level, const char* file, int line, const char* format, ...) {
  if (level < g_min_level.load()) return;
  char message[1024];
  va_list args;
  va_start(args, format);
  std::vsnprintf(message, sizeof(message), format, args);
  va_end(args);
  // Strip directories from __FILE__ to keep logs short and free of build paths.
  const char* base = file;
  for (const char* p = file; *p; ++p) {
    if (*p == '\\' || *p == '/') base = p + 1;
  }
  g_sink.load()(level, base, line, message);
}

}  // namespace orbit::log
