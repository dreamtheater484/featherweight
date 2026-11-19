// The workbook tab's cell editing (s2-plan "Editing in the grid"): typing over a cell, F2, the
// box over the cell and the formula bar, Enter and Tab as Excel moves after an entry, point mode.
// The text and keys are CellEditor's; committing goes through the edit API (s2-model).
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <iterator>
#include <unordered_set>
#include <utility>

#include "Commands.h"
#include "Util.h"
#include "core/Strings.h"
#include "sheet/calc/Typing.h"
#include "sheet/numfmt/Parse.h"
#include "sheet/view/WorkbookView.h"
#include "ui/Theme.h"

using namespace gfx;

namespace fw::sheet::view {

namespace {

std::vector<std::wstring_view> Lines(const std::wstring& s) {
  std::vector<std::wstring_view> out;
  size_t start = 0;
  for (;;) {
    const size_t nl = s.find(L'\n', start);
    out.push_back(std::wstring_view(s).substr(start, nl == std::wstring::npos ? std::wstring::npos : nl - start));
    if (nl == std::wstring::npos) return out;
    start = nl + 1;
  }
}

// The line holding `caret`: [first, end).
std::pair<int, int> LineAround(const std::wstring& s, int caret) {
  const int n = static_cast<int>(s.size());
  int a = std::clamp(caret, 0, n), b = a;
  while (a > 0 && s[static_cast<size_t>(a) - 1] != L'\n') --a;
  while (b < n && s[static_cast<size_t>(b)] != L'\n') ++b;
  return {a, b};
}

// The character boundary in `line` nearest to `x` (from the line's start).
int HitIn(Canvas& m, std::wstring_view line, float x, float size) {
  const int n = static_cast<int>(line.size());
  if (x <= 0 || n == 0) return 0;
  if (m.MeasureText(line, size) <= x) return n;
  int lo = 1, hi = n;  // the first prefix at least x wide
  while (lo < hi) {
    const int mid = (lo + hi) / 2;
    if (m.MeasureText(line.substr(0, static_cast<size_t>(mid)), size) >= x) hi = mid;
    else lo = mid + 1;
  }
  const float right = m.MeasureText(line.substr(0, static_cast<size_t>(lo)), size);
  const float left = m.MeasureText(line.substr(0, static_cast<size_t>(lo) - 1), size);
  return x - left < right - x ? lo - 1 : lo;
}

// The reference colors, in the order references appear (Excel's range finder).
constexpr uint32_t kRefColors[] = {0x1A6FD6, 0xD1342C, 0x8A3FC2, 0x2E8A3E, 0xC7621A, 0x168C9C, 0xB8336A};

std::wstring ColLetters(uint32_t col) {
  std::wstring s;
  for (uint32_t v = col + 1; v > 0; v = (v - 1) / 26) s.insert(s.begin(), static_cast<wchar_t>(L'A' + (v - 1) % 26));
  return s;
}

// A reference for the range, with the $ signs `old` had (A1, $A$1:B2, $A:B, 1:$2).
std::wstring RefText(std::wstring_view old, int kind, uint32_t r0, uint32_t c0, uint32_t r1, uint32_t c1) {
  const size_t colon = old.find(L':');
  const std::wstring_view p1 = old.substr(0, colon);
  const std::wstring_view p2 = colon == std::wstring_view::npos ? p1 : old.substr(colon + 1);
  auto absFirst = [](std::wstring_view p) { return !p.empty() && p[0] == L'$'; };
  auto absRow = [](std::wstring_view p) { return p.find(L'$', 1) != std::wstring_view::npos; };
  auto col = [](uint32_t c, bool abs) { return (abs ? L"$" : L"") + ColLetters(c); };
  auto row = [](uint32_t r, bool abs) { return (abs ? L"$" : L"") + std::to_wstring(r + 1); };
  if (kind == 1) return col(c0, absFirst(p1)) + L":" + col(c1, absFirst(p2));
  if (kind == 2) return row(r0, absFirst(p1)) + L":" + row(r1, absFirst(p2));
  std::wstring s = col(c0, absFirst(p1)) + row(r0, absRow(p1));
  if (r0 != r1 || c0 != c1) s += L":" + col(c1, absFirst(p2)) + row(r1, absRow(p2));
  return s;
}

std::wstring Lower(std::wstring s) {
  for (wchar_t& c : s) c = static_cast<wchar_t>(std::towlower(c));
  return s;
}

}  // namespace

UINT WorkbookView::SemicolonKey() {
  const SHORT k = VkKeyScanW(L';');
  return k == -1 ? VK_OEM_1 : static_cast<UINT>(k & 0xFF);
}

void WorkbookView::AutoComplete() {
  autoText_.clear();
  if (renaming_ >= 0) return;
  const std::wstring& t = editor_.Text();
  if (t.empty() || editor_.IsFormula() || editor_.HasSelection() || editor_.Caret() != static_cast<int>(t.size())) return;
  // Numbers, dates and times don't complete: only text with a letter.
  if (std::none_of(t.begin(), t.end(), [](wchar_t c) { return std::iswalpha(c) != 0; })) return;
  if (!autoLoaded_) {
    autoLoaded_ = true;
    // The run of cells above and below in the column, to a blank: its text entries (not formulas).
    const Sheet& sheet = Current();
    const uint32_t row = editCell_.row, col = editCell_.col;
    std::vector<std::wstring> run;
    auto blank = [](const CellInfo& c) { return c.value.type == ValueType::Empty && !c.hasFormula; };
    auto take = [&](const CellInfo& c) {
      if (!c.hasFormula && c.value.type == ValueType::String) run.push_back(Utf8ToWide(c.value.text));
    };
    int64_t prev = -1;
    if (row > 0)
      sheet.ForEachCell({0, col, row - 1, col}, [&](const CellInfo& c) {
        if (blank(c) || (prev >= 0 && c.row != prev + 1)) run.clear();
        prev = blank(c) ? -1 : static_cast<int64_t>(c.row);
        if (!blank(c)) take(c);
        return true;
      });
    if (prev != static_cast<int64_t>(row) - 1) run.clear();  // the run must reach the cell
    if (row + 1 < kMaxRows) {
      uint32_t expect = row + 1;
      sheet.ForEachCell({row + 1, col, kMaxRows - 1, col}, [&](const CellInfo& c) {
        if (c.row != expect || blank(c)) return false;
        ++expect;
        take(c);
        return true;
      });
    }
    std::unordered_set<std::wstring> seen;
    for (std::wstring& w : run) {
      std::wstring low = Lower(w);
      if (seen.insert(low).second) autoList_.emplace_back(std::move(low), std::move(w));
    }
  }
  // The one entry that starts with what's typed (ignoring case); two or more propose nothing.
  const std::wstring low = Lower(t);
  const std::wstring* match = nullptr;
  for (const auto& [l, w] : autoList_) {
    if (l.size() < low.size() || l.compare(0, low.size(), low) != 0) continue;
    if (match) return;
    match = &w;
  }
  if (!match || match->size() <= t.size()) return;
  const int typed = static_cast<int>(t.size());
  autoText_ = *match;
  editor_.Insert(match->substr(t.size()));
  editor_.SetCaret(typed, false);
  editor_.SetCaret(static_cast<int>(editor_.Text().size()), true);
}

void WorkbookView::InsertNow(bool time) {
  SYSTEMTIME st;
  GetLocalTime(&st);
  wchar_t b[128] = {};
  const int n = time ? GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &st, nullptr, b, 128)
                     : GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &st, nullptr, b, 128, nullptr);
  if (n <= 1) return;
  if (!editor_.Active()) BeginEdit(CellEditor::Mode::Enter, L"");
  if (!editor_.Active()) return;
  editor_.Insert(b);
  autoText_.clear();
  host_.ViewInvalidate();
}

