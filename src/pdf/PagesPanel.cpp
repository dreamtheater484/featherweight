// The Pages panel (F1, f1-pdf): see PagesPanel.h.
#include "pdf/PagesPanel.h"

#include <algorithm>
#include <cmath>

#include "Commands.h"
#include "pdf/PdfText.h"
#include "ui/Icons.h"
#include "ui/Menu.h"
#include "ui/Theme.h"

using namespace gfx;

namespace {

constexpr wchar_t kRotateLeftGlyph = 0xE80C;   // RotateMapLeft
constexpr wchar_t kRotateRightGlyph = 0xE80D;  // RotateMapRight
constexpr wchar_t kInsertGlyph = 0xE710;       // Add
constexpr wchar_t kExtractGlyph = 0xEDE1;      // Export
constexpr wchar_t kSplitGlyph = 0xE8C6;        // Cut

bool KeyDown(int vk) { return GetKeyState(vk) < 0; }

}  // namespace

std::wstring PdfView::PagesPanel::Title() const { return T(L"Pages"); }

int PdfView::PagesPanel::Count() const { return v_.Ready() ? v_.PageCount() : 0; }

std::vector<Control*> PdfView::PagesPanel::Layout(const D2D1_RECT_F& body, float ui) {
  body_ = body;
  ui_ = ui;
  const float u = ui, size = 32 * u, top = body.top + 6 * u;
  float x = body.left + 10 * u;
  for (int a = 0; a < kActionCount; ++a) {
    if (a == kInsert) x += 10 * u;  // the page's own actions, then the ones that bring or make files
    if (a == kUndo) x = body.right - 10 * u - 2 * size;
    actions_[a] = RectF(x, top, x + size, top + size);
    x += size;
  }
  ClampScroll();
  if (pendingScroll_ >= 0) {
    const int page = pendingScroll_;
    pendingScroll_ = -1;
    ScrollIntoView(page);
  }
  return {};
}

D2D1_RECT_F PdfView::PagesPanel::Grid() const {
  return RectF(body_.left + 12 * ui_, body_.top + 46 * ui_, body_.right - 12 * ui_, body_.bottom - 34 * ui_);
}

float PdfView::PagesPanel::CellW() const { return (Width(Grid()) - 12 * ui_) / Columns(); }
float PdfView::PagesPanel::CellH() const { return std::round(CellW() * 1.25f) + 22 * ui_; }

PdfView::PagesPanel::Cell PdfView::PagesPanel::CellOf(int page) const {
  const D2D1_RECT_F g = Grid();
  const int row = page / Columns(), col = page % Columns();
  const float x = g.left + col * (CellW() + 12 * ui_), y = g.top + row * (CellH() + 8 * ui_) - scroll_;
  Cell c;
  c.box = RectF(x + 6 * ui_, y + 6 * ui_, x + CellW() - 6 * ui_, y + CellH() - 24 * ui_);
  c.label = RectF(x, y + CellH() - 22 * ui_, x + CellW(), y + CellH() - 2 * ui_);
  return c;
}

int PdfView::PagesPanel::PageAt(float x, float y) const {
  const D2D1_RECT_F g = Grid();
  if (!Contains(g, x, y)) return -1;
  const int col = static_cast<int>((x - g.left) / (CellW() + 12 * ui_));
  const int row = static_cast<int>((y - g.top + scroll_) / (CellH() + 8 * ui_));
  if (col < 0 || col >= Columns() || row < 0) return -1;
  const int page = row * Columns() + col;
  return page < Count() ? page : -1;
}

int PdfView::PagesPanel::GapAt(float x, float y) const {
  const D2D1_RECT_F g = Grid();
  const int n = Count();
  if (n == 0) return 0;
  const float cw = CellW() + 12 * ui_, ch = CellH() + 8 * ui_;
  const int row = std::max(0, static_cast<int>(std::floor((y - g.top + scroll_) / ch)));
  const float fx = (x - g.left) / cw;  // 0..columns: half a cell either side of a gap
  const int col = std::clamp(static_cast<int>(std::lround(fx)), 0, Columns());
  return std::clamp(row * Columns() + col, 0, n);
}

