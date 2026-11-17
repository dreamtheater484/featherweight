// The workbook tab's cell editing (s2-plan "Editing in the grid"): typing over a cell, F2, the
// box over the cell and the formula bar, Enter and Tab as Excel moves after an entry, point mode.
// The text and keys are CellEditor's; committing goes through the edit API (s2-model).
#include <algorithm>
#include <cmath>
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

}  // namespace

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
    // A rectangle past the text's end, so it is clipped by the box rather than trimmed.
    c.Text(l, {left, top, xAt(len) + b.size, top + b.line}, Rgb(0x000000), b.size);
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
  c.Text(line, {left, box.top, left + xAt(len) + size, box.bottom}, p.text, size);
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
