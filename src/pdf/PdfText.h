#pragma once

// The PDF tab's strings (F1, f1-pdf) go through f1-lang's catalog: T() and Fmt() (core/Lang.h).

#include <string>

#include "core/Lang.h"

namespace pdftext {

// "1 page" or "3 pages", with the number in it.
inline std::wstring Pages(int n) { return n == 1 ? std::wstring(T(L"1 page")) : Fmt(T(L"{0} pages"), {std::to_wstring(n)}); }

}  // namespace pdftext