void PdfView::PagesPanel::ClampScroll() {
  const int rows = (Count() + Columns() - 1) / Columns();
  const float content = rows * (CellH() + 8 * ui_), view = Height(Grid());
  scroll_ = std::clamp(scroll_, 0.0f, std::max(0.0f, content - view));
}

void PdfView::PagesPanel::ScrollIntoView(int page) {
  if (page < 0 || page >= Count()) return;
  const D2D1_RECT_F g = Grid();
  if (Height(g) <= 0) {  // not laid out yet: Layout shows it
    pendingScroll_ = page;
    return;
  }
  const Cell c = CellOf(page);
  const float top = c.box.top - 6 * ui_, bottom = c.label.bottom + 2 * ui_;
  if (top < g.top) scroll_ -= g.top - top;
  else if (bottom > g.bottom) scroll_ += bottom - g.bottom;
  ClampScroll();
}

std::vector<int> PdfView::PagesPanel::Selection() const {
  std::vector<int> out;
  for (int i = 0; i < static_cast<int>(selected_.size()) && i < Count(); ++i)
    if (selected_[i]) out.push_back(i);
  return out;
}

void PdfView::PagesPanel::Select(const std::vector<int>& pages, int focus) {
  selected_.assign(Count(), false);
  for (int p : pages)
    if (p >= 0 && p < Count()) selected_[p] = true;
  focus_ = anchor_ = std::clamp(focus, -1, Count() - 1);
  ScrollIntoView(focus_);
}

void PdfView::PagesPanel::Clamp() {
  selected_.resize(Count(), false);
  if (focus_ >= Count()) focus_ = Count() - 1;
  if (anchor_ >= Count()) anchor_ = Count() - 1;
  hover_ = -1;
  ClampScroll();
}

bool PdfView::PagesPanel::ActionEnabled(int a) const {
  if (a == kUndo) return v_.CanUndoPages();
  if (a == kRedo) return v_.CanRedoPages();
  if (!v_.Ready() || !v_.PageToolsBlocked().empty()) return false;
  const size_t chosen = v_.ToolPages().size();
  switch (a) {
    case kDelete: return chosen > 0 && static_cast<int>(chosen) < Count();
    case kSplit: return Count() > 1;
    case kInsert: return true;
    default: return chosen > 0;
  }
}

int PdfView::PagesPanel::ActionAt(float x, float y) const {
  for (int a = 0; a < kActionCount; ++a)
    if (Contains(actions_[a], x, y)) return a;
  return -1;
}

void PdfView::PagesPanel::RunAction(int a) {
  if (!ActionEnabled(a)) return;
  switch (a) {
    case kRotateLeft: v_.Execute(CmdPdfRotatePagesLeft); break;
    case kRotateRight: v_.Execute(CmdPdfRotatePagesRight); break;
    case kDelete: v_.Execute(CmdPdfDeletePages); break;
    case kInsert: v_.Execute(CmdPdfInsertPages); break;
    case kExtract: v_.Execute(CmdPdfExtractPages); break;
    case kUndo: v_.UndoPages(); break;
    case kRedo: v_.RedoPages(); break;
    case kSplit: {
      HMENU m = CreatePopupMenu();
      menu::AppendItem(m, CmdPdfSplitEvery1, T(L"Into single pages"));
      menu::AppendItem(m, CmdPdfSplitEvery1 + 1, T(L"Every 2 pages"));
      menu::AppendItem(m, CmdPdfSplitEvery1 + 2, T(L"Every 5 pages"));
      menu::AppendItem(m, CmdPdfSplitEvery1 + 3, T(L"Every 10 pages"));
      const std::vector<int> sel = Selection();
      const bool starts = std::any_of(sel.begin(), sel.end(), [](int p) { return p > 0; });
      menu::AppendItem(m, CmdPdfSplitSelected, T(L"Before each selected page"), starts);
      POINT pt{static_cast<LONG>(actions_[kSplit].left), static_cast<LONG>(actions_[kSplit].bottom)};
      HWND hwnd = v_.host_.ViewHwnd();
      ClientToScreen(hwnd, &pt);
      theme::ApplyToMenus();
      const int cmd = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_LEFTBUTTON, pt.x, pt.y, hwnd, nullptr);
      DestroyMenu(m);
      if (cmd) v_.Execute(cmd);
      break;
    }
  }
  v_.host_.ViewInvalidate();
}

