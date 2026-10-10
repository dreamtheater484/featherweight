#include "Toolbar.h"

#include <algorithm>
#include <cmath>

#include "Icons.h"
#include "Theme.h"

using namespace gfx;

namespace {
constexpr float kBtn = 36;   // icon button width
constexpr float kBtnH = 32;  // icon button height
constexpr float kGap = 2;
constexpr float kSep = 13;   // width taken by a separator
}  // namespace

Button* Toolbar::Make(int cmd, wchar_t glyph, const wchar_t* tooltip) {
  Button* b = nullptr;
  switch (cmd) {
    case CmdOpen: b = &open_; break;
    case CmdSave: b = &save_; break;
    case CmdPrint: b = &print_; break;
    case CmdPrevPage: b = &prev_; break;
    case CmdNextPage: b = &next_; break;
    case CmdZoomOut: b = &zoomOut_; break;
    case CmdZoomMenu: b = &zoom_; break;
    case CmdZoomIn: b = &zoomIn_; break;
    case CmdViewMenu: b = &view_; break;
    case CmdRotateRight: b = &rotate_; break;
    case CmdFind: b = &search_; break;
    case CmdToggleInvert: b = &invert_; break;
    case CmdFullScreen: b = &full_; break;
    case CmdMoreMenu: b = &more_; break;
  }
  b->id = cmd;
  b->glyph = glyph;
  b->tooltip = tooltip;
  all_.push_back(b);
  set_.Add(b);
  return b;
}

Toolbar::Toolbar() {
  Make(CmdOpen, icon::FolderOpen, L"Open (Ctrl+O)");
  Make(CmdSave, icon::Save, L"Save (Ctrl+S)");
  Make(CmdPrint, icon::Print, L"Print (Ctrl+P)");
  Make(CmdPrevPage, icon::ChevronUp, L"Previous page (Page Up)");
  Make(CmdNextPage, icon::ChevronDown, L"Next page (Page Down)");
  Make(CmdZoomOut, icon::ZoomOut, L"Zoom out (Ctrl+−)");
  Make(CmdZoomMenu, 0, L"Zoom");
  Make(CmdZoomIn, icon::ZoomIn, L"Zoom in (Ctrl+=)");
  Make(CmdViewMenu, icon::ViewContinuous, L"Page layout");
  Make(CmdRotateRight, icon::Rotate, L"Rotate (Ctrl+Shift+=)");
  Make(CmdFind, icon::Search, L"Find (Ctrl+F)");
  Make(CmdToggleInvert, icon::QuietHours, L"Night reading: dark pages (Ctrl+I)");
  Make(CmdFullScreen, icon::FullScreen, L"Full screen (F11)");
  Make(CmdMoreMenu, icon::More, L"More");
  zoom_.dropdown = true;
  view_.dropdown = true;
  prev_.glyphSize = next_.glyphSize = 14;
  pageBox_.digitsOnly = true;
  pageBox_.maxLength = 6;
  pageBox_.align = Align::Center;
}

