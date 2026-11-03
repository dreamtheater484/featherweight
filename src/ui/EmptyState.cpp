#include "ui/EmptyState.h"

#include <algorithm>
#include <cmath>
#include <cwctype>

#include "Util.h"
#include "core/Strings.h"
#include "ui/Controls.h"
#include "ui/Icons.h"
#include "ui/Logo.h"
#include "ui/Theme.h"

using namespace gfx;

namespace {

constexpr float kColumnW = 560;
constexpr float kRowH = 40;
constexpr int kMaxRows = 8;
constexpr wchar_t kHint[] = L"or drop a file anywhere";

wchar_t KindGlyph(fw::FileKind kind) {
  switch (kind) {
    case fw::FileKind::Pdf: return icon::PDF;
    case fw::FileKind::Document: return icon::TextDocument;
    case fw::FileKind::Workbook: return icon::Table;
    case fw::FileKind::Presentation: return icon::Slideshow;
    default: return icon::Document;
  }
}

// The user's profile folder: recent files inside it show their folder without it.
const std::wstring& Home() {
  static const std::wstring home = [] {
    wchar_t buf[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"USERPROFILE", buf, MAX_PATH);
    return n > 0 && n < MAX_PATH ? std::wstring(buf, n) : std::wstring();
  }();
  return home;
}

}  // namespace

EmptyState::EmptyState() {
  open_.id = 1;
  open_.text = L"Open…";
  open_.glyph = icon::FolderOpen;
  open_.style = ButtonStyle::Primary;
  open_.tooltip = L"Open a file (Ctrl+O)";
  set_.Add(&open_);
  for (const fw::FileKindInfo& k : fw::FileKinds()) {
    Button& b = tiles_[static_cast<int>(k.kind)];
    b.id = 10 + static_cast<int>(k.kind);
    b.text = k.noun;
    b.text[0] = static_cast<wchar_t>(towupper(b.text[0]));
    b.glyph = KindGlyph(k.kind);
    b.style = ButtonStyle::Standard;
    b.tooltip = L"New " + std::wstring(k.noun);
    b.visible = false;
    set_.Add(&b);
  }
}

std::vector<int> EmptyState::Tiles() const {
  std::vector<int> kinds;
  for (int k = 0; k < fw::kFileKindCount; ++k)
    if (tiles_[k].visible) kinds.push_back(k);
  return kinds;
}

