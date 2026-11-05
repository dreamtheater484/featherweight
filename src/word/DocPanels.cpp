#include "word/DocPanels.h"

#include <algorithm>
#include <cmath>

#include "core/Strings.h"
#include "doc/edit/HeaderFooter.h"
#include "ui/Gfx.h"
#include "ui/Theme.h"
#include "ui/Uia.h"

using namespace gfx;
namespace edit = fw::doc::edit;
using fw::doc::kNone;

namespace {

std::wstring W(std::u16string_view s) { return std::wstring(s.begin(), s.end()); }
std::wstring Wide(std::string_view s) { return fw::Utf8ToWide(std::string(s)); }
std::u16string U16(std::wstring_view s) { return std::u16string(s.begin(), s.end()); }
fw::doc::Pos ToDoc(fw::doc::layout::TextPos p) { return {p.para, p.offset}; }
fw::doc::layout::TextPos ToText(fw::doc::Pos p) { return {p.para, p.offset}; }

std::wstring Trim(std::wstring s) {
  while (!s.empty() && iswspace(s.back())) s.pop_back();
  size_t i = 0;
  while (i < s.size() && iswspace(s[i])) ++i;
  return s.substr(i);
}

bool StartsNoCase(std::string_view s, std::string_view prefix) {
  if (s.size() < prefix.size()) return false;
  for (size_t i = 0; i < prefix.size(); ++i)
    if (std::tolower(static_cast<unsigned char>(s[i])) != prefix[i]) return false;
  return true;
}

// What was typed in the address box, as a link's target: a web address gets its https://, a mail
// address its mailto:, a file path its file:///.
std::string Address(const std::wstring& typed) {
  std::string a = fw::WideToUtf8(typed);
  if (a.empty() || a[0] == '#' || a.find("://") != std::string::npos || StartsNoCase(a, "mailto:")) return a;
  if (a.size() > 2 && a[1] == ':' && (a[2] == '\\' || a[2] == '/')) {
    std::replace(a.begin(), a.end(), '\\', '/');
    return "file:///" + a;
  }
  if (a.find('@') != std::string::npos && a.find('/') == std::string::npos) return "mailto:" + a;
  return "https://" + a;
}

}  // namespace

// ---------------------------------------------------------------------------
// The push button

void DocView::PushButton::Draw(Canvas& c) {
  if (!visible) return;
  const Palette& p = theme::P();
  const float u = Ui();
  D2D1_COLOR_F bg = primary ? (pressed_ ? p.accentPressed : hover_ ? p.accentHover : p.accent)
                            : (pressed_ ? p.pressed : hover_ ? p.hover : p.inputBg);
  if (!enabled) bg = Alpha(bg, 0.5f);
  c.FillRound(rc, 4 * u, bg);
  if (!primary) c.StrokeRound(rc, 4 * u, p.inputBorder, 1);
  const D2D1_COLOR_F fg = !enabled ? p.textDisabled : primary ? p.onAccent : p.text;
  const float w = c.MeasureText(text, 13 * u);
  const float x = std::round((rc.left + rc.right - w) / 2);
  c.Text(text, RectF(x, rc.top, x + w + 2, rc.bottom), fg, 13 * u);
  if (FocusVisible()) ctl::DrawFocusRing(c, rc, u);
}

bool DocView::PushButton::OnMouseMove(float x, float y) {
  const bool h = enabled && Hit(x, y);
  const bool changed = h != hover_;
  hover_ = h;
  return changed;
}

void DocView::PushButton::OnMouseDown(float x, float y, bool, bool) {
  pressed_ = enabled && Hit(x, y);
  if (pressed_) TakeFocus();
}

void DocView::PushButton::OnMouseUp(float x, float y) {
  const bool fire = pressed_ && Hit(x, y);
  pressed_ = false;
  if (fire) Invoke();
}

bool DocView::PushButton::OnKeyDown(UINT vk, bool, bool, bool) {
  if (!enabled || (vk != VK_RETURN && vk != VK_SPACE)) return false;
  Invoke();
  return true;
}

