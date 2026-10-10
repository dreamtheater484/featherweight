#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace fw {

struct SemVer {
  int major = 0, minor = 0, patch = 0;
  std::string prerelease;  // empty for a normal release
};

// Accepts "1.2.3", "v1.2.3", "1.2" and "1.2.3-beta.1".
std::optional<SemVer> ParseSemVer(std::string_view text);

// <0 if a < b, 0 if equal, >0 if a > b. A pre-release sorts before its release.
int CompareSemVer(const SemVer& a, const SemVer& b);

}  // namespace fw
