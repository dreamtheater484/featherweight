// Cut, copy and paste (s2-plan "Clipboard"): what Copy puts on the clipboard is remembered, so a
// paste in this workbook brings formulas (references moved) and formats; text from anywhere else
// is read as typed, cell by cell. Paste special: values, formulas, formats, transposed. The copied
// cells keep a dashed border until the next edit or Esc. Every paste is one step.
#include <algorithm>

#include "Util.h"
#include "core/Strings.h"
#include "sheet/calc/Typing.h"
#include "sheet/view/WorkbookView.h"
#include "ui/Theme.h"

using namespace gfx;

namespace fw::sheet::view {

namespace {

// Tab-separated text as Excel writes it: rows by line breaks, quoted cells may hold tabs, line
// breaks and doubled quotes.
std::vector<std::vector<std::wstring>> ParseTsv(const std::wstring& text) {
  std::vector<std::vector<std::wstring>> rows(1);
  std::wstring cell;
  bool quoted = false;
  for (size_t i = 0; i < text.size(); ++i) {
    const wchar_t ch = text[i];
    if (quoted) {
      if (ch == L'"' && i + 1 < text.size() && text[i + 1] == L'"') cell += L'"', ++i;
      else if (ch == L'"') quoted = false;
      else cell += ch;
    } else if (ch == L'"' && cell.empty()) {
      quoted = true;
    } else if (ch == L'\t') {
      rows.back().push_back(std::move(cell));
      cell.clear();
    } else if (ch == L'\r' || ch == L'\n') {
      if (ch == L'\r' && i + 1 < text.size() && text[i + 1] == L'\n') ++i;
      rows.back().push_back(std::move(cell));
      cell.clear();
      rows.emplace_back();
    } else {
      cell += ch;
    }
  }
  if (!cell.empty() || !rows.back().empty()) rows.back().push_back(std::move(cell));
  while (!rows.empty() && rows.back().empty()) rows.pop_back();  // the last line break
  return rows;
}

}  // namespace

void WorkbookView::Cut() {
  Copy();
  if (clip_) clip_->cut = true;
  host_.ViewInvalidate();
}

void WorkbookView::Paste(PasteMode mode, bool transpose) {
  if (!Ready() || preview_ || ChartSheet()) return;
  if (editor_.Active() && !CommitEdit(false)) return;
  if (std::u16string why; !Ed().CanEdit(&why)) {
    host_.ViewMessage(BannerKind::Info, std::wstring(why.begin(), why.end()));
    return;
  }
  const int sheet = static_cast<int>(sheet_);
  const Range sel = Selected();
  std::vector<std::pair<CellAddr, edit::CellInput>> cells;
  std::vector<std::pair<CellAddr, uint32_t>> formats;  // paste formats only
  Range pasted{};
  // This workbook's own copy: the cells themselves.
  const Sheet* from = nullptr;
  if (clip_ && util::ClipboardSequence() == clip_->seq)
    for (size_t i = 0; i < book_->SheetCount(); ++i)
      if (&book_->GetSheet(i) == clip_->sheet) from = clip_->sheet;
  if (from) {
    const Range src = clip_->range;
    const int fromSheet = static_cast<int>(std::find(sheetOrder_.begin(), sheetOrder_.end(), from) - sheetOrder_.begin());
    const uint32_t h = src.lastRow - src.firstRow + 1, w = src.lastCol - src.firstCol + 1;
    const uint32_t th = transpose ? w : h, tw = transpose ? h : w;
    // Repeated over a selection that is a whole number of copies, else once at its corner.
    const uint32_t selH = sel.lastRow - sel.firstRow + 1, selW = sel.lastCol - sel.firstCol + 1;
    const uint32_t ny = !clip_->cut && selH % th == 0 && selW % tw == 0 ? selH / th : 1;
    const uint32_t nx = !clip_->cut && selH % th == 0 && selW % tw == 0 ? selW / tw : 1;
    if (uint64_t{sel.firstRow} + uint64_t{th} * ny > kMaxRows || uint64_t{sel.firstCol} + uint64_t{tw} * nx > kMaxCols) {
      host_.ViewMessage(BannerKind::Warning, L"The cells don't fit there: there aren't enough rows or columns.");
      return;
    }
    if (uint64_t{h} * w * ny * nx > 1000000) {
      host_.ViewMessage(BannerKind::Warning, L"That's more cells than Featherweight pastes at once.");
      return;
    }
    calc::Engine& names = Calc().engine();
    for (uint32_t by = 0; by < ny; ++by)
      for (uint32_t bx = 0; bx < nx; ++bx)
        for (uint32_t r = src.firstRow; r <= src.lastRow; ++r)
          for (uint32_t c = src.firstCol; c <= src.lastCol; ++c) {
            const uint32_t dr = transpose ? c - src.firstCol : r - src.firstRow;
            const uint32_t dc = transpose ? r - src.firstRow : c - src.firstCol;
            const CellAddr to{sel.firstRow + by * th + dr, sel.firstCol + bx * tw + dc};
            CellInfo cell;
            if (!from->GetCell(r, c, cell)) cell = CellInfo{r, c};
            if (mode == PasteMode::Formats) {
              formats.emplace_back(to, cell.style);
              continue;
            }
            edit::CellInput in;
            FormulaInfo f;
            if (mode != PasteMode::Values && cell.hasFormula && from->GetFormula(r, c, f)) {
              // A copy moves its relative references; a cut keeps the formula as it reads in
              // its own cell (shared formulas are written from their first cell).
              calc::CompileOptions origin, dest;
              origin.sheet = fromSheet;
              origin.row = static_cast<int32_t>(f.origin.row);
              origin.col = static_cast<int32_t>(f.origin.col);
              dest.sheet = sheet;
              dest.row = static_cast<int32_t>(clip_->cut ? r : to.row);
              dest.col = static_cast<int32_t>(clip_->cut ? c : to.col);
              in = edit::CellInput::Formula(calc::CopyFormula("=" + std::string(f.text), origin, dest, names).substr(1));
            } else {
              switch (cell.value.type) {
                case ValueType::Empty: break;
                case ValueType::Number: in = edit::CellInput::Number(cell.value.number); break;
                case ValueType::String: in = edit::CellInput::Text(std::string(cell.value.text)); break;
                case ValueType::Bool: in = edit::CellInput::Bool(cell.value.boolean); break;
                case ValueType::Error: in = edit::CellInput::Error(cell.value.error); break;
              }
            }
            if (mode == PasteMode::All) in.WithStyle(cell.style);
            cells.emplace_back(to, std::move(in));
          }
    pasted = {sel.firstRow, sel.firstCol, sel.firstRow + th * ny - 1, sel.firstCol + tw * nx - 1};
  } else {
    // Text from anywhere: each cell as if typed there.
    const std::vector<std::vector<std::wstring>> rows = ParseTsv(util::GetClipboardText());
    if (rows.empty()) return;
    size_t width = 0;
    for (const auto& r : rows) width = std::max(width, r.size());
    const uint32_t th = static_cast<uint32_t>(transpose ? width : rows.size());
    const uint32_t tw = static_cast<uint32_t>(transpose ? rows.size() : width);
    if (uint64_t{sel.firstRow} + th > kMaxRows || uint64_t{sel.firstCol} + tw > kMaxCols) {
      host_.ViewMessage(BannerKind::Warning, L"The cells don't fit there: there aren't enough rows or columns.");
      return;
    }
    for (size_t r = 0; r < rows.size(); ++r)
      for (size_t c = 0; c < rows[r].size(); ++c) {
        const CellAddr to{sel.firstRow + static_cast<uint32_t>(transpose ? c : r), sel.firstCol + static_cast<uint32_t>(transpose ? r : c)};
        edit::CellInput in;
        std::string formula, message;
        size_t pos = 0;
        // A formula that doesn't read stays as its text, as Excel pastes it.
        if (!InputFor(rows[r][c], sheet, to, in, formula, message, pos)) in = edit::CellInput::Text(WideToUtf8(rows[r][c]));
        cells.emplace_back(to, std::move(in));
      }
    pasted = {sel.firstRow, sel.firstCol, sel.firstRow + th - 1, sel.firstCol + tw - 1};
  }
  const bool cut = from && clip_->cut;
  const Range cutFrom = cut ? clip_->range : Range{};
  edit::EditGroup group(Ed(), cut ? u"Move" : u"Paste");
  pasting_ = true;
  edit::Result r;
  if (cut && from == &Current()) r = Ed().ClearAll(sheet, cutFrom);  // the cells move: their place empties first
  else if (cut) r = Ed().ClearAll(static_cast<int>(std::find(sheetOrder_.begin(), sheetOrder_.end(), from) - sheetOrder_.begin()), cutFrom);
  if (r && !cells.empty()) r = Ed().SetCells(sheet, cells);
  for (const auto& [at, style] : formats)
    if (r) r = Ed().SetStyle(sheet, Range::Cell(at.row, at.col), style);
  pasting_ = false;
  if (EditRefused(r)) return;
  group.done = true;
  if (cut) clip_.reset();  // a cut pastes once
  Select({pasted.firstRow, pasted.firstCol}, {pasted.lastRow, pasted.lastCol}, false);
  host_.ViewInvalidate();
}

void WorkbookView::DrawClipBorder(Canvas& c, const Pane& pane) {
  if (!clip_ || clip_->sheet != &Current()) return;
  const Range& r = clip_->range;
  const D2D1_RECT_F f = CellRect(pane, r.firstRow, r.firstCol, r.lastRow, r.lastCol);
  // Dashes along the range's edges (Excel's marching ants, standing still: no idle redraws).
  // White under accent dashes, as thick as the selection's border so they show over it.
  const float u = std::max(1.f, std::round(ui_)), dash = 4 * u, gap = 3 * u, w = std::max(2.f, std::round(2 * ui_));
  const D2D1_COLOR_F ink = theme::P().accent;
  const float l = f.left - w / 2, t = f.top - w / 2, rr = f.right - 1 - w / 2, b = f.bottom - 1 - w / 2;
  const D2D1_COLOR_F white = Rgb(0xFFFFFF);
  c.Fill({l, t, rr + w, t + w}, white);
  c.Fill({l, b, rr + w, b + w}, white);
  c.Fill({l, t, l + w, b + w}, white);
  c.Fill({rr, t, rr + w, b + w}, white);
  for (float x = l; x < rr + w; x += dash + gap) {
    c.Fill({x, t, std::min(x + dash, rr + w), t + w}, ink);
    c.Fill({x, b, std::min(x + dash, rr + w), b + w}, ink);
  }
  for (float y = t; y < b + w; y += dash + gap) {
    c.Fill({l, y, l + w, std::min(y + dash, b + w)}, ink);
    c.Fill({rr, y, rr + w, std::min(y + dash, b + w)}, ink);
  }
}

}  // namespace fw::sheet::view