bool WorkbookView::CycleReference() {
  if (!editor_.IsFormula()) return false;
  std::wstring text = editor_.Text();
  size_t from = static_cast<size_t>(std::min(editor_.Caret(), editor_.Anchor()));
  size_t to = static_cast<size_t>(std::max(editor_.Caret(), editor_.Anchor()));
  if (!calc::CycleReference(text, from, to, Region())) return false;
  editor_.SetText(text, static_cast<int>(from), static_cast<int>(to));
  return true;
}

bool WorkbookView::RefHit(int x, int y, RefGrab& g) {
  if (!editor_.Active() || !editor_.IsFormula() || !gfx::Contains(cells_, static_cast<float>(x), static_cast<float>(y)))
    return false;
  const std::vector<Pane> panes = Panes();
  const Pane* pane = nullptr;
  for (const Pane& p : panes)
    if (x >= p.rc.left && x < p.rc.right && y >= p.rc.top && y < p.rc.bottom) pane = &p;
  if (!pane) return false;
  const float fx = static_cast<float>(x), fy = static_cast<float>(y);
  const float h = std::round(5 * ui_) / 2 + 1, slop = std::max(3.f, std::round(3 * ui_));
  const std::vector<RefBox>& refs = Refs();
  for (size_t i = refs.size(); i-- > 0;) {  // the last drawn is on top
    const RefBox& r = refs[i];
    if ((r.sheet == -1 ? static_cast<int>(editSheet_) : r.sheet) != static_cast<int>(sheet_)) continue;
    const D2D1_RECT_F f = CellRect(*pane, r.r0, r.c0, r.r1, r.c1);
    const float right = f.right - 1, bottom = f.bottom - 1;
    for (const bool left : {true, false})
      for (const bool top : {true, false}) {
        if (std::fabs(fx - (left ? f.left : right)) > h || std::fabs(fy - (top ? f.top : bottom)) > h) continue;
        g.index = i;
        g.resize = true;
        g.nwse = left == top;
        // The opposite corner stays (on a sheet read right to left the first column is on the right).
        g.anchor = {top ? r.r1 : r.r0, (left != rtl_) ? r.c1 : r.c0};
        return true;
      }
    const bool onV = (std::fabs(fx - f.left) <= slop || std::fabs(fx - right) <= slop) && fy >= f.top - slop && fy <= bottom + slop;
    const bool onH = (std::fabs(fy - f.top) <= slop || std::fabs(fy - bottom) <= slop) && fx >= f.left - slop && fx <= right + slop;
    if (!onV && !onH) continue;
    auto cell = CellAt(x, y);
    if (!cell) continue;
    g.index = i;
    g.resize = false;
    g.dRow = std::clamp(cell->row, r.r0, r.r1) - r.r0;
    g.dCol = std::clamp(cell->col, r.c0, r.c1) - r.c0;
    return true;
  }
  return false;
}

void WorkbookView::RefDrag(int x, int y) {
  const std::vector<RefBox>& refs = Refs();
  if (refGrab_.index >= refs.size()) return;
  const RefBox r = refs[refGrab_.index];
  const int px = static_cast<int>(std::clamp(static_cast<float>(x), cells_.left, cells_.right - 1));
  const int py = static_cast<int>(std::clamp(static_cast<float>(y), cells_.top, cells_.bottom - 1));
  auto cell = CellAt(px, py);
  if (!cell) return;
  uint32_t r0, c0, r1, c1;
  if (refGrab_.resize) {
    r0 = std::min(refGrab_.anchor.row, cell->row);
    r1 = std::max(refGrab_.anchor.row, cell->row);
    c0 = std::min(refGrab_.anchor.col, cell->col);
    c1 = std::max(refGrab_.anchor.col, cell->col);
  } else {
    const uint32_t h = r.r1 - r.r0, w = r.c1 - r.c0;
    r0 = std::min(cell->row - std::min(cell->row, refGrab_.dRow), kMaxRows - 1 - h);
    c0 = std::min(cell->col - std::min(cell->col, refGrab_.dCol), kMaxCols - 1 - w);
    r1 = r0 + h;
    c1 = c0 + w;
  }
  // Whole columns stay whole columns, whole rows whole rows.
  if (r.kind == 1) r0 = 0, r1 = kMaxRows - 1;
  if (r.kind == 2) c0 = 0, c1 = kMaxCols - 1;
  if (r0 == r.r0 && c0 == r.c0 && r1 == r.r1 && c1 == r.c1) return;
  const std::wstring& text = editor_.Text();
  const int end = r.at + r.length;
  const std::wstring old = text.substr(static_cast<size_t>(r.refAt), static_cast<size_t>(end - r.refAt));
  editor_.ReplaceSpan(r.refAt, end, RefText(old, r.kind, r0, c0, r1, c1));
  ScrollIntoView(*cell);
  host_.ViewInvalidate();
}

const std::vector<WorkbookView::RefBox>& WorkbookView::Refs() {
  if (!editor_.Active() || !editor_.IsFormula() || renaming_ >= 0) {
    refs_.clear();
    refsText_.clear();
    return refs_;
  }
  if (editor_.Text() == refsText_) return refs_;
  refsText_ = editor_.Text();
  refs_.clear();
  for (const calc::RefSpan& f : calc::ReferenceSpans(refsText_, Region())) {
    RefBox r;
    r.at = static_cast<int>(f.begin);
    r.length = static_cast<int>(f.end - f.begin);
    r.sheet = f.sheet.empty() ? -1 : book_->FindSheet(WideToUtf8(f.sheet));
    if (r.sheet < 0 && !f.sheet.empty()) r.sheet = -2;
    r.r0 = static_cast<uint32_t>(f.r1);
    r.c0 = static_cast<uint32_t>(f.c1);
    r.r1 = static_cast<uint32_t>(f.r2);
    r.c1 = static_cast<uint32_t>(f.c2);
    r.color = f.color % static_cast<int>(std::size(kRefColors));
    // The reference after its sheet, and whether it is whole columns (no digits) or rows (no letters).
    const std::wstring_view text = std::wstring_view(refsText_).substr(f.begin, f.end - f.begin);
    const size_t bang = text.rfind(L'!');
    r.refAt = r.at + (bang == std::wstring_view::npos ? 0 : static_cast<int>(bang) + 1);
    const std::wstring_view ref = text.substr(bang == std::wstring_view::npos ? 0 : bang + 1);
    const bool digits = ref.find_first_of(L"0123456789") != std::wstring_view::npos;
    const bool letters = std::any_of(ref.begin(), ref.end(), [](wchar_t c) { return std::iswalpha(c) != 0; });
    r.kind = !digits ? 1 : !letters ? 2 : 0;
    refs_.push_back(r);
  }
  return refs_;
}

