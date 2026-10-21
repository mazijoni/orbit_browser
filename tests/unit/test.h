#pragma once

// Minimal test harness (the code base is built without exceptions, which rules
// out most off-the-shelf frameworks' default configurations).

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace test {

struct Case {
  const char* name;
  std::function<void()> fn;
};

std::vector<Case>& Registry();
void Fail(const char* file, int line, const std::string& message);

struct Registrar {
  Registrar(const char* name, std::function<void()> fn) { Registry().push_back({name, fn}); }
};

std::string Narrow(const std::wstring& s);

}  // namespace test

#define TEST_CONCAT2(a, b) a##b
#define TEST_CONCAT(a, b) TEST_CONCAT2(a, b)
#define TEST(name)                                                              \
  static void name();                                                           \
  static ::test::Registrar TEST_CONCAT(registrar_, name)(#name, &name);         \
  static void name()

#define EXPECT_TRUE(cond) \
  do { if (!(cond)) ::test::Fail(__FILE__, __LINE__, "expected true: " #cond); } while (0)
#define EXPECT_FALSE(cond) \
  do { if (cond) ::test::Fail(__FILE__, __LINE__, "expected false: " #cond); } while (0)
#define EXPECT_EQ(a, b)                                                           \
  do {                                                                            \
    if (!((a) == (b))) ::test::Fail(__FILE__, __LINE__, "expected equal: " #a " == " #b); \
  } while (0)
#define EXPECT_WEQ(a, b)                                                                  \
  do {                                                                                    \
    const std::wstring _a = (a), _b = (b);                                                \
    if (_a != _b)                                                                         \
      ::test::Fail(__FILE__, __LINE__,                                                    \
                   std::string(#a " == " #b ": got '") + ::test::Narrow(_a) + "' want '" + \
                       ::test::Narrow(_b) + "'");                                         \
  } while (0)
