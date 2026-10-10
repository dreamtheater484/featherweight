#include "core/SemVer.h"

namespace fw {

std::optional<SemVer> ParseSemVer(std::string_view text) {
  if (!text.empty() && (text[0] == 'v' || text[0] == 'V')) text.remove_prefix(1);
  SemVer v;
  size_t dash = text.find('-');
  if (dash != std::string_view::npos) {
    v.prerelease = std::string(text.substr(dash + 1));
    text = text.substr(0, dash);
    if (v.prerelease.empty()) return std::nullopt;
  }
  int parts[3] = {0, 0, 0};
  int count = 0;
  size_t pos = 0;
  while (pos <= text.size() && count < 3) {
    size_t dot = text.find('.', pos);
    std::string_view piece = text.substr(pos, dot == std::string_view::npos ? std::string_view::npos : dot - pos);
    if (piece.empty() || piece.size() > 6) return std::nullopt;
    int n = 0;
    for (char c : piece) {
      if (c < '0' || c > '9') return std::nullopt;
      n = n * 10 + (c - '0');
    }
    parts[count++] = n;
    if (dot == std::string_view::npos) {
      pos = text.size() + 1;
      break;
    }
    pos = dot + 1;
  }
  if (count < 2 || pos <= text.size()) return std::nullopt;
  v.major = parts[0];
  v.minor = parts[1];
  v.patch = parts[2];
  return v;
}

int CompareSemVer(const SemVer& a, const SemVer& b) {
  if (a.major != b.major) return a.major < b.major ? -1 : 1;
  if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
  if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
  if (a.prerelease == b.prerelease) return 0;
  if (a.prerelease.empty()) return 1;
  if (b.prerelease.empty()) return -1;
  return a.prerelease < b.prerelease ? -1 : 1;
}

}  // namespace fw