void DocView::PushButton::Invoke() {
  if (enabled && onInvoke) onInvoke();
}

void DocView::PushButton::Describe(uia::Tree& t, int parent) {
  if (!visible) return;
  const int i = t.Add(parent, id, UIA_ButtonControlTypeId, text, rc);
  t[i].enabled = enabled;
  t[i].focusable = true;
  t[i].focus = [this] { TakeFocus(); };
  t[i].focused = Focused();
  t[i].invoke = [this] { Invoke(); };
}

// ---------------------------------------------------------------------------
// Rows of labels and controls

std::vector<Control*> DocPanelRows::LayOutRows(const std::vector<Row>& rows, const D2D1_RECT_F& body, float ui) {
  ui_ = ui;
  labels_.clear();
  std::vector<Control*> out;
  const float left = body.left + 16 * ui, right = body.right - 16 * ui, gap = 8 * ui;
  float y = body.top + 12 * ui;
  for (const Row& r : rows) {
    if (!r.label.empty()) {
      labels_.push_back({r.label, RectF(left, y, right, y + 18 * ui)});
      y += 22 * ui;
    }
    std::vector<Control*> shown;
    for (Control* c : r.controls)
      if (c->visible) shown.push_back(c);
    if (shown.empty()) continue;
    const float h = r.height * ui, w = (right - left - gap * static_cast<float>(shown.size() - 1)) / static_cast<float>(shown.size());
    for (size_t i = 0; i < shown.size(); ++i) {
      const float x = left + static_cast<float>(i) * (w + gap);
      shown[i]->rc = RectF(x, y, x + w, std::min(y + h, body.bottom - 8 * ui));
      out.push_back(shown[i]);
    }
    y += h + 14 * ui;
  }
  noteRc_ = RectF(left, y, right, y + 20 * ui);
  return out;
}

void DocPanelRows::DrawLabels(Canvas& c) const {
  const Palette& p = theme::P();
  for (const auto& [text, rc] : labels_) c.Text(text, rc, p.textSecondary, 12 * ui_);
  if (!note_.empty()) c.Text(note_, noteRc_, p.text, 12 * ui_);
}

// ---------------------------------------------------------------------------
// Links

DocView::LinkPanel::LinkPanel(DocView& view) : v_(view) {
  address_.label = L"Address";
  address_.tooltip = L"A web or mail address, or a heading or bookmark in this document";
  text_.label = L"Text to display";
  text_.autoComplete = false;
  address_.onCommit = [this](const std::wstring&, int index) {
    // Picking a place shows its text when there's none yet.
    if (index >= 0 && text_.Text().empty() && !editing_ && v_.SelFrom() == v_.SelTo()) {
      const std::wstring& item = address_.items[static_cast<size_t>(index)];
      const size_t colon = item.find(L": ");
      text_.SetText(Trim(colon == std::wstring::npos ? item : item.substr(colon + 2)));
    }
  };
  apply_.primary = true;
  apply_.onInvoke = [this] { Apply(); };
  remove_.text = L"Remove link";
  remove_.onInvoke = [this] { Remove(); };
}

std::vector<Control*> DocView::LinkPanel::Layout(const D2D1_RECT_F& body, float ui) {
  return LayOutRows({{L"Text to display", {&text_}}, {L"Address, or a place in this document", {&address_}},
                     {L"", {&apply_, &remove_}}},
                    body, ui);
}

