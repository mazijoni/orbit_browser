#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace orbit {

// Memory/CPU figures for one process. All sizes in bytes.
//   private_working_set: resident pages not shared with other processes
//                        (Task Manager's "Memory" column).
//   working_set:         all resident pages, including shared ones.
//   commit:              private committed memory (PagefileUsage/PrivateUsage).
struct ProcessMetrics {
  uint32_t pid = 0;
  std::wstring type;  // "browser", "renderer", "gpu-process", "utility", ... or "unknown"
  uint64_t private_working_set = 0;
  uint64_t working_set = 0;
  uint64_t commit = 0;
  uint64_t cpu_time_100ns = 0;
};

// Collects |root_pid| and all of its descendant processes.
std::vector<ProcessMetrics> CollectProcessTree(uint32_t root_pid);

// Extracts Chromium's "--type=" value from a command line ("" -> "browser").
std::wstring ChromiumProcessType(const std::wstring& command_line);

}  // namespace orbit
