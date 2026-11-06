#include "pkg/Limits.h"

#include <cstdio>

namespace fw::pkg {

const Limits& Limits::Default() {
  static const Limits limits;
  return limits;
}

std::string FormatBytes(uint64_t bytes) {
  static const char* const kUnits[] = {"KB", "MB", "GB", "TB"};
  if (bytes < 1024) return std::to_string(bytes) + (bytes == 1 ? " byte" : " bytes");
  double v = static_cast<double>(bytes);
  int unit = -1;
  while (v >= 1024 && unit < 3) {
    v /= 1024;
    ++unit;
  }
  char buf[32];
  if (v < 10 && v != static_cast<double>(static_cast<uint64_t>(v)))
    std::snprintf(buf, sizeof(buf), "%.1f %s", v, kUnits[unit]);
  else
    std::snprintf(buf, sizeof(buf), "%.0f %s", v, kUnits[unit]);
  return buf;
}

}  // namespace fw::pkg