void EmptyState::Layout(const RECT& area, float ui, const std::vector<RecentFile>& recent) {
  area_ = area;
  ui_ = ui;
  const float u = ui;
  const float areaH = static_cast<float>(area.bottom - area.top);
  const float colW = std::min(kColumnW * u, static_cast<float>(area.right - area.left) - 48 * u);
  const float cx = (area.left + area.right) / 2.0f;
  const float left = std::round(cx - colW / 2);
  Canvas measure;

  for (const fw::FileKindInfo& k : fw::FileKinds())
    tiles_[static_cast<int>(k.kind)].visible = fw::EditorShipped(k.kind);
  const std::vector<int> tiles = Tiles();

  const float openW = std::round(measure.MeasureText(open_.text, 13 * u, DWRITE_FONT_WEIGHT_SEMI_BOLD) + 72 * u);
  const float hintW = std::ceil(measure.MeasureText(kHint, 13 * u)) + 2;
  hintBeside_ = openW + 16 * u + hintW <= colW;
  const float openRowH = hintBeside_ ? 36 * u : 66 * u;
  const float headH = 40 * u + 28 * u + openRowH + (tiles.empty() ? 0 : 32 * u + 40 * u);
  const float listChrome = 32 * u + 26 * u;  // the gap and the "Recent" header
  int maxRows = static_cast<int>((areaH - headH - listChrome - 48 * u) / (kRowH * u));
  int rows = std::clamp(maxRows, 0, std::min(static_cast<int>(recent.size()), kMaxRows));
  float total = headH + (rows > 0 ? listChrome + rows * kRowH * u : 0);
  float y = std::round(std::max(area.top + 32 * u, area.top + (areaH - total) / 2 - 16 * u));

  // The logo and the name, side by side.
  const float logoS = 40 * u;
  const float titleW = std::ceil(measure.MeasureText(L"Featherweight", 24 * u, DWRITE_FONT_WEIGHT_SEMI_BOLD)) + 2;
  float x = std::round(cx - (logoS + 12 * u + titleW) / 2);
  logo_ = RectF(x, y, x + logoS, y + logoS);
  title_ = RectF(x + logoS + 12 * u, y, x + logoS + 12 * u + titleW, y + logoS);
  y += logoS + 28 * u;

  if (hintBeside_) {
    x = std::round(cx - (openW + 16 * u + hintW) / 2);
    open_.rc = RectF(x, y, x + openW, y + 36 * u);
    hint_ = RectF(x + openW + 16 * u, y, x + openW + 16 * u + hintW, y + 36 * u);
  } else {
    open_.rc = RectF(std::round(cx - openW / 2), y, std::round(cx - openW / 2) + openW, y + 36 * u);
    hint_ = RectF(left, y + 46 * u, left + colW, y + 66 * u);
  }
  y += openRowH;

  if (!tiles.empty()) {
    y += 32 * u;
    const float labelW = 64 * u, gap = 8 * u;
    const int n = static_cast<int>(tiles.size());
    newLabel_ = RectF(left, y, left + labelW, y + 40 * u);
    float natural = 132 * u;
    for (int k : tiles) natural = std::max(natural, measure.MeasureText(tiles_[k].text, 13 * u) + 64 * u);
    const float w = std::floor(std::min(natural, (colW - labelW - gap * (n - 1)) / n));
    x = left + labelW;
    for (int k : tiles) {
      tiles_[k].rc = RectF(x, y, x + w, y + 40 * u);
      x += w + gap;
    }
    y += 40 * u;
  }

  listHeader_ = RectF(left, y + 32 * u, left + colW, y + 52 * u);
  y += listChrome;
  rows_.clear();
  paths_.clear();
  for (int i = 0; i < rows; ++i) {
    rows_.push_back(RectF(left, y, left + colW, y + kRowH * u));
    paths_.push_back(recent[i].path);
    y += kRowH * u;
  }
  if (rowHover_.size() != rows_.size()) rowHover_.assign(rows_.size(), Anim());

  // The keyboard stays on the same file when the list changes, or on the row where a removed one was.
  for (int i = 0; i < rows; ++i)
    if (!selectedPath_.empty() && fw::EqualsNoCase(paths_[i], selectedPath_)) selected_ = i;
  selected_ = std::min(selected_, rows - 1);
  selectedPath_ = selected_ >= 0 ? paths_[selected_] : L"";
  const bool tileGone = focus_ == Focus::New && (focusTile_ < 0 || !tiles_[focusTile_].visible);
  if ((focus_ == Focus::List && rows == 0) || tileGone) focus_ = Focus::Open;
}

