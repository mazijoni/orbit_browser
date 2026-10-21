#include <cstring>

#include "core/strings.h"
#include "test.h"

namespace test {
namespace {
int g_failures = 0;
}

std::vector<Case>& Registry() {
  static std::vector<Case> cases;
  return cases;
}

void Fail(const char* file, int line, const std::string& message) {
  ++g_failures;
  std::printf("    FAIL %s:%d: %s\n", file, line, message.c_str());
}

std::string Narrow(const std::wstring& s) { return orbit::ToUtf8(s); }

}  // namespace test

int main(int argc, char** argv) {
  const char* filter = argc > 1 ? argv[1] : nullptr;
  int run = 0, failed = 0;
  for (const auto& c : test::Registry()) {
    if (filter && !std::strstr(c.name, filter)) continue;
    const int before = test::g_failures;
    c.fn();
    ++run;
    const bool ok = test::g_failures == before;
    if (!ok) ++failed;
    std::printf("[%s] %s\n", ok ? " OK " : "FAIL", c.name);
  }
  std::printf("\n%d test(s), %d failed\n", run, failed);
  return failed == 0 ? 0 : 1;
}