void WorkbookView::DrawRefBorders(Canvas& c, const Pane& pane) {
  if (!editor_.Active() || !editor_.IsFormula()) return;
  for (const RefBox& r : Refs()) {
    const int sheet = r.sheet == -1 ? static_cast<int>(editSheet_) : r.sheet;
    if (sheet != static_cast<int>(sheet_)) continue;
    const uint32_t r0 = std::max(r.r0, pane.row0), r1 = std::min(r.r1, std::min(pane.row1, kMaxRows) - 1);
    const uint32_t c0 = std::max(r.c0, pane.col0), c1 = std::min(r.c1, std::min(pane.col1, kMaxCols) - 1);
    if (r0 > r1 || c0 > c1) continue;
    const D2D1_COLOR_F ink = Rgb(kRefColors[r.color]);
    // On the range's gridlines, as the selection's border is (CellRect is the whole range, so the
    // pane's clip keeps the part shown).
    const D2D1_RECT_F f = CellRect(pane, r.r0, r.c0, r.r1, r.c1);
    const float w = std::max(1.f, std::round(1.5f * ui_)), l = f.left - w / 2, t = f.top - w / 2;
    const float rr = f.right - 1 - w / 2, b = f.bottom - 1 - w / 2;
    c.Fill({l, t, rr + w, t + w}, ink);
    c.Fill({l, b, rr + w, b + w}, ink);
    c.Fill({l, t, l + w, b + w}, ink);
    c.Fill({rr, t, rr + w, b + w}, ink);
    // The corners' handles (dragged to move or size the reference).
    const float h = std::round(5 * ui_);
    for (float x : {f.left, f.right - 1})
      for (float y : {f.top, f.bottom - 1}) c.Fill({x - h / 2, y - h / 2, x + h / 2, y + h / 2}, ink);
  }
}

bool WorkbookView::PointClick(int x, int y) {
  if (!editor_.Active() || editSheet_ != sheet_ || !editor_.CanPoint(true)) return false;
  Bar v, h;
  Bars(v, h);
  const float fx = static_cast<float>(x), fy = static_cast<float>(y);
  if ((v.shown && gfx::Contains(v.track, fx, fy)) || (h.shown && gfx::Contains(h.track, fx, fy))) return false;
  auto cell = CellAt(x, y);
  if (!cell) return false;
  // Pointing by mouse is point mode: the arrows then move the reference.
  editor_.SetMode(CellEditor::Mode::Enter);
  point_ = pointEnd_ = *cell;
  editor_.SetPointRef(Utf8ToWide(CellName(cell->row, cell->col)));
  mouse_ = Mouse::Point;
  host_.ViewInvalidate();
  return true;
}

void WorkbookView::PointDrag(int x, int y) {
  if (!editor_.Active() || !editor_.Pointing()) return;
  const int px = static_cast<int>(std::clamp(static_cast<float>(x), cells_.left, cells_.right - 1));
  const int py = static_cast<int>(std::clamp(static_cast<float>(y), cells_.top, cells_.bottom - 1));
  auto cell = CellAt(px, py);
  if (!cell || *cell == pointEnd_) return;
  pointEnd_ = *cell;
  const uint32_t r0 = std::min(point_.row, pointEnd_.row), r1 = std::max(point_.row, pointEnd_.row);
  const uint32_t c0 = std::min(point_.col, pointEnd_.col), c1 = std::max(point_.col, pointEnd_.col);
  std::wstring ref = Utf8ToWide(CellName(r0, c0));
  if (r0 != r1 || c0 != c1) ref += L":" + Utf8ToWide(CellName(r1, c1));
  editor_.SetPointRef(ref);
  host_.ViewInvalidate();
}

void WorkbookView::DrawEditLine(Canvas& c, std::wstring_view line, int lineAt, float left, float top, float bottom,
                                float size, D2D1_COLOR_F ink) {
  const int len = static_cast<int>(line.size());
  auto xAt = [&](int i) { return left + c.MeasureText(line.substr(0, static_cast<size_t>(i)), size); };
  auto run = [&](int from, int to, D2D1_COLOR_F color) {
    if (to <= from) return;
    const float x = xAt(from);
    const std::wstring_view part = line.substr(static_cast<size_t>(from), static_cast<size_t>(to - from));
    c.Text(part, {x, top, x + c.MeasureText(part, size) + size, bottom}, color, size);
  };
  int at = 0;
  for (const RefBox& r : Refs()) {
    const int from = std::clamp(r.at - lineAt, 0, len), to = std::clamp(r.at + r.length - lineAt, 0, len);
    if (to <= from || from < at) continue;
    run(at, from, ink);
    run(from, to, Rgb(kRefColors[r.color]));
    at = to;
  }
  run(at, len, ink);
}

edit::Editor& WorkbookView::Ed() {
  if (!edit_) {
    edit_ = std::make_unique<edit::Editor>(*book_);
    edit_->OnChange([this](const std::vector<edit::Change>& changes) { Edited(changes); });
    // Crash recovery appends the edits to a journal (none for a CSV book: its autosave is whole).
    edit_->SetJournal(file_->JournalFor(*book_));
    // A number typed into a table's header becomes its text as the region shows it. UserLocale()
    // is one object, refreshed in place, so this keeps following it.
    edit_->SetLocale(&UserLocale());
  }
  return *edit_;
}

const calc::FormulaLocale& WorkbookView::Region() const {
  if (!region_) region_ = calc::MakeFormulaLocale(text_->Locale());
  return *region_;
}

bool WorkbookView::EditRefused(const edit::Result& r) {
  if (r) return false;
  host_.ViewMessage(BannerKind::Warning, std::wstring(r.message.begin(), r.message.end()));
  return true;
}

void WorkbookView::BeginEdit(CellEditor::Mode mode, const std::wstring& text) {
  if (!Ready() || ChartSheet() || preview_) return;
  std::u16string why;
  if (!Ed().CanEdit(&why)) {
    host_.ViewMessage(BannerKind::Info, std::wstring(why.begin(), why.end()));
    return;
  }
  if (LockedForTyping()) return;  // a protected sheet's locked cell: Excel's message at once
  editCell_ = active_;
  editSheet_ = sheet_;
  editInBar_ = false;
  barScroll_ = 0;
  autoLoaded_ = false;
  autoList_.clear();
  autoText_.clear();
  editReplaces_ = mode == CellEditor::Mode::Enter && text.empty();
  fnClosed_ = false;
  fnList_.clear();
  editor_.SetPointRule([this](std::wstring_view t, size_t caret) { return calc::CanPointAt(t, caret, Region()); });
  editor_.Begin(text, mode);
  ScrollIntoView(active_);
  host_.ViewStateChanged();
  host_.ViewInvalidate();
}

