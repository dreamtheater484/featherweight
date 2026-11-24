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

// Sizes in DIPs at the page's scale (EmptyState::s_).
constexpr float kColumnW = 920;  // the content column: 920 px at 1366, about 1,070 on large screens
constexpr float kGutter = 24;    // at least this much beside it
constexpr float kRowH = 52;      // a recent file: its name, and its folder under it
constexpr float kCardH = 88;     // a New card
constexpr float kChipH = 34;     // a template
constexpr size_t kFilterFrom = 9;  // recent files from which the filter shows
constexpr wchar_t kSubtitle[] = L"Open a file, or drop one anywhere in this window";

// The page grows with the window: as designed up to 1366 × 700 DIPs, then by a fifth for each
// doubling of the width (and of the height, so a wide, low window doesn't overflow), to 1.3.
float Grow(float wDip, float hDip) {
  auto g = [](float v, float base) { return v > base ? 1 + 0.2f * std::log2(v / base) : 1.0f; };
  return std::clamp(std::min(g(wDip, 1366), g(hDip, 700)), 1.0f, 1.3f);
}

wchar_t KindGlyph(fw::FileKind kind) {
  switch (kind) {
    case fw::FileKind::Pdf: return icon::PDF;
    case fw::FileKind::Document: return icon::TextDocument;
    case fw::FileKind::Workbook: return icon::Table;
    case fw::FileKind::Presentation: return icon::Slideshow;
    default: return icon::Document;
  }
}

// A muted tint for each kind of file, on its icon only, so the list scans by type: Word's blue,
// Excel's green, PowerPoint's orange, a PDF's red.
D2D1_COLOR_F Tint(fw::FileKind kind) {
  const Palette& p = theme::P();
  if (p.highContrast) return p.text;
  switch (kind) {
    case fw::FileKind::Document: return p.dark ? Rgb(0x8AABDD) : Rgb(0x2E5A9C);
    case fw::FileKind::Workbook: return p.dark ? Rgb(0x76BB91) : Rgb(0x237347);
    case fw::FileKind::Presentation: return p.dark ? Rgb(0xE49C7D) : Rgb(0xB8502F);
    case fw::FileKind::Pdf: return p.dark ? Rgb(0xE58D86) : Rgb(0xB23A33);
    default: return p.textSecondary;
  }
}

// The cards' surface: a step lighter than the page, a hairline and a soft shadow.
struct Surface {
  D2D1_COLOR_F fill, hover, press, line, shadow;
};
Surface Surf() {
  const Palette& p = theme::P();
  if (p.highContrast) return {p.docBg, p.docBg, p.docBg, p.text, Rgb(0, 0)};
  if (p.dark) return {Rgb(0x2B2B2B), Rgb(0x333333), Rgb(0x292929), Rgb(0xFFFFFF, 0.07f), Rgb(0x000000, 0.55f)};
  return {Rgb(0xFFFFFF), Rgb(0xFAFAFB), Rgb(0xF2F2F4), Rgb(0x000000, 0.075f), Rgb(0x1C2433, 0.17f)};
}

// A card, raised a little more while hovered and less while pressed. `elevation` 1 for the cards,
// less for the templates.
void DrawCard(Canvas& c, const D2D1_RECT_F& r, float radius, float s, float elevation, float hover, float press) {
  const Surface su = Surf();
  const float lift = std::max(0.0f, hover - press);
  if (su.shadow.a > 0 && elevation > 0)
    c.SoftShadow(r, radius, (4 + 4 * lift) * s * elevation, Alpha(su.shadow, (0.75f + 0.35f * lift) * elevation),
                 (1 + 2 * lift) * s * elevation);
  c.FillRound(r, radius, Mix(Mix(su.fill, su.hover, hover), su.press, press));
  c.StrokeRound(Inflate(r, -0.5f, -0.5f), radius - 0.5f, su.line, 1);
}

// A shortcut as a small key cap: "Ctrl+N".
float KeysWidth(Canvas& c, std::wstring_view keys, float s) {
  return keys.empty() ? 0 : std::ceil(c.MeasureText(keys, 11.5f * s)) + 14 * s;
}
void DrawKeys(Canvas& c, std::wstring_view keys, float right, float cy, float s, D2D1_COLOR_F text, D2D1_COLOR_F line) {
  const float w = KeysWidth(c, keys, s);
  const D2D1_RECT_F r = RectF(std::round(right - w), std::round(cy - 11 * s), std::round(right), std::round(cy + 11 * s));
  c.StrokeRound(Inflate(r, -0.5f, -0.5f), 4 * s, line, 1);
  c.Text(keys, r, text, 11.5f * s, DWRITE_FONT_WEIGHT_REGULAR, Align::Center);
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

bool Shown(const D2D1_RECT_F& r) { return r.right > r.left; }

void Move(D2D1_RECT_F& r, float dy) {
  if (!Shown(r)) return;
  r.top += dy;
  r.bottom += dy;
}

}  // namespace

EmptyState::EmptyState() {
  filter_.placeholder = L"Filter recent files";
  filter_.maxLength = 120;
  filter_.trailingInset = 28;  // the magnifier
}

