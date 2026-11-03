#include "ui/Toolbar.h"

#include <algorithm>
#include <cmath>

#include "ui/Icons.h"
#include "ui/Theme.h"

using namespace gfx;

namespace {
constexpr float kBtn = 36;   // icon button width
constexpr float kBtnH = 32;  // icon button height
constexpr float kGap = 2;
constexpr float kSep = 13;   // width taken by a separator
}  // namespace

Button* Toolbar::Make(Button& b, int cmd, wchar_t glyph, const wchar_t* tooltip) {
  b.id = cmd;
  b.glyph = glyph;
  b.tooltip = tooltip;
  all_.push_back(&b);
  set_.Add(&b);
  return &b;
}

Button& Toolbar::ItemButton(const ToolItem& item) {
  auto [it, added] = items_.try_emplace(item.cmd);
  Button& b = it->second;
  if (added) {
    b.id = item.cmd;
    all_.push_back(&b);
    set_.Add(&b);
  }
  b.glyph = item.glyph;
  b.tooltip = item.tooltip;
  b.glyphSize = item.glyphSize;
  b.dropdown = item.dropdown;
  b.enabled = item.enabled;
  b.checked = item.checked;
  return b;
}

Toolbar::Toolbar() {
  Make(open_, CmdOpen, icon::FolderOpen, L"Open (Ctrl+O)");
  Make(prev_, CmdPrevPage, icon::ChevronUp, L"Previous page (Page Up)");
  Make(next_, CmdNextPage, icon::ChevronDown, L"Next page (Page Down)");
  Make(zoomOut_, CmdZoomOut, icon::ZoomOut, L"Zoom out (Ctrl+−)");
  Make(zoom_, CmdZoomMenu, 0, L"Zoom");
  Make(zoomIn_, CmdZoomIn, icon::ZoomIn, L"Zoom in (Ctrl+=)");
  Make(more_, CmdMoreMenu, icon::More, L"More");
  zoom_.dropdown = true;
  prev_.glyphSize = next_.glyphSize = 14;
  pageBox_.digitsOnly = true;
  pageBox_.maxLength = 6;
  pageBox_.align = Align::Center;
}

void Toolbar::Layout(const RECT& rc, float ui, const ToolbarModel& m) {
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

  // Left group: Open, then the tab's own buttons.
  float x = rc.left + 8 * u;
  place(open_, x, kBtn * u);
  x += (kBtn + kGap) * u;
  for (const ToolItem& item : m.left) {
    place(ItemButton(item), x, item.width * u);
    x += (item.width + (item.joinNext ? 0 : kGap)) * u;
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
  for (const ToolItem& item : m.right) right.push_back({&ItemButton(item), item.width * u, item.priority, item.sepBefore});
  right.push_back({&more_, kBtn * u, 9, false});

  // Center group width.
  pageLabelText_ = m.pageCount > 0 ? L"of " + std::to_wstring(m.pageCount) : L"";
  float labelW = 0;
  if (!pageLabelText_.empty()) {
    IDWriteTextFormat* fmt = Canvas::Format(13 * u, DWRITE_FONT_WEIGHT_REGULAR, false);
    ComPtr<IDWriteTextLayout> layout;
    DWrite()->CreateTextLayout(pageLabelText_.c_str(), static_cast<UINT32>(pageLabelText_.size()), fmt, 1000, 100,
                               &layout);
    DWRITE_TEXT_METRICS tm{};
    if (layout) layout->GetMetrics(&tm);
    labelW = std::ceil(tm.widthIncludingTrailingWhitespace);
  }
  const float pageBoxW = std::max(40.f, 10.f + 8.f * std::to_wstring(std::max(1, m.pageCount)).size()) * u;
  const float zoomW = 76 * u;
  float centerW = 0;
  if (m.pager) {
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

  if (m.pager) {
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

  // State of the center group (the tab's own buttons got theirs above).
  prev_.enabled = m.page > 0;
  next_.enabled = m.page + 1 < m.pageCount;
  zoom_.text = m.zoomText;
  if (!pageBox_.focused) pageBox_.SetText(m.pageCount > 0 ? std::to_wstring(m.page + 1) : L"");
}

void Toolbar::Draw(Canvas& c, const ToolbarModel& m, bool caretVisible) {
  const Palette& p = theme::P();
  D2D1_RECT_F r = RectF(rc_);
  c.Fill(r, p.chrome);
  c.Fill(RectF(r.left, r.bottom - 1, r.right, r.bottom), p.chromeBorder);
  for (Button* b : all_) DrawButton(c, *b, ui_);
  for (const auto& sep : separators_) c.Fill(sep, Alpha(p.textSecondary, 0.28f));
  if (m.pager && pageBox_.rc.right > 0) {
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