void DocView::LinkPanel::Open() {
  const fw::doc::Document& doc = *v_.doc_;
  note_.clear();
  places_.clear();
  std::vector<std::wstring> items;
  for (const Heading& h : v_.Headings()) {
    items.push_back(L"Heading: " + std::wstring(static_cast<size_t>(h.level) * 2, L' ') + h.text);
    places_.push_back({h.para, {}});
  }
  for (const edit::BookmarkInfo& b : edit::Bookmarks(doc)) {
    if (b.hidden) continue;
    items.push_back(L"Bookmark: " + Wide(b.name));
    places_.push_back({kNone, b.name});
  }
  address_.items = std::move(items);
  const auto link = edit::LinkAt(doc, ToDoc(v_.focus_));
  editing_ = link.has_value();
  if (editing_) {
    at_ = v_.focus_;
    address_.SetText(Wide(link->target));
    text_.SetText(v_.TextOf(ToText(link->from), ToText(link->to), 400));
    apply_.text = L"Update link";
  } else {
    address_.SetText(L"");
    const bool one = v_.anchor_.para == v_.focus_.para;
    text_.SetText(v_.HasSelection() && one ? v_.TextOf(std::min(v_.anchor_, v_.focus_), std::max(v_.anchor_, v_.focus_), 400)
                                           : L"");
    apply_.text = L"Insert link";
  }
  remove_.visible = editing_;
}

void DocView::LinkPanel::Apply() {
  const std::wstring typed = Trim(address_.Text());
  if (typed.empty()) {
    note_ = L"Type an address, or pick a heading or bookmark.";
    v_.host_.ViewInvalidate();
    return;
  }
  edit::Editor& ed = *v_.editor_;
  const size_t marksBefore = edit::Bookmarks(*v_.doc_).size();
  ed.BeginGroup(u"Link");
  std::string target;
  auto item = std::find(address_.items.begin(), address_.items.end(), typed);
  if (item != address_.items.end()) {
    const Place& place = places_[static_cast<size_t>(item - address_.items.begin())];
    const std::string name = place.para != kNone ? edit::BookmarkForParagraph(ed, place.para) : place.bookmark;
    if (!name.empty()) target = "#" + name;
  } else {
    target = Address(typed);
  }
  const std::u16string shown = U16(text_.Text());
  edit::Result r;
  if (target.empty()) {
    r.ok = false;
    r.message = u"That place can't be linked to.";
  } else if (editing_) {
    r = edit::EditLink(ed, ToDoc(at_), target, shown);
  } else {
    r = edit::InsertLink(ed, v_.SelFrom(), v_.SelTo(), target, shown.empty() ? U16(typed) : shown);
  }
  ed.EndGroup();
  if (!r) {
    if (edit::Bookmarks(*v_.doc_).size() != marksBefore) ed.Undo();  // the heading's bookmark goes too
    v_.SyncFile();
    note_ = W(r.message);
    v_.host_.ViewInvalidate();
    return;
  }
  v_.Apply(r);
  v_.host_.ViewShowPanel(nullptr);
}

void DocView::LinkPanel::Remove() {
  if (!editing_) return;
  if (v_.Apply(edit::RemoveLink(*v_.editor_, ToDoc(at_)))) v_.host_.ViewShowPanel(nullptr);
}

// ---------------------------------------------------------------------------
// Bookmarks

DocView::BookmarkPanel::BookmarkPanel(DocView& view) : v_(view) {
  name_.label = L"Bookmark name";
  name_.autoComplete = false;
  list_.label = L"Bookmarks";
  list_.rowHeight = 28;
  list_.onSelect = [this](int i) {
    if (i >= 0 && static_cast<size_t>(i) < marks_.size()) name_.SetText(Wide(marks_[static_cast<size_t>(i)].name));
  };
  list_.onActivate = [this](int) { GoTo(); };
  add_.text = L"Add";
  add_.primary = true;
  add_.onInvoke = [this] { Add(); };
  goTo_.text = L"Go to";
  goTo_.onInvoke = [this] { GoTo(); };
  delete_.text = L"Delete";
  delete_.onInvoke = [this] { Delete(); };
}

std::vector<Control*> DocView::BookmarkPanel::Layout(const D2D1_RECT_F& body, float ui) {
  const float listHeight = std::max(80.0f, (body.bottom - body.top) / ui - 220);
  return LayOutRows({{L"Bookmark name (letters, digits and _)", {&name_}},
                     {L"", {&add_, &goTo_, &delete_}},
                     {L"In this document", {&list_}, listHeight}},
                    body, ui);
}