void WorkbookView::BeginRename(size_t sheet) {
  if (!Ready() || preview_ || sheet >= book_->SheetCount()) return;
  if (editor_.Active() && !CommitEdit(false)) return;
  std::u16string why;
  if (!Ed().CanEdit(&why)) {
    host_.ViewMessage(BannerKind::Info, std::wstring(why.begin(), why.end()));
    return;
  }
  renaming_ = static_cast<int>(sheet);
  editInBar_ = false;
  editReplaces_ = false;
  fnList_.clear();
  autoText_.clear();
  editor_.SetPointRule(nullptr);
  editor_.Begin(Utf8ToWide(book_->GetSheet(sheet).Name()), CellEditor::Mode::Edit);
  editor_.SelectAll();  // as Excel selects the whole name
  host_.ViewStateChanged();
  host_.ViewInvalidate();
}

bool WorkbookView::OnChar(wchar_t ch, UINT mods) {
  if (!Ready() || preview_ || ChartSheet()) return false;
  if (editor_.Active()) {
    if (ch == L'%' && autoPercent_ && !editor_.HasSelection() && static_cast<size_t>(editor_.Caret()) + 1 == editor_.Text().size() &&
        editor_.Text().back() == L'%') {
      editor_.SetText(editor_.Text(), editor_.Caret() + 1, editor_.Caret() + 1);
      autoPercent_ = false;
      host_.ViewInvalidate();
      return true;
    }
    if (editor_.OnChar(ch)) {
      fnClosed_ = false;
      AutoComplete();
      UpdateFormulaHelp();
      host_.ViewInvalidate();
    }
    return true;
  }
  if (ch < 0x20 || ch == 0x7F) return false;
  if ((mods & kModCtrl) && !(mods & kModAlt)) return false;
  // Typing over a cell replaces it (Enter mode).
  BeginEdit(CellEditor::Mode::Enter, L"");
  editor_.OnChar(ch);
  // A number typed into a percent cell is a percentage: the "%" comes with the first digit, '-'
  // or '+', the caret before it (Excel's automatic percent entry; s2-format measured it).
  autoPercent_ = false;
  if (editor_.Active() && ((ch >= L'0' && ch <= L'9') || ch == L'-' || ch == L'+') && editor_.Text() == std::wstring(1, ch)) {
    const Xf& xf = book_->GetStyles().CellXf(Current().StyleAt(active_.row, active_.col));
    if (xf.numFmt != 0 && text_->FormatOf(xf.numFmt).HasPercent()) {
      editor_.SetText(editor_.Text() + L"%", 1, 1);
      autoPercent_ = true;
      host_.ViewInvalidate();
    }
  }
  AutoComplete();
  UpdateFormulaHelp();
  return true;
}

bool WorkbookView::EditKey(UINT vk, UINT mods) {
  const bool ctrl = (mods & kModCtrl) != 0, shift = (mods & kModShift) != 0, alt = (mods & kModAlt) != 0;
  if (ctrl && !alt && vk == SemicolonKey()) {
    InsertNow(shift);
    return true;
  }
  if (vk == VK_F4 && !ctrl && !alt && !shift) {
    if (CycleReference()) host_.ViewInvalidate();
    UpdateFormulaHelp();
    return true;
  }
  if (FormulaHelpKey(vk, ctrl, shift, alt)) return true;
  // The clipboard while typing: the text, not the cells.
  if (ctrl && !alt && !shift && (vk == 'C' || vk == 'X' || vk == 'V')) {
    if (vk == 'V') {
      std::wstring t = util::GetClipboardText();
      while (!t.empty() && (t.back() == L'\n' || t.back() == L'\r')) t.pop_back();
      std::wstring one;
      for (size_t i = 0; i < t.size(); ++i)
        if (!(t[i] == L'\r' && i + 1 < t.size() && t[i + 1] == L'\n')) one += t[i] == L'\r' ? L'\n' : t[i];
      editor_.Insert(one);
    } else if (editor_.HasSelection()) {
      const int a = std::min(editor_.Caret(), editor_.Anchor()), b = std::max(editor_.Caret(), editor_.Anchor());
      util::SetClipboardText(editor_.Text().substr(static_cast<size_t>(a), static_cast<size_t>(b - a)));
      if (vk == 'X') editor_.Insert(L"");
    }
    UpdateFormulaHelp();
    host_.ViewInvalidate();
    return true;
  }
  const CellEditor::Result r = editor_.OnKey(vk, ctrl, shift, alt);
  switch (r.action) {
    case CellEditor::Action::None: return true;  // nothing else happens while a cell is edited
    case CellEditor::Action::Handled: break;
    case CellEditor::Action::Cancel:
      renaming_ = -1;
      editor_.End();
      host_.ViewStateChanged();
      break;
    case CellEditor::Action::Commit: {
      const bool renamed = renaming_ >= 0;
      if (!CommitEdit(false) || renamed) break;
      if (vk == VK_TAB) {
        if (tabStartCol_ == UINT32_MAX) tabStartCol_ = active_.col;
      } else if (vk != VK_RETURN) {
        tabStartCol_ = UINT32_MAX;
      }
      MoveAfterEntry(r.dRow, r.dCol);
      break;
    }
    case CellEditor::Action::CommitFill: CommitEdit(true); break;
    case CellEditor::Action::Point: PointMove(r.dRow, r.dCol, r.extend); break;
  }
  UpdateFormulaHelp();
  host_.ViewInvalidate();
  return true;
}