void PdfView::PagesPanel::Draw(Canvas& c) {
  if (static_cast<int>(selected_.size()) != Count()) Clamp();
  const Palette& p = theme::P();
  const float u = ui_;
  // The actions along the top.
  const wchar_t glyphs[kActionCount] = {kRotateLeftGlyph, kRotateRightGlyph, icon::Delete, kInsertGlyph,
                                        kExtractGlyph,    kSplitGlyph,       icon::Undo,   icon::Redo};
  for (int a = 0; a < kActionCount; ++a) {
    const bool on = ActionEnabled(a);
    const D2D1_RECT_F r = Inflate(actions_[a], -2 * u, -2 * u);
    if (on && a == pressedAction_) c.FillRound(r, 4 * u, p.pressed);
    else if (on && a == hoverAction_) c.FillRound(r, 4 * u, p.hover);
    c.Icon(glyphs[a], actions_[a], on ? p.text : p.textDisabled, 16 * u);
  }
  const D2D1_RECT_F g = Grid();
  c.Line(body_.left, g.top - 4 * u, body_.right, g.top - 4 * u, p.chromeBorder, std::max(1.0f, u));

  // The thumbnails.
  const int n = Count();
  const std::vector<fw::PageSizePt>* sizes = v_.Pages();
  c.PushClip(g);
  const int current = v_.CurrentPage();
  for (int i = 0; i < n && sizes; ++i) {
    const Cell cell = CellOf(i);
    if (cell.label.bottom < g.top) continue;
    if (cell.box.top > g.bottom) break;
    // The page fitted in its box, as the tab shows it (with the view's rotation), on the label.
    float pw = (*sizes)[i].w, ph = (*sizes)[i].h;
    if (v_.ViewRotation() % 2) std::swap(pw, ph);
    const float k = std::min(Width(cell.box) / std::max(1.0f, pw), Height(cell.box) / std::max(1.0f, ph));
    const float w = std::round(pw * k), h = std::round(ph * k);
    const float cx = (cell.box.left + cell.box.right) / 2;
    const D2D1_RECT_F pic = RectF(std::round(cx - w / 2), std::round(cell.box.bottom - h), std::round(cx - w / 2) + w,
                                  cell.box.bottom);
    const D2D1_RECT_F whole = RectF(cell.box.left - 6 * u, pic.top - 6 * u, cell.box.right + 6 * u, cell.label.bottom);
    if (i < static_cast<int>(selected_.size()) && selected_[i]) c.FillRound(whole, 6 * u, p.selection);
    else if (i == hover_) c.FillRound(whole, 6 * u, p.hover);
    c.SoftShadow(pic, 1.0f * u, 3.0f * u, Alpha(p.pageShadow, 0.25f));
    if (ID2D1Bitmap* bmp = v_.Thumbnail(i, 3000 + std::abs(i - current))) {
      c.DC()->DrawBitmap(bmp, pic, 1.0f, D2D1_INTERPOLATION_MODE_LINEAR);
    } else {
      c.Fill(pic, Rgb(0xFFFFFF));
    }
    const bool sel = i < static_cast<int>(selected_.size()) && selected_[i];
    if (sel) c.StrokeRound(Inflate(pic, 2.0f * u, 2.0f * u), 3 * u, p.accent, 2 * u);
    else c.Stroke(pic, Alpha(p.chromeBorder, 0.8f), std::max(1.0f, u * 0.75f));
    c.Text(std::to_wstring(i + 1), cell.label, i == current ? p.accentText : p.textSecondary, 12 * u,
           i == current ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_REGULAR, Align::Center);
    if (focused_ && i == focus_) ctl::DrawFocusRing(c, whole, u);
  }
  // Where a drag drops the pages: a bar in the gap.
  if (dragging_ && gap_ >= 0 && n > 0) {
    // The gap at the start of a row is also the end of the row above: the bar goes where the pointer is.
    const bool after = gap_ == n || (gap_ % Columns() == 0 && gap_ > 0 && dragY_ < CellOf(gap_).box.top - 6 * u);
    const Cell cell = CellOf(after ? n - 1 : gap_);
    const float x = after ? cell.box.right + 9 * u : cell.box.left - 9 * u;
    c.FillRound(RectF(x - 1.5f * u, cell.box.top - 4 * u, x + 1.5f * u, cell.label.bottom), 1.5f * u, p.accent);
  }
  c.PopClip();

  // What's selected, or why the tools are off.
  const D2D1_RECT_F status = RectF(body_.left + 14 * u, g.bottom + 6 * u, body_.right - 14 * u, body_.bottom - 2 * u);
  const std::wstring blocked = v_.PageToolsBlocked();
  if (!blocked.empty()) {
    c.TextWrapped(blocked, status, p.textSecondary, 12 * u, 2);
  } else {
    const size_t chosen = Selection().size();
    const std::wstring line = chosen == 0 ? pdftext::Pages(n)
                                          : pdftext::Fill(T(L"{0} of {1} selected"),
                                                          {std::to_wstring(chosen), pdftext::Pages(n)});
    c.Text(line, status, p.textSecondary, 12 * u);
  }
}

