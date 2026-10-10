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

int main() {
  int failedCases = 0;
  for (const auto& c : test::Registry()) {
    int before = test::g_failures;
    c.fn();
    bool ok = test::g_failures == before;
    if (!ok) ++failedCases;
    std::printf("%s %s\n", ok ? "[ ok ]" : "[FAIL]", c.name);
  }
  std::printf("\n%zu tests, %d failed\n", test::Registry().size(), failedCases);
  return failedCases == 0 ? 0 : 1;
}