// What typing `text` into a cell writes (as Excel converts it): a value in the region's form
// (and the format typing gives a General cell), or a formula. False for a formula Excel refuses:
// `message` says why and `pos` is where in `text`.
bool WorkbookView::InputFor(const std::wstring& text, int sheet, CellAddr cell, edit::CellInput& in, std::string& formula,
                            std::string& message, size_t& pos) {
  in = edit::CellInput{};  // empty text clears the cell
  formula.clear();
  if (text.empty()) return true;
  const Sheet& s = book_->GetSheet(static_cast<size_t>(sheet));
  const Xf& xf = book_->GetStyles().CellXf(s.StyleAt(cell.row, cell.col));
  calc::CompileOptions at;
  at.sheet = sheet;
  at.row = static_cast<int32_t>(cell.row);
  at.col = static_cast<int32_t>(cell.col);
  bool asFormula = text[0] == L'=';
  const bool textFormat = xf.numFmt == 49;  // "@": what's typed stays text
  TypedValue tv;
  if (!asFormula && !textFormat) {
    SYSTEMTIME now;
    GetLocalTime(&now);
    ParseTyped(text, text_->Locale(), book_->Date1904(), now.wYear, tv);
    // "+A1" and "-A1" are formulas, "-5" a number.
    if ((text[0] == L'+' || text[0] == L'-') && text.size() > 1 && tv.kind != TypedValue::Number) asFormula = true;
  }
  if (asFormula) {
    const std::string typed = WideToUtf8(text[0] == L'=' ? text : L"=" + text);
    const calc::EnteredFormula e = calc::EnterFormula(typed, Region(), at, Calc().engine());
    if (!e.formula) {
      message = e.message;
      int caret = static_cast<int>(Utf8ToWide(std::string_view(typed).substr(0, std::min(e.pos, typed.size()))).size());
      if (text[0] != L'=') --caret;
      pos = static_cast<size_t>(std::max(0, caret));
      return false;
    }
    formula = e.text;
    in = edit::CellInput::Formula(e.text.substr(1));
    // One that gives an array spills from the cell, as in Excel 365 (a dynamic array).
    Range spill;
    if (Calc().SpillSize(in.formula, sheet, cell.row, cell.col, spill)) {
      in.dynamic = true;
      in.arrayRange = spill;
    }
    return true;
  }
  if (textFormat) {
    in = edit::CellInput::Text(WideToUtf8(text));
    return true;
  }
  ErrorCode code{};
  switch (tv.kind) {
    case TypedValue::Number: in = edit::CellInput::Number(tv.number); break;
    case TypedValue::Bool: in = edit::CellInput::Bool(tv.number != 0); break;
    case TypedValue::Error:
      in = sheet::ParseError(WideToUtf8(tv.text), code) ? edit::CellInput::Error(code) : edit::CellInput::Text(WideToUtf8(tv.text));
      break;
    case TypedValue::Text: in = edit::CellInput::Text(WideToUtf8(tv.text)); break;
  }
  // The format typing gives (a date, a percentage...) where Excel changes the cell's (s2-format's
  // TypedFormat: General always, a built-in currency, percent, date... only for another kind),
  // and an apostrophe's quotePrefix.
  Xf want = xf;
  bool restyle = false;
  if (const uint32_t f = TypedFormat(xf.numFmt, tv.format); f != xf.numFmt) {
    want.numFmt = f;
    want.applyNumFmt = true;
    restyle = true;
  }
  if (tv.kind == TypedValue::Text && tv.quoted != xf.quotePrefix) {
    want.quotePrefix = tv.quoted;
    restyle = true;
  }
  if (restyle) in.WithStyle(Ed().AddXf(want));
  return true;
}

bool WorkbookView::CommitEdit(bool fill) {
  if (!editor_.Active()) return true;
  if (renaming_ >= 0) {
    // A sheet's new name (Excel's rules, and formulas that use it follow).
    if (editor_.Changed() && EditRefused(Ed().RenameSheet(renaming_, WideToUtf8(editor_.Text())))) return false;
    renaming_ = -1;
    editor_.End();
    host_.ViewStateChanged();
    host_.ViewInvalidate();
    return true;
  }
  std::wstring text = editor_.Text();
  // AutoComplete's entry keeps its own case.
  if (!autoText_.empty() && Lower(text) == Lower(autoText_)) text = autoText_;
  auto finish = [&] {
    editor_.End();
    editInBar_ = false;
    host_.ViewStateChanged();
    host_.ViewInvalidate();
  };
  if ((!editor_.Changed() && !editReplaces_ && !fill) || editSheet_ >= book_->SheetCount()) {
    finish();
    return true;
  }
  const int sheet = static_cast<int>(editSheet_);
  const Sheet& s = book_->GetSheet(editSheet_);
  calc::CompileOptions at;
  at.sheet = sheet;
  at.row = static_cast<int32_t>(editCell_.row);
  at.col = static_cast<int32_t>(editCell_.col);
  edit::CellInput in;
  std::string formula;  // invariant, with "="
  std::string message;
  size_t pos = 0;
  if (!InputFor(text, sheet, editCell_, in, formula, message, pos)) {
    // Excel's message, and the caret where it puts it.
    host_.ViewMessage(BannerKind::Warning, Utf8ToWide(message));
    editor_.SetMode(CellEditor::Mode::Edit);
    editor_.SetCaret(static_cast<int>(pos), false);
    host_.ViewInvalidate();
    return false;
  }
  if (!ValidateEntry(sheet, editCell_, in)) return false;  // s3-data: the cell's data validation
  edit::Result r;
  if (fill) {
    // Ctrl+Enter: every selected cell (whole rows or columns up to the used cells, not a million),
    // a formula's references moving as a fill moves them.
    Range sel = Selected();
    if (const std::optional<Range> used = s.UsedRange()) {
      if (sel.firstRow == 0 && sel.lastRow == kMaxRows - 1) sel.lastRow = std::max(used->lastRow, editCell_.row);
      if (sel.firstCol == 0 && sel.lastCol == kMaxCols - 1) sel.lastCol = std::max(used->lastCol, editCell_.col);
    }
    const uint64_t count = uint64_t{sel.lastRow - sel.firstRow + 1} * (sel.lastCol - sel.firstCol + 1);
    if (count > 1000000) {
      host_.ViewMessage(BannerKind::Warning, L"That's more cells than Featherweight fills at once.");
      return false;
    }
    std::vector<std::pair<CellAddr, edit::CellInput>> cells;
    cells.reserve(static_cast<size_t>(count));
    for (uint32_t row = sel.firstRow; row <= sel.lastRow; ++row)
      for (uint32_t col = sel.firstCol; col <= sel.lastCol; ++col) {
        edit::CellInput c = in;
        if (!formula.empty() && (row != editCell_.row || col != editCell_.col)) {
          calc::CompileOptions to = at;
          to.row = static_cast<int32_t>(row);
          to.col = static_cast<int32_t>(col);
          c.formula = calc::CopyFormula(formula, at, to, Calc().engine()).substr(1);
        }
        cells.emplace_back(CellAddr{row, col}, std::move(c));
      }
    r = Ed().SetCells(sheet, cells);
  } else {
    r = Ed().SetCell(sheet, editCell_.row, editCell_.col, in);
  }
  if (EditRefused(r)) return false;
  if (!fill) AutoExpandTable(sheet, editCell_);  // s3-data: the table grows, as in Excel
  finish();
  return true;
}

