#include "sheet/view/WorkbookPanels.h"

#include <algorithm>
#include <cwctype>

#include "core/Strings.h"
#include "sheet/calc/Typing.h"
#include "ui/Theme.h"

using namespace gfx;

namespace fw::sheet::view {

namespace {

bool SameText(std::wstring_view a, std::wstring_view b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](wchar_t x, wchar_t y) {
           return std::towlower(x) == std::towlower(y);
         });
}

}  // namespace

// ---------------------------------------------------------------------------
// Row height and column width

WorkbookView::SizePanel::SizePanel(WorkbookView& view) : v_(view) {
  size_.label = L"Size";
  size_.onChange = [this](double value) { v_.SetSizes(rows_, value); };
}

std::vector<Control*> WorkbookView::SizePanel::Layout(const D2D1_RECT_F& body, float ui) {
  ui_ = ui;
  const float left = body.left + 16 * ui, right = body.right - 16 * ui;
  labelRc_ = {left, body.top + 12 * ui, right, body.top + 30 * ui};
  size_.rc = {left, body.top + 34 * ui, left + 140 * ui, body.top + 66 * ui};
  noteRc_ = {left, size_.rc.bottom + 12 * ui, right, size_.rc.bottom + 52 * ui};
  return {&size_};
}

void WorkbookView::SizePanel::Draw(Canvas& c) {
  const Palette& p = theme::P();
  c.Text(rows_ ? L"Height of the selected rows" : L"Width of the selected columns", labelRc_, p.textSecondary, 12 * ui_);
  c.TextWrapped(rows_ ? L"In points. 0 hides the rows." : L"In characters of the default font. 0 hides the columns.",
                noteRc_, p.textSecondary, 12 * ui_, 2);
  if (focusWanted_) {
    size_.TakeFocus();
    focusWanted_ = false;
  }
}

void WorkbookView::SizePanel::Open(bool rows) {
  rows_ = rows;
  size_.unit = rows ? L" pt" : L"";
  size_.decimals = 2;
  size_.minValue = 0;
  size_.maxValue = rows ? 409 : 255;  // Excel's limits
  size_.step = rows ? 0.75 : 1;
  size_.SetValue(v_.CurrentSize(rows));
  focusWanted_ = true;
}

// ---------------------------------------------------------------------------
// Go to

WorkbookView::GoToPanel::GoToPanel(WorkbookView& view) : v_(view) {
  ref_.label = L"Reference";
  ref_.tooltip = L"A cell (B12), a range (A1:C5), another sheet's (Sheet2!A1) or a name";
  ref_.onEnter = [this] {
    if (v_.GoTo(ref_.Text())) {
      note_.clear();
      v_.host_.ViewShowPanel(nullptr);
      v_.host_.ViewFocusPage();
    } else {
      note_ = L"There's no cell, range or name like that.";
      v_.host_.ViewInvalidate();
    }
  };
}

std::vector<Control*> WorkbookView::GoToPanel::Layout(const D2D1_RECT_F& body, float ui) {
  ui_ = ui;
  const float left = body.left + 16 * ui, right = body.right - 16 * ui;
  labelRc_ = {left, body.top + 12 * ui, right, body.top + 30 * ui};
  ref_.rc = {left, body.top + 34 * ui, right, body.top + 66 * ui};
  noteRc_ = {left, ref_.rc.bottom + 12 * ui, right, ref_.rc.bottom + 52 * ui};
  return {&ref_};
}

void WorkbookView::GoToPanel::Draw(Canvas& c) {
  const Palette& p = theme::P();
  c.Text(L"Reference or name", labelRc_, p.textSecondary, 12 * ui_);
  c.TextWrapped(note_.empty() ? L"Enter goes there." : note_, noteRc_, note_.empty() ? p.textSecondary : p.text, 12 * ui_, 2);
  if (focusWanted_) {
    ref_.TakeFocus();
    focusWanted_ = false;
  }
}