void EmptyState::BuildItems(const std::vector<std::wstring>& templates, bool showRecent, size_t recentCount) {
  std::wstring key;
  for (const fw::FileKindInfo& k : fw::FileKinds()) key += fw::EditorShipped(k.kind) ? L'1' : L'0';
  key += showRecent ? L"s" : L"h";
  key += recentCount > 0 ? L"r|" : L"-|";
  for (const std::wstring& t : templates) key += t + L"|";
  if (key == itemsKey_) return;
  itemsKey_ = key;

  // The keyboard stays on the same button when the list is made again.
  Part focusPart = Part::Open;
  int focusIndex = -1;
  if (focus_ == Focus::Item && focusItem_ >= 0 && focusItem_ < static_cast<int>(items_.size())) {
    focusPart = items_[focusItem_].part;
    focusIndex = items_[focusItem_].index;
  }

  std::vector<Item> items;
  auto add = [&](Part part, int index, fw::FileKind kind, wchar_t glyph, std::wstring text, std::wstring tip) -> Item& {
    Item& it = items.emplace_back();
    it.part = part;
    it.index = index;
    it.kind = kind;
    it.glyph = glyph;
    it.text = std::move(text);
    it.tip = std::move(tip);
    return it;
  };
  add(Part::Open, -1, fw::FileKind::Unknown, icon::FolderOpen, L"Open…", L"Open a file (Ctrl+O)").keys = L"Ctrl+O";
  bool anyEditor = false;
  for (const fw::FileKindInfo& k : fw::FileKinds()) {
    if (!fw::EditorShipped(k.kind)) continue;
    anyEditor = true;
    const bool doc = k.kind == fw::FileKind::Document;
    Item& it = add(Part::New, static_cast<int>(k.kind), k.kind, KindGlyph(k.kind), L"Blank " + std::wstring(k.noun),
                   L"New " + std::wstring(k.noun) + (doc ? L" (Ctrl+N)" : L""));
    it.sub = k.extensions.empty() ? L"" : k.extensions[0];
    if (doc) it.keys = L"Ctrl+N";
  }
  if (fw::EditorShipped(fw::FileKind::Document)) {
    const wchar_t glyphs[kTemplateCount] = {icon::Mail, icon::Contact, icon::Report};
    const wchar_t* tips[kTemplateCount] = {L"New letter, from a template", L"New CV, from a template",
                                           L"New report, from a template"};
    for (int i = 0; i < kTemplateCount; ++i)
      add(Part::Template, i, fw::FileKind::Document, glyphs[i], kTemplates[i], tips[i]);
  }
  for (size_t i = 0; i < templates.size(); ++i) {
    std::wstring name = fw::FileNameOf(templates[i]);
    name = name.substr(0, name.find_last_of(L'.'));
    const fw::FileKind kind = fw::KindOfPath(templates[i]);
    add(Part::UserTemplate, static_cast<int>(i), kind, KindGlyph(kind), name, templates[i]);
  }
  if (anyEditor)
    add(Part::More, -1, fw::FileKind::Unknown, icon::FolderOpen, L"More templates…",
        L"Open a template from your templates folder");
  if (!showRecent) add(Part::ShowHide, -1, fw::FileKind::Unknown, icon::View, L"Show recent files", L"Show recent files");
  else if (recentCount > 0) add(Part::ShowHide, -1, fw::FileKind::Unknown, icon::Hide, L"", L"Hide recent files");

  items_ = std::move(items);
  hoverItem_ = pressedItem_ = -1;
  if (focus_ == Focus::Item) {
    focusItem_ = 0;
    for (size_t i = 0; i < items_.size(); ++i)
      if (items_[i].part == focusPart && items_[i].index == focusIndex) focusItem_ = static_cast<int>(i);
  }
}

EmptyState::Result EmptyState::ItemResult(const Item& it) const {
  using K = Result::Kind;
  switch (it.part) {
    case Part::Open: return {K::Open};
    case Part::New: return {K::New, it.index};
    case Part::Template: return {K::Template, it.index};
    case Part::UserTemplate: return {K::UserTemplate, it.index};
    case Part::More: return {K::MoreTemplates};
    case Part::ShowHide: return {showRecent_ ? K::HideRecent : K::ShowRecent};
  }
  return {};
}

