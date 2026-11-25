#pragma once

// The PDF tab's new strings (F1, f1-pdf) go through the catalog's T() (f1-lang, docs/f1-plan.md).
// Until the catalog is in this branch, T() is the English text as it is.

#include <initializer_list>
#include <string>

#if __has_include("core/Lang.h")
#include "core/Lang.h"
#else
inline std::wstring T(const wchar_t* text, const wchar_t* /*context*/ = nullptr) { return text; }
#endif

namespace pdftext {

// A translated sentence with its blanks filled: "{0}" is the first value, "{1}" the second.
inline std::wstring Fill(std::wstring pattern, std::initializer_list<std::wstring> values) {
  int k = 0;
  for (const std::wstring& v : values) {
    const std::wstring mark = L"{" + std::to_wstring(k++) + L"}";
    for (size_t at = pattern.find(mark); at != std::wstring::npos; at = pattern.find(mark, at + v.size()))
      pattern.replace(at, mark.size(), v);
  }
  return pattern;
}

// "1 page" or "3 pages", with the number in it.
inline std::wstring Pages(int n) {
  return n == 1 ? std::wstring(T(L"1 page")) : Fill(T(L"{0} pages"), {std::to_wstring(n)});
}

}  // namespace pdftext
