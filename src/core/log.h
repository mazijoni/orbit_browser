#pragma once

// Minimal leveled logging for application code. Messages go to a pluggable sink
// (the browser installs one that writes to the user-data log file). Privacy
// rule: never log URLs, page titles, or form data above kVerbose.

namespace orbit::log {

enum class Level { kVerbose = 0, kInfo = 1, kWarning = 2, kError = 3 };

using Sink = void (*)(Level level, const char* file, int line, const char* message);

void SetSink(Sink sink);
void SetMinLevel(Level level);
void Write(Level level, const char* file, int line, const char* format, ...);

}  // namespace orbit::log

#define ORBIT_LOG(level, ...) \
  ::orbit::log::Write(::orbit::log::Level::level, __FILE__, __LINE__, __VA_ARGS__)
#define ORBIT_LOG_VERBOSE(...) ORBIT_LOG(kVerbose, __VA_ARGS__)
#define ORBIT_LOG_INFO(...) ORBIT_LOG(kInfo, __VA_ARGS__)
#define ORBIT_LOG_WARNING(...) ORBIT_LOG(kWarning, __VA_ARGS__)
#define ORBIT_LOG_ERROR(...) ORBIT_LOG(kError, __VA_ARGS__)