void DocView::BookmarkPanel::Open() {
  note_.clear();
  name_.SetText(L"");
  Refresh();
}

void DocView::BookmarkPanel::Refresh() {
  marks_.clear();
  for (edit::BookmarkInfo& b : edit::Bookmarks(*v_.doc_))
    if (!b.hidden) marks_.push_back(std::move(b));
  list_.items.clear();
  for (const auto& b : marks_) list_.items.push_back(Wide(b.name));
  list_.SetSelected(-1);
  v_.host_.ViewInvalidate();
}

void DocView::BookmarkPanel::Add() {
  const std::string name = fw::WideToUtf8(Trim(name_.Text()));
  edit::Result r = edit::InsertBookmark(*v_.editor_, v_.SelFrom(), v_.SelTo(), name);
  v_.SyncFile();
  note_ = r ? L"Added “" + Wide(name) + L"”." : W(r.message);
  Refresh();
}

void DocView::BookmarkPanel::GoTo() {
  const auto b = edit::FindBookmark(*v_.doc_, fw::WideToUtf8(Trim(name_.Text())));
  if (!b) {
    note_ = L"There's no bookmark with that name.";
    v_.host_.ViewInvalidate();
    return;
  }
  note_.clear();
  v_.GoTo(ToText(b->from), ToText(b->to));
}

void DocView::BookmarkPanel::Delete() {
  const std::string name = fw::WideToUtf8(Trim(name_.Text()));
  edit::Result r = edit::DeleteBookmark(*v_.editor_, name);
  v_.SyncFile();
  note_ = r ? L"Deleted “" + Wide(name) + L"”." : W(r.message);
  if (r) name_.SetText(L"");
  Refresh();
}

// ---------------------------------------------------------------------------
// Page numbers

namespace {
constexpr std::string_view kFormats[] = {"decimal", "lowerRoman", "upperRoman", "lowerLetter", "upperLetter"};
}

DocView::PageNumberPanel::PageNumberPanel(DocView& view) : v_(view) {
  position_.label = L"Position";
  position_.items = {L"Top of page (header)", L"Bottom of page (footer)", L"At the cursor"};
  position_.selected = 1;
  align_.label = L"Alignment";
  align_.items = {L"Left", L"Center", L"Right"};
  align_.selected = 1;
  style_.label = L"Style";
  style_.items = {L"1", L"Page 1 of 3"};
  style_.selected = 0;
  insert_.text = L"Insert page number";
  insert_.primary = true;
  insert_.onInvoke = [this] { Insert(); };
  format_.label = L"Number format";
  format_.items = {L"1, 2, 3", L"i, ii, iii", L"I, II, III", L"a, b, c", L"A, B, C"};
  numbering_.label = L"Numbering";
  numbering_.items = {L"Continue from the previous section", L"Start at"};
  start_.label = L"Start at";
  start_.minValue = 0;
  start_.maxValue = 32767;
  start_.value = 1;
  apply_.text = L"Apply to this section";
  apply_.onInvoke = [this] { ApplyFormat(); };
}

std::vector<Control*> DocView::PageNumberPanel::Layout(const D2D1_RECT_F& body, float ui) {
  return LayOutRows({{L"Position", {&position_}},
                     {L"Alignment and style", {&align_, &style_}},
                     {L"", {&insert_}},
                     {L"Number format", {&format_}},
                     {L"Numbering", {&numbering_, &start_}},
                     {L"", {&apply_}}},
                    body, ui);
}

uint32_t DocView::PageNumberPanel::Section() const {
  if (v_.hf_) return v_.hf_->section;
  const auto& sections = v_.doc_->Sections();
  if (v_.StoryOf(v_.focus_) == v_.doc_->MainStory())
    for (uint32_t i = 0; i < sections.size(); ++i)
      if (sections[i].endPara == kNone || v_.focus_.para <= sections[i].endPara) return i;
  return 0;
}