void WorkbookView::GoToPanel::Open() {
  note_.clear();
  std::vector<std::wstring> names;
  for (const DefinedName& n : v_.book_->Names())
    if (!n.hidden && n.name.rfind("_xlnm.", 0) != 0 && (n.sheet < 0 || n.sheet == static_cast<int>(v_.sheet_)))
      names.push_back(Utf8ToWide(n.name));
  std::sort(names.begin(), names.end());
  ref_.items = std::move(names);
  ref_.SetText(L"");
  focusWanted_ = true;
}

// ---------------------------------------------------------------------------
// The view's side of them

void WorkbookView::ShowSizePanel(bool rows) {
  if (!sizePanel_) sizePanel_ = std::make_unique<SizePanel>(*this);
  sizePanel_->Open(rows);
  host_.ViewShowPanel(sizePanel_.get());
}

void WorkbookView::ShowGoTo() {
  if (!goToPanel_) goToPanel_ = std::make_unique<GoToPanel>(*this);
  goToPanel_->Open();
  host_.ViewShowPanel(goToPanel_.get());
}

double WorkbookView::CurrentSize(bool rows) const {
  const Sheet& s = Current();
  const SheetFormat& f = s.Format();
  if (rows) {
    const RowInfo r = s.Row(active_.row);
    return r.hidden ? 0 : r.height > 0 ? r.height : f.defaultRowHeight;
  }
  const ColumnInfo c = s.Column(active_.col);
  if (c.hidden) return 0;
  if (c.width > 0) return c.width;
  return f.defaultColWidth > 0 ? f.defaultColWidth : f.baseColWidth + 0.43;  // 8.43 with Calibri 11
}

void WorkbookView::SetSizes(bool rows, double value) {
  const Range sel = Selected();
  const int sheet = static_cast<int>(sheet_);
  edit::EditGroup group(Ed(), rows ? u"Row Height" : u"Column Width");
  // 0 hides them, as in Excel; another size shows them again.
  edit::Result r = rows ? Ed().HideRows(sheet, sel.firstRow, sel.lastRow, value <= 0)
                        : Ed().HideColumns(sheet, sel.firstCol, sel.lastCol, value <= 0);
  if (r && value > 0)
    r = rows ? Ed().SetRowHeight(sheet, sel.firstRow, sel.lastRow, value)
             : Ed().SetColumnWidth(sheet, sel.firstCol, sel.lastCol, value);
  if (EditRefused(r)) return;
  group.done = true;
}

bool WorkbookView::GoTo(const std::wstring& typed) {
  std::wstring text = typed;
  while (!text.empty() && std::iswspace(text.back())) text.pop_back();
  while (!text.empty() && std::iswspace(text.front())) text.erase(text.begin());
  if (text.empty() || !Ready()) return false;
  // A defined name goes where it refers.
  for (const DefinedName& n : book_->Names())
    if ((n.sheet < 0 || n.sheet == static_cast<int>(sheet_)) && SameText(Utf8ToWide(n.name), text)) {
      text = Utf8ToWide(n.formula);
      break;
    }
  const std::wstring f = L"=" + text;
  const std::vector<calc::RefSpan> spans = calc::ReferenceSpans(f, Region());
  if (spans.size() != 1 || spans[0].begin != 1 || spans[0].end != f.size()) return false;
  const calc::RefSpan& s = spans[0];
  size_t target = sheet_;
  if (!s.sheet.empty()) {
    const int i = book_->FindSheet(WideToUtf8(s.sheet));
    if (i < 0 || !SheetShown(static_cast<size_t>(i))) return false;
    target = static_cast<size_t>(i);
  }
  if (target != sheet_) ShowSheet(target);
  Select({static_cast<uint32_t>(s.r1), static_cast<uint32_t>(s.c1)}, {static_cast<uint32_t>(s.r2), static_cast<uint32_t>(s.c2)});
  return true;
}

}  // namespace fw::sheet::view