void EmptyState::Draw(Canvas& c, const std::vector<RecentFile>& recent) {
  const Palette& p = theme::P();
  const float u = ui_;
  logo::Draw(c, logo_.left, logo_.top, Width(logo_));
  c.Text(L"Featherweight", title_, p.text, 24 * u, DWRITE_FONT_WEIGHT_SEMI_BOLD);
  DrawButton(c, open_, u);
  if (focus_ == Focus::Open && focusVisible_) ctl::DrawFocusRing(c, open_.rc, u);
  c.Text(kHint, hint_, p.textSecondary, 13 * u, DWRITE_FONT_WEIGHT_REGULAR,
         hintBeside_ ? Align::Leading : Align::Center);

  const std::vector<int> tiles = Tiles();
  if (!tiles.empty()) {
    c.Text(L"New", newLabel_, p.textSecondary, 13 * u, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    for (int k : tiles) {
      DrawButton(c, tiles_[k], u);
      if (focus_ == Focus::New && focusTile_ == k && focusVisible_) ctl::DrawFocusRing(c, tiles_[k].rc, u);
    }
  }
  if (rows_.empty()) return;

  c.Text(L"Recent", listHeader_, p.textSecondary, 13 * u, DWRITE_FONT_WEIGHT_SEMI_BOLD);
  const int64_t now = util::UnixNow();
  const int n = static_cast<int>(std::min(rows_.size(), recent.size()));
  // The times line up in a column as wide as the widest.
  std::vector<std::wstring> when(n);
  float whenW = 28 * u;
  for (int i = 0; i < n; ++i) {
    when[i] = fw::RelativeTime(now - recent[i].lastOpened);
    whenW = std::max(whenW, c.MeasureText(when[i], 12 * u));
  }
  whenW = std::ceil(whenW) + 2;
  const bool listFocus = focus_ == Focus::List && focusVisible_;
  for (int i = 0; i < n; ++i) {
    const D2D1_RECT_F& r = rows_[i];
    const RecentFile& f = recent[i];
    const float cy = (r.top + r.bottom) / 2;
    const bool selected = listFocus && i == selected_;
    if (selected) c.FillRound(r, 6 * u, Alpha(p.hover, 1.3f));
    c.FillRound(r, 6 * u, Alpha(p.hover, rowHover_[i].V() * 1.2f));
    // The kind's glyph in a soft accent square.
    D2D1_RECT_F box = RectF(r.left + 10 * u, cy - 14 * u, r.left + 38 * u, cy + 14 * u);
    c.FillRound(box, 6 * u, Alpha(p.accent, 0.12f));
    c.Icon(KindGlyph(f.Kind()), box, p.accentText, 15 * u);

    const float right = r.right - 12 * u;
    if (i == hoverRow_) {
      D2D1_RECT_F xr = RemoveRect(i);
      if (hoverRemove_) c.FillRound(xr, 4 * u, p.hover);
      c.Icon(icon::Cancel, xr, p.textSecondary, 10 * u);
    } else {
      c.Text(when[i], RectF(right - whenW, r.top, right, r.bottom), p.textSecondary, 12 * u, DWRITE_FONT_WEIGHT_REGULAR,
             Align::Trailing);
    }
    const float tx = r.left + 50 * u, textRight = right - whenW - 16 * u;
    const float nameW = std::floor((textRight - tx) * 0.55f);
    c.Text(fw::FileNameOf(f.path), RectF(tx, r.top, tx + nameW, r.bottom), p.text, 13.5f * u,
           DWRITE_FONT_WEIGHT_SEMI_BOLD);
    // The folder, shortened: inside the profile without it, then with middle folders left out.
    const float fx = tx + nameW + 16 * u;
    const std::wstring full = fw::ShortFolder(fw::DirOf(f.path), Home());
    std::wstring folder = full;
    for (int drop = 1; c.MeasureText(folder, 12.5f * u) > textRight - fx; ++drop) {
      std::wstring shorter = fw::ElideFolder(full, drop);
      if (shorter == folder) break;
      folder = shorter;
    }
    c.Text(folder, RectF(fx, r.top, textRight, r.bottom), p.textSecondary, 12.5f * u);
    if (selected) ctl::DrawFocusRing(c, Inflate(r, -3 * u, -3 * u), u);
  }
}

int EmptyState::RowAt(int x, int y) const {
  for (size_t i = 0; i < rows_.size(); ++i)
    if (Contains(rows_[i], (float)x, (float)y)) return static_cast<int>(i);
  return -1;
}

D2D1_RECT_F EmptyState::RemoveRect(int row) const {
  const D2D1_RECT_F& r = rows_[row];
  float cy = (r.top + r.bottom) / 2, right = r.right - 12 * ui_;
  return RectF(right - 28 * ui_, cy - 14 * ui_, right, cy + 14 * ui_);
}

bool EmptyState::OverRemove(int row, int x, int y) const {
  return row >= 0 && Contains(RemoveRect(row), (float)x, (float)y);
}

int EmptyState::TileAt(int x, int y) const {
  for (int k : Tiles())
    if (tiles_[k].Hit((float)x, (float)y)) return k;
  return -1;
}

void EmptyState::FocusOn(Focus f, int tile) {
  focus_ = f;
  focusVisible_ = true;
  if (f == Focus::New) focusTile_ = tile;
  if (f == Focus::List && selected_ < 0 && !rows_.empty()) Select(0);
}

void EmptyState::Select(int row) {
  if (row < 0 || row >= static_cast<int>(paths_.size())) return;
  selected_ = row;
  selectedPath_ = paths_[row];
}

void EmptyState::MoveFocus(bool forward) {
  // Tab order: Open…, the New tiles, the list. It goes round.
  struct Stop {
    Focus focus;
    int tile;
  };
  std::vector<Stop> stops{{Focus::Open, -1}};
  for (int k : Tiles()) stops.push_back({Focus::New, k});
  if (!rows_.empty()) stops.push_back({Focus::List, -1});
  const int n = static_cast<int>(stops.size());
  int at = -1;
  for (int i = 0; i < n; ++i)
    if (stops[i].focus == focus_ && (focus_ != Focus::New || stops[i].tile == focusTile_)) at = i;
  int next = at < 0 ? (forward ? 0 : n - 1) : (at + (forward ? 1 : n - 1)) % n;
  FocusOn(stops[next].focus, stops[next].tile);
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
  // A click moves the keyboard to what was clicked, and hides the focus ring until a key is pressed.
  if (open_.Hit((float)x, (float)y)) FocusOn(Focus::Open, -1);
  else if (int tile = TileAt(x, y); tile >= 0) FocusOn(Focus::New, tile);
  else if (pressedRow_ >= 0 && !pressedRemove_) {
    Select(pressedRow_);
    FocusOn(Focus::List, -1);
  }
  focusVisible_ = false;
}

EmptyState::Result EmptyState::OnMouseUp(int x, int y) {
  Result r;
  int id = set_.OnMouseUp((float)x, (float)y);
  if (id == 1) {
    r.kind = Result::Kind::Open;
    return r;
  }
  if (id >= 10 && id < 10 + fw::kFileKindCount) {
    r.kind = Result::Kind::New;
    r.index = id - 10;
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

bool EmptyState::OnKeyDown(UINT vk, bool shift, Result& result) {
  const int rows = static_cast<int>(rows_.size());
  switch (vk) {
    case VK_TAB:
      MoveFocus(!shift);
      return true;
    case VK_RETURN:
    case VK_SPACE:
      if (focus_ == Focus::Open) result = {Result::Kind::Open, -1};
      else if (focus_ == Focus::New) result = {Result::Kind::New, focusTile_};
      else if (focus_ == Focus::List && selected_ >= 0 && vk == VK_RETURN)
        result = {Result::Kind::OpenRecent, selected_};
      else return false;
      focusVisible_ = true;
      return true;
    case VK_DELETE:
      if (focus_ != Focus::List || selected_ < 0) return false;
      result = {Result::Kind::RemoveRecent, selected_};
      focusVisible_ = true;
      return true;
    case VK_UP:
    case VK_DOWN:
    case VK_HOME:
    case VK_END:
    case VK_PRIOR:
    case VK_NEXT: {
      if (focus_ != Focus::List || rows == 0) return false;
      int i = rows - 1;  // End, Page Down
      if (vk == VK_UP) i = selected_ - 1;
      else if (vk == VK_DOWN) i = selected_ + 1;
      else if (vk == VK_HOME || vk == VK_PRIOR) i = 0;
      Select(std::clamp(i, 0, rows - 1));
      focusVisible_ = true;
      return true;
    }
    case VK_LEFT:
    case VK_RIGHT: {
      if (focus_ != Focus::New) return false;
      const std::vector<int> tiles = Tiles();
      auto at = std::find(tiles.begin(), tiles.end(), focusTile_) - tiles.begin();
      at = std::clamp<ptrdiff_t>(at + (vk == VK_RIGHT ? 1 : -1), 0, static_cast<ptrdiff_t>(tiles.size()) - 1);
      FocusOn(Focus::New, tiles[at]);
      return true;
    }
    default: return false;
  }
}

bool EmptyState::OverClickable(int x, int y) const {
  return open_.Hit((float)x, (float)y) || TileAt(x, y) >= 0 || RowAt(x, y) >= 0;
}

std::wstring EmptyState::TooltipAt(int x, int y, const std::vector<RecentFile>& recent) const {
  if (open_.Hit((float)x, (float)y)) return open_.tooltip;
  if (int tile = TileAt(x, y); tile >= 0) return tiles_[tile].tooltip;
  int row = RowAt(x, y);
  if (row >= 0 && OverRemove(row, x, y)) return L"Remove from list";
  if (row >= 0 && row < static_cast<int>(recent.size())) return recent[row].path;
  return {};
}

void EmptyState::Describe(uia::Tree& t, int parent, const std::vector<RecentFile>& recent,
                          const std::function<void(Result)>& act) {
  int open = uia::AddButton(t, parent, 1, open_, [act] { act({Result::Kind::Open, -1}); });
  t[open].focusable = true;
  t[open].focused = focus_ == Focus::Open;
  t[open].focus = [this] { FocusOn(Focus::Open, -1); };

  const std::vector<int> tiles = Tiles();
  if (!tiles.empty()) {
    const D2D1_RECT_F &first = tiles_[tiles.front()].rc, &last = tiles_[tiles.back()].rc;
    int group =
        t.Add(parent, 3, UIA_GroupControlTypeId, L"New", RectF(newLabel_.left, first.top, last.right, last.bottom));
    for (int k : tiles) {
      int tile = uia::AddButton(t, group, k, tiles_[k], [act, k] { act({Result::Kind::New, k}); });
      t[tile].focusable = true;
      t[tile].focused = focus_ == Focus::New && focusTile_ == k;
      t[tile].focus = [this, k] { FocusOn(Focus::New, k); };
    }
  }

  const int rows = static_cast<int>(std::min(rows_.size(), recent.size()));
  if (rows == 0) return;
  D2D1_RECT_F all = RectF(rows_.front().left, rows_.front().top, rows_.front().right, rows_[rows - 1].bottom);
  int list = t.Add(parent, 2, UIA_ListControlTypeId, L"Recent", all);
  t[list].orientation = OrientationType_Vertical;
  t[list].selectionContainer = true;
  t[list].focusable = true;
  t[list].focus = [this] { FocusOn(Focus::List, -1); };
  const int64_t now = util::UnixNow();
  for (int i = 0; i < rows; ++i) {
    const RecentFile& f = recent[i];
    int item = t.Add(list, i, UIA_ListItemControlTypeId, fw::FileNameOf(f.path), rows_[i]);
    uia::Node& n = t[item];
    n.help = f.path;
    n.status = fw::RelativeTime(now - f.lastOpened);
    n.invoke = [act, i] { act({Result::Kind::OpenRecent, i}); };
    n.positionInSet = i + 1;
    n.sizeOfSet = rows;
    n.selectable = true;
    n.selected = i == selected_;
    n.focusable = true;
    n.focused = focus_ == Focus::List && i == selected_;
    n.select = n.focus = [this, i] {
      Select(i);
      FocusOn(Focus::List, -1);
    };
    int remove = t.Add(item, 1, UIA_ButtonControlTypeId, L"Remove from list", RemoveRect(i));
    t[remove].invoke = [act, i] { act({Result::Kind::RemoveRecent, i}); };
  }
}