void WorkbookView::Edited(const std::vector<edit::Change>& changes) {
  SyncEngine(changes);
  // The unsaved dot and the owner file; undoing back to what was saved makes the tab clean again.
  const bool wasDirty = file_->Dirty();
  file_->Changed(Ed().Modified(), {});
  if (file_->Dirty() != wasDirty) host_.ViewStateChanged();
  if (!pasting_) clip_.reset();  // an edit ends the copied cells' border, as in Excel
  bool sheets = false, layout = false;
  for (const edit::Change& c : changes) {
    sheets |= (c.what & edit::kSheets) != 0;
    layout |= (c.what & (edit::kRowProps | edit::kColProps | edit::kMerges | edit::kShift)) != 0 &&
              c.sheet == static_cast<int>(sheet_);
  }
  if (sheets) SheetsChanged();
  else if (layout) Restructure();
  RulesEdited(changes);  // s3-data: conditional formats and validations read again
  breakPages_.reset();    // s3-print: Page Break Preview paginates again
  // Notes and drawings that moved (inserted or deleted cells, a sort): read again, the shown
  // sheet's now and the others' when they are shown.
  if (!sheets)
    for (const edit::Change& c : changes) {
      if (!(c.what & (edit::kShift | edit::kObjects)) || c.sheet < 0 || static_cast<size_t>(c.sheet) >= objects_.size())
        continue;
      objects_[static_cast<size_t>(c.sheet)].reset();
      if (c.sheet == static_cast<int>(sheet_)) {
        objects_[sheet_] = SheetObjects::Load(*book_, sheet_);
        note_.reset();
        PlaceObjects();
      }
    }
  // Frozen panes (Freeze panes, or undoing it): the view follows the file's.
  for (const edit::Change& c : changes)
    if ((c.what & edit::kSheetProps) && c.sheet == static_cast<int>(sheet_) && !sheets) {
      const SheetView* v = ViewOf();
      const bool frozen = v && (v->paneState == PaneState::Frozen || v->paneState == PaneState::FrozenSplit) &&
                          (v->xSplit > 0 || v->ySplit > 0);
      const uint32_t rows = frozen ? static_cast<uint32_t>(std::max(0.0, v->ySplit)) : 0;
      const uint32_t cols = frozen ? static_cast<uint32_t>(std::max(0.0, v->xSplit)) : 0;
      if (frozen != Frozen() || rows != frozenRow1_ - frozenRow0_ || cols != frozenCol1_ - frozenCol0_) PanesChanged();
      break;
    }
  if (!SheetShown(sheet_)) ShowSheet(NextShownSheet(sheet_, 1));  // hidden: the next one shows
  if (text_) text_->Clear();  // keyed by cell: it wouldn't see the new values
  totals_ = Totals{};
  for (auto& r : rules_)
    if (r) r->Changed();
  host_.ViewStateChanged();
  host_.ViewInvalidate();
}

void WorkbookView::CheckCircular() {
  if (!calc_ || calc_->Opening()) return;
  int sheet = 0;
  int32_t row = 0, col = 0;
  std::optional<std::tuple<int, int32_t, int32_t>> now;
  if (calc_->CircularCell(sheet, row, col)) now = std::make_tuple(sheet, row, col);
  // Excel's warning, once when one appears (none while iterative calculation is on).
  if (now && !circular_)
    host_.ViewMessage(BannerKind::Warning,
                      L"Circular reference: a formula refers to its own cell, directly or indirectly. It may calculate "
                      L"incorrectly.");
  if (now != circular_) {
    circular_ = now;
    host_.ViewStateChanged();
  }
}

// The engine follows the edit (s2-calc's incremental recalculation): what the edit dirtied is
// recalculated at idle, a slice at a time (WorkbookView::IdleWork).
void WorkbookView::SyncEngine(const std::vector<edit::Change>& changes) {
  bool structure = false, cells = false;
  for (const edit::Change& c : changes) {
    structure |= (c.what & (edit::kSheets | edit::kNames | edit::kShift)) != 0;
    cells |= (c.what & (edit::kValues | edit::kFormulas | edit::kRowProps | edit::kTables)) != 0;
  }
  if (!structure && !cells) return;
  for (auto& o : objects_)  // charts read typed values again (a recalculation may change nothing)
    if (o) o->ValuesChanged();
  calc::WorkbookCalc& calc = Calc();
  recalc_ = true;
  if (structure) {
    calc.StructureEdited();
    return;
  }
  for (const edit::Change& c : changes) {
    if (c.what & (edit::kValues | edit::kFormulas)) calc.CellsEdited(c.sheet, c.cells);
    // s3-data: rows hidden or shown (a filter, undo): SUBTOTAL and AGGREGATE over them again.
    if ((c.what & edit::kRowProps) && c.sheet >= 0 && !calc.Opening()) {
      calc::Area rows;
      rows.r1 = static_cast<int32_t>(std::min<uint32_t>(c.cells.firstRow, kMaxRows - 1));
      rows.r2 = static_cast<int32_t>(std::min<uint32_t>(c.cells.lastRow, kMaxRows - 1));
      rows.c2 = static_cast<int16_t>(kMaxCols - 1);
      calc.engine().Changed(c.sheet, rows);
    }
    // s3-data: a table resized, added or removed: what refers to it, over its old and new cells.
    if ((c.what & edit::kTables) && c.sheet >= 0 && !calc.Opening()) {
      calc::Area t;
      t.r1 = static_cast<int32_t>(std::min<uint32_t>(c.cells.firstRow, kMaxRows - 1));
      t.r2 = static_cast<int32_t>(std::min<uint32_t>(c.cells.lastRow, kMaxRows - 1));
      t.c1 = static_cast<int16_t>(std::min<uint32_t>(c.cells.firstCol, kMaxCols - 1));
      t.c2 = static_cast<int16_t>(std::min<uint32_t>(c.cells.lastCol, kMaxCols - 1));
      calc.engine().TablesChanged();
      calc.engine().Changed(c.sheet, t);
    }
  }
}

void WorkbookView::MoveAfterEntry(int dRow, int dCol) {
  // Enter after a run of Tabs goes to the next row in the column the Tabs started from.
  if (dRow == 1 && dCol == 0 && tabStartCol_ != UINT32_MAX) {
    const CellAddr to{std::min(active_.row + 1, kMaxRows - 1), tabStartCol_};
    tabStartCol_ = UINT32_MAX;
    Select(to, to);
    return;
  }
  MoveActive(dRow, dCol, false, false);
}

void WorkbookView::PointMove(int dRow, int dCol, bool extend) {
  if (!editor_.Pointing()) point_ = pointEnd_ = editCell_;
  auto step = [&](CellAddr& c) {
    c.row = static_cast<uint32_t>(std::clamp<int64_t>(static_cast<int64_t>(c.row) + dRow, 0, kMaxRows - 1));
    c.col = static_cast<uint32_t>(std::clamp<int64_t>(static_cast<int64_t>(c.col) + dCol, 0, kMaxCols - 1));
  };
  if (extend) {
    step(pointEnd_);
  } else {
    step(point_);
    pointEnd_ = point_;
  }
  const uint32_t r0 = std::min(point_.row, pointEnd_.row), r1 = std::max(point_.row, pointEnd_.row);
  const uint32_t c0 = std::min(point_.col, pointEnd_.col), c1 = std::max(point_.col, pointEnd_.col);
  std::wstring ref = Utf8ToWide(CellName(r0, c0));
  if (r0 != r1 || c0 != c1) ref += L":" + Utf8ToWide(CellName(r1, c1));
  editor_.SetPointRef(ref);
  ScrollIntoView(pointEnd_);
}

