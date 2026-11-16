// Tiny dependency-free test harness. TEST registers a function; CHECK records
// a failure and continues; REQUIRE stops the current test.
#pragma once

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace drtest {

struct Case {
  const char* name;
  std::function<void()> fn;
};

std::vector<Case>& registry();
void record_failure(const char* file, int line, const std::string& expr);
std::string temp_dir(const char* name);

struct Registrar {
  Registrar(const char* name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};

struct Abort {};

}  // namespace drtest

#define DR_CONCAT2(a, b) a##b
#define DR_CONCAT(a, b) DR_CONCAT2(a, b)

#define TEST(name)                                                              \
  static void name();                                                           \
  static ::drtest::Registrar DR_CONCAT(reg_, name)(#name, name);                \
  static void name()

#define CHECK(expr)                                                             \
  do {                                                                          \
    if (!(expr)) ::drtest::record_failure(__FILE__, __LINE__, #expr);           \
  } while (0)

#define REQUIRE(expr)                                                           \
  do {                                                                          \
    if (!(expr)) {                                                              \
      ::drtest::record_failure(__FILE__, __LINE__, #expr);                      \
      throw ::drtest::Abort{};                                                  \
    }                                                                           \
  } while (0)
