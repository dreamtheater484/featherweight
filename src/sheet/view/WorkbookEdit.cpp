// The workbook tab's cell editing (s2-plan "Editing in the grid"): typing over a cell, F2, the
// box over the cell and the formula bar, Enter and Tab as Excel moves after an entry, point mode.
// The text and keys are CellEditor's; committing goes through the edit API (s2-model).
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <iterator>
#include <utility>

#include "Commands.h"
#include "core/Strings.h"
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

bool Word(wchar_t c) { return std::iswalnum(c) || c == L'_' || c == L'.' || c > 0x7F; }
bool Letter(wchar_t c) { return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z'); }
bool Digit(wchar_t c) { return c >= L'0' && c <= L'9'; }

// A column ("$AB") at `i`: its index and where it ends, or false.
bool ScanCol(std::wstring_view s, size_t& i, uint32_t& col) {
  size_t j = i + (i < s.size() && s[i] == L'$');
  uint32_t v = 0;
  size_t n = 0;
  while (j < s.size() && Letter(s[j]) && n < 4) {
    v = v * 26 + static_cast<uint32_t>(std::towupper(s[j]) - L'A' + 1);
    ++j;
    ++n;
  }
  if (n == 0 || n > 3 || v > kMaxCols) return false;
  col = v - 1;
  i = j;
  return true;
}

bool ScanRow(std::wstring_view s, size_t& i, uint32_t& row) {
  size_t j = i + (i < s.size() && s[i] == L'$');
  uint64_t v = 0;
  size_t n = 0;
  while (j < s.size() && Digit(s[j]) && n < 8) v = v * 10 + (s[j++] - L'0'), ++n;
  if (n == 0 || v == 0 || v > kMaxRows) return false;
  row = static_cast<uint32_t>(v - 1);
  i = j;
  return true;
}

// The references in a formula being typed: A1, A1:B2, A:B, 1:2, with $ and a sheet prefix
// (Sheet1!, 'My sheet'!). Nothing in strings; not a name followed by "(" (LOG10(). A stand-in
// until s2-calc's reference spans land.
struct Found {
  size_t at, length;
  std::wstring sheet;  // empty: none named
  uint32_t r0, c0, r1, c1;
};
std::vector<Found> ScanReferences(std::wstring_view s) {
  std::vector<Found> out;
  size_t i = 1;  // after "="
  while (i < s.size()) {
    const wchar_t ch = s[i];
    if (ch == L'"') {  // a string: "" is a quote inside it
      for (++i; i < s.size(); ++i)
        if (s[i] == L'"' && !(i + 1 < s.size() && s[i + 1] == L'"')) break;
        else if (s[i] == L'"') ++i;
      ++i;
      continue;
    }
    if (!(Word(ch) || ch == L'$' || ch == L'\'')) {
      ++i;
      continue;
    }
    const size_t start = i;
    std::wstring sheet;
    size_t j = i;
    if (ch == L'\'') {  // 'My sheet'!
      std::wstring name;
      for (++j; j < s.size(); ++j) {
        if (s[j] == L'\'') {
          if (j + 1 < s.size() && s[j + 1] == L'\'') {
            name += L'\'';
            ++j;
            continue;
          }
          break;
        }
        name += s[j];
      }
      if (j + 1 < s.size() && s[j + 1] == L'!') {
        sheet = name;
        j += 2;
      } else {
        i = j + 1;
        continue;
      }
    } else {
      size_t k = j;
      while (k < s.size() && Word(s[k])) ++k;
      if (k < s.size() && s[k] == L'!' && k > j) {
        sheet.assign(s.substr(j, k - j));
        j = k + 1;
      }
    }
    // The reference itself.
    Found f{start, 0, sheet, 0, 0, 0, 0};
    size_t k = j;
    bool ok = false;
    uint32_t col = 0, row = 0;
    if (size_t a = k; ScanCol(s, a, col)) {
      if (size_t b = a; ScanRow(s, b, row)) {  // A1, maybe A1:B2
        f.r0 = f.r1 = row;
        f.c0 = f.c1 = col;
        k = b;
        ok = true;
        if (k < s.size() && s[k] == L':') {
          size_t c = k + 1;
          uint32_t col2 = 0, row2 = 0;
          if (ScanCol(s, c, col2) && ScanRow(s, c, row2)) {
            f.r1 = row2;
            f.c1 = col2;
            k = c;
          }
        }
      } else if (a < s.size() && s[a] == L':') {  // A:B
        size_t c = a + 1;
        uint32_t col2 = 0;
        if (ScanCol(s, c, col2) && !(c < s.size() && (Digit(s[c]) || s[c] == L'$'))) {
          f.c0 = col;
          f.c1 = col2;
          f.r0 = 0;
          f.r1 = kMaxRows - 1;
          k = c;
          ok = true;
        }
      }
    }
    if (!ok) {  // 1:2
      size_t a = k;
      uint32_t row2 = 0;
      if (ScanRow(s, a, row) && a < s.size() && s[a] == L':') {
        size_t c = a + 1;
        if (ScanRow(s, c, row2)) {
          f.r0 = row;
          f.r1 = row2;
          f.c0 = 0;
          f.c1 = kMaxCols - 1;
          k = c;
          ok = true;
        }
      }
    }
    // Not part of a longer name or number, and not a function.
    if (ok && (start == 0 || !(Word(s[start - 1]) || s[start - 1] == L'$')) &&
        !(k < s.size() && (Word(s[k]) || s[k] == L'(' || s[k] == L'$' || s[k] == L'!'))) {
      if (f.r0 > f.r1) std::swap(f.r0, f.r1);
      if (f.c0 > f.c1) std::swap(f.c0, f.c1);
      f.length = k - start;
      out.push_back(std::move(f));
      i = k;
      continue;
    }
    // Skip the whole word.
    i = std::max(j, start + 1);
    while (i < s.size() && (Word(s[i]) || s[i] == L'$')) ++i;
  }
  return out;
}

}  // namespace

