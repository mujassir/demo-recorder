#include <cstring>
#include <filesystem>
#include <random>

#include "test.h"

namespace drtest {

namespace {
int g_failures = 0;
}

std::vector<Case>& registry() {
  static std::vector<Case> cases;
  return cases;
}

void record_failure(const char* file, int line, const std::string& expr) {
  ++g_failures;
  std::fprintf(stderr, "  FAIL %s:%d: %s\n", file, line, expr.c_str());
}

std::string temp_dir(const char* name) {
  namespace fs = std::filesystem;
  std::random_device rd;
  fs::path p = fs::temp_directory_path() / ("dr_test_" + std::string(name) + "_" + std::to_string(rd()));
  fs::remove_all(p);
  fs::create_directories(p);
  return p.string();
}

}  // namespace drtest

int main(int argc, char** argv) {
  const char* filter = argc > 1 ? argv[1] : nullptr;
  int ran = 0;
  int failed_cases = 0;
  for (auto& c : drtest::registry()) {
    if (filter && !std::strstr(c.name, filter)) continue;
    int before = drtest::g_failures;
    try {
      c.fn();
    } catch (const drtest::Abort&) {
    } catch (const std::exception& e) {
      drtest::record_failure(c.name, 0, std::string("exception: ") + e.what());
    }
    ++ran;
    bool failed = drtest::g_failures != before;
    failed_cases += failed;
    std::printf("%s %s\n", failed ? "[FAIL]" : "[ ok ]", c.name);
  }
  std::printf("\n%d tests, %d failed\n", ran, failed_cases);
  return failed_cases == 0 ? 0 : 1;
}