void DocView::PageNumberPanel::Open() {
  note_.clear();
  const auto& sections = v_.doc_->Sections();
  const uint32_t s = Section();
  if (s < sections.size()) {
    const fw::doc::SectionProps& props = sections[s].props;
    format_.selected = 0;
    for (size_t i = 0; i < std::size(kFormats); ++i)
      if (props.pageNumberFormat == kFormats[i]) format_.selected = static_cast<int>(i);
    numbering_.selected = props.pageNumberStart ? 1 : 0;
    start_.SetValue(props.pageNumberStart.value_or(1));
  }
}

void DocView::PageNumberPanel::Insert() {
  edit::Editor& ed = *v_.editor_;
  const fw::doc::Justification jc = align_.selected == 0   ? fw::doc::Justification::Start
                                    : align_.selected == 2 ? fw::doc::Justification::End
                                                           : fw::doc::Justification::Center;
  const bool ofPages = style_.selected == 1;
  if (position_.selected == 2) {
    // At the cursor: the field (and "Page … of …") in the text.
    ed.BeginGroup(u"Page Number");
    fw::doc::Pos at = ToDoc(v_.focus_);
    edit::Result r;
    if (ofPages && (r = ed.InsertText(at, u"Page "))) at = r.caret;
    if (r && (r = edit::InsertField(ed, at, edit::kPageField, u"1", u"Page Number"))) at = r.caret;
    if (r && ofPages && (r = ed.InsertText(at, u" of "))) at = r.caret;
    if (r && ofPages && (r = edit::InsertField(ed, at, edit::kPagesField, u"1", u"Page Number"))) at = r.caret;
    ed.EndGroup();
    if (!r && ofPages && ed.UndoName() == u"Page Number") ed.Undo();
    if (v_.Apply(r)) v_.host_.ViewShowPanel(nullptr);
    else note_ = W(r.message);
    return;
  }
  const bool footer = position_.selected == 1;
  const fw::doc::HdrFtrType type = v_.hf_ ? v_.hf_->type : fw::doc::HdrFtrType::Default;
  edit::Result r = edit::InsertPageNumber(ed, Section(), footer, type, jc, ofPages);
  v_.SyncFile();
  if (!r) {
    note_ = W(r.message);
    v_.host_.ViewInvalidate();
    return;
  }
  // Shown where it went: the header or footer, edited in place.
  int page = v_.currentPage_;
  if (const auto& c = v_.CaretBox()) page = static_cast<int>(c->page);
  if (!v_.hf_ || v_.hf_->footer != footer) v_.EnterHeaderFooter(page, footer);
  if (v_.hf_) {
    v_.anchor_ = v_.focus_ = ToText(r.caret);
    v_.CaretMoved(true);
  }
  v_.host_.ViewShowPanel(nullptr);
}

void DocView::PageNumberPanel::ApplyFormat() {
  const size_t f = format_.selected >= 0 ? static_cast<size_t>(format_.selected) : 0;
  std::optional<int32_t> start;
  if (numbering_.selected == 1) start = static_cast<int32_t>(std::lround(start_.value));
  edit::Result r = edit::SetPageNumbering(*v_.editor_, Section(), kFormats[std::min(f, std::size(kFormats) - 1)], start);
  v_.SyncFile();
  note_ = r ? L"Page numbering changed." : W(r.message);
  v_.caretRevision_ = UINT32_MAX;
  v_.TakeNewPages();
  v_.host_.ViewInvalidate();
}

// ---------------------------------------------------------------------------
// The navigation pane

std::vector<Control*> DocView::NavPanel::Layout(const D2D1_RECT_F& body, float ui) {
  body_ = body;
  ui_ = ui;
  const float mid = (body.left + body.right) / 2;
  tabs_[0] = RectF(body.left + 12 * ui, body.top + 6 * ui, mid - 2 * ui, body.top + 34 * ui);
  tabs_[1] = RectF(mid + 2 * ui, body.top + 6 * ui, body.right - 12 * ui, body.top + 34 * ui);
  return {};
}

