#include "TestHarness.h"

namespace test {

static int g_failures = 0;

std::vector<Case>& Registry() {
  static std::vector<Case> cases;
  return cases;
}

void Fail(const char* file, int line, const std::string& what) {
  ++g_failures;
  std::printf("  FAILED %s(%d): %s\n", file, line, what.c_str());
}

}  // namespace test

// fw_tests [name]: every test, or those whose names contain `name`.
int main(int argc, char** argv) {
  int failedCases = 0;
  const std::string only = argc > 1 ? argv[1] : "";
  for (const auto& c : test::Registry()) {
    if (!only.empty() && std::string(c.name).find(only) == std::string::npos) continue;
    int before = test::g_failures;
    c.fn();
    bool ok = test::g_failures == before;
    if (!ok) ++failedCases;
    std::printf("%s %s\n", ok ? "[ ok ]" : "[FAIL]", c.name);
  }
  std::printf("\n%zu tests, %d failed\n", test::Registry().size(), failedCases);
  return failedCases == 0 ? 0 : 1;
}