void EmptyState::Layout(const RECT& area, float ui, const std::vector<RecentFile>& recent,
                        const std::vector<std::wstring>& templates, bool showRecent) {
  area_ = area;
  ui_ = ui;
  showRecent_ = showRecent;
  const float areaW = static_cast<float>(area.right - area.left), areaH = static_cast<float>(area.bottom - area.top);
  const float s = s_ = ui * Grow(areaW / ui, areaH / ui);
  const float colW = std::floor(std::min(kColumnW * s, areaW - 2 * kGutter * s));
  const float left = std::round(area.left + (areaW - colW) / 2), right = left + colW;
  Canvas measure;
  BuildItems(templates, showRecent, recent.size());

  // Everything is placed from y = 0, then moved down to its place: a little above the middle, or
  // scrolled when the page is taller than the window.
  float y = 0;

  // The header: the logo and the name, and Open… on the right (under them in a narrow window).
  const float logoS = std::round(52 * s);
  logo_ = RectF(left, y, left + logoS, y + logoS);
  const float tx = left + logoS + 16 * s;
  const float titleW = std::ceil(measure.MeasureText(L"Featherweight", 28 * s, DWRITE_FONT_WEIGHT_SEMI_BOLD)) + 2;
  const float subW = std::ceil(measure.MeasureText(kSubtitle, 14 * s)) + 2;
  Item& open = items_[0];
  const float openW = std::round(measure.MeasureText(open.text, 14 * s, DWRITE_FONT_WEIGHT_SEMI_BOLD) +
                                 KeysWidth(measure, open.keys, s) + 64 * s);
  const float openH = std::round(40 * s);
  const bool beside = tx + std::max(titleW, subW) + 32 * s + openW <= right;
  title_ = RectF(tx, y, right, y + 32 * s);
  subtitle_ = RectF(tx, y + 32 * s, beside ? right - openW - 16 * s : right, y + logoS);
  if (beside) {
    const float oy = std::round(y + (logoS - openH) / 2);
    open.rc = RectF(right - openW, oy, right, oy + openH);
    y += logoS;
  } else {
    y += logoS + 20 * s;
    open.rc = RectF(left, y, left + openW, y + openH);
    y += openH;
  }

  std::vector<int> cards, chips;
  int showHide = -1;
  for (size_t i = 0; i < items_.size(); ++i) {
    const Part part = items_[i].part;
    if (part == Part::New) cards.push_back(static_cast<int>(i));
    else if (part == Part::Template || part == Part::UserTemplate || part == Part::More) chips.push_back(static_cast<int>(i));
    else if (part == Part::ShowHide) showHide = static_cast<int>(i);
  }

  // New: a card for each editor, side by side while each gets 220 DIPs.
  newLabel_ = templatesLabel_ = {};
  if (!cards.empty()) {
    y += 44 * s;
    newLabel_ = RectF(left, y, right, y + 24 * s);
    y += 24 * s + 12 * s;
    const float gap = 16 * s, h = std::round(kCardH * s);
    const int n = static_cast<int>(cards.size());
    int cols = n;
    while (cols > 1 && (colW - gap * (cols - 1)) / cols < 220 * s) --cols;
    const float w = (colW - gap * (cols - 1)) / cols;
    for (int i = 0; i < n; ++i) {
      const int col = i % cols, line = i / cols;
      const float x0 = std::round(left + col * (w + gap)), x1 = col == cols - 1 ? right : std::round(left + col * (w + gap) + w);
      const float top = y + line * (h + gap);
      items_[cards[i]].rc = RectF(x0, top, x1, top + h);
    }
    const int lines = (n + cols - 1) / cols;
    y += lines * h + (lines - 1) * gap;
  }

  // The templates, in a strip under the cards: two lines at most, then "More templates…".
  if (!chips.empty()) {
    y += cards.empty() ? 44 * s : 18 * s;
    const float labelW = std::ceil(measure.MeasureText(L"Templates", 13 * s, DWRITE_FONT_WEIGHT_SEMI_BOLD)) + 20 * s;
    const float h = std::round(kChipH * s), gap = 8 * s, x0 = left + labelW;
    templatesLabel_ = RectF(left, y, x0, y + h);
    auto chipW = [&](const Item& it) {
      return std::min(std::round(measure.MeasureText(it.text, 13.5f * s) + 50 * s), right - x0);
    };
    const bool more = items_[chips.back()].part == Part::More;
    const float moreW = more ? chipW(items_[chips.back()]) : 0;
    float x = x0;
    int line = 0;
    for (size_t i = 0; i < chips.size(); ++i) {
      Item& it = items_[chips[i]];
      const float w = chipW(it);
      const bool isMore = more && i + 1 == chips.size();
      // On the last line, room is kept for "More templates…".
      const float keep = line == 1 && !isMore && more ? moreW + gap : 0;
      if (x + w + keep > right && x > x0) {
        if (line == 1 && !isMore) {
          it.rc = {};  // left to "More templates…"
          continue;
        }
        if (line == 1) x = right - w;  // no room even so: "More templates…" goes last on the line
        else {
          ++line;
          x = x0;
        }
      }
      const float top = y + line * (h + gap);
      it.rc = RectF(std::round(x), top, std::round(x + w), top + h);
      x += w + gap;
    }
    y += (line + 1) * h + line * gap;
  }

  // Recent files: the header with the filter and the show/hide button, then the card.
  y += 44 * s;
  const float headH = std::round(32 * s);
  recentLabel_ = RectF(left, y, left + 200 * s, y + headH);
  float hx = right;
  if (showHide >= 0) {
    Item& sh = items_[showHide];
    const float w = sh.text.empty() ? headH : std::round(measure.MeasureText(sh.text, 13 * s) + 48 * s);
    sh.rc = RectF(hx - w, y, hx, y + headH);
    hx -= w + 8 * s;
  }
  filterShown_ = showRecent && (recent.size() >= kFilterFrom || !filter_.Text().empty());
  filter_.rc = {};
  if (filterShown_) {
    const float w = std::round(std::min(280 * s, colW * 0.45f));
    filter_.rc = RectF(hx - w, y, hx, y + headH);
  }
  y += headH + 12 * s;

  rows_.clear();
  paths_.clear();
  pinnedRows_ = 0;
  recentCard_ = note_ = {};
  noteText_.clear();
  float centerH = 0;  // the height the page is centred by
  if (!showRecent) {
    noteText_ = L"Recent files are hidden.";
    note_ = RectF(left, y - 4 * s, right, y + 20 * s);
    y += 20 * s;
  } else {
    // Pinned files first, then the others, each as recently opened; the filter keeps the files
    // whose name has its text.
    const std::wstring needle = fw::ToLower(fw::Trim(filter_.Text()));
    std::vector<int> order;
    for (bool pinned : {true, false})
      for (size_t i = 0; i < recent.size(); ++i)
        if (recent[i].pinned == pinned &&
            (needle.empty() || fw::ToLower(fw::FileNameOf(recent[i].path)).find(needle) != std::wstring::npos)) {
          order.push_back(static_cast<int>(i));
          if (pinned) ++pinnedRows_;
        }
    const float pad = 6 * s, rowH = std::round(kRowH * s);
    // While the filter has text, the page stays where the whole list put it, so nothing moves
    // under the typing.
    const size_t pinnedAll = std::count_if(recent.begin(), recent.end(), [](const RecentFile& f) { return f.pinned; });
    if (!recent.empty())
      centerH = y + 2 * pad + recent.size() * rowH + (pinnedAll > 0 && pinnedAll < recent.size() ? 9 * s : 0);
    if (order.empty()) {
      recentCard_ = RectF(left, y, right, y + std::round(64 * s));
      note_ = recentCard_;
      noteText_ = recent.empty() ? L"Files you open show up here."
                                 : L"No recent file’s name has “" + fw::Trim(filter_.Text()) + L"” in it.";
    } else {
      float ry = y + pad;
      for (size_t k = 0; k < order.size(); ++k) {
        if (static_cast<int>(k) == pinnedRows_ && k > 0) ry += 9 * s;  // a line between pinned and the rest
        rows_.push_back({order[k], RectF(left + pad, std::round(ry), right - pad, std::round(ry + rowH))});
        paths_.push_back(recent[order[k]].path);
        ry += rowH;
      }
      recentCard_ = RectF(left, y, right, std::round(ry + pad));
    }
    y = recentCard_.bottom;
  }
  contentH_ = y;
  centerH = std::max(centerH, contentH_);

  // Where it all goes. The keyboard's place is scrolled into view after a key.
  const float margin = 32 * s;
  float base;
  if (centerH + 2 * margin <= areaH) {
    scroll_ = scrollMax_ = 0;
    base = area.top + std::max(margin, std::floor((areaH - centerH) * 0.4f));
  } else {
    scrollMax_ = std::max(0.0f, contentH_ + 2 * margin - areaH);
    if (reveal_) {
      D2D1_RECT_F at{};
      if (focus_ == Focus::Item && focusItem_ >= 0 && focusItem_ < static_cast<int>(items_.size())) at = items_[focusItem_].rc;
      else if (focus_ == Focus::List && selected_ >= 0 && selected_ < static_cast<int>(rows_.size())) at = rows_[selected_].rc;
      else if (focus_ == Focus::Filter && filterShown_) at = filter_.rc;
      if (Shown(at)) {
        const float top = area.top + margin - scroll_ + at.top, bottom = area.top + margin - scroll_ + at.bottom;
        if (top < area.top + 12 * s) scroll_ -= area.top + 12 * s - top;
        else if (bottom > area.bottom - 12 * s) scroll_ += bottom - (area.bottom - 12 * s);
      }
    }
    scroll_ = std::clamp(scroll_, 0.0f, scrollMax_);
    base = area.top + margin - scroll_;
  }
  reveal_ = false;
  base = std::round(base);
  for (D2D1_RECT_F* r : {&logo_, &title_, &subtitle_, &newLabel_, &templatesLabel_, &recentLabel_, &recentCard_, &note_, &filter_.rc})
    Move(*r, base);
  for (Item& it : items_) Move(it.rc, base);
  for (Row& r : rows_) Move(r.rc, base);

  // The keyboard stays on the same file when the list changes, or on the row where a removed one was.
  const int rows = static_cast<int>(rows_.size());
  if (rowHover_.size() != rows_.size()) rowHover_.assign(rows_.size(), Anim());
  if (hoverRow_ >= rows) hoverRow_ = -1;
  for (int i = 0; i < rows; ++i)
    if (!selectedPath_.empty() && fw::EqualsNoCase(paths_[i], selectedPath_)) selected_ = i;
  selected_ = std::min(selected_, rows - 1);
  selectedPath_ = selected_ >= 0 ? paths_[selected_] : L"";
  if ((focus_ == Focus::List && rows == 0) || (focus_ == Focus::Filter && !filterShown_)) {
    focus_ = Focus::Item;
    focusItem_ = 0;
  }
  if (focus_ == Focus::Item && (focusItem_ < 0 || focusItem_ >= static_cast<int>(items_.size()) || !Shown(items_[focusItem_].rc)))
    focusItem_ = 0;
}

