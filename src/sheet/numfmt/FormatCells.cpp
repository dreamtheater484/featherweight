#include "sheet/numfmt/FormatCells.h"

#include <algorithm>

namespace fw::sheet {

namespace {

std::wstring Zeros(int decimals) { return decimals > 0 ? L"." + std::wstring(static_cast<size_t>(decimals), L'0') : L""; }

// A pattern's two decimals ("0.00") as `decimals`.
std::wstring WithDecimals(std::wstring p, int decimals) {
  for (size_t at = p.find(L"0.00"); at != std::wstring::npos; at = p.find(L"0.00", at + 1))
    p.replace(at + 1, 3, Zeros(decimals));
  return p;
}

std::wstring FirstSection(const std::wstring& code) {
  size_t i = 0;
  for (bool quoted = false; i < code.size(); ++i) {
    if (code[i] == '"') quoted = !quoted;
    else if (code[i] == '\\') ++i;
    else if (code[i] == ';' && !quoted) break;
  }
  return code.substr(0, std::min(i, code.size()));
}

// The four negative styles over a positive pattern: -1234.10, red 1234.10, (1234.10), red (1234.10).
std::wstring Negatives(const std::wstring& p, int style, const std::wstring& ownNegative) {
  switch (style) {
    case 1: return p + L";[Red]" + (ownNegative.empty() ? p : ownNegative);
    case 2: return p + L"_);(" + p + L")";
    case 3: return p + L"_);[Red](" + p + L")";
    default: return ownNegative.empty() ? p : p + L";" + ownNegative;
  }
}

}  // namespace

std::vector<std::wstring> FormatTypes(FormatCategory category, const NumLocale&) {
  switch (category) {
    case FormatCategory::Date:
      return {L"m/d/yyyy", L"[$-x-sysdate]dddd, mmmm dd, yyyy", L"d-mmm", L"d-mmm-yy", L"dd-mmm-yy", L"mmm-yy",
              L"mmmm-yy", L"mmmm d, yyyy", L"d-mmm-yyyy", L"m/d/yy h:mm", L"m/d/yyyy h:mm"};
    case FormatCategory::Time:
      return {L"[$-x-systime]h:mm:ss AM/PM", L"h:mm", L"h:mm AM/PM", L"h:mm:ss", L"h:mm:ss AM/PM", L"mm:ss.0",
              L"[h]:mm:ss", L"m/d/yy h:mm AM/PM"};
    case FormatCategory::Fraction:
      return {L"# ?/?", L"# ??/??", L"# ???/???", L"# ?/2", L"# ?/4", L"# ?/8", L"# ??/16", L"# ?/10", L"# ??/100"};
    default: return {};
  }
}

std::wstring FormatCode(const FormatChoice& c, const NumLocale& locale) {
  const int d = std::clamp(c.decimals, 0, 30);
  switch (c.category) {
    case FormatCategory::General: return L"General";
    case FormatCategory::Text: return L"@";
    case FormatCategory::Custom: return c.code.empty() ? L"General" : c.code;
    case FormatCategory::Percentage: return L"0" + Zeros(d) + L"%";
    case FormatCategory::Scientific: return L"0" + Zeros(d) + L"E+00";
    case FormatCategory::Number: return Negatives(std::wstring(c.thousands ? L"#,##0" : L"0") + Zeros(d), c.negative, {});
    case FormatCategory::Currency: {
      // The region's currency format (built-in 7: "$#,##0.00_);($#,##0.00)", en-NL "€ #,##0.00;€ -#,##0.00").
      const std::wstring own = WithDecimals(locale.builtin[7], d);
      std::wstring p = FirstSection(own);
      const std::wstring rest = own.size() > p.size() ? own.substr(p.size() + 1) : std::wstring();
      if (p.size() >= 2 && p.compare(p.size() - 2, 2, L"_)") == 0) p.resize(p.size() - 2);  // the pad is the style's
      return Negatives(p, c.negative, rest.find(L'(') == std::wstring::npos ? rest : std::wstring());
    }
    case FormatCategory::Accounting: return WithDecimals(locale.builtin[44], d);
    case FormatCategory::Date:
    case FormatCategory::Time:
    case FormatCategory::Fraction: {
      const auto types = FormatTypes(c.category, locale);
      return types.empty() ? L"General" : types[static_cast<size_t>(std::clamp(c.type, 0, static_cast<int>(types.size()) - 1))];
    }
  }
  return L"General";
}

FormatChoice FormatChoiceOf(std::wstring_view code, const NumLocale& locale) {
  FormatChoice c;
  if (code.empty() || code == L"General") return c;
  auto match = [&](FormatCategory category, int decimals, bool thousands, int negative, int type) {
    FormatChoice x;
    x.category = category, x.decimals = decimals, x.thousands = thousands, x.negative = negative, x.type = type;
    if (FormatCode(x, locale) != code) return false;
    c = x;
    return true;
  };
  if (code == L"@") return c.category = FormatCategory::Text, c;
  for (int d = 0; d <= 30; ++d) {
    for (int neg = 0; neg < 4; ++neg)
      if (match(FormatCategory::Number, d, false, neg, 0) || match(FormatCategory::Number, d, true, neg, 0) ||
          match(FormatCategory::Currency, d, false, neg, 0))
        return c;
    if (match(FormatCategory::Accounting, d, false, 0, 0) || match(FormatCategory::Percentage, d, false, 0, 0) ||
        match(FormatCategory::Scientific, d, false, 0, 0))
      return c;
  }
  for (FormatCategory cat : {FormatCategory::Date, FormatCategory::Time, FormatCategory::Fraction}) {
    const auto types = FormatTypes(cat, locale);
    for (size_t k = 0; k < types.size(); ++k)
      if (match(cat, 2, false, 0, static_cast<int>(k))) return c;
  }
  c = FormatChoice{};
  c.category = FormatCategory::Custom;
  c.code = code;
  return c;
}

namespace {

// One section of a code with a decimal more (delta 1) or less (-1), as Excel's buttons change it:
// a digit after the decimal point added or removed (the point with the last one), or ".0" after
// the last digit when there's no point. Text, quoted or escaped, and [..] parts are skipped.
std::wstring SectionDecimals(const std::wstring& s, int delta) {
  size_t point = std::wstring::npos, lastDigit = std::wstring::npos, lastDecimal = std::wstring::npos;
  bool quoted = false;
  int bracket = 0;
  for (size_t i = 0; i < s.size(); ++i) {
    const wchar_t ch = s[i];
    if (ch == L'"') quoted = !quoted;
    if (quoted || ch == L'"') continue;
    if (ch == L'\\' || ch == L'_' || ch == L'*') {
      ++i;
      continue;
    }
    if (ch == L'[') ++bracket;
    else if (ch == L']') --bracket;
    if (bracket) continue;
    if (ch == L'E' || ch == L'e') break;  // the exponent's digits aren't decimals
    if (ch == L'.' && point == std::wstring::npos) point = i;
    else if (ch == L'0' || ch == L'#' || ch == L'?') {
      lastDigit = i;
      if (point != std::wstring::npos) lastDecimal = i;
    }
  }
  std::wstring out = s;
  if (delta > 0) {
    if (point != std::wstring::npos) out.insert(lastDecimal != std::wstring::npos ? lastDecimal + 1 : point + 1, L"0");
    else if (lastDigit != std::wstring::npos) out.insert(lastDigit + 1, L".0");
  } else if (lastDecimal != std::wstring::npos) {
    out.erase(lastDecimal, 1);
    bool more = false;
    for (size_t i = point + 1; i < lastDecimal; ++i) more = more || out[i] == L'0' || out[i] == L'#' || out[i] == L'?';
    if (!more) out.erase(point, 1);
  }
  return out;
}

}  // namespace

std::wstring FormatDecimals(std::wstring_view codeView, int delta) {
  const std::wstring code(codeView);
  std::wstring out;
  size_t start = 0;
  bool quoted = false;
  int bracket = 0;
  for (size_t i = 0; i <= code.size(); ++i) {
    const wchar_t ch = i < code.size() ? code[i] : L';';
    if (i < code.size() && ch == L'"') quoted = !quoted;
    else if (!quoted && ch == L'\\') ++i;
    else if (!quoted && ch == L'[') ++bracket;
    else if (!quoted && ch == L']') --bracket;
    else if (!quoted && !bracket && ch == L';') {
      out += SectionDecimals(code.substr(start, i - start), delta);
      if (i < code.size()) out += L';';
      start = i + 1;
    }
  }
  return out;
}


}  // namespace fw::sheet
