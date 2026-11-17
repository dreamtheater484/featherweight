// The workbook tab's cell editing (s2-plan "Editing in the grid"): typing over a cell, F2, the
// box over the cell and the formula bar, Enter and Tab as Excel moves after an entry, point mode.
// The text and keys are CellEditor's; committing goes through the edit API (s2-model).
#include <algorithm>
#include <cmath>

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

}  // namespace

void WorkbookView::BeginEdit(CellEditor::Mode mode, const std::wstring& text) {
  if (!Ready() || ChartSheet() || preview_) return;
  editCell_ = active_;
  editSheet_ = sheet_;
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
  const std::wstring& text = editor_.Text();
  const std::vector<std::wstring_view> lines = Lines(text);
  const int selFrom = std::min(editor_.Caret(), editor_.Anchor()), selTo = std::max(editor_.Caret(), editor_.Anchor());
  int at = 0;  // index of the line's first character
  for (size_t i = 0; i < lines.size(); ++i) {
    const std::wstring_view l = lines[i];
    const float top = b.rc.top + b.pad / 2 + b.line * static_cast<float>(i);
    const float left = b.rc.left + b.pad;
    auto xAt = [&](int col) { return left + c.MeasureText(l.substr(0, static_cast<size_t>(col)), b.size); };
    const int len = static_cast<int>(l.size());
    const int s0 = std::clamp(selFrom - at, 0, len), s1 = std::clamp(selTo - at, 0, len);
    if (s1 > s0) c.Fill({xAt(s0), top, xAt(s1), top + b.line}, Mix(p.accent, Rgb(0xFFFFFF), 0.7f));
    c.Text(l, {left, top, b.rc.right - b.pad, top + b.line}, Rgb(0x000000), b.size);
    const int caret = editor_.Caret() - at;
    if (caret >= 0 && caret <= len && (i + 1 == lines.size() || caret < len + 1)) {
      if (editor_.Caret() >= at && editor_.Caret() <= at + len) {
        const float x = std::round(xAt(caret));
        c.Fill({x, top + 1, x + std::max(1.f, std::round(ui_)), top + b.line - 1}, Rgb(0x000000));
      }
    }
    at += len + 1;
  }
  c.PopClip();
}

std::optional<RECT> WorkbookView::CaretRect() {
  EditBox b;
  if (!EditGeometry(b)) return std::nullopt;
  const std::wstring& text = editor_.Text();
  const int caret = editor_.Caret();
  const size_t lineStart = text.rfind(L'\n', caret > 0 ? static_cast<size_t>(caret) - 1 : 0);
  const size_t from = caret > 0 && lineStart != std::wstring::npos ? lineStart + 1 : 0;
  const int line = static_cast<int>(std::count(text.begin(), text.begin() + caret, L'\n'));
  Canvas measure;
  const float x = b.rc.left + b.pad + measure.MeasureText(std::wstring_view(text).substr(from, caret - from), b.size);
  const float y = b.rc.top + b.pad / 2 + b.line * static_cast<float>(line);
  return RECT{static_cast<LONG>(x), static_cast<LONG>(y), static_cast<LONG>(x + 1), static_cast<LONG>(y + b.line)};
}

}  // namespace fw::sheet::view