void EmptyState::DrawItem(Canvas& c, const Item& it, bool focused) const {
  const Palette& p = theme::P();
  const float s = s_;
  const D2D1_RECT_F& r = it.rc;
  if (!Shown(r) || r.bottom < area_.top || r.top > area_.bottom) return;
  const float cy = (r.top + r.bottom) / 2, hover = it.hover.V(), press = it.press.V();
  switch (it.part) {
    case Part::Open: {
      // The one accent on the page: the primary action.
      D2D1_COLOR_F fill = Mix(Mix(p.accent, p.accentHover, hover), p.accentPressed, press);
      c.FillRound(r, 6 * s, fill);
      DrawGlyph(c, it.glyph, RectF(r.left + 14 * s, r.top, r.left + 34 * s, r.bottom), p.onAccent, 16 * s);
      c.Text(it.text, RectF(r.left + 42 * s, r.top, r.right, r.bottom), p.onAccent, 14 * s, DWRITE_FONT_WEIGHT_SEMI_BOLD);
      DrawKeys(c, it.keys, r.right - 12 * s, cy, s, Alpha(p.onAccent, 0.85f), Alpha(p.onAccent, 0.45f));
      break;
    }
    case Part::New: {
      DrawCard(c, r, 8 * s, s, 1, hover, press);
      const D2D1_COLOR_F tint = Tint(it.kind);
      const D2D1_RECT_F box = RectF(r.left + 18 * s, std::round(cy - 24 * s), r.left + 66 * s, std::round(cy + 24 * s));
      c.FillRound(box, 10 * s, Alpha(tint, p.dark ? 0.16f : 0.10f));
      DrawGlyph(c, it.glyph, box, tint, 22 * s);
      const float tx = box.right + 16 * s, keysW = KeysWidth(c, it.keys, s);
      const float textRight = r.right - 16 * s - (keysW > 0 ? keysW + 8 * s : 0);
      c.Text(it.text, RectF(tx, cy - 22 * s, textRight, cy + 1 * s), p.text, 15.5f * s, DWRITE_FONT_WEIGHT_SEMI_BOLD);
      c.Text(it.sub, RectF(tx, cy + 1 * s, textRight, cy + 21 * s), p.textSecondary, 12.5f * s);
      if (keysW > 0) DrawKeys(c, it.keys, r.right - 16 * s, cy, s, p.textSecondary, Surf().line);
      break;
    }
    case Part::Template:
    case Part::UserTemplate: {
      DrawCard(c, r, 6 * s, s, 0.6f, hover, press);
      DrawGlyph(c, it.glyph, RectF(r.left + 10 * s, r.top, r.left + 30 * s, r.bottom), Tint(it.kind), 15 * s);
      c.Text(it.text, RectF(r.left + 36 * s, r.top, r.right - 12 * s, r.bottom), p.text, 13.5f * s);
      break;
    }
    case Part::More:
    case Part::ShowHide: {
      // Quiet buttons: only a hover.
      c.FillRound(r, 6 * s, Alpha(p.hover, hover * 1.2f + press));
      if (it.text.empty()) {
        c.Icon(it.glyph, r, p.textSecondary, 15 * s);
      } else {
        DrawGlyph(c, it.glyph, RectF(r.left + 10 * s, r.top, r.left + 30 * s, r.bottom), p.textSecondary, 15 * s);
        c.Text(it.text, RectF(r.left + 36 * s, r.top, r.right - 8 * s, r.bottom), p.text, 13.5f * s);
      }
      break;
    }
  }
  if (focused) ctl::DrawFocusRing(c, r, ui_);
}

