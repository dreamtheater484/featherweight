#pragma once

// The PDF tab's new strings (F1, f1-pdf) go through the catalog's T() and Fmt() (f1-lang,
// src/core/Lang.h). Until the catalog is in this branch, both are stood in for here with the same
// signatures: T() is the English text as it is, Fmt() fills {0}..{9}. Once core/Lang.h is merged
// the stand-ins drop out by themselves.

#include <initializer_list>
#include <string>
#include <string_view>

#if __has_include("core/Lang.h")
#include "core/Lang.h"
#else
inline const wchar_t* T(const wchar_t* en) { return en; }
inline const wchar_t* T(const wchar_t* en, const wchar_t* /*context*/) { return en; }
inline std::wstring Fmt(const wchar_t* pattern, std::initializer_list<std::wstring_view> args) {
  std::wstring out;
  const std::wstring_view p(pattern);
  for (size_t at = 0; at < p.size(); ++at) {
    if (p[at] == L'{' && at + 2 < p.size() && p[at + 1] >= L'0' && p[at + 1] <= L'9' && p[at + 2] == L'}') {
      const size_t k = static_cast<size_t>(p[at + 1] - L'0');
      if (k < args.size()) out += *(args.begin() + k);
      at += 2;
    } else {
      out += p[at];
    }
  }
  return out;
}
#endif

namespace pdftext {

// "1 page" or "3 pages", with the number in it.
inline std::wstring Pages(int n) { return n == 1 ? std::wstring(T(L"1 page")) : Fmt(T(L"{0} pages"), {std::to_wstring(n)}); }

}  // namespace pdftext
