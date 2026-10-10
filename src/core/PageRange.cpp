#include "core/PageRange.h"

#include "core/Strings.h"

namespace fw {

std::optional<std::vector<int>> ParsePageRanges(std::wstring_view text, int pageCount) {
  std::vector<int> pages;
  std::wstring normalized(text);
  for (auto& c : normalized)
    if (c == L';') c = L',';
  for (const auto& rawPart : Split(normalized, L',')) {
    std::wstring part = Trim(rawPart);
    if (part.empty()) continue;
    size_t dash = part.find(L'-');
    int first = 0, last = 0;
    if (dash == std::wstring::npos) {
      if (!ParseInt(part, first)) return std::nullopt;
      last = first;
    } else {
      std::wstring a = Trim(std::wstring_view(part).substr(0, dash));
      std::wstring b = Trim(std::wstring_view(part).substr(dash + 1));
      if (a.empty()) first = 1;
      else if (!ParseInt(a, first)) return std::nullopt;
      if (b.empty()) last = pageCount;
      else if (!ParseInt(b, last)) return std::nullopt;
    }
    if (first < 1 || last < 1 || first > pageCount || last > pageCount) return std::nullopt;
    if (first <= last) {
      for (int p = first; p <= last; ++p) pages.push_back(p - 1);
    } else {
      for (int p = first; p >= last; --p) pages.push_back(p - 1);
    }
  }
  if (pages.empty()) return std::nullopt;
  return pages;
}

}  // namespace fw