void EmptyState::Draw(Canvas& c, const std::vector<RecentFile>& recent, bool caretVisible) {
  const Palette& p = theme::P();
  const float s = s_;
  c.PushClip(RectF(area_));
  logo::Draw(c, logo_.left, logo_.top, Width(logo_));
  c.Text(L"Featherweight", title_, p.text, 28 * s, DWRITE_FONT_WEIGHT_SEMI_BOLD);
  c.Text(kSubtitle, subtitle_, p.textSecondary, 14 * s);
  if (Shown(newLabel_)) c.Text(L"New", newLabel_, p.text, 16 * s, DWRITE_FONT_WEIGHT_SEMI_BOLD);
  if (Shown(templatesLabel_)) c.Text(L"Templates", templatesLabel_, p.textSecondary, 13 * s, DWRITE_FONT_WEIGHT_SEMI_BOLD);
  for (size_t i = 0; i < items_.size(); ++i)
    DrawItem(c, items_[i], focus_ == Focus::Item && focusItem_ == static_cast<int>(i) && focusVisible_);

  c.Text(L"Recent", recentLabel_, p.text, 16 * s, DWRITE_FONT_WEIGHT_SEMI_BOLD);
  if (filterShown_) {
    filter_.Draw(c, s, caretVisible);
    c.Icon(icon::Search, RectF(filter_.rc.right - 30 * s, filter_.rc.top, filter_.rc.right - 6 * s, filter_.rc.bottom),
           p.textSecondary, 13 * s);
  }
  if (Shown(recentCard_)) DrawCard(c, recentCard_, 8 * s, s, 1, 0, 0);
  if (!noteText_.empty())
    c.Text(noteText_, note_, p.textSecondary, 13.5f * s, DWRITE_FONT_WEIGHT_REGULAR,
           showRecent_ ? Align::Center : Align::Leading);

  const int n = static_cast<int>(rows_.size());
  if (n > 0) {
    const int64_t now = util::UnixNow();
    // The times line up in a column as wide as the widest; the row's buttons take its place on hover.
    std::vector<std::wstring> when(n);
    float whenW = 0;
    for (int k = 0; k < n; ++k) {
      if (rows_[k].recent >= static_cast<int>(recent.size())) {  // the list changed since the layout
        c.PopClip();
        return;
      }
      when[k] = fw::RelativeTime(now - recent[rows_[k].recent].lastOpened);
      whenW = std::max(whenW, c.MeasureText(when[k], 12.5f * s));
    }
    whenW = std::ceil(whenW) + 2;
    const float side = std::max(whenW + 24 * s, 2 * 32 * s + 4 * s);
    if (pinnedRows_ > 0 && pinnedRows_ < n) {
      const float ly = std::round((rows_[pinnedRows_ - 1].rc.bottom + rows_[pinnedRows_].rc.top) / 2) + 0.5f;
      c.Line(recentCard_.left + 16 * s, ly, recentCard_.right - 16 * s, ly, Surf().line, 1);
    }
    const bool listFocus = focus_ == Focus::List && focusVisible_;
    for (int k = 0; k < n; ++k) {
      const D2D1_RECT_F& r = rows_[k].rc;
      if (r.bottom < area_.top || r.top > area_.bottom) continue;
      const RecentFile& f = recent[rows_[k].recent];
      const bool selected = listFocus && k == selected_;
      if (selected) c.FillRound(r, 6 * s, Alpha(p.hover, 1.3f));
      c.FillRound(r, 6 * s, Alpha(p.hover, rowHover_[k].V() * 1.2f));
      if (k == pressedRow_ && pressedButton_ == kNoButton && k == hoverRow_) c.FillRound(r, 6 * s, p.pressed);
      const float cy = (r.top + r.bottom) / 2;
      const D2D1_COLOR_F tint = Tint(f.Kind());
      const D2D1_RECT_F box = RectF(r.left + 10 * s, std::round(cy - 18 * s), r.left + 46 * s, std::round(cy + 18 * s));
      c.FillRound(box, 8 * s, Alpha(tint, p.dark ? 0.16f : 0.10f));
      DrawGlyph(c, KindGlyph(f.Kind()), box, tint, 17 * s);

      const float rx = r.right - 12 * s;
      if (k == hoverRow_) {
        for (int b : {kPinButton, kRemoveButton}) {
          const D2D1_RECT_F br = ButtonRect(k, b);
          if (hoverButton_ == b) c.FillRound(br, 6 * s, pressedButton_ == b && pressedRow_ == k ? p.pressed : p.hover);
          if (b == kPinButton) c.Icon(f.pinned ? icon::Unpin : icon::Pin, br, p.textSecondary, 14 * s);
          else c.Icon(icon::Cancel, br, p.textSecondary, 11 * s);
        }
      } else {
        c.Text(when[k], RectF(rx - whenW, r.top, rx, r.bottom), p.textSecondary, 12.5f * s, DWRITE_FONT_WEIGHT_REGULAR,
               Align::Trailing);
        if (f.pinned)
          c.Icon(icon::Pinned, RectF(rx - whenW - 24 * s, r.top, rx - whenW - 6 * s, r.bottom), p.textSecondary, 12 * s);
      }
      // The name has the whole width; the folder under it, shortened: inside the profile without it,
      // then with middle folders left out.
      const float tx = box.right + 14 * s, textRight = rx - side - 12 * s;
      c.Text(fw::FileNameOf(f.path), RectF(tx, r.top + 5 * s, textRight, cy + 2 * s), p.text, 14.5f * s,
             DWRITE_FONT_WEIGHT_SEMI_BOLD);
      const std::wstring full = fw::ShortFolder(fw::DirOf(f.path), Home());
      std::wstring folder = full;
      for (int drop = 1; c.MeasureText(folder, 12.5f * s) > textRight - tx; ++drop) {
        std::wstring shorter = fw::ElideFolder(full, drop);
        if (shorter == folder) break;
        folder = shorter;
      }
      c.Text(folder, RectF(tx, cy + 1 * s, textRight, r.bottom - 6 * s), p.textSecondary, 12.5f * s);
      if (selected) ctl::DrawFocusRing(c, Inflate(r, -3 * ui_, -3 * ui_), ui_);
    }
  }

  // Where the page is when it scrolls: a thin bar, as the panels have.
  if (scrollMax_ > 0) {
    const float h = static_cast<float>(area_.bottom - area_.top), total = h + scrollMax_;
    const float thumb = std::max(32 * s, h * h / total), top = area_.top + (h - thumb) * (scroll_ / scrollMax_);
    c.FillRound(RectF(area_.right - 7 * s, top + 2 * s, area_.right - 3 * s, top + thumb - 2 * s), 2 * s, p.scrollThumb);
  }
  c.PopClip();
}

