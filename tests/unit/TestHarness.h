#pragma once

// A tiny dependency-free test harness.
#include <cmath>
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
void Fail(const char* file, int line, const std::string& what);

struct Registrar {
  Registrar(const char* name, std::function<void()> fn) { Registry().push_back({name, std::move(fn)}); }
};

}  // namespace test

#define FW_CONCAT2(a, b) a##b
#define FW_CONCAT(a, b) FW_CONCAT2(a, b)
#define TEST(name)                                                              \
  static void name();                                                           \
  static test::Registrar FW_CONCAT(reg_, name)(#name, name);                    \
  static void name()

#define CHECK(cond) \
  do { if (!(cond)) test::Fail(__FILE__, __LINE__, #cond); } while (0)
#define CHECK_EQ(a, b) \
  do { if (!((a) == (b))) test::Fail(__FILE__, __LINE__, #a " == " #b); } while (0)
#define CHECK_NEAR(a, b, eps) \
  do { if (std::fabs((a) - (b)) > (eps)) test::Fail(__FILE__, __LINE__, #a " ~= " #b); } while (0)