void DocView::NavPanel::Refresh() {
  if (edits_ == v_.edits_) return;
  headings_ = v_.Headings();
  edits_ = v_.edits_;
}

size_t DocView::NavPanel::Count() const { return results_ ? v_.matches_.size() : headings_.size(); }

int DocView::NavPanel::RowAt(float x, float y) const {
  const float top = body_.top + 44 * ui_, row = 28 * ui_;
  if (x < body_.left || x >= body_.right || y < top || y >= body_.bottom) return -1;
  const auto i = static_cast<int>(std::floor((y - top + scroll_) / row));
  return i >= 0 && static_cast<size_t>(i) < Count() ? i : -1;
}

int DocView::NavPanel::Current() const {
  if (results_) return v_.current_;
  const TextPos caret = v_.focus_;
  if (v_.doc_->Para(caret.para).story != v_.doc_->MainStory()) return -1;
  int cur = -1;
  for (size_t i = 0; i < headings_.size() && headings_[i].para <= caret.para; ++i) cur = static_cast<int>(i);
  return cur;
}

void DocView::NavPanel::Draw(Canvas& c) {
  Refresh();
  const Palette& p = theme::P();
  const float u = ui_;
  const wchar_t* names[2] = {L"Headings", L"Results"};
  for (int t = 0; t < 2; ++t) {
    const bool on = (t == 1) == results_;
    std::wstring label = names[t];
    if (t == 1 && !v_.matches_.empty()) label += L" (" + std::to_wstring(v_.matches_.size()) + L")";
    c.Text(label, tabs_[t], on ? p.text : p.textSecondary, 13 * u, on ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_REGULAR,
           Align::Center);
    if (on) c.Fill(RectF(tabs_[t].left + 8 * u, tabs_[t].bottom - 2 * u, tabs_[t].right - 8 * u, tabs_[t].bottom), p.accent);
  }
  const float top = body_.top + 44 * u, row = 28 * u;
  const D2D1_RECT_F list = RectF(body_.left, top, body_.right, body_.bottom);
  const size_t n = Count();
  if (n == 0) {
    const wchar_t* empty = results_ ? L"Search with Ctrl+F: the results show here." : L"Text in heading styles shows here.";
    c.Text(empty, RectF(list.left + 16 * u, top + 4 * u, list.right - 16 * u, top + 28 * u), p.textSecondary, 12 * u);
    return;
  }
  // The heading the caret is under stays in view as the caret moves.
  const int cur = Current();
  if (cur != followed_ && cur >= 0) {
    const float y = static_cast<float>(cur) * row;
    if (y < scroll_) scroll_ = y;
    else if (y + row > scroll_ + (list.bottom - list.top)) scroll_ = y + row - (list.bottom - list.top);
    followed_ = cur;
  }
  c.PushClip(list);
  const auto firstRow = static_cast<size_t>(std::max(0.0f, std::floor(scroll_ / row)));
  for (size_t i = firstRow; i < n; ++i) {
    const float y = top + static_cast<float>(i) * row - scroll_;
    if (y > list.bottom) break;
    const D2D1_RECT_F r = RectF(list.left + 8 * u, y, list.right - 8 * u, y + row);
    if (static_cast<int>(i) == cur) c.FillRound(r, 4 * u, p.selection);
    else if (static_cast<int>(i) == hover_) c.FillRound(r, 4 * u, p.hover);
    std::wstring text;
    float indent = 8 * u;
    if (results_) {
      const Match& m = v_.matches_[i];
      std::vector<uint32_t> map;
      std::wstring para;
      v_.VisibleText(m.from.para, para, map);
      size_t at = 0;
      while (at < map.size() && map[at] < m.from.offset) ++at;
      const size_t from = at > 24 ? at - 24 : 0;
      text = (from > 0 ? L"…" : L"") + para.substr(from, 80);
    } else {
      text = headings_[i].text;
      indent += static_cast<float>(headings_[i].level) * 14 * u;
    }
    const bool bold = !results_ && headings_[i].level == 0;
    c.Text(text, RectF(r.left + indent, r.top, r.right - 4 * u, r.bottom), p.text, 13 * u,
           bold ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_REGULAR);
    if (focused_ && static_cast<int>(i) == selected_) ctl::DrawFocusRing(c, r, u);
  }
  c.PopClip();
}