bool EmptyState::InView(int x, int y) const {
  return x >= area_.left && x < area_.right && y >= area_.top && y < area_.bottom;
}

int EmptyState::ItemAt(int x, int y) const {
  if (!InView(x, y)) return -1;
  for (size_t i = 0; i < items_.size(); ++i)
    if (Shown(items_[i].rc) && Contains(items_[i].rc, (float)x, (float)y)) return static_cast<int>(i);
  return -1;
}

int EmptyState::RowAt(int x, int y) const {
  if (!InView(x, y)) return -1;
  for (size_t i = 0; i < rows_.size(); ++i)
    if (Contains(rows_[i].rc, (float)x, (float)y)) return static_cast<int>(i);
  return -1;
}

D2D1_RECT_F EmptyState::ButtonRect(int row, int button) const {
  const D2D1_RECT_F& r = rows_[row].rc;
  const float cy = (r.top + r.bottom) / 2, right = r.right - 8 * s_ - (button == kPinButton ? 34 * s_ : 0);
  return RectF(right - 32 * s_, cy - 16 * s_, right, cy + 16 * s_);
}

int EmptyState::ButtonAt(int row, int x, int y) const {
  if (row < 0 || row >= static_cast<int>(rows_.size())) return kNoButton;
  for (int b : {kPinButton, kRemoveButton})
    if (Contains(ButtonRect(row, b), (float)x, (float)y)) return b;
  return kNoButton;
}

bool EmptyState::OverFilter(int x, int y) const {
  return filterShown_ && InView(x, y) && Contains(filter_.rc, (float)x, (float)y);
}

void EmptyState::FocusOn(Focus f, int item) {
  focus_ = f;
  focusVisible_ = true;
  if (f == Focus::Item) focusItem_ = item;
  if (f == Focus::List && selected_ < 0 && !rows_.empty()) Select(0);
}

void EmptyState::Select(int row) {
  if (row < 0 || row >= static_cast<int>(paths_.size())) return;
  selected_ = row;
  selectedPath_ = paths_[row];
}

std::vector<EmptyState::Stop> EmptyState::Stops() const {
  // Tab order: Open…, the cards, the templates, the filter, show or hide, the list. It goes round.
  std::vector<Stop> stops;
  int showHide = -1;
  for (size_t i = 0; i < items_.size(); ++i) {
    if (!Shown(items_[i].rc)) continue;
    if (items_[i].part == Part::ShowHide) showHide = static_cast<int>(i);
    else stops.push_back({Focus::Item, static_cast<int>(i)});
  }
  if (filterShown_) stops.push_back({Focus::Filter, -1});
  if (showHide >= 0) stops.push_back({Focus::Item, showHide});
  if (!rows_.empty()) stops.push_back({Focus::List, -1});
  return stops;
}

bool EmptyState::Go(const std::vector<Stop>& stops, int from, bool forward) {
  const int n = static_cast<int>(stops.size());
  if (n == 0) return false;
  const int next = from < 0 ? (forward ? 0 : n - 1) : (from + (forward ? 1 : n - 1)) % n;
  reveal_ = true;
  if (stops[next].focus == Focus::Filter) {
    focus_ = Focus::Filter;
    return true;
  }
  FocusOn(stops[next].focus, stops[next].item);
  return false;
}

bool EmptyState::MoveFocus(bool forward) {
  const std::vector<Stop> stops = Stops();
  int at = -1;
  for (size_t i = 0; i < stops.size(); ++i)
    if (stops[i].focus == focus_ && (focus_ != Focus::Item || stops[i].item == focusItem_)) at = static_cast<int>(i);
  return Go(stops, at, forward);
}

