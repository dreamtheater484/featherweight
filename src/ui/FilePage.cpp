#include "ui/FilePage.h"

#include <algorithm>
#include <cmath>

#include "ui/Controls.h"
#include "ui/Icons.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"

using namespace gfx;

namespace ui {

namespace {
constexpr float kRailW = 200;
constexpr float kRailRow = 40;
constexpr float kRow = 40, kRowDetail = 54, kHeading = 34;
constexpr float kContentMaxW = 640;
constexpr wchar_t kTips[FilePage::kSectionCount] = {L'N', L'O', L'I', L'S', L'A', L'P', L'E', L'C', L'T'};
}  // namespace

const wchar_t* FilePage::SectionName(Section s) {
  static const wchar_t* const names[kSectionCount] = {L"New",   L"Open",   L"Info",  L"Save",   L"Save as",
                                                      L"Print", L"Export", L"Close", L"Options"};
  return names[s];
}

void FilePage::Open(Section s) {
  open_ = true;
  tips_ = false;
  section_ = s;
  scroll_ = 0;
  hot_ = pressed_ = -1;
  focus_ = -1;
}

void FilePage::Choose(Section s) {
  if (s == section_) return;
  section_ = s;
  scroll_ = 0;
}

void FilePage::Layout(const RECT& rc, float ui, Model m) {
  model_ = std::move(m);
  rc_ = rc;
  ui_ = ui;
  const float u = ui;
  const float top = static_cast<float>(rc.top), bottom = static_cast<float>(rc.bottom);
  rail_ = RectF(static_cast<float>(rc.left), top, rc.left + kRailW * u, bottom);
  content_ = RectF(rail_.right, top, static_cast<float>(rc.right), bottom);
  hots_.clear();
  Hot back;
  back.back = true;
  back.rc = RectF(rail_.left + 12 * u, top + 12 * u, rail_.left + 48 * u, top + 48 * u);
  hots_.push_back(back);
  float y = top + 60 * u;
  for (int s = 0; s < kSectionCount; ++s) {
    Hot h;
    h.section = s;
    if (s == kOptions) {
      h.rc = RectF(rail_.left, bottom - (kRailRow + 12) * u, rail_.right, bottom - 12 * u);
    } else {
      h.rc = RectF(rail_.left, y, rail_.right, y + kRailRow * u);
      y += kRailRow * u;
      if (s == kOpen || s == kClose) y += 12 * u;  // the groups of the rail, as Office's
    }
    hots_.push_back(h);
  }
  // The section's entries, under its title.
  const float left = content_.left + 40 * u;
  const float width = std::min(kContentMaxW * u, content_.right - left - 40 * u);
  float ey = top + (section_ == kInfo ? 140 : 92) * u - scroll_;
  const std::vector<Entry>& entries = model_.entries[section_];
  const float first = ey;
  for (int i = 0; i < static_cast<int>(entries.size()); ++i) {
    const Entry& e = entries[i];
    const float h = (!e.cmd ? kHeading : e.detail.empty() ? kRow : kRowDetail) * u;
    if (e.cmd) {
      Hot hot;
      hot.entry = i;
      hot.rc = RectF(left, ey, left + width, ey + h - 2 * u);
      hots_.push_back(hot);
    }
    ey += h;
  }
  contentH_ = ey - first;
  const float room = bottom - first - 24 * u + scroll_;
  if (scroll_ > 0 && contentH_ <= room) scroll_ = 0;
  if (focus_ >= static_cast<int>(hots_.size())) focus_ = -1;
  if (hot_ >= static_cast<int>(hots_.size())) hot_ = -1;
}

void FilePage::Draw(Canvas& c) {
  if (!open_) return;
  const Palette& p = theme::P();
  const float u = ui_;
  c.Fill(RectF(rc_), p.chrome);
  c.Fill(rail_, Mix(p.titleBar, p.accent, p.dark ? 0.10f : 0.07f));
  for (int i = 0; i < static_cast<int>(hots_.size()); ++i) {
    const Hot& h = hots_[i];
    const bool hot = i == hot_, focused = i == focus_;
    if (h.back) {
      if (hot) c.FillEllipse((h.rc.left + h.rc.right) / 2, (h.rc.top + h.rc.bottom) / 2, Width(h.rc) / 2, Height(h.rc) / 2,
                             i == pressed_ ? p.pressed : p.hover);
      c.Icon(0xE72B /* back */, h.rc, p.text, 16 * u);
      if (focused) ctl::DrawFocusRing(c, h.rc, u);
      continue;
    }
    if (h.section >= 0) {
      const Section s = static_cast<Section>(h.section);
      const bool enabled = model_.enabled[s];
      const bool selected = s == section_ && !model_.run[s];
      const D2D1_RECT_F r = RectF(h.rc.left + 6 * u, h.rc.top + 2 * u, h.rc.right - 6 * u, h.rc.bottom - 2 * u);
      if (selected) c.FillRound(r, 4 * u, p.hover);
      if (hot && enabled) c.FillRound(r, 4 * u, i == pressed_ ? p.pressed : p.hover);
      if (selected) {
        const float cy = (r.top + r.bottom) / 2;
        c.FillRound(RectF(r.left, cy - 9 * u, r.left + 3 * u, cy + 9 * u), 1.5f * u, p.accent);
      }
      c.Text(SectionName(s), RectF(r.left + 18 * u, r.top, r.right - 8 * u, r.bottom), enabled ? p.text : p.textDisabled, 14 * u,
             selected ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_REGULAR);
      if (focused) ctl::DrawFocusRing(c, r, u);
      if (tips_) {
        const std::wstring tip(1, kTips[s]);
        const D2D1_RECT_F b = RectF(r.left + 2 * u, r.bottom - 15 * u, r.left + 18 * u, r.bottom + 2 * u);
        c.FillRound(b, 3 * u, p.dark ? Rgb(0xE8E8E8) : Rgb(0x3A3A3A));
        c.Text(tip, b, p.dark ? Rgb(0x1A1A1A) : Rgb(0xFFFFFF), 11 * u, DWRITE_FONT_WEIGHT_SEMI_BOLD, Align::Center);
      }
    }
  }
  // The section.
  if (model_.run[section_]) return;
  const float left = content_.left + 40 * u;
  c.PushClip(content_);
  c.Text(SectionName(section_), RectF(left, content_.top + 26 * u - scroll_, content_.right - 20 * u, content_.top + 66 * u - scroll_),
         p.text, 28 * u, DWRITE_FONT_WEIGHT_SEMI_LIGHT);
  if (section_ == kInfo) {
    c.Text(model_.title, RectF(left, content_.top + 74 * u - scroll_, content_.right - 20 * u, content_.top + 96 * u - scroll_), p.text,
           15 * u, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    c.Text(model_.path, RectF(left, content_.top + 98 * u - scroll_, content_.right - 20 * u, content_.top + 118 * u - scroll_),
           p.textSecondary, 12 * u);
  }
  const std::vector<Entry>& entries = model_.entries[section_];
  float ey = content_.top + (section_ == kInfo ? 140 : 92) * u - scroll_;
  const float width = std::min(kContentMaxW * u, content_.right - left - 40 * u);
  for (int i = 0; i < static_cast<int>(entries.size()); ++i) {
    const Entry& e = entries[i];
    const float h = (!e.cmd ? kHeading : e.detail.empty() ? kRow : kRowDetail) * u;
    const D2D1_RECT_F r = RectF(left, ey, left + width, ey + h - 2 * u);
    ey += h;
    if (!e.cmd) {
      c.Text(e.label, RectF(r.left + 4 * u, r.top + 8 * u, r.right, r.bottom), p.textSecondary, 13 * u, DWRITE_FONT_WEIGHT_SEMI_BOLD);
      continue;
    }
    int hi = -1;
    for (int k = 0; k < static_cast<int>(hots_.size()); ++k)
      if (hots_[k].entry == i) hi = k;
    if (hi == hot_ && e.enabled) c.FillRound(r, 4 * u, hi == pressed_ ? p.pressed : p.hover);
    if (hi == focus_) ctl::DrawFocusRing(c, r, u);
    const D2D1_COLOR_F fg = e.enabled ? p.text : p.textDisabled;
    float x = r.left + 12 * u;
    if (e.toggle || e.radio) {
      const float s = 18 * u, cy = std::round((r.top + r.bottom) / 2);
      const D2D1_RECT_F box = RectF(x, cy - s / 2, x + s, cy + s / 2);
      if (e.radio) {
        c.FillEllipse(x + s / 2, cy, s / 2, s / 2, e.checked ? p.accent : p.inputBg);
        if (e.checked) c.FillEllipse(x + s / 2, cy, s / 5, s / 5, p.onAccent);
        else c.StrokeRound(box, s / 2, p.inputBorder, 1);
      } else {
        c.FillRound(box, 4 * u, e.checked ? p.accent : p.inputBg);
        if (e.checked) c.Icon(icon::CheckMark, box, p.onAccent, 12 * u);
        else c.StrokeRound(box, 4 * u, p.inputBorder, 1);
      }
      x += s + 14 * u;
    } else if (e.glyph) {
      DrawGlyph(c, e.glyph, RectF(x, r.top, x + 22 * u, r.bottom), fg, 20 * u);
      x += 36 * u;
    }
    if (e.detail.empty()) {
      c.Text(e.label, RectF(x, r.top, r.right - 8 * u, r.bottom), fg, 14 * u);
    } else {
      c.Text(e.label, RectF(x, r.top + 6 * u, r.right - 8 * u, r.top + 28 * u), fg, 14 * u);
      c.Text(e.detail, RectF(x, r.top + 27 * u, r.right - 8 * u, r.bottom - 4 * u), p.textSecondary, 12 * u);
    }
  }
  c.PopClip();
}

bool FilePage::Hit(int x, int y) const { return open_ && Contains(RectF(rc_), static_cast<float>(x), static_cast<float>(y)); }

int FilePage::HotAt(int x, int y) const {
  const float fx = static_cast<float>(x), fy = static_cast<float>(y);
  for (int i = 0; i < static_cast<int>(hots_.size()); ++i)
    if (Contains(hots_[i].rc, fx, fy) && (hots_[i].entry < 0 || Contains(content_, fx, fy))) return i;
  return -1;
}

bool FilePage::OnMouseMove(int x, int y) {
  const int h = HotAt(x, y);
  if (h == hot_) return false;
  hot_ = h;
  return true;
}

void FilePage::OnMouseDown(int x, int y) {
  tips_ = false;
  focus_ = -1;
  pressed_ = HotAt(x, y);
  // A section shows as the button goes down, as Office's rail does.
  if (pressed_ >= 0 && hots_[pressed_].section >= 0 && !model_.run[hots_[pressed_].section] && model_.enabled[hots_[pressed_].section])
    Choose(static_cast<Section>(hots_[pressed_].section));
}

void FilePage::OnMouseUp(int x, int y) {
  const int down = pressed_;
  pressed_ = -1;
  if (down >= 0 && HotAt(x, y) == down) Activate(down);
}

bool FilePage::OnWheel(int delta, int x, int y) {
  if (!Contains(content_, static_cast<float>(x), static_cast<float>(y))) return false;
  const float room = content_.bottom - content_.top - (section_ == kInfo ? 140 : 92) * ui_ - 24 * ui_;
  scroll_ = std::clamp(scroll_ - delta / 120.0f * 48 * ui_, 0.f, std::max(0.f, contentH_ - room));
  return true;
}

void FilePage::Activate(int i) {
  if (i < 0 || i >= static_cast<int>(hots_.size())) return;
  const Hot h = hots_[i];
  if (h.back) {
    if (onClose) onClose();
    return;
  }
  if (h.section >= 0) {
    const Section s = static_cast<Section>(h.section);
    if (!model_.enabled[s]) return;
    if (const int cmd = model_.run[s]) {
      if (onClose) onClose();
      if (onCommand) onCommand(cmd);
    } else {
      Choose(s);
    }
    return;
  }
  const Entry e = model_.entries[section_][h.entry];
  if (!e.enabled || !e.cmd) return;
  if (!e.toggle && !e.radio && onClose) onClose();
  if (onCommand) onCommand(e.cmd);
}

bool FilePage::OnKeyDown(UINT vk, bool shift) {
  if (!open_) return false;
  if (tips_) {
    tips_ = false;
    for (int s = 0; s < kSectionCount; ++s)
      if (vk == static_cast<UINT>(kTips[s])) {
        Activate(1 + s);
        return true;
      }
    if (vk != VK_ESCAPE) return true;
  }
  const int railFirst = 1, railLast = kSectionCount;  // hots_: back, the rail, the entries
  const int entryFirst = railLast + 1, entryLast = static_cast<int>(hots_.size()) - 1;
  auto inRail = [&](int i) { return i >= railFirst && i <= railLast; };
  switch (vk) {
    case VK_ESCAPE:
      if (onClose) onClose();
      return true;
    case VK_UP:
    case VK_DOWN: {
      const int step = vk == VK_UP ? -1 : 1;
      if (focus_ < 0) focus_ = railFirst + section_ - step;
      if (inRail(focus_) || focus_ == 0) {
        int f = focus_;
        do f = std::clamp(f + step, railFirst, railLast);
        while (f != railFirst && f != railLast && !model_.enabled[f - 1]);
        focus_ = f;
        if (!model_.run[focus_ - 1]) Choose(static_cast<Section>(focus_ - 1));
      } else if (entryLast >= entryFirst) {
        focus_ = std::clamp(focus_ + step, entryFirst, entryLast);
      }
      return true;
    }
    case VK_TAB:
      if (inRail(focus_) || focus_ <= 0) focus_ = entryLast >= entryFirst && !shift ? entryFirst : railFirst + section_;
      else focus_ = railFirst + section_;
      return true;
    case VK_RETURN:
    case VK_SPACE:
      Activate(focus_ < 0 ? railFirst + section_ : focus_);
      return true;
    default: return false;
  }
}

std::wstring FilePage::TooltipAt(int x, int y) const {
  const int i = HotAt(x, y);
  if (i < 0) return {};
  if (hots_[i].back) return L"Back (Esc)";
  if (hots_[i].entry >= 0) {
    const Entry& e = model_.entries[section_][hots_[i].entry];
    return e.detail.empty() ? L"" : e.detail;
  }
  return {};
}

void FilePage::Describe(uia::Tree& t, int parent) {
  if (!open_) return;
  int page = t.Add(parent, 30000, UIA_PaneControlTypeId, L"File", RectF(rc_));
  for (int i = 0; i < static_cast<int>(hots_.size()); ++i) {
    const Hot& h = hots_[i];
    const std::wstring name = h.back ? L"Back" : h.section >= 0 ? SectionName(static_cast<Section>(h.section))
                                                               : model_.entries[section_][h.entry].label;
    int n = t.Add(page, 30001 + i, UIA_ButtonControlTypeId, name, h.rc);
    t[n].invoke = [this, i] { Activate(i); };
  }
}

}  // namespace ui