bool WorkbookView::EditGeometry(EditBox& box) {
  if (!editor_.Active() || !Ready()) return false;
  if (renaming_ >= 0) {
    // Over the sheet's tab, as wide as the name.
    for (const TabRect& t : TabRects()) {
      if (static_cast<int>(t.sheet) != renaming_) continue;
      Canvas measure;
      box.size = 12.5f * ui_;
      box.line = std::ceil(box.size * 1.25f);
      box.pad = 4 * ui_;
      const float mid = std::round((t.rc.top + t.rc.bottom) / 2);
      const float w = measure.MeasureText(editor_.Text(), box.size);
      box.rc = {t.rc.left, mid - box.line / 2 - box.pad / 2, std::max(t.rc.right, t.rc.left + w + 3 * box.pad),
                mid + box.line / 2 + box.pad / 2};
      return true;
    }
    return false;
  }
  if (editSheet_ != sheet_) return false;
  const std::vector<Pane> panes = Panes();
  const Pane* pane = nullptr;
  for (const Pane& p : panes)
    if (editCell_.row >= p.row0 && editCell_.row < p.row1 && editCell_.col >= p.col0 && editCell_.col < p.col1) pane = &p;
  if (!pane) return false;
  Range at = Range::Cell(editCell_.row, editCell_.col);
  if (const Range* m = Current().MergeAt(editCell_.row, editCell_.col)) at = *m;
  const D2D1_RECT_F cell = CellRect(*pane, at.firstRow, at.firstCol, at.lastRow, at.lastCol);
  const Styles& st = book_->GetStyles();
  const Font& font = st.FontOf(st.CellXf(Current().StyleAt(editCell_.row, editCell_.col)));
  const double k = K();
  box.size = static_cast<float>(FontMeasure::Size(font, st.DefaultFont()) * 96.0 / 72.0 * k);
  box.line = std::ceil(box.size * 1.25f);
  box.pad = static_cast<float>(2 * k);
  // As wide as the text (it grows to the right, as Excel's does), at least the cell.
  Canvas measure;
  float w = 0;
  const std::vector<std::wstring_view> lines = Lines(editor_.Text());
  for (std::wstring_view l : lines) w = std::max(w, measure.MeasureText(l, box.size));
  box.rc = cell;
  box.rc.right = std::min(std::max(cell.right, cell.left + w + 3 * box.pad), std::max(cell.right, cells_.right));
  box.rc.bottom = std::max(cell.bottom, cell.top + box.line * static_cast<float>(lines.size()) + box.pad);
  return true;
}

void WorkbookView::DrawEditor(Canvas& c) {
  if (renaming_ < 0) DrawEditBox(c, cells_);
}

void WorkbookView::DrawEditBox(Canvas& c, const D2D1_RECT_F& clip) {
  EditBox b;
  if (!EditGeometry(b)) return;
  const Palette& p = theme::P();
  c.PushClip(clip);
  c.Fill(b.rc, Rgb(0xFFFFFF));
  c.Stroke(b.rc, p.accent, std::max(1.f, ui_));
  c.PushClip(b.rc);
  const std::wstring& text = editor_.Text();
  const std::vector<std::wstring_view> lines = Lines(text);
  // With the caret in the formula bar the cell shows the text only.
  const bool caretHere = !editInBar_;
  const int selFrom = std::min(editor_.Caret(), editor_.Anchor()), selTo = std::max(editor_.Caret(), editor_.Anchor());
  int at = 0;  // index of the line's first character
  for (size_t i = 0; i < lines.size(); ++i) {
    const std::wstring_view l = lines[i];
    const float top = b.rc.top + b.pad / 2 + b.line * static_cast<float>(i);
    const float left = b.rc.left + b.pad;
    auto xAt = [&](int col) { return left + c.MeasureText(l.substr(0, static_cast<size_t>(col)), b.size); };
    const int len = static_cast<int>(l.size());
    const int s0 = std::clamp(selFrom - at, 0, len), s1 = std::clamp(selTo - at, 0, len);
    if (caretHere && s1 > s0) c.Fill({xAt(s0), top, xAt(s1), top + b.line}, Mix(p.accent, Rgb(0xFFFFFF), 0.7f));
    DrawEditLine(c, l, at, left, top, top + b.line, b.size, Rgb(0x000000));
    const int caret = editor_.Caret() - at;
    if (caretHere && caret >= 0 && caret <= len) {
      const float x = std::round(xAt(caret));
      c.Fill({x, top + 1, x + std::max(1.f, std::round(ui_)), top + b.line - 1}, Rgb(0x000000));
    }
    at += len + 1;
  }
  c.PopClip();
  c.PopClip();
}

void WorkbookView::DrawBarText(Canvas& c) {
  const Palette& p = theme::P();
  const D2D1_RECT_F box = FormulaBox();
  const float size = FormulaBarSize();
  c.PushClip(box);
  if (!editor_.Active() || renaming_ >= 0) {
    std::wstring text = FormulaBarText();
    bool spilled = false;  // a cell a dynamic array spilled into: its anchor's formula, greyed (Excel)
    if (calc::Area a; calc_ && calc_->engine().SpillRangeAt(static_cast<int>(sheet_), static_cast<int32_t>(active_.row),
                                                          static_cast<int32_t>(active_.col), a) &&
                      !(static_cast<uint32_t>(a.r1) == active_.row && static_cast<uint32_t>(a.c1) == active_.col)) {
      text = EditTextAt(Current(), static_cast<uint32_t>(a.r1), static_cast<uint32_t>(a.c1));
      spilled = true;
    }
    if (const size_t nl = text.find_first_of(L"\r\n"); nl != std::wstring::npos) text = text.substr(0, nl) + L" …";
    c.Text(text, box, spilled ? p.textSecondary : p.text, size);
    c.PopClip();
    return;
  }
  // While editing, the bar shows the caret's line, scrolled to keep the caret in sight.
  const std::wstring& text = editor_.Text();
  const auto [from, to] = LineAround(text, editor_.Caret());
  const std::wstring_view line = std::wstring_view(text).substr(static_cast<size_t>(from), static_cast<size_t>(to - from));
  const int len = static_cast<int>(line.size());
  auto xAt = [&](int i) { return c.MeasureText(line.substr(0, static_cast<size_t>(i)), size); };
  const int caret = editor_.Caret() - from;
  const float caretX = xAt(caret), width = Width(box);
  if (caretX - barScroll_ > width - 2) barScroll_ = caretX - width + 2;
  if (caretX < barScroll_) barScroll_ = caretX;
  const float left = box.left - barScroll_;
  const float lineH = std::ceil(size * 1.25f), top = std::round((box.top + box.bottom - lineH) / 2);
  if (editInBar_) {
    const int s0 = std::clamp(std::min(editor_.Caret(), editor_.Anchor()) - from, 0, len);
    const int s1 = std::clamp(std::max(editor_.Caret(), editor_.Anchor()) - from, 0, len);
    if (s1 > s0) c.Fill({left + xAt(s0), top, left + xAt(s1), top + lineH}, Alpha(p.accent, 0.3f));
  }
  DrawEditLine(c, line, from, left, box.top, box.bottom, size, p.text);
  if (editInBar_) {
    const float x = std::round(left + caretX);
    c.Fill({x, top + 1, x + std::max(1.f, std::round(ui_)), top + lineH - 1}, p.text);
  }
  c.PopClip();
}