void EmptyState::LeaveFilter(bool forward) {
  const std::vector<Stop> stops = Stops();
  int at = -1;
  for (size_t i = 0; i < stops.size(); ++i)
    if (stops[i].focus == Focus::Filter) at = static_cast<int>(i);
  if (at >= 0 && stops.size() > 1) Go(stops, at, forward);
  focusVisible_ = true;
}

void EmptyState::FocusList() {
  if (rows_.empty()) return LeaveFilter(true);
  FocusOn(Focus::List, -1);
  reveal_ = true;
}

bool EmptyState::OnMouseMove(int x, int y) {
  bool changed = false;
  const int item = ItemAt(x, y);
  if (item != hoverItem_) {
    if (hoverItem_ >= 0 && hoverItem_ < static_cast<int>(items_.size())) items_[hoverItem_].hover.To(0, 150);
    if (item >= 0) items_[item].hover.To(1, 90);
    hoverItem_ = item;
    changed = true;
  }
  const int row = item >= 0 ? -1 : RowAt(x, y);
  const int button = ButtonAt(row, x, y);
  if (row != hoverRow_) {
    if (hoverRow_ >= 0 && hoverRow_ < static_cast<int>(rowHover_.size())) rowHover_[hoverRow_].To(0, 150);
    if (row >= 0 && row < static_cast<int>(rowHover_.size())) rowHover_[row].To(1, 80);
    hoverRow_ = row;
    changed = true;
  }
  if (button != hoverButton_) {
    hoverButton_ = button;
    changed = true;
  }
  return changed;
}

void EmptyState::OnMouseLeave() {
  if (hoverItem_ >= 0 && hoverItem_ < static_cast<int>(items_.size())) items_[hoverItem_].hover.To(0, 150);
  hoverItem_ = -1;
  if (hoverRow_ >= 0 && hoverRow_ < static_cast<int>(rowHover_.size())) rowHover_[hoverRow_].To(0, 150);
  hoverRow_ = -1;
  hoverButton_ = kNoButton;
}

void EmptyState::OnMouseDown(int x, int y) {
  pressedItem_ = ItemAt(x, y);
  pressedRow_ = pressedItem_ >= 0 ? -1 : RowAt(x, y);
  pressedButton_ = ButtonAt(pressedRow_, x, y);
  // A click moves the keyboard to what was clicked, and hides the focus ring until a key is pressed.
  if (pressedItem_ >= 0) {
    items_[pressedItem_].press.To(1, 60);
    FocusOn(Focus::Item, pressedItem_);
  } else if (pressedRow_ >= 0 && pressedButton_ == kNoButton) {
    Select(pressedRow_);
    FocusOn(Focus::List, -1);
  }
  focusVisible_ = false;
}

EmptyState::Result EmptyState::OnMouseUp(int x, int y) {
  Result r;
  if (pressedItem_ >= 0 && pressedItem_ < static_cast<int>(items_.size())) {
    items_[pressedItem_].press.To(0, 140);
    if (ItemAt(x, y) == pressedItem_) r = ItemResult(items_[pressedItem_]);
  }
  pressedItem_ = -1;
  const int row = RowAt(x, y);
  if (row >= 0 && row == pressedRow_ && ButtonAt(row, x, y) == pressedButton_) {
    r.kind = pressedButton_ == kRemoveButton ? Result::Kind::RemoveRecent
             : pressedButton_ == kPinButton  ? Result::Kind::PinRecent
                                             : Result::Kind::OpenRecent;
    r.index = rows_[row].recent;
  }
  pressedRow_ = -1;
  pressedButton_ = kNoButton;
  return r;
}

EmptyState::Result EmptyState::OnRightClick(int x, int y) {
  const int row = RowAt(x, y);
  if (row < 0) return {};
  Select(row);
  FocusOn(Focus::List, -1);
  focusVisible_ = false;
  Result r{Result::Kind::RecentMenu, rows_[row].recent};
  r.at = POINT{x, y};
  return r;
}

bool EmptyState::OnWheel(int delta) {
  if (scrollMax_ <= 0) return false;
  const float was = scroll_;
  scroll_ = std::clamp(scroll_ - delta / 120.0f * 72 * s_, 0.0f, scrollMax_);
  return scroll_ != was;
}

bool EmptyState::OnKeyDown(UINT vk, bool shift, Result& result) {
  using K = Result::Kind;
  const int rows = static_cast<int>(rows_.size());
  const bool item = focus_ == Focus::Item && focusItem_ >= 0 && focusItem_ < static_cast<int>(items_.size());
  switch (vk) {
    case VK_TAB:
      if (MoveFocus(!shift)) result = {K::FocusFilter};  // the window gives the filter the keyboard
      return true;
    case VK_RETURN:
    case VK_SPACE:
      if (item) result = ItemResult(items_[focusItem_]);
      else if (focus_ == Focus::List && selected_ >= 0 && vk == VK_RETURN) result = {K::OpenRecent, rows_[selected_].recent};
      else return false;
      focusVisible_ = true;
      return true;
    case VK_DELETE:
      if (focus_ != Focus::List || selected_ < 0) return false;
      result = {K::RemoveRecent, rows_[selected_].recent};
      focusVisible_ = true;
      return true;
    case VK_APPS:
      if (focus_ != Focus::List || selected_ < 0) return false;
      result = {K::RecentMenu, rows_[selected_].recent};
      result.at = POINT{static_cast<LONG>(rows_[selected_].rc.left + 48 * s_), static_cast<LONG>(rows_[selected_].rc.bottom)};
      focusVisible_ = true;
      return true;
    case VK_UP:
    case VK_DOWN:
    case VK_HOME:
    case VK_END:
    case VK_PRIOR:
    case VK_NEXT: {
      if (focus_ != Focus::List || rows == 0) return false;
      if (vk == VK_UP && selected_ == 0 && filterShown_) {  // up from the first file: the filter
        focus_ = Focus::Filter;
        result = {K::FocusFilter};
        return true;
      }
      const int page = std::max(1, static_cast<int>((area_.bottom - area_.top) / (kRowH * s_)) - 1);
      int i = rows - 1;  // End
      if (vk == VK_UP) i = selected_ - 1;
      else if (vk == VK_DOWN) i = selected_ + 1;
      else if (vk == VK_HOME) i = 0;
      else if (vk == VK_PRIOR) i = selected_ - page;
      else if (vk == VK_NEXT) i = selected_ + page;
      Select(std::clamp(i, 0, rows - 1));
      focusVisible_ = reveal_ = true;
      return true;
    }
    case VK_LEFT:
    case VK_RIGHT: {
      // Through the cards, or through the templates.
      if (!item) return false;
      auto group = [&](int i) {
        const Part part = items_[i].part;
        return part == Part::Template || part == Part::UserTemplate || part == Part::More ? Part::Template : part;
      };
      const Part g = group(focusItem_);
      const int step = vk == VK_RIGHT ? 1 : -1;
      for (int i = focusItem_ + step; i >= 0 && i < static_cast<int>(items_.size()); i += step)
        if (group(i) == g && Shown(items_[i].rc)) {
          FocusOn(Focus::Item, i);
          break;
        }
      focusVisible_ = reveal_ = true;
      return true;
    }
    default: return false;
  }
}

