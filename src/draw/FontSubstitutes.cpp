#include "draw/FontSubstitutes.h"

#include <windows.h>

#include <deque>
#include <vector>

namespace draw {
namespace {

struct Row {
  const wchar_t* family;
  FontSubstitute substitutes[3];
};

// Windows' own families first; Office's (Arial Narrow, Garamond), where a PC has them, only where
// Windows has nothing close. Scales and heights: measured on the corpus (docs/f1-plan.md, f1-word).
constexpr Row kRows[] = {
    // Office's default since 2024 and its relatives.
    {L"Aptos", {{L"Segoe UI"}, {L"Calibri"}, {L"Arial"}}},
    {L"Aptos Display", {{L"Segoe UI", 0.935f, 1.22f}, {L"Calibri"}, {L"Arial"}}},
    {L"Aptos Narrow", {{L"Arial Narrow"}, {L"Segoe UI", 0.85f}, {L"Arial", 0.82f}}},
    {L"Aptos Serif", {{L"Cambria"}, {L"Georgia"}, {L"Times New Roman"}}},
    {L"Aptos Mono", {{L"Consolas"}, {L"Courier New"}}},
    // The other families Office downloads for documents.
    {L"Arial Nova", {{L"Arial"}}},
    {L"Browallia New", {{L"Leelawadee UI"}, {L"Tahoma"}}},
    {L"DM Sans", {{L"Segoe UI"}, {L"Arial"}}},
    {L"EB Garamond", {{L"Garamond"}, {L"Palatino Linotype", 0.9f}, {L"Georgia", 0.88f}}},
    {L"HGPGothicE", {{L"MS PGothic"}, {L"Yu Gothic"}}},
    {L"IBM Plex Mono", {{L"Consolas"}, {L"Courier New"}}},
    {L"Libre Baskerville", {{L"Georgia"}, {L"Cambria"}}},
    {L"Mangal", {{L"Nirmala UI"}}},
    {L"MingLiU", {{L"NSimSun"}, {L"Microsoft JhengHei"}}},
    {L"Montserrat", {{L"Verdana"}, {L"Segoe UI"}}},
    {L"MS Mincho", {{L"MS Gothic"}, {L"Yu Gothic"}}},
    {L"MS PMincho", {{L"MS PGothic"}, {L"Yu Gothic"}}},
    {L"Open Sans", {{L"Segoe UI", 1.06f, 1.36f}, {L"Arial"}}},
    {L"Oswald", {{L"Bahnschrift Condensed"}, {L"Arial Narrow"}, {L"Segoe UI", 0.8f}}},
    {L"PMingLiU", {{L"SimSun"}, {L"Microsoft JhengHei"}}},
    {L"Poppins", {{L"Segoe UI"}, {L"Verdana"}}},
    {L"Raleway", {{L"Segoe UI"}, {L"Arial"}}},
    {L"Roboto", {{L"Arial"}, {L"Segoe UI"}}},
    {L"Source Code Pro", {{L"Consolas"}, {L"Courier New"}}},
    {L"Source Sans Pro", {{L"Segoe UI"}, {L"Arial"}}},
    {L"Univers", {{L"Arial"}, {L"Segoe UI"}}},
    {L"Yu Mincho", {{L"Yu Gothic"}, {L"MS Gothic"}}},
    // macOS's and older names.
    {L"Arial Narrow", {{L"Arial", 0.82f}}},
    {L"Book Antiqua", {{L"Palatino Linotype"}}},
    {L"Courier", {{L"Courier New"}}},
    {L"Helvetica", {{L"Arial"}}},
    {L"Helvetica Neue", {{L"Arial"}}},
    {L"Myriad Pro", {{L"Segoe UI"}}},  // PowerPoint's choice (cs-6476's PDF)
    {L"Times", {{L"Times New Roman"}}},
    {L"Times Roman", {{L"Times New Roman"}}},
};

bool Same(std::wstring_view a, std::wstring_view b) {
  return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

size_t Count(const Row& row) {
  size_t n = 0;
  while (n < 3 && row.substitutes[n].family) ++n;
  return n;
}

// FW_SUBSTITUTES rows (fwtool only), searched before the table.
struct Tuned {
  std::wstring family;
  std::vector<FontSubstitute> substitutes;
};
std::vector<Tuned>& TunedRows() {
  static std::vector<Tuned> rows;
  return rows;
}
std::deque<std::wstring>& TunedNames() {
  static std::deque<std::wstring> names;  // stable addresses for FontSubstitute::family
  return names;
}

std::wstring_view Trim(std::wstring_view s) {
  while (!s.empty() && s.front() == L' ') s.remove_prefix(1);
  while (!s.empty() && s.back() == L' ') s.remove_suffix(1);
  return s;
}

}  // namespace

std::span<const FontSubstitute> BuiltInSubstitutes(std::wstring_view family) {
  for (const Tuned& t : TunedRows())
    if (Same(t.family, family)) return t.substitutes;
  for (const Row& row : kRows)
    if (Same(row.family, family)) return {row.substitutes, Count(row)};
  return {};
}

void SetSubstituteTuning(std::wstring_view spec) {
  while (!spec.empty()) {
    const size_t semi = spec.find(L';');
    std::wstring_view row = spec.substr(0, semi);
    spec = semi == std::wstring_view::npos ? std::wstring_view() : spec.substr(semi + 1);
    const size_t eq = row.find(L'=');
    if (eq == std::wstring_view::npos) continue;
    Tuned t{std::wstring(Trim(row.substr(0, eq))), {}};
    std::wstring_view list = row.substr(eq + 1);
    while (!list.empty()) {
      const size_t comma = list.find(L',');
      std::wstring_view one = list.substr(0, comma);
      list = comma == std::wstring_view::npos ? std::wstring_view() : list.substr(comma + 1);
      std::wstring_view name = one.substr(0, one.find(L':'));
      FontSubstitute s{TunedNames().emplace_back(Trim(name)).c_str()};
      if (const size_t c1 = one.find(L':'); c1 != std::wstring_view::npos) {
        s.scale = static_cast<float>(_wtof(std::wstring(one.substr(c1 + 1)).c_str()));
        if (const size_t c2 = one.find(L':', c1 + 1); c2 != std::wstring_view::npos)
          s.height = static_cast<float>(_wtof(std::wstring(one.substr(c2 + 1)).c_str()));
      }
      if (!(s.scale > 0)) s.scale = 1;
      t.substitutes.push_back(s);
    }
    if (!t.family.empty() && !t.substitutes.empty()) TunedRows().push_back(std::move(t));
  }
}

}  // namespace draw