void Toolbar::Layout(const RECT& rc, float ui, const ToolbarState& s) {
  rc_ = rc;
  ui_ = ui;
  separators_.clear();
  const float u = ui;
  const float top = rc.top + std::round(((rc.bottom - rc.top) - kBtnH * u) / 2);
  const float h = kBtnH * u;
  auto place = [&](Button& b, float x, float w) {
    b.rc = RectF(std::round(x), top, std::round(x + w), top + h);
    b.visible = true;
  };
  for (Button* b : all_) b->visible = false;
  pageBox_.rc = {};

  // Left group.
  float x = rc.left + 8 * u;
  place(open_, x, kBtn * u);
  x += (kBtn + kGap) * u;
  if (s.ready) {
    place(save_, x, kBtn * u);
    x += (kBtn + kGap) * u;
    place(print_, x, kBtn * u);
    x += (kBtn + kGap) * u;
  }
  const float leftEnd = x;

  // Right group, dropping less important buttons when space is tight.
  struct Item {
    Button* b;
    float w;
    int priority;  // lower = dropped first
    bool sepBefore;
  };
  std::vector<Item> right;
  if (s.ready) {
    right.push_back({&view_, 52 * u, 1, false});
    right.push_back({&rotate_, kBtn * u, 0, false});
    right.push_back({&search_, kBtn * u, 5, true});
    right.push_back({&invert_, kBtn * u, 3, false});
    right.push_back({&full_, kBtn * u, 2, false});
  }
  right.push_back({&more_, kBtn * u, 9, false});

  // Center group width.
  pageLabelText_ = s.pageCount > 0 ? L"of " + std::to_wstring(s.pageCount) : L"";
  Canvas measure;
  float labelW = 0;
  if (!pageLabelText_.empty()) {
    IDWriteTextFormat* fmt = Canvas::Format(13 * u, DWRITE_FONT_WEIGHT_REGULAR, false);
    ComPtr<IDWriteTextLayout> layout;
    DWrite()->CreateTextLayout(pageLabelText_.c_str(), static_cast<UINT32>(pageLabelText_.size()), fmt, 1000, 100,
                               &layout);
    DWRITE_TEXT_METRICS m{};
    if (layout) layout->GetMetrics(&m);
    labelW = std::ceil(m.widthIncludingTrailingWhitespace);
  }
  const float pageBoxW = std::max(40.f, 10.f + 8.f * std::to_wstring(std::max(1, s.pageCount)).size()) * u;
  const float zoomW = 76 * u;
  float centerW = 0;
  if (s.ready) {
    centerW = kBtn * u + 4 * u + pageBoxW + 8 * u + labelW + 4 * u + kBtn * u  // pages
              + kSep * u                                                     // separator
              + kBtn * u + zoomW + kBtn * u + 2 * kGap * u;                  // zoom
  }

  auto rightWidth = [&]() {
    float w = 0;
    for (const auto& it : right) w += it.w + kGap * u + (it.sepBefore ? kSep * u : 0);
    return w;
  };
  float available = static_cast<float>(rc.right - rc.left) - (leftEnd - rc.left) - 8 * u;
  while (rightWidth() + centerW + 16 * u > available && right.size() > 1) {
    auto victim = std::min_element(right.begin(), right.end(),
                                   [](const Item& a, const Item& b) { return a.priority < b.priority; });
    right.erase(victim);
  }
  float rx = rc.right - 8 * u;
  for (auto it = right.rbegin(); it != right.rend(); ++it) {
    rx -= it->w;
    place(*it->b, rx, it->w);
    rx -= kGap * u;
    if (it->sepBefore) {
      separators_.push_back(RectF(rx - kSep * u / 2, top + 7 * u, rx - kSep * u / 2 + 1, top + h - 7 * u));
      rx -= kSep * u;
    }
  }
  const float rightStart = rx;

  if (s.ready) {
    float cx = std::round((rc.left + rc.right) / 2.0f - centerW / 2);
    cx = std::max(cx, leftEnd + 8 * u);
    if (cx + centerW > rightStart) cx = std::max(leftEnd + 4 * u, rightStart - centerW);
    place(prev_, cx, kBtn * u);
    cx += kBtn * u + 4 * u;
    float boxH = 28 * u;
    float by = std::round((rc.top + rc.bottom) / 2.0f - boxH / 2);
    pageBox_.rc = RectF(cx, by, cx + pageBoxW, by + boxH);
    cx += pageBoxW + 8 * u;
    pageLabel_ = RectF(cx, top, cx + labelW + 2, top + h);
    cx += labelW + 4 * u;
    place(next_, cx, kBtn * u);
    cx += kBtn * u;
    separators_.push_back(RectF(cx + kSep * u / 2, top + 7 * u, cx + kSep * u / 2 + 1, top + h - 7 * u));
    cx += kSep * u;
    place(zoomOut_, cx, kBtn * u);
    cx += kBtn * u + kGap * u;
    place(zoom_, cx, zoomW);
    cx += zoomW + kGap * u;
    place(zoomIn_, cx, kBtn * u);
  }

  // State.
  save_.enabled = s.modified;
  print_.enabled = s.ready && s.canPrint;
  print_.tooltip = s.ready && !s.canPrint ? L"This PDF doesn’t allow printing" : L"Print (Ctrl+P)";
  prev_.enabled = s.page > 0;
  next_.enabled = s.page + 1 < s.pageCount;
  zoom_.text = s.zoomText;
  invert_.checked = s.inverted;
  search_.checked = s.searchOpen;
  full_.glyph = s.fullscreen ? icon::BackToWindow : icon::FullScreen;
  full_.tooltip = s.fullscreen ? L"Exit full screen (F11)" : L"Full screen (F11)";
  view_.glyph = s.viewMode == fw::ViewMode::Single   ? icon::ViewSingle
                : s.viewMode == fw::ViewMode::Facing ? icon::ViewFacing
                                                     : icon::ViewContinuous;
  if (!pageBox_.focused) pageBox_.SetText(s.pageCount > 0 ? std::to_wstring(s.page + 1) : L"");
}

void Toolbar::Draw(Canvas& c, const ToolbarState& s, bool caretVisible) {
  const Palette& p = theme::P();
  D2D1_RECT_F r = RectF(rc_);
  c.Fill(r, p.chrome);
  c.Fill(RectF(r.left, r.bottom - 1, r.right, r.bottom), p.chromeBorder);
  for (Button* b : all_) DrawButton(c, *b, ui_);
  for (const auto& sep : separators_) c.Fill(sep, Alpha(p.textSecondary, 0.28f));
  if (s.ready && pageBox_.rc.right > 0) {
    pageBox_.Draw(c, ui_, caretVisible);
    c.Text(pageLabelText_, pageLabel_, p.textSecondary, 13 * ui_);
  }
}

bool Toolbar::OnMouseMove(int x, int y) { return set_.OnMouseMove((float)x, (float)y); }
void Toolbar::OnMouseLeave() { set_.OnMouseLeave(); }
bool Toolbar::OnMouseDown(int x, int y) { return set_.OnMouseDown((float)x, (float)y); }
int Toolbar::OnMouseUp(int x, int y) { return set_.OnMouseUp((float)x, (float)y); }

std::wstring Toolbar::TooltipAt(int x, int y) const {
  if (HitPageBox(x, y)) return L"Go to page (Ctrl+G)";
  for (Button* b : all_)
    if (b->visible && Contains(b->rc, (float)x, (float)y)) return b->tooltip;
  return {};
}

D2D1_RECT_F Toolbar::ButtonRect(int cmd) const {
  for (Button* b : all_)
    if (b->id == cmd) return b->rc;
  return {};
}