void PdfView::PagesPanel::SetFocus(bool has, bool) {
  focused_ = has;
  if (has && focus_ < 0 && Count() > 0) focus_ = anchor_ = std::clamp(v_.CurrentPage(), 0, Count() - 1);
}

void PdfView::PagesPanel::Click(int page, bool ctrl, bool shift) {
  if (static_cast<int>(selected_.size()) != Count()) selected_.assign(Count(), false);
  if (shift && anchor_ >= 0) {
    if (!ctrl) std::fill(selected_.begin(), selected_.end(), false);
    for (int i = std::min(anchor_, page); i <= std::max(anchor_, page); ++i) selected_[i] = true;
  } else if (ctrl) {
    selected_[page] = !selected_[page];
    anchor_ = page;
  } else {
    std::fill(selected_.begin(), selected_.end(), false);
    selected_[page] = true;
    anchor_ = page;
  }
  focus_ = page;
}

bool PdfView::PagesPanel::OnMouseDown(float x, float y, bool) {
  const int a = ActionAt(x, y);
  if (a >= 0) {
    pressedAction_ = ActionEnabled(a) ? a : -1;
    return true;
  }
  const int page = PageAt(x, y);
  if (page < 0) return false;
  const bool ctrl = KeyDown(VK_CONTROL), shift = KeyDown(VK_SHIFT);
  if (static_cast<int>(selected_.size()) != Count()) selected_.assign(Count(), false);
  deferSelect_ = !ctrl && !shift && selected_[page];
  if (!deferSelect_) Click(page, ctrl, shift);
  focus_ = page;
  pressed_ = true;
  dragging_ = false;
  pressPage_ = page;
  pressX_ = x;
  pressY_ = y;
  if (!ctrl && !shift) v_.GoToPage(page);
  return true;
}

