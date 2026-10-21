#include "core/process_metrics.h"

#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <winternl.h>

#include <map>

namespace orbit {
namespace {

// Reads a process's command line through its PEB using only documented
// structures (winternl.h). Sandboxed Chromium children may refuse
// PROCESS_VM_READ; callers then report the type as "unknown".
std::wstring ReadCommandLine(HANDLE process) {
  using NtQueryInformationProcessFn =
      NTSTATUS(NTAPI*)(HANDLE, PROCESSINFOCLASS, PVOID, ULONG, PULONG);
  static const auto query = reinterpret_cast<NtQueryInformationProcessFn>(
      ::GetProcAddress(::GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess"));
  if (!query) return {};
  PROCESS_BASIC_INFORMATION pbi{};
  if (query(process, ProcessBasicInformation, &pbi, sizeof(pbi), nullptr) != 0 ||
      !pbi.PebBaseAddress) {
    return {};
  }
  PEB peb{};
  RTL_USER_PROCESS_PARAMETERS params{};
  SIZE_T read = 0;
  if (!::ReadProcessMemory(process, pbi.PebBaseAddress, &peb, sizeof(peb), &read) ||
      !::ReadProcessMemory(process, peb.ProcessParameters, &params, sizeof(params), &read)) {
    return {};
  }
  const USHORT bytes = params.CommandLine.Length;
  if (bytes == 0 || bytes > 32766 * sizeof(wchar_t)) return {};
  std::wstring command_line(bytes / sizeof(wchar_t), L'\0');
  if (!::ReadProcessMemory(process, params.CommandLine.Buffer, command_line.data(), bytes,
                           &read)) {
    return {};
  }
  return command_line;
}

}  // namespace

std::wstring ChromiumProcessType(const std::wstring& command_line) {
  if (command_line.empty()) return L"unknown";
  const size_t pos = command_line.find(L"--type=");
  if (pos == std::wstring::npos) return L"browser";
  const size_t start = pos + 7;
  size_t end = command_line.find_first_of(L" \"", start);
  if (end == std::wstring::npos) end = command_line.size();
  return command_line.substr(start, end - start);
}

std::vector<ProcessMetrics> CollectProcessTree(uint32_t root_pid) {
  std::map<uint32_t, uint32_t> parent_of;
  HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return {};
  PROCESSENTRY32W entry{sizeof(entry)};
  for (BOOL ok = ::Process32FirstW(snapshot, &entry); ok; ok = ::Process32NextW(snapshot, &entry)) {
    parent_of[entry.th32ProcessID] = entry.th32ParentProcessID;
  }
  ::CloseHandle(snapshot);

  // Breadth-first expansion of the tree rooted at |root_pid|. (PIDs can be
  // reused, but a reused PID cannot claim a still-running parent's id.)
  std::vector<uint32_t> tree{root_pid};
  for (size_t i = 0; i < tree.size(); ++i) {
    for (const auto& [pid, parent] : parent_of) {
      if (parent == tree[i] && pid != tree[i] && pid != 0) tree.push_back(pid);
    }
  }

  std::vector<ProcessMetrics> out;
  for (uint32_t pid : tree) {
    ProcessMetrics m;
    m.pid = pid;
    HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!process) process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) continue;
    m.type = (pid == root_pid) ? L"browser" : ChromiumProcessType(ReadCommandLine(process));
    PROCESS_MEMORY_COUNTERS_EX2 counters{};
    counters.cb = sizeof(counters);
    if (::GetProcessMemoryInfo(process, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                               sizeof(counters))) {
      m.private_working_set = counters.PrivateWorkingSetSize;
      m.working_set = counters.WorkingSetSize;
      m.commit = counters.PrivateUsage;
    }
    FILETIME created, exited, kernel, user;
    if (::GetProcessTimes(process, &created, &exited, &kernel, &user)) {
      auto to64 = [](FILETIME f) {
        return (static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime;
      };
      m.cpu_time_100ns = to64(kernel) + to64(user);
    }
    ::CloseHandle(process);
    out.push_back(std::move(m));
  }
  return out;
}

}  // namespace orbit