int WorkbookView::BarHit(float x) {
  const std::wstring& text = editor_.Text();
  const auto [from, to] = LineAround(text, editor_.Caret());
  Canvas measure;
  const std::wstring_view line = std::wstring_view(text).substr(static_cast<size_t>(from), static_cast<size_t>(to - from));
  return from + HitIn(measure, line, x - FormulaBox().left + barScroll_, FormulaBarSize());
}

int WorkbookView::BoxHit(const EditBox& b, float x, float y) {
  const std::vector<std::wstring_view> lines = Lines(editor_.Text());
  const int last = static_cast<int>(lines.size()) - 1;
  const int line = std::clamp(static_cast<int>(std::floor((y - b.rc.top - b.pad / 2) / b.line)), 0, last);
  int at = 0;
  for (int i = 0; i < line; ++i) at += static_cast<int>(lines[static_cast<size_t>(i)].size()) + 1;
  Canvas measure;
  return at + HitIn(measure, lines[static_cast<size_t>(line)], x - b.rc.left - b.pad, b.size);
}

bool WorkbookView::EditMouseDown(float x, float y, UINT mods, bool doubleClick) {
  const bool shift = (mods & kModShift) != 0;
  auto place = [&](int at) {
    if (doubleClick) editor_.SelectWord(at);
    else editor_.SetCaret(at, shift);
    UpdateFormulaHelp();
    mouse_ = Mouse::EditText;
    host_.ViewInvalidate();
  };
  if (renaming_ < 0 && gfx::Contains(FormulaBox(), x, y)) {
    if (!editor_.Active()) {
      BeginEdit(CellEditor::Mode::Edit, FormulaBarText());
      if (!editor_.Active()) return true;
      editor_.SetCaret(0, false);  // the bar was showing the first line
    }
    // Typing in the bar moves the caret with the arrows (Edit mode), as Excel's does.
    editInBar_ = true;
    editor_.SetMode(CellEditor::Mode::Edit);
    place(BarHit(x));
    return true;
  }
  EditBox b;
  if (editor_.Active() && EditGeometry(b) && gfx::Contains(b.rc, x, y)) {
    editInBar_ = false;
    editor_.SetMode(CellEditor::Mode::Edit);
    place(BoxHit(b, x, y));
    return true;
  }
  return false;
}

void WorkbookView::EditMouseMove(float x, float y) {
  if (!editor_.Active()) return;
  EditBox b;
  if (editInBar_) editor_.SetCaret(BarHit(x), true);
  else if (EditGeometry(b)) editor_.SetCaret(BoxHit(b, x, y), true);
  host_.ViewInvalidate();
}

std::optional<RECT> WorkbookView::CaretRect() {
  if (!editor_.Active()) return std::nullopt;
  const std::wstring& text = editor_.Text();
  const int caret = editor_.Caret();
  const auto [from, to] = LineAround(text, caret);
  (void)to;
  Canvas measure;
  if (editInBar_) {
    const D2D1_RECT_F box = FormulaBox();
    const float size = FormulaBarSize(), lineH = std::ceil(size * 1.25f);
    const float x = box.left - barScroll_ + measure.MeasureText(std::wstring_view(text).substr(from, caret - from), size);
    const float y = std::round((box.top + box.bottom - lineH) / 2);
    return RECT{static_cast<LONG>(x), static_cast<LONG>(y), static_cast<LONG>(x + 1), static_cast<LONG>(y + lineH)};
  }
  EditBox b;
  if (!EditGeometry(b)) return std::nullopt;
  const int line = static_cast<int>(std::count(text.begin(), text.begin() + caret, L'\n'));
  const float x = b.rc.left + b.pad + measure.MeasureText(std::wstring_view(text).substr(from, caret - from), b.size);
  const float y = b.rc.top + b.pad / 2 + b.line * static_cast<float>(line);
  return RECT{static_cast<LONG>(x), static_cast<LONG>(y), static_cast<LONG>(x + 1), static_cast<LONG>(y + b.line)};
}

// ---------------------------------------------------------------------------
// Replace

bool WorkbookView::ReplacedText(const Match& m, const std::wstring& with, std::wstring& out) const {
  if (query_.empty() || m.sheet >= book_->SheetCount()) return false;
  const std::wstring text = EditTextAt(book_->GetSheet(m.sheet), m.row, m.col);
  const std::wstring hay = matchCase_ ? text : Lower(text), needle = matchCase_ ? query_ : Lower(query_);
  if (wholeWords_) {  // the whole content, as Excel's "Match entire cell contents"
    if (hay != needle) return false;
    out = with;
    return true;
  }
  out.clear();
  size_t from = 0;
  bool any = false;
  for (size_t at; (at = hay.find(needle, from)) != std::wstring::npos; from = at + needle.size()) {
    out += text.substr(from, at - from) + with;
    any = true;
  }
  out += text.substr(from);
  return any;
}

bool WorkbookView::ReplaceCurrent(const std::wstring& with) {
  if (!CanReplace() || matchIndex_ < 0 || matchIndex_ >= static_cast<int>(matches_.size())) return false;
  if (editor_.Active() && !CommitEdit(false)) return false;
  const Match m = matches_[static_cast<size_t>(matchIndex_)];
  std::wstring text;
  if (ReplacedText(m, with, text)) {
    edit::CellInput in;
    std::string formula, message;
    size_t pos = 0;
    if (!InputFor(text, static_cast<int>(m.sheet), {m.row, m.col}, in, formula, message, pos)) {
      host_.ViewMessage(BannerKind::Warning, Utf8ToWide(message));
      return false;
    }
    if (EditRefused(Ed().SetCell(static_cast<int>(m.sheet), m.row, m.col, in))) return false;
  }
  // This one is done: on to the next.
  matches_.erase(matches_.begin() + matchIndex_);
  if (matches_.empty()) {
    matchIndex_ = -1;
  } else {
    if (matchIndex_ >= static_cast<int>(matches_.size())) matchIndex_ = 0;
    ShowMatch();
  }
  host_.ViewStateChanged();
  return true;
}

int WorkbookView::ReplaceAll(const std::wstring& with) {
  if (!CanReplace() || query_.empty()) return 0;
  if (editor_.Active() && !CommitEdit(false)) return 0;
  while (!searchDone_) SearchIdle(1e300);  // every match first
  std::vector<std::pair<CellAddr, edit::CellInput>> cells;
  for (const Match& m : matches_) {
    std::wstring text;
    if (m.sheet != sheet_ || !ReplacedText(m, with, text)) continue;
    edit::CellInput in;
    std::string formula, message;
    size_t pos = 0;
    if (InputFor(text, static_cast<int>(m.sheet), {m.row, m.col}, in, formula, message, pos))
      cells.emplace_back(CellAddr{m.row, m.col}, std::move(in));
  }
  if (cells.empty() || EditRefused(Ed().SetCells(static_cast<int>(sheet_), cells))) return 0;
  StartSearch(query_, matchCase_, wholeWords_);
  return static_cast<int>(cells.size());
}

}  // namespace fw::sheet::view