const std::vector<WorkbookView::RefBox>& WorkbookView::Refs() {
  if (!editor_.Active() || !editor_.IsFormula()) {
    refs_.clear();
    refsText_.clear();
    return refs_;
  }
  if (editor_.Text() == refsText_ && !refs_.empty()) return refs_;
  refsText_ = editor_.Text();
  refs_.clear();
  int colors = 0;
  for (const Found& f : ScanReferences(refsText_)) {
    RefBox r;
    r.at = static_cast<int>(f.at);
    r.length = static_cast<int>(f.length);
    r.sheet = f.sheet.empty() ? -1 : book_->FindSheet(WideToUtf8(f.sheet));
    if (r.sheet < 0 && !f.sheet.empty()) r.sheet = -2;
    r.r0 = f.r0;
    r.c0 = f.c0;
    r.r1 = f.r1;
    r.c1 = f.c1;
    // The same range keeps its color.
    r.color = -1;
    for (const RefBox& o : refs_)
      if (o.sheet == r.sheet && o.r0 == r.r0 && o.c0 == r.c0 && o.r1 == r.r1 && o.c1 == r.c1) r.color = o.color;
    if (r.color < 0) r.color = colors++ % static_cast<int>(std::size(kRefColors));
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

void WorkbookView::BeginEdit(CellEditor::Mode mode, const std::wstring& text) {
  if (!Ready() || ChartSheet() || preview_) return;
  editCell_ = active_;
  editSheet_ = sheet_;
  editInBar_ = false;
  barScroll_ = 0;
  editor_.Begin(text, mode);
  ScrollIntoView(active_);
  host_.ViewStateChanged();
  host_.ViewInvalidate();
}

bool WorkbookView::OnChar(wchar_t ch, UINT mods) {
  if (!Ready() || preview_ || ChartSheet()) return false;
  if (editor_.Active()) {
    if (editor_.OnChar(ch)) host_.ViewInvalidate();
    return true;
  }
  if (ch < 0x20 || ch == 0x7F) return false;
  if ((mods & kModCtrl) && !(mods & kModAlt)) return false;
  // Typing over a cell replaces it (Enter mode).
  BeginEdit(CellEditor::Mode::Enter, L"");
  editor_.OnChar(ch);
  return true;
}

bool WorkbookView::EditKey(UINT vk, UINT mods) {
  const bool ctrl = (mods & kModCtrl) != 0, shift = (mods & kModShift) != 0, alt = (mods & kModAlt) != 0;
  const CellEditor::Result r = editor_.OnKey(vk, ctrl, shift, alt);
  switch (r.action) {
    case CellEditor::Action::None: return true;  // nothing else happens while a cell is edited
    case CellEditor::Action::Handled: break;
    case CellEditor::Action::Cancel:
      editor_.End();
      host_.ViewStateChanged();
      break;
    case CellEditor::Action::Commit:
      CommitEdit(false);
      if (vk == VK_TAB) {
        if (tabStartCol_ == UINT32_MAX) tabStartCol_ = active_.col;
      } else if (vk != VK_RETURN) {
        tabStartCol_ = UINT32_MAX;
      }
      MoveAfterEntry(r.dRow, r.dCol);
      break;
    case CellEditor::Action::CommitFill: CommitEdit(true); break;
    case CellEditor::Action::Point: PointMove(r.dRow, r.dCol, r.extend); break;
  }
  host_.ViewInvalidate();
  return true;
}

void WorkbookView::CommitEdit(bool fill) {
  if (!editor_.Active()) return;
  const std::wstring text = editor_.Text();
  const bool changed = editor_.Changed();
  editor_.End();
  editInBar_ = false;
  // Writing the cell (or, with Ctrl+Enter, every selected cell) is the edit API's: s2-model's
  // operations. Until it lands the entry only ends here.
  (void)text;
  (void)changed;
  (void)fill;
  host_.ViewStateChanged();
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
  if (!editor_.Active() || editSheet_ != sheet_ || !Ready()) return false;
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
  EditBox b;
  if (!EditGeometry(b)) return;
  const Palette& p = theme::P();
  c.PushClip(cells_);
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
  if (!editor_.Active()) {
    std::wstring text = FormulaBarText();
    if (const size_t nl = text.find_first_of(L"\r\n"); nl != std::wstring::npos) text = text.substr(0, nl) + L" …";
    c.Text(text, box, p.text, size);
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
    mouse_ = Mouse::EditText;
    host_.ViewInvalidate();
  };
  if (gfx::Contains(FormulaBox(), x, y)) {
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

}  // namespace fw::sheet::view