void DocView::NavPanel::SetFocus(bool has, bool) {
  focused_ = has;
  if (has && selected_ < 0) selected_ = std::max(0, Current());
}

bool DocView::NavPanel::OnMouseDown(float x, float y, bool) {
  for (int t = 0; t < 2; ++t)
    if (Contains(tabs_[t], x, y)) {
      results_ = t == 1;
      scroll_ = 0;
      followed_ = hover_ = selected_ = -1;
      return true;
    }
  const int row = RowAt(x, y);
  if (row < 0) return false;
  selected_ = row;
  Activate(row);
  return true;
}

bool DocView::NavPanel::OnMouseMove(float x, float y) {
  const int row = RowAt(x, y);
  if (row == hover_) return false;
  hover_ = row;
  return true;
}

void DocView::NavPanel::OnMouseLeave() { hover_ = -1; }

bool DocView::NavPanel::OnWheel(int delta, float, float) {
  const float row = 28 * ui_, view = body_.bottom - body_.top - 44 * ui_;
  const float most = std::max(0.0f, static_cast<float>(Count()) * row - view);
  const float next = std::clamp(scroll_ - static_cast<float>(delta) / 120.0f * 3 * row, 0.0f, most);
  if (next == scroll_) return false;
  scroll_ = next;
  return true;
}

bool DocView::NavPanel::OnKeyDown(UINT vk, bool, bool) {
  const int n = static_cast<int>(Count());
  if (n == 0) return false;
  switch (vk) {
    case VK_UP: selected_ = std::max(0, selected_ - 1); break;
    case VK_DOWN: selected_ = std::min(n - 1, selected_ + 1); break;
    case VK_HOME: selected_ = 0; break;
    case VK_END: selected_ = n - 1; break;
    case VK_RETURN:
    case VK_SPACE:
      if (selected_ >= 0) Activate(selected_);
      return true;
    default: return false;
  }
  const float row = 28 * ui_, view = body_.bottom - body_.top - 44 * ui_, y = static_cast<float>(selected_) * row;
  if (y < scroll_) scroll_ = y;
  else if (y + row > scroll_ + view) scroll_ = y + row - view;
  return true;
}

void DocView::NavPanel::Activate(int row) {
  if (row < 0 || static_cast<size_t>(row) >= Count()) return;
  if (results_) {
    v_.current_ = row;
    const Match& m = v_.matches_[static_cast<size_t>(row)];
    v_.GoTo(m.from, m.to);
  } else {
    const TextPos at{headings_[static_cast<size_t>(row)].para, 0};
    v_.GoTo(at, at);
  }
  v_.host_.ViewStateChanged();
}

void DocView::NavPanel::Describe(uia::Tree& t, int parent, const std::function<void()>& takeFocus) {
  Refresh();
  const int list = t.Add(parent, 1, UIA_ListControlTypeId, results_ ? L"Search results" : L"Headings", body_);
  t[list].focusable = true;
  t[list].focus = takeFocus;
  const float top = body_.top + 44 * ui_, row = 28 * ui_;
  for (size_t i = 0; i < Count() && i < 500; ++i) {
    const float y = top + static_cast<float>(i) * row - scroll_;
    std::wstring name = results_ ? L"Result " + std::to_wstring(i + 1) : headings_[i].text;
    const int item = t.Add(list, static_cast<int64_t>(i) + 2, UIA_ListItemControlTypeId, name,
                           RectF(body_.left, y, body_.right, y + row));
    t[item].invoke = [this, i] { Activate(static_cast<int>(i)); };
  }
}
