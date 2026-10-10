#include "EmptyState.h"

#include <algorithm>
#include <cmath>

#include "Icons.h"
#include "Logo.h"
#include "Theme.h"
#include "Util.h"
#include "core/Strings.h"

using namespace gfx;

namespace {
constexpr float kColumnW = 480;
constexpr float kRowH = 52;
}  // namespace

EmptyState::EmptyState() {
  open_.id = 1;
  open_.text = L"Open PDF";
  open_.glyph = icon::FolderOpen;
  open_.style = ButtonStyle::Primary;
  open_.tooltip = L"Open a PDF (Ctrl+O)";
  set_.Add(&open_);
}

void EmptyState::Layout(const RECT& area, float ui, const std::vector<RecentFile>& recent) {
  area_ = area;
  ui_ = ui;
  const float u = ui;
  const float areaH = static_cast<float>(area.bottom - area.top);
  const float colW = std::min(kColumnW * u, static_cast<float>(area.right - area.left) - 48 * u);
  const float cx = (area.left + area.right) / 2.0f;
  const float left = std::round(cx - colW / 2);

  const float headH = 64 * u + 16 * u + 30 * u + 22 * u + 36 * u + 14 * u + 20 * u;
  int maxRows = static_cast<int>((areaH - headH - 140 * u) / (kRowH * u));
  int rows = std::clamp(maxRows, 0, std::min(static_cast<int>(recent.size()), 8));
  float listH = rows > 0 ? 48 * u + rows * kRowH * u : 0;
  float total = headH + listH;
  float y = std::round(std::max(area.top + 32 * u, area.top + (areaH - total) / 2 - 16 * u));

  logo_ = RectF(cx - 32 * u, y, cx + 32 * u, y + 64 * u);
  y += 64 * u + 16 * u;
  title_ = RectF(left, y, left + colW, y + 30 * u);
  y += 30 * u + 22 * u;
  float bw = 168 * u;
  open_.rc = RectF(std::round(cx - bw / 2), y, std::round(cx + bw / 2), y + 36 * u);
  y += 36 * u + 14 * u;
  hint_ = RectF(left, y, left + colW, y + 20 * u);
  y += 20 * u + 40 * u;
  listHeader_ = RectF(left, y, left + colW, y + 20 * u);
  y += 28 * u;
  rows_.clear();
  for (int i = 0; i < rows; ++i) {
    rows_.push_back(RectF(left, y, left + colW, y + kRowH * u));
    y += kRowH * u;
  }
  if (rowHover_.size() != rows_.size()) rowHover_.assign(rows_.size(), Anim());
}

void EmptyState::Draw(Canvas& c, const std::vector<RecentFile>& recent) {
  const Palette& p = theme::P();
  const float u = ui_;
  logo::Draw(c, logo_.left, logo_.top, Width(logo_));
  c.Text(L"Featherweight", title_, p.text, 24 * u, DWRITE_FONT_WEIGHT_SEMI_BOLD, Align::Center);
  DrawButton(c, open_, u);
  c.Text(L"or drop a PDF anywhere in this window", hint_, p.textSecondary, 13 * u, DWRITE_FONT_WEIGHT_REGULAR,
         Align::Center);
  if (rows_.empty()) return;

  c.Text(L"Recent", listHeader_, p.textSecondary, 13 * u, DWRITE_FONT_WEIGHT_SEMI_BOLD);
  const int64_t now = util::UnixNow();
  for (size_t i = 0; i < rows_.size() && i < recent.size(); ++i) {
    const D2D1_RECT_F& r = rows_[i];
    float hv = rowHover_[i].V();
    c.FillRound(r, 6 * u, Alpha(p.hover, hv * 1.2f));
    float cy = (r.top + r.bottom) / 2;
    // Document glyph in a soft accent circle.
    c.FillRound(RectF(r.left + 10 * u, cy - 16 * u, r.left + 42 * u, cy + 16 * u), 6 * u, Alpha(p.accent, 0.12f));
    c.Icon(icon::Document, RectF(r.left + 10 * u, cy - 16 * u, r.left + 42 * u, cy + 16 * u), p.accentText, 16 * u);

    const RecentFile& f = recent[i];
    std::wstring when = fw::RelativeTime(now - f.lastOpened);
    float whenW = c.MeasureText(when, 12 * u) + 2;
    float right = r.right - 12 * u;
    bool showRemove = static_cast<int>(i) == hoverRow_;
    if (showRemove) {
      D2D1_RECT_F xr = RectF(right - 28 * u, cy - 14 * u, right, cy + 14 * u);
      if (hoverRemove_) c.FillRound(xr, 4 * u, p.hover);
      c.Icon(icon::Cancel, xr, p.textSecondary, 10 * u);
      right -= 36 * u;
    } else {
      c.Text(when, RectF(right - whenW, r.top, right, r.bottom), p.textSecondary, 12 * u, DWRITE_FONT_WEIGHT_REGULAR,
             Align::Trailing);
      right -= whenW + 16 * u;
    }
    float tx = r.left + 54 * u;
    c.Text(fw::FileNameOf(f.path), RectF(tx, r.top + 7 * u, right, cy + 2 * u), p.text, 13.5f * u,
           DWRITE_FONT_WEIGHT_SEMI_BOLD);
    c.Text(fw::DirOf(f.path), RectF(tx, cy + 1 * u, right, r.bottom - 6 * u), p.textSecondary, 12 * u);
  }
}