bool EmptyState::OverClickable(int x, int y) const { return ItemAt(x, y) >= 0 || RowAt(x, y) >= 0; }

std::wstring EmptyState::TooltipAt(int x, int y, const std::vector<RecentFile>& recent) const {
  if (int i = ItemAt(x, y); i >= 0) return items_[i].tip;
  const int row = RowAt(x, y);
  if (row < 0 || rows_[row].recent >= static_cast<int>(recent.size())) return {};
  const RecentFile& f = recent[rows_[row].recent];
  switch (ButtonAt(row, x, y)) {
    case kPinButton: return f.pinned ? L"Unpin from list" : L"Pin to list";
    case kRemoveButton: return L"Remove from list";
    default: return f.path;
  }
}

void EmptyState::Describe(uia::Tree& t, int parent, const std::vector<RecentFile>& recent,
                          const std::function<void(Result)>& act, const std::function<void()>& focusFilter) {
  // The cards and the templates in a group each.
  D2D1_RECT_F bounds[2] = {};
  for (const Item& it : items_) {
    const int g = it.part == Part::New ? 0 : it.part == Part::Open || it.part == Part::ShowHide ? -1 : 1;
    if (g < 0 || !Shown(it.rc)) continue;
    D2D1_RECT_F& b = bounds[g];
    b = Shown(b) ? RectF(std::min(b.left, it.rc.left), std::min(b.top, it.rc.top), std::max(b.right, it.rc.right),
                         std::max(b.bottom, it.rc.bottom))
                 : it.rc;
  }
  int groups[2] = {-1, -1};
  for (size_t i = 0; i < items_.size(); ++i) {
    const Item& it = items_[i];
    if (!Shown(it.rc)) continue;
    const int g = it.part == Part::New ? 0 : it.part == Part::Open || it.part == Part::ShowHide ? -1 : 1;
    int under = parent;
    if (g >= 0) {
      if (groups[g] < 0) groups[g] = t.Add(parent, 3 + g, UIA_GroupControlTypeId, g == 0 ? L"New" : L"Templates", bounds[g]);
      under = groups[g];
    }
    const int node = t.Add(under, 10 + static_cast<int64_t>(i), UIA_ButtonControlTypeId, it.text.empty() ? it.tip : it.text, it.rc);
    uia::Node& n = t[node];
    n.help = it.tip;
    const Result res = ItemResult(it);
    n.invoke = [act, res] { act(res); };
    n.focusable = true;
    n.focused = focus_ == Focus::Item && focusItem_ == static_cast<int>(i);
    n.focus = [this, i] { FocusOn(Focus::Item, static_cast<int>(i)); };
  }
  if (filterShown_) uia::AddTextBox(t, parent, 5, filter_, L"Filter recent files", focusFilter);

  const int rows = static_cast<int>(rows_.size());
  if (rows == 0) return;
  const D2D1_RECT_F all = RectF(rows_.front().rc.left, rows_.front().rc.top, rows_.front().rc.right, rows_.back().rc.bottom);
  const int list = t.Add(parent, 2, UIA_ListControlTypeId, L"Recent", all);
  t[list].orientation = OrientationType_Vertical;
  t[list].selectionContainer = true;
  t[list].focusable = true;
  t[list].focus = [this] { FocusOn(Focus::List, -1); };
  const int64_t now = util::UnixNow();
  for (int i = 0; i < rows; ++i) {
    const int index = rows_[i].recent;
    if (index >= static_cast<int>(recent.size())) break;
    const RecentFile& f = recent[index];
    const int item = t.Add(list, i, UIA_ListItemControlTypeId, fw::FileNameOf(f.path), rows_[i].rc);
    uia::Node& n = t[item];
    n.help = f.path;
    n.status = fw::RelativeTime(now - f.lastOpened) + (f.pinned ? L", pinned" : L"");
    n.invoke = [act, index] { act({Result::Kind::OpenRecent, index}); };
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
    const int pin = t.Add(item, 1, UIA_ButtonControlTypeId, f.pinned ? L"Unpin from list" : L"Pin to list", ButtonRect(i, kPinButton));
    t[pin].invoke = [act, index] { act({Result::Kind::PinRecent, index}); };
    const int remove = t.Add(item, 2, UIA_ButtonControlTypeId, L"Remove from list", ButtonRect(i, kRemoveButton));
    t[remove].invoke = [act, index] { act({Result::Kind::RemoveRecent, index}); };
  }
}