bool PdfView::PagesPanel::OnMouseMove(float x, float y) {
  if (pressed_) {
    if (!dragging_ && std::hypot(x - pressX_, y - pressY_) > 5 * ui_ && v_.PageToolsBlocked().empty()) {
      dragging_ = true;
      deferSelect_ = false;
    }
    if (!dragging_) return false;
    // Near the top or bottom the list scrolls, so a page can go anywhere.
    const D2D1_RECT_F g = Grid();
    if (y < g.top + 24 * ui_) scroll_ -= 14 * ui_;
    else if (y > g.bottom - 24 * ui_) scroll_ += 14 * ui_;
    ClampScroll();
    gap_ = GapAt(x, y);
    dragY_ = y;
    return true;
  }
  const int page = PageAt(x, y), action = ActionAt(x, y);
  if (page == hover_ && action == hoverAction_) return false;
  hover_ = page;
  hoverAction_ = action;
  return true;
}

void PdfView::PagesPanel::OnMouseUp(float x, float y) {
  if (pressedAction_ >= 0) {
    const int a = pressedAction_;
    pressedAction_ = -1;
    if (ActionAt(x, y) == a) RunAction(a);
    return;
  }
  if (!pressed_) return;
  pressed_ = false;
  if (dragging_) {
    dragging_ = false;
    const int gap = GapAt(x, y);
    gap_ = -1;
    v_.MovePages(Selection(), gap);
  } else if (deferSelect_ && pressPage_ >= 0 && pressPage_ < Count()) {
    Click(pressPage_, false, false);
  }
  deferSelect_ = false;
  v_.host_.ViewInvalidate();
}

void PdfView::PagesPanel::OnMouseLeave() {
  hover_ = hoverAction_ = -1;
}

bool PdfView::PagesPanel::OnWheel(int delta, float, float) {
  const float before = scroll_;
  scroll_ -= static_cast<float>(delta) / WHEEL_DELTA * (CellH() + 8 * ui_) * 0.75f;
  ClampScroll();
  return scroll_ != before;
}

bool PdfView::PagesPanel::OnKeyDown(UINT vk, bool ctrl, bool shift) {
  const int n = Count();
  if (n == 0) return false;
  if (ctrl && (vk == 'Z' || vk == 'Y')) {
    vk == 'Z' ? v_.UndoPages() : v_.RedoPages();
    return true;
  }
  if (ctrl && vk == 'A') {
    selected_.assign(n, true);
    return true;
  }
  if (vk == VK_DELETE) {
    RunAction(kDelete);
    return true;
  }
  if (vk == VK_RETURN || vk == VK_SPACE) {
    if (focus_ >= 0) {
      if (vk == VK_SPACE && ctrl) Click(focus_, true, false);
      v_.GoToPage(focus_);
    }
    return true;
  }
  int to = focus_ < 0 ? 0 : focus_;
  switch (vk) {
    case VK_LEFT: to -= 1; break;
    case VK_RIGHT: to += 1; break;
    case VK_UP: to -= Columns(); break;
    case VK_DOWN: to += Columns(); break;
    case VK_HOME: to = 0; break;
    case VK_END: to = n - 1; break;
    default: return false;
  }
  to = std::clamp(to, 0, n - 1);
  if (shift) Click(to, false, true);
  else if (!ctrl) Click(to, false, false);
  focus_ = to;
  if (!ctrl) v_.GoToPage(to);
  ScrollIntoView(to);
  return true;
}

std::wstring PdfView::PagesPanel::TooltipAt(float x, float y) {
  switch (ActionAt(x, y)) {
    case kRotateLeft: return T(L"Rotate counterclockwise");
    case kRotateRight: return T(L"Rotate clockwise");
    case kDelete: return T(L"Delete pages (Delete)");
    case kInsert: return T(L"Insert pages from a PDF…");
    case kExtract: return T(L"Extract pages as a new PDF…");
    case kSplit: return T(L"Split into several PDFs");
    case kUndo: return T(L"Undo (Ctrl+Z)");
    case kRedo: return T(L"Redo (Ctrl+Y)");
    default: break;
  }
  const int page = PageAt(x, y);
  return page >= 0 ? pdftext::Fill(T(L"Page {0}"), {std::to_wstring(page + 1)}) : std::wstring();
}

void PdfView::PagesPanel::Describe(uia::Tree&, int, const std::function<void()>&) {}
