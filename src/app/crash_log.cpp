#include "app/crash_log.h"

#include <windows.h>

#include <crtdbg.h>
#include <csignal>
#include <dbghelp.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/log.h"

namespace orbit::crash_log {
namespace {

// "libcef.dll+0x1234" for an address, or "?" outside any module.
std::string ModuleOffset(const void* address) {
  HMODULE module = nullptr;
  if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(address), &module)) {
    return "?";
  }
  char path[MAX_PATH] = {};
  ::GetModuleFileNameA(module, path, MAX_PATH);
  const char* name = std::strrchr(path, '\\');
  char out[MAX_PATH + 32];
  std::snprintf(out, sizeof(out), "%s+0x%llx", name ? name + 1 : path,
                static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(address) -
                                                reinterpret_cast<uintptr_t>(module)));
  return out;
}

std::filesystem::path g_dump_dir;
HANDLE g_exit_log = INVALID_HANDLE_VALUE;  // orbit.log, appended without locks
bool g_clean_exit = false;

// The process is ending without main()'s clean shutdown (something called
// ExitProcess): log who did, from the loader's detach. Other threads are gone
// by now and may have held the log's lock, so this writes on its own handle.
struct ExitWatch {
  ~ExitWatch() {
    if (g_clean_exit || g_exit_log == INVALID_HANDLE_VALUE) return;
    void* frames[62];
    const USHORT n = ::RtlCaptureStackBackTrace(0, 62, frames, nullptr);
    std::string text = "process exit without a clean shutdown, called from:";
    for (USHORT i = 0; i < n; ++i) text += " " + ModuleOffset(frames[i]);
    SYSTEMTIME t;
    ::GetLocalTime(&t);
    char stamp[40];
    std::snprintf(stamp, sizeof(stamp), "%04d-%02d-%02d %02d:%02d:%02d.%03d ERROR ", t.wYear, t.wMonth, t.wDay,
                  t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    text = stamp + text + "\n";
    DWORD written = 0;
    ::WriteFile(g_exit_log, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
  }
};

std::atomic<int> g_logged{0};
constexpr int kMaxLogged = 3;  // first-chance faults may be handled; keep the log short

bool IsFatal(DWORD code) {
  return code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_STACK_OVERFLOW ||
         code == EXCEPTION_ILLEGAL_INSTRUCTION || code == EXCEPTION_INT_DIVIDE_BY_ZERO ||
         code == 0xC0000409 /* fail fast / stack buffer overrun */ ||
         code == 0xC000041D /* fatal user callback exception */ || code == 0xC0000374 /* heap corruption */;
}

LONG CALLBACK OnException(EXCEPTION_POINTERS* info) {
  const DWORD code = info->ExceptionRecord->ExceptionCode;
  if (!IsFatal(code) || g_logged.fetch_add(1) >= kMaxLogged) return EXCEPTION_CONTINUE_SEARCH;
  const void* at = info->ExceptionRecord->ExceptionAddress;
  ORBIT_LOG_ERROR("crash: exception 0x%08lx at %s (thread %lu)", code, ModuleOffset(at).c_str(),
                  ::GetCurrentThreadId());
  LogStack("crash:  ");
  return EXCEPTION_CONTINUE_SEARCH;
}

void PruneDumps() {
  std::error_code ec;
  std::vector<std::filesystem::path> dumps;
  for (const auto& entry : std::filesystem::directory_iterator(g_dump_dir, ec)) {
    if (entry.path().extension() == L".dmp") dumps.push_back(entry.path());
  }
  if (dumps.size() <= 5) return;
  std::sort(dumps.begin(), dumps.end());  // names sort by time
  for (size_t i = 0; i + 5 < dumps.size(); ++i) std::filesystem::remove(dumps[i], ec);
}

LONG WINAPI OnUnhandled(EXCEPTION_POINTERS* info) {
  ORBIT_LOG_ERROR("crash: unhandled exception 0x%08lx at %s", info->ExceptionRecord->ExceptionCode,
                  ModuleOffset(info->ExceptionRecord->ExceptionAddress).c_str());
  if (!g_dump_dir.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(g_dump_dir, ec);
    SYSTEMTIME t;
    ::GetLocalTime(&t);
    wchar_t name[64];
    std::swprintf(name, 64, L"orbit-%04d%02d%02d-%02d%02d%02d.dmp", t.wYear, t.wMonth, t.wDay, t.wHour,
                  t.wMinute, t.wSecond);
    const std::filesystem::path path = g_dump_dir / name;
    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                nullptr);
    if (file != INVALID_HANDLE_VALUE) {
      MINIDUMP_EXCEPTION_INFORMATION exception{::GetCurrentThreadId(), info, FALSE};
      const BOOL ok = ::MiniDumpWriteDump(::GetCurrentProcess(), ::GetCurrentProcessId(), file,
                                          static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithThreadInfo),
                                          &exception, nullptr, nullptr);
      ::CloseHandle(file);
      ORBIT_LOG_ERROR("crash: minidump %s", ok ? "written to the Crashes folder" : "failed");
      PruneDumps();
    }
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

int OnCrtReport(int type, char* message, int* result) {
  if (type == _CRT_WARN) return FALSE;
  ORBIT_LOG_ERROR("crash: CRT report: %s", message ? message : "");
  LogStack("crash:  ");
  *result = 0;
  return FALSE;
}

void OnAbort(int) {
  ORBIT_LOG_ERROR("crash: abort()");
  LogStack("crash:  ");
}

}  // namespace

void LogStack(const char* prefix) {
  static std::atomic<bool> symbols{false};
  HANDLE process = ::GetCurrentProcess();
  if (!symbols.exchange(true)) {
    ::SymSetOptions(::SymGetOptions() | SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    ::SymInitialize(process, nullptr, TRUE);
  }
  void* frames[32];
  const USHORT n = ::RtlCaptureStackBackTrace(0, 32, frames, nullptr);
  alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256];
  for (USHORT i = 0; i < n; ++i) {
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 255;
    DWORD64 displacement = 0;
    IMAGEHLP_LINE64 line{sizeof(line)};
    DWORD line_displacement = 0;
    const DWORD64 address = reinterpret_cast<DWORD64>(frames[i]);
    const bool named = ::SymFromAddr(process, address, &displacement, symbol);
    const bool lined = ::SymGetLineFromAddr64(process, address, &line_displacement, &line);
    ORBIT_LOG_ERROR("%s #%u %s %s %s:%lu", prefix, i, ModuleOffset(frames[i]).c_str(),
                    named ? symbol->Name : "", lined ? line.FileName : "", lined ? line.LineNumber : 0ul);
  }
}

void Install(const std::filesystem::path& user_data) {
  g_dump_dir = user_data / L"Crashes";
  ::SetUnhandledExceptionFilter(&OnUnhandled);  // again on each call: Chromium may replace it
  static bool once = false;
  if (once) return;
  once = true;
  g_exit_log = ::CreateFileW((user_data / L"orbit.log").c_str(), FILE_APPEND_DATA,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
  static ExitWatch watch;
  ::AddVectoredExceptionHandler(1, &OnException);
  _CrtSetReportHook2(_CRT_RPTHOOK_INSTALL, &OnCrtReport);
  std::signal(SIGABRT, &OnAbort);
}

void MarkCleanExit() { g_clean_exit = true; }

}  // namespace orbit::crash_log
