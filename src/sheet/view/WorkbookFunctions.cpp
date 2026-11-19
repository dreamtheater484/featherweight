// Formula AutoComplete (s2-plan "Formula entry"): while a function's name is typed, the functions
// that start with it, from s2-calc's list (Up and Down choose, Tab puts it in with "(", Esc
// closes the list); inside a call, the tip with its arguments, the one the caret is in in bold.
#include <algorithm>
#include <cmath>
#include <cctype>
#include <cwctype>

#include "core/Strings.h"
#include "sheet/calc/Typing.h"
#include "sheet/view/WorkbookView.h"
#include "ui/Theme.h"

using namespace gfx;

namespace fw::sheet::view {

namespace {

constexpr size_t kShown = 8;  // rows the list shows at once

bool NameChar(wchar_t c) { return std::iswalnum(c) || c == L'.' || c == L'_'; }

}  // namespace

void WorkbookView::UpdateFormulaHelp() {
  fnList_.clear();
  fnFrom_ = -1;
  if (!editor_.Active() || !editor_.IsFormula() || editor_.HasSelection() || fnClosed_ || renaming_ >= 0) return;
  const std::wstring& t = editor_.Text();
  const int caret = editor_.Caret();
  if (caret < static_cast<int>(t.size()) && NameChar(t[static_cast<size_t>(caret)])) return;
  int from = caret;
  while (from > 1 && NameChar(t[static_cast<size_t>(from) - 1])) --from;
  if (from == caret || !std::iswalpha(t[static_cast<size_t>(from)])) return;
  // A name where an operand starts (after "=", an operator, "(" or a separator), outside text.
  const wchar_t before = t[static_cast<size_t>(from) - 1];
  if (!std::wcschr(L"=+-*/^&(,;:<> ", before)) return;
  if (std::count(t.begin(), t.begin() + from, L'"') % 2) return;
  std::string prefix;
  for (int i = from; i < caret; ++i) prefix += static_cast<char>(std::towupper(t[static_cast<size_t>(i)]));
  size_t count = 0;
  const calc::FunctionHelp* all = calc::FunctionHelpList(count);
  for (size_t i = 0; i < count; ++i)
    if (std::string_view(all[i].name).substr(0, prefix.size()) == prefix) fnList_.push_back(&all[i]);
  // Defined names in scope too, as Excel lists them among the functions (A to Z).
  nameHelp_.clear();
  nameTexts_.clear();
  if (Ready()) {
    const int sheet = static_cast<int>(editSheet_);
    std::vector<const DefinedName*> names;
    for (const DefinedName& n : book_->Names()) {
      if (n.hidden || n.name.rfind("_xlnm.", 0) == 0 || (n.sheet >= 0 && n.sheet != sheet) || n.name.size() < prefix.size())
        continue;
      bool match = true;
      for (size_t k = 0; k < prefix.size() && match; ++k)
        match = std::towupper(static_cast<unsigned char>(n.name[k])) == static_cast<unsigned char>(prefix[k]);
      if (match) names.push_back(&n);
    }
    nameTexts_.reserve(names.size() * 2);
    for (const DefinedName* n : names) {
      nameTexts_.push_back(n->name);
      nameTexts_.push_back("Defined name: =" + n->formula);
    }
    for (size_t k = 0; k < names.size(); ++k) nameHelp_.push_back({nameTexts_[2 * k].c_str(), nullptr, nameTexts_[2 * k + 1].c_str()});
    for (const calc::FunctionHelp& h : nameHelp_) fnList_.push_back(&h);
    std::stable_sort(fnList_.begin(), fnList_.end(), [](const calc::FunctionHelp* a, const calc::FunctionHelp* b) {
      return _stricmp(a->name, b->name) < 0;
    });
  }
  if (fnList_.empty()) return;
  fnFrom_ = from;
  fnSel_ = 0;
  fnTop_ = 0;
}

void WorkbookView::InsertFunction(size_t i) {
  if (i >= fnList_.size() || fnFrom_ < 0) return;
  const std::wstring name = Utf8ToWide(fnList_[i]->name) + (fnList_[i]->args ? L"(" : L"");  // a name: no "("
  editor_.ReplaceSpan(fnFrom_, editor_.Caret(), name);
  editor_.SetCaret(fnFrom_ + static_cast<int>(name.size()), false);
  fnList_.clear();
  fnFrom_ = -1;
  host_.ViewInvalidate();
}

bool WorkbookView::FormulaHelpKey(UINT vk, bool ctrl, bool shift, bool alt) {
  if (fnList_.empty() || ctrl || alt) return false;
  switch (vk) {
    case VK_UP:
    case VK_DOWN:
    case VK_PRIOR:
    case VK_NEXT: {
      const int step = vk == VK_UP ? -1 : vk == VK_DOWN ? 1 : vk == VK_PRIOR ? -static_cast<int>(kShown) : static_cast<int>(kShown);
      fnSel_ = static_cast<size_t>(std::clamp(static_cast<int>(fnSel_) + step, 0, static_cast<int>(fnList_.size()) - 1));
      if (fnSel_ < fnTop_) fnTop_ = fnSel_;
      if (fnSel_ >= fnTop_ + kShown) fnTop_ = fnSel_ - kShown + 1;
      host_.ViewInvalidate();
      return true;
    }
    case VK_TAB:
      if (shift) return false;
      InsertFunction(fnSel_);
      return true;
    case VK_ESCAPE:
      // The first Esc closes the list; the next cancels the entry.
      fnList_.clear();
      fnClosed_ = true;
      host_.ViewInvalidate();
      return true;
    default: return false;
  }
}

bool WorkbookView::FormulaHelpClick(float x, float y, bool doubleClick) {
  if (fnList_.empty() || !gfx::Contains(fnListRc_, x, y) || fnRowH_ <= 0) return false;
  const size_t i = fnTop_ + static_cast<size_t>((y - fnListRc_.top) / fnRowH_);
  if (i < fnList_.size()) {
    fnSel_ = i;
    if (doubleClick) InsertFunction(i);
  }
  host_.ViewInvalidate();
  return true;
}

void WorkbookView::DrawFormulaHelp(Canvas& c) {
  fnListRc_ = {};
  if (!editor_.Active() || !editor_.IsFormula()) return;
  // Under the formula bar while typing there, else under the box over the cell.
  D2D1_POINT_2F at{};
  if (editInBar_) {
    at = {FormulaBox().left, formulaBar_.bottom + 2 * ui_};
  } else {
    EditBox b;
    if (!EditGeometry(b)) return;
    at = {b.rc.left, b.rc.bottom + 2 * ui_};
  }
  const Palette& p = theme::P();
  const float u = ui_, size = 12.5f * u, pad = 8 * u;
  Canvas measure;
  if (!fnList_.empty()) {
    fnRowH_ = std::round(22 * u);
    const size_t rows = std::min(kShown, fnList_.size());
    float w = 140 * u;
    for (size_t i = fnTop_; i < fnTop_ + rows; ++i)
      w = std::max(w, measure.MeasureText(Utf8ToWide(fnList_[i]->name), size) + 2 * pad);
    fnListRc_ = {at.x, at.y, at.x + w, at.y + fnRowH_ * static_cast<float>(rows)};
    c.SoftShadow(fnListRc_, 4 * u, 6 * u, p.popupShadow, 2 * u);
    c.FillRound(fnListRc_, 4 * u, p.popupBg);
    c.StrokeRound(fnListRc_, 4 * u, p.popupBorder, 1);
    for (size_t i = fnTop_; i < fnTop_ + rows; ++i) {
      const float top = fnListRc_.top + fnRowH_ * static_cast<float>(i - fnTop_);
      const D2D1_RECT_F row = {fnListRc_.left + 2 * u, top + 1, fnListRc_.right - 2 * u, top + fnRowH_ - 1};
      if (i == fnSel_) c.FillRound(row, 3 * u, Alpha(p.accent, 0.18f));
      c.Text(Utf8ToWide(fnList_[i]->name), Inflate(row, -pad + 2 * u, 0), p.text, size);
    }
    // The chosen function's description beside the list.
    const std::wstring about = Utf8ToWide(fnList_[fnSel_]->about);
    const float tw = 260 * u, top = fnListRc_.top + fnRowH_ * static_cast<float>(fnSel_ - fnTop_);
    const float h = c.TextWrapped(about, {0, 0, tw - 2 * pad, 1000}, p.text, size, 4, false);
    const D2D1_RECT_F tip = {fnListRc_.right + 4 * u, top, fnListRc_.right + 4 * u + tw, top + h + 2 * pad};
    c.FillRound(tip, 4 * u, p.popupBg);
    c.StrokeRound(tip, 4 * u, p.popupBorder, 1);
    c.TextWrapped(about, Inflate(tip, -pad, -pad), p.textSecondary, size, 4);
    return;
  }
  // The argument tip: the call the caret is in.
  std::wstring fn;
  int arg = 0;
  if (!calc::CallAt(editor_.Text(), static_cast<size_t>(editor_.Caret()), Region(), fn, arg)) return;
  const calc::FunctionHelp* help = calc::FindFunctionHelp(WideToUtf8(fn));
  if (!help) return;
  size_t b0 = 0, b1 = 0;
  const std::wstring tip = calc::ArgumentTip(*help, Region(), arg, b0, b1);
  b0 = std::min(b0, tip.size());
  b1 = std::clamp(b1, b0, tip.size());
  const std::wstring_view v(tip);
  const float w0 = measure.MeasureText(v.substr(0, b0), size);
  const float w1 = measure.MeasureText(v.substr(b0, b1 - b0), size, DWRITE_FONT_WEIGHT_BOLD);
  const float w2 = measure.MeasureText(v.substr(b1), size);
  const float h = std::round(22 * u);
  const D2D1_RECT_F box = {at.x, at.y, at.x + w0 + w1 + w2 + 2 * pad, at.y + h};
  c.FillRound(box, 4 * u, p.popupBg);
  c.StrokeRound(box, 4 * u, p.popupBorder, 1);
  float x = box.left + pad;
  c.Text(v.substr(0, b0), {x, box.top, x + w0 + size, box.bottom}, p.text, size);
  x += w0;
  c.Text(v.substr(b0, b1 - b0), {x, box.top, x + w1 + size, box.bottom}, p.text, size, DWRITE_FONT_WEIGHT_BOLD);
  x += w1;
  c.Text(v.substr(b1), {x, box.top, x + w2 + size, box.bottom}, p.text, size);
}

void WorkbookView::AutoSum() {
  if (!Ready() || preview_ || ChartSheet()) return;
  const Sheet& s = Current();
  // What AutoSum needs to know of a cell: empty, a number (from a SUM or SUBTOTAL: a total), or
  // anything else.
  auto kind = [&](int32_t r, int32_t c) {
    CellInfo ci;
    if (r < 0 || c < 0 || static_cast<uint32_t>(r) >= kMaxRows || static_cast<uint32_t>(c) >= kMaxCols ||
        !s.GetCell(static_cast<uint32_t>(r), static_cast<uint32_t>(c), ci) ||
        (ci.value.type == ValueType::Empty && !ci.hasFormula))
      return calc::SumCell::kEmpty;
    if (ci.value.type != ValueType::Number) return calc::SumCell::kOther;
    FormulaInfo f;
    if (ci.hasFormula && s.GetFormula(static_cast<uint32_t>(r), static_cast<uint32_t>(c), f)) {
      std::string t(f.text);
      for (char& ch : t) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
      if (t.rfind("SUM(", 0) == 0 || t.rfind("SUBTOTAL(", 0) == 0) return calc::SumCell::kTotal;
    }
    return calc::SumCell::kNumber;
  };
  // Several cells selected: their totals at once, under the columns (or beside a row), with no cell
  // being edited and one undo step, as Excel does (s4 friction W5). Entering the total in the first
  // number, as from one cell, would replace it on Enter.
  Range sel = Selected();
  if (sel != Expand(Range{active_.row, active_.col, active_.row, active_.col})) {
    if (editor_.Active() && !CommitEdit(false)) return;
    // Whole rows or columns: up to the last cells in use in them.
    const bool wholeCols = sel.firstRow == 0 && sel.lastRow == kMaxRows - 1;
    const bool wholeRows = sel.firstCol == 0 && sel.lastCol == kMaxCols - 1;
    if (wholeCols || wholeRows) {
      const std::optional<Range> used = s.UsedRange();
      if (!used) return;
      if (wholeCols) sel.lastRow = std::max(used->lastRow, sel.firstRow);
      if (wholeRows) sel.lastCol = std::max(used->lastCol, sel.firstCol);
    }
    if (uint64_t{sel.lastRow - sel.firstRow + 1} * (sel.lastCol - sel.firstCol + 1) > 1000000) {
      host_.ViewMessage(BannerKind::Warning, L"That's more cells than Featherweight totals at once.");
      return;
    }
    auto lineEmpty = [&](bool row, uint32_t at) {
      for (uint32_t i = row ? sel.firstCol : sel.firstRow; i <= (row ? sel.lastCol : sel.lastRow); ++i)
        if (kind(static_cast<int32_t>(row ? at : i), static_cast<int32_t>(row ? i : at)) != calc::SumCell::kEmpty)
          return false;
      return true;
    };
    // The empty rows and columns at the end of whole ones aren't the selection's own.
    if (wholeCols)
      while (sel.lastRow > sel.firstRow && lineEmpty(true, sel.lastRow)) --sel.lastRow;
    if (wholeRows)
      while (sel.lastCol > sel.firstCol && lineEmpty(false, sel.lastCol)) --sel.lastCol;
    const std::vector<calc::AutoSumCell> sums = calc::ProposeAutoSums(
        static_cast<int32_t>(sel.firstRow), static_cast<int32_t>(sel.firstCol), static_cast<int32_t>(sel.lastRow),
        static_cast<int32_t>(sel.lastCol), static_cast<int32_t>(kMaxRows - 1), static_cast<int32_t>(kMaxCols - 1),
        kind);
    if (!sums.empty()) {
      std::vector<std::pair<CellAddr, edit::CellInput>> cells;
      for (const calc::AutoSumCell& t : sums)
        cells.emplace_back(CellAddr{static_cast<uint32_t>(t.row), static_cast<uint32_t>(t.col)},
                           edit::CellInput::Formula(t.formula.substr(1)));
      EditRefused(Ed().SetCells(static_cast<int>(sheet_), cells));
      host_.ViewInvalidate();
      return;
    }
    // Nothing to total: from an empty active cell, as from one cell (=SUM() to be given a range);
    // never by editing a cell that holds something.
    if (kind(static_cast<int32_t>(active_.row), static_cast<int32_t>(active_.col)) != calc::SumCell::kEmpty) {
      host_.ViewMessage(BannerKind::Warning, L"There's no empty cell left on the sheet for the total.");
      return;
    }
  }
  const calc::AutoSumProposal p =
      calc::ProposeAutoSum(static_cast<int32_t>(active_.row), static_cast<int32_t>(active_.col), kind);
  BeginEdit(CellEditor::Mode::Enter, L"");
  if (!editor_.Active()) return;
  const std::wstring text = Utf8ToWide(calc::ToLocal(p.formula, Region()));
  const size_t open = text.find(L'('), close = text.rfind(L')');
  editor_.SetText(text, static_cast<int>(text.size()), static_cast<int>(text.size()));
  if (open != std::wstring::npos && close != std::wstring::npos && close > open) {
    if (p.found && p.cells.empty() && close > open + 1) {
      editor_.SetPointSpan(static_cast<int>(open) + 1, static_cast<int>(close - open - 1));
      point_ = {static_cast<uint32_t>(p.r1), static_cast<uint32_t>(p.c1)};
      pointEnd_ = {static_cast<uint32_t>(p.r2), static_cast<uint32_t>(p.c2)};
    } else {
      editor_.SetCaret(static_cast<int>(close), false);  // =SUM(|): the range is the user's to give
    }
  }
  UpdateFormulaHelp();
  host_.ViewInvalidate();
}

}  // namespace fw::sheet::view