int EmptyState::RowAt(int x, int y) const {
  for (size_t i = 0; i < rows_.size(); ++i)
    if (Contains(rows_[i], (float)x, (float)y)) return static_cast<int>(i);
  return -1;
}

bool EmptyState::OverRemove(int row, int x, int y) const {
  if (row < 0) return false;
  const D2D1_RECT_F& r = rows_[row];
  float cy = (r.top + r.bottom) / 2;
  float right = r.right - 12 * ui_;
  return Contains(RectF(right - 28 * ui_, cy - 14 * ui_, right, cy + 14 * ui_), (float)x, (float)y);
}

bool EmptyState::OnMouseMove(int x, int y) {
  bool changed = set_.OnMouseMove((float)x, (float)y);
  int row = RowAt(x, y);
  bool rem = OverRemove(row, x, y);
  if (row != hoverRow_) {
    if (hoverRow_ >= 0 && hoverRow_ < static_cast<int>(rowHover_.size())) rowHover_[hoverRow_].To(0, 150);
    if (row >= 0) rowHover_[row].To(1, 80);
    hoverRow_ = row;
    changed = true;
  }
  if (rem != hoverRemove_) {
    hoverRemove_ = rem;
    changed = true;
  }
  return changed;
}

void EmptyState::OnMouseLeave() {
  set_.OnMouseLeave();
  if (hoverRow_ >= 0 && hoverRow_ < static_cast<int>(rowHover_.size())) rowHover_[hoverRow_].To(0, 150);
  hoverRow_ = -1;
  hoverRemove_ = false;
}

void EmptyState::OnMouseDown(int x, int y) {
  set_.OnMouseDown((float)x, (float)y);
  pressedRow_ = RowAt(x, y);
  pressedRemove_ = OverRemove(pressedRow_, x, y);
}

EmptyState::Result EmptyState::OnMouseUp(int x, int y) {
  Result r;
  if (set_.OnMouseUp((float)x, (float)y) == 1) {
    r.kind = Result::Kind::Open;
    return r;
  }
  int row = RowAt(x, y);
  if (row >= 0 && row == pressedRow_) {
    bool rem = OverRemove(row, x, y);
    if (rem && pressedRemove_) r.kind = Result::Kind::RemoveRecent;
    else if (!rem && !pressedRemove_) r.kind = Result::Kind::OpenRecent;
    r.index = row;
  }
  pressedRow_ = -1;
  return r;
}

bool EmptyState::OverClickable(int x, int y) const {
  return open_.Hit((float)x, (float)y) || RowAt(x, y) >= 0;
}

std::wstring EmptyState::TooltipAt(int x, int y, const std::vector<RecentFile>& recent) const {
  if (open_.Hit((float)x, (float)y)) return open_.tooltip;
  int row = RowAt(x, y);
  if (row >= 0 && OverRemove(row, x, y)) return L"Remove from list";
  if (row >= 0 && row < static_cast<int>(recent.size())) return recent[row].path;
  return {};
}
