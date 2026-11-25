// The presentation's comments (P3, p3-review): the Comments pane, the markers on the slide, Review >
// Comments and Ctrl+Alt+M, as PowerPoint 365 has them (PresentationComments.h).

#include "slide/view/PresentationComments.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "core/Lang.h"
#include "core/Strings.h"
#include "slide/Presentation.h"
#include "slide/edit/Edit.h"
#include "slide/view/ObjectTool.h"
#include "ui/Icons.h"
#include "ui/Theme.h"

using namespace gfx;

namespace fw::slide::view {

namespace {

// "2026-11-24T09:07:35.328" -> "24 Nov 2026, 10:07" (local time; `utc`: stored in UTC).
std::wstring When(const std::string& date, bool utc) {
  static const wchar_t* kMonths[] = {T(L"Jan"), T(L"Feb"), T(L"Mar"), T(L"Apr"), T(L"May"), T(L"Jun"), T(L"Jul"), T(L"Aug"), T(L"Sep"), T(L"Oct"), T(L"Nov"), T(L"Dec")};
  if (date.size() < 16) return Utf8ToWide(date);
  SYSTEMTIME t{};
  t.wYear = static_cast<WORD>(std::atoi(date.substr(0, 4).c_str()));
  t.wMonth = static_cast<WORD>(std::atoi(date.substr(5, 2).c_str()));
  t.wDay = static_cast<WORD>(std::atoi(date.substr(8, 2).c_str()));
  t.wHour = static_cast<WORD>(std::atoi(date.substr(11, 2).c_str()));
  t.wMinute = static_cast<WORD>(std::atoi(date.substr(14, 2).c_str()));
  if (t.wMonth < 1 || t.wMonth > 12 || t.wDay < 1) return Utf8ToWide(date);
  SYSTEMTIME local = t;
  if (utc && !SystemTimeToTzSpecificLocalTime(nullptr, &t, &local)) local = t;
  wchar_t time[8];
  std::swprintf(time, std::size(time), L"%02u:%02u", local.wHour, local.wMinute);
  return std::to_wstring(local.wDay) + L" " + kMonths[local.wMonth - 1] + L" " + std::to_wstring(local.wYear) + L", " + time;
}

// A color per author, as PowerPoint gives each reviewer one.
D2D1_COLOR_F AuthorColor(const std::wstring& name) {
  static constexpr uint32_t kColors[] = {0x2B579A, 0xB7472A, 0x217346, 0x8E44AD, 0xC27C0E, 0x0F7B8A, 0xA4262C, 0x5C2D91};
  uint32_t h = 2166136261u;
  for (wchar_t ch : name) h = (h ^ static_cast<uint32_t>(ch)) * 16777619u;
  return Rgb(kColors[h % std::size(kColors)]);
}

std::wstring Trimmed(const std::wstring& s) {
  size_t b = s.find_first_not_of(L" \t\r\n"), e = s.find_last_not_of(L" \t\r\n");
  return b == std::wstring::npos ? std::wstring() : s.substr(b, e - b + 1);
}

std::wstring Message(const edit::Result& r) { return std::wstring(r.message.begin(), r.message.end()); }

constexpr int kOwned[] = {CmdNewComment, CmdReviewPane, CmdDeleteThread, CmdDeleteSlideComments, CmdDeleteAllComments,
                          CmdPrevComment, CmdNextComment, CmdShowCommentMarkup};

}  // namespace

PresentationView::CommentsPanel& PresentationView::Comments() {
  if (!commentsPanel_) commentsPanel_ = std::make_unique<CommentsPanel>(*this);
  return *commentsPanel_;
}

PresentationView::CommentsPanel::CommentsPanel(PresentationView& view) : v_(view) {
  name_.label = T(L"Your name");
  name_.autoComplete = false;
  name_.tooltip = T(L"Comments are made with this name");
  name_.onCommit = [this](const std::wstring& text, int) {
    v_.host_.ViewSetReviewer(Trimmed(text));
    name_.SetText(Writer().name);
  };
  box_.label = T(L"New comment");
  box_.autoComplete = false;
  box_.onEnter = [this] {
    if (!editing_.empty() || newThread_ || picked_.empty()) Post(box_.Text());
    else Reply(box_.Text());
  };
  post_.text = T(L"Post");
  post_.onInvoke = [this] { box_.onEnter(); };
  reply_.text = T(L"New");
  reply_.onInvoke = [this] { Open(true); };
  edit_.text = T(L"Edit");
  edit_.onInvoke = [this] { Edit(); };
  resolve_.text = T(L"Resolve");
  resolve_.onInvoke = [this] { Resolve(); };
  delete_.text = T(L"Delete");
  delete_.onInvoke = [this] { Delete(); };
}

edit::Editor* PresentationView::CommentsPanel::Ed() const { return v_.Ready() && v_.SlideCount() ? v_.SlideEditor() : nullptr; }

const std::vector<edit::Comment>& PresentationView::CommentsPanel::List() {
  edit::Editor* ed = Ed();
  if (!ed) {
    list_.clear();
    listSlide_ = SIZE_MAX;
    return list_;
  }
  if (listSlide_ != v_.current_ || listState_ != ed->StateId()) {
    list_ = edit::SlideComments(*ed, v_.current_);
    listSlide_ = v_.current_;
    listState_ = ed->StateId();
    if (std::none_of(list_.begin(), list_.end(), [&](const edit::Comment& c) { return c.id == picked_; })) picked_.clear();
  }
  return list_;
}

bool PresentationView::CommentsPanel::AnyInDeck() {
  edit::Editor* ed = Ed();
  if (!ed) return false;
  if (!List().empty()) return true;
  if (anyState_ != ed->StateId()) {
    const std::vector<size_t> counts = edit::CommentCounts(*ed);
    anyInDeck_ = std::any_of(counts.begin(), counts.end(), [](size_t n) { return n > 0; });
    anyState_ = ed->StateId();
  }
  return anyInDeck_;
}

edit::CommentAuthor PresentationView::CommentsPanel::Writer() const {
  edit::CommentAuthor who;
  std::wstring name = Trimmed(v_.host_.ViewReviewer());
  if (name.empty()) {
    wchar_t buf[257];
    DWORD n = static_cast<DWORD>(std::size(buf));
    if (GetUserNameW(buf, &n) && n > 1) name.assign(buf, n - 1);
  }
  if (!name.empty()) who.name = name;
  return who;
}

bool PresentationView::CommentsPanel::Shown() const { return v_.host_.ViewShownPanel() == this; }

// ---------------------------------------------------------------------------
// Markers

draw::Point PresentationView::CommentsPanel::Anchor(const edit::Comment& c) {
  if (drag_ == c.id && dragMoved_) return dragAt_;
  if (c.hasPos) return c.pos;
  // Anchored to a shape without a place of its own: its top right corner, as PowerPoint shows it.
  if (edit::Editor* ed = Ed(); ed && c.shape)
    if (const Slide* s = ed->Deck().GetSlide(v_.current_))
      if (const Shape* sh = s->FindShape(c.shape)) return {sh->bounds.right, sh->bounds.top};
  return {0, 0};
}

D2D1_RECT_F PresentationView::CommentsPanel::Marker(const edit::Comment& c, const D2D1_RECT_F& slide) {
  const float k = Width(slide) / std::max(1.0f, v_.deck_->SlideSize().width);
  const draw::Point p = Anchor(c);
  const float u = v_.ui_, x = std::round(slide.left + p.x * k), y = std::round(slide.top + p.y * k);
  return RectF(x, y, x + 26 * u, y + 22 * u);
}

void PresentationView::CommentsPanel::DrawMarkers(Canvas& c, const D2D1_RECT_F& slide) {
  if (!markers_) return;
  const Palette& p = theme::P();
  const float u = v_.ui_;
  for (const edit::Comment& cm : List()) {
    if (!cm.parent.empty()) continue;
    const D2D1_RECT_F r = Marker(cm, slide);
    D2D1_COLOR_F color = AuthorColor(cm.author);
    if (cm.resolved) color.a = 0.45f;
    c.SoftShadow(r, 4 * u, 3 * u, Rgb(0x000000, 0.18f), 1 * u);
    c.FillRound(r, 4 * u, color);
    // The bubble's tail, at its bottom left.
    c.FillRound(RectF(r.left, r.bottom - 8 * u, r.left + 8 * u, r.bottom + 4 * u), 1 * u, color);
    const std::wstring label = cm.initials.empty() ? edit::CommentInitials(cm.author) : cm.initials;
    c.Text(label.empty() ? L"?" : label, r, Rgb(0xFFFFFF), 11 * u, DWRITE_FONT_WEIGHT_SEMI_BOLD, Align::Center);
    if (cm.id == picked_) c.StrokeRound(Inflate(r, 2 * u, 2 * u), 5 * u, p.accent, std::max(1.f, std::round(2 * u)));
  }
}

std::string PresentationView::CommentsPanel::MarkerAt(float x, float y) {
  if (!markers_) return {};
  const D2D1_RECT_F slide = v_.SlideRect();
  const std::vector<edit::Comment>& list = List();
  for (auto it = list.rbegin(); it != list.rend(); ++it)  // the one drawn last is on top
    if (it->parent.empty() && Contains(Marker(*it, slide), x, y)) return it->id;
  return {};
}

bool PresentationView::CommentsPanel::MouseDown(float x, float y) {
  const std::string id = MarkerAt(x, y);
  if (id.empty()) return false;
  picked_ = id;
  for (const edit::Comment& c : List())
    if (c.id == id) dragFrom_ = Anchor(c);
  const draw::Point at = v_.SlidePoint(static_cast<int>(x), static_cast<int>(y));
  dragGrab_ = {at.x - dragFrom_.x, at.y - dragFrom_.y};
  dragAt_ = dragFrom_;
  dragMoved_ = false;
  drag_ = id;
  Open(false);
  return true;
}

bool PresentationView::CommentsPanel::MouseMove(float x, float y) {
  if (drag_.empty()) return false;
  const draw::Point at = v_.SlidePoint(static_cast<int>(x), static_cast<int>(y));
  const draw::Point to{at.x - dragGrab_.x, at.y - dragGrab_.y};
  const float k = Width(v_.SlideRect()) / std::max(1.0f, v_.deck_->SlideSize().width);
  if (!dragMoved_ && std::hypot((to.x - dragFrom_.x) * k, (to.y - dragFrom_.y) * k) < 4 * v_.ui_) return true;
  if (edit::Editor* ed = Ed(); !ed || !ed->CanEdit()) return true;  // a deck that can't change: picked only
  dragMoved_ = true;
  const draw::Size size = v_.deck_->SlideSize();
  dragAt_ = {std::clamp(to.x, 0.f, std::max(0.f, size.width - 1)), std::clamp(to.y, 0.f, std::max(0.f, size.height - 1))};
  v_.host_.ViewInvalidate();
  return true;
}

bool PresentationView::CommentsPanel::MouseUp(float, float) {
  if (drag_.empty()) return false;
  const std::string id = drag_;
  const bool moved = dragMoved_;
  drag_.clear();
  dragMoved_ = false;
  if (moved)
    if (edit::Editor* ed = Ed()) Done(edit::MoveComment(*ed, v_.current_, id, dragAt_), nullptr);
  v_.host_.ViewInvalidate();
  return true;
}

std::wstring PresentationView::CommentsPanel::Tooltip(float x, float y) {
  const std::string id = MarkerAt(x, y);
  for (const edit::Comment& c : List()) {
    if (c.id != id) continue;
    std::wstring text = c.text;
    if (text.size() > 120) text = text.substr(0, 117) + L"...";
    return (c.author.empty() ? std::wstring(T(L"Someone")) : c.author) + L": " + text;
  }
  return {};
}

// ---------------------------------------------------------------------------
// Commands

bool PresentationView::CommentsPanel::Owns(int cmd) { return std::find(std::begin(kOwned), std::end(kOwned), cmd) != std::end(kOwned); }

CommandState PresentationView::CommentsPanel::Query(int cmd) {
  CommandState s;
  edit::Editor* ed = Ed();
  const bool editable = ed && ed->CanEdit();
  const std::vector<edit::Comment>& list = List();
  auto anyInDeck = [&] { return AnyInDeck(); };
  switch (cmd) {
    case CmdNewComment:
      s.label = T(L"New comment");
      s.shortcut = L"Ctrl+Alt+M";
      s.enabled = editable;
      break;
    case CmdReviewPane:
      s.label = T(L"Comments pane");
      s.enabled = ed != nullptr;
      s.checked = Shown();
      break;
    case CmdShowCommentMarkup:
      s.label = T(L"Show markup");
      s.enabled = ed != nullptr;
      s.checked = markers_;
      break;
    case CmdDeleteThread:
      s.label = T(L"Delete comment");
      s.enabled = editable && !picked_.empty();
      break;
    case CmdDeleteSlideComments:
      s.label = T(L"Delete all comments on this slide");
      s.enabled = editable && !list.empty();
      break;
    case CmdDeleteAllComments:
      s.label = T(L"Delete all comments in this presentation");
      s.enabled = editable && anyInDeck();
      break;
    case CmdPrevComment:
    case CmdNextComment:
      s.label = cmd == CmdPrevComment ? T(L"Previous comment") : T(L"Next comment");
      s.enabled = ed && anyInDeck();
      break;
    default: return s;
  }
  return s;
}

bool PresentationView::CommentsPanel::Execute(int cmd) {
  edit::Editor* ed = Ed();
  if (!ed) return false;
  switch (cmd) {
    case CmdNewComment: Open(true); return true;
    case CmdReviewPane:
      if (Shown()) v_.host_.ViewShowPanel(nullptr);
      else Open(false);
      return true;
    case CmdShowCommentMarkup:
      markers_ = !markers_;
      v_.host_.ViewStateChanged();
      v_.host_.ViewInvalidate();
      return true;
    case CmdDeleteThread: Delete(); return true;
    case CmdDeleteSlideComments: return Done(edit::DeleteAllComments(*ed, {v_.current_}), T(L"Comments deleted."));
    case CmdDeleteAllComments: return Done(edit::DeleteAllComments(*ed, {}), T(L"Comments deleted."));
    case CmdPrevComment: Step(false); return true;
    case CmdNextComment: Step(true); return true;
    default: return false;
  }
}

bool PresentationView::CommentsPanel::ScriptOp(const std::wstring& op, const std::wstring& arg) {
  if (op != L"comment") return false;
  const size_t bar = arg.find(L'|');
  const std::wstring what = arg.substr(0, bar), text = bar == std::wstring::npos ? std::wstring() : arg.substr(bar + 1);
  bool ok = true;
  if (what == L"new") {
    Open(true);
    ok = Post(text);
  } else if (what == L"reply") {
    if (picked_.empty())
      for (const edit::Comment& c : List())
        if (c.parent.empty()) picked_ = c.id;  // the last thread
    ok = Reply(text);
  } else if (what == L"edit" || what == L"resolve" || what == L"delete") {
    if (picked_.empty())
      for (const edit::Comment& c : List())
        if (c.parent.empty()) picked_ = c.id;
    if (what == L"edit") {
      editing_ = picked_;
      ok = Post(text);
    } else if (what == L"resolve") {
      Resolve();
    } else {
      Delete();
    }
  } else if (what == L"next" || what == L"prev") {
    Step(what == L"next");
  } else if (what == L"pane") {
    Open(false);
  } else {
    ok = false;
  }
  if (!ok) std::fwprintf(stderr, L"comment:%ls: %ls\n", arg.c_str(), note_.c_str());
  return true;
}

void PresentationView::CommentsPanel::Open(bool startNew) {
  if (startNew) {
    // Where the new thread's marker goes: beside the selected shape, else stacked at the top left.
    at_ = {12, 12};
    const uint32_t shape = v_.Objects().SelectedShape();
    edit::Editor* ed = Ed();
    const Slide* s = ed ? ed->Deck().GetSlide(v_.current_) : nullptr;
    if (const Shape* sh = s && shape ? s->FindShape(shape) : nullptr) {
      at_ = {sh->bounds.right, sh->bounds.top};
    } else {
      int threads = 0;
      for (const edit::Comment& c : List()) threads += c.parent.empty();
      at_.y += 26.f * static_cast<float>(threads % 12);
    }
    const draw::Size size = v_.deck_->SlideSize();
    at_ = {std::clamp(at_.x, 0.f, std::max(0.f, size.width - 20)), std::clamp(at_.y, 0.f, std::max(0.f, size.height - 20))};
    newThread_ = true;
    editing_.clear();
    box_.ReplaceText(L"");
    focusWanted_ = &box_;
  } else {
    newThread_ = picked_.empty();
    editing_.clear();
    if (!picked_.empty()) focusWanted_ = &box_;
  }
  if (!Shown()) v_.host_.ViewShowPanel(this);
  seenState_ = UINT64_MAX;  // the cards again
  v_.host_.ViewStateChanged();
  v_.host_.ViewInvalidate();
}

void PresentationView::CommentsPanel::Step(bool forward) {
  edit::Editor* ed = Ed();
  if (!ed) return;
  struct Thread {
    size_t slide;
    std::string id;
  };
  std::vector<Thread> all;
  for (size_t s = 0; s < v_.SlideCount(); ++s)
    for (const edit::Comment& c : s == v_.current_ ? List() : edit::SlideComments(*ed, s))
      if (c.parent.empty()) all.push_back({s, c.id});
  if (all.empty()) {
    v_.host_.ViewMessage(BannerKind::Info, T(L"There are no comments in this presentation."));
    return;
  }
  // From the picked thread, else from the current slide's place in the deck.
  int at = -1;
  for (size_t i = 0; i < all.size(); ++i)
    if (all[i].slide == v_.current_ && all[i].id == picked_) at = static_cast<int>(i);
  size_t next = 0;
  const int n = static_cast<int>(all.size());
  if (at >= 0) {
    next = static_cast<size_t>((at + (forward ? 1 : n - 1)) % n);
  } else if (forward) {
    next = 0;
    while (next < all.size() && all[next].slide < v_.current_) ++next;
    if (next == all.size()) next = 0;
  } else {
    next = all.size() - 1;
    while (next > 0 && all[next].slide > v_.current_) --next;
    if (all[next].slide > v_.current_) next = all.size() - 1;
  }
  if (all[next].slide != v_.current_) v_.GoToSlide(all[next].slide);
  picked_ = all[next].id;
  listSlide_ = SIZE_MAX;  // read for the slide now shown, keeping the pick
  List();
  picked_ = all[next].id;
  Open(false);
}

// ---------------------------------------------------------------------------
// Changes

bool PresentationView::CommentsPanel::Done(const edit::Result& r, const wchar_t* ok) {
  note_ = r ? (ok ? ok : L"") : Message(r);
  if (!r && !Shown()) v_.Done(r);  // a banner when the pane can't say it
  seenState_ = UINT64_MAX;
  v_.host_.ViewStateChanged();
  v_.host_.ViewInvalidate();
  return r.ok;
}

bool PresentationView::CommentsPanel::Post(const std::wstring& text) {
  edit::Editor* ed = Ed();
  if (!ed) return false;
  edit::Result r;
  if (!editing_.empty()) {
    r = edit::EditComment(*ed, v_.current_, editing_, text);
    if (r) editing_.clear();
  } else {
    std::string id;
    r = edit::AddComment(*ed, v_.current_, at_, text, Writer(), &id);
    if (r) {
      picked_ = id;
      newThread_ = false;
    }
  }
  if (r) box_.ReplaceText(L"");
  return Done(r, T(L"Posted."));
}

bool PresentationView::CommentsPanel::Reply(const std::wstring& text) {
  edit::Editor* ed = Ed();
  if (!ed || picked_.empty()) return false;
  const edit::Result r = edit::ReplyToComment(*ed, v_.current_, picked_, text, Writer());
  if (r) box_.ReplaceText(L"");
  return Done(r, T(L"Reply posted."));
}

void PresentationView::CommentsPanel::Edit() {
  if (picked_.empty()) return;
  for (const edit::Comment& c : List()) {
    if (c.id != picked_) continue;
    // The box holds one line of up to 256 characters: a longer comment, or one with line breaks, would
    // lose text when saved from it, so it isn't edited here (never damage a file).
    if (c.text.size() > 256 || c.text.find(L'\n') != std::wstring::npos) {
      note_ = T(L"This comment is too long to edit here: reply to it instead.");
      seenState_ = UINT64_MAX;
      v_.host_.ViewInvalidate();
      return;
    }
    box_.ReplaceText(c.text);
  }
  editing_ = picked_;
  focusWanted_ = &box_;
  seenState_ = UINT64_MAX;
  v_.host_.ViewInvalidate();
}

void PresentationView::CommentsPanel::Resolve() {
  edit::Editor* ed = Ed();
  if (!ed || picked_.empty()) return;
  bool resolved = false;
  for (const edit::Comment& c : List())
    if (c.id == picked_) resolved = c.resolved;
  Done(edit::ResolveComment(*ed, v_.current_, picked_, !resolved), resolved ? T(L"Open again.") : T(L"Resolved."));
}

void PresentationView::CommentsPanel::Delete() {
  edit::Editor* ed = Ed();
  if (!ed || picked_.empty()) return;
  const edit::Result r = edit::DeleteComment(*ed, v_.current_, picked_);
  if (r) {
    picked_.clear();
    editing_.clear();
    newThread_ = true;
  }
  Done(r, T(L"Deleted."));
}

// ---------------------------------------------------------------------------
// The pane

std::vector<Control*> PresentationView::CommentsPanel::Layout(const D2D1_RECT_F& body, float ui) {
  ui_ = ui;
  Refresh();
  labels_.clear();
  std::vector<Control*> out;
  const float left = body.left + 16 * ui, right = body.right - 16 * ui, gap = 6 * ui, rowH = 30 * ui;
  float y = body.top + 10 * ui;
  auto label = [&](const std::wstring& text) {
    labels_.push_back({text, RectF(left, y, right, y + 16 * ui)});
    y += 20 * ui;
  };
  auto row = [&](std::initializer_list<Control*> controls) {
    const float n = static_cast<float>(controls.size());
    const float w = (right - left - gap * (n - 1)) / n;
    float x = left;
    for (Control* c : controls) {
      c->rc = RectF(x, y, x + w, y + rowH);
      out.push_back(c);
      x += w + gap;
    }
    y += rowH + 8 * ui;
  };
  label(T(L"Your name"));
  row({&name_});
  label(!editing_.empty() ? T(L"Edit comment") : !newThread_ && !picked_.empty() ? T(L"Reply") : T(L"New comment"));
  row({&box_});
  row({&post_});
  row({&reply_, &edit_, &resolve_, &delete_});
  noteRc_ = RectF(left, y, right, y + 18 * ui);
  y += 22 * ui;
  cardsRc_ = RectF(body.left, y, body.right, body.bottom);
  PlaceCards();
  return out;
}

void PresentationView::CommentsPanel::Refresh() {
  edit::Editor* ed = Ed();
  seenSlide_ = v_.current_;
  seenState_ = ed ? ed->StateId() : 0;
  const bool editable = ed && ed->CanEdit();
  if (!name_.Focused()) name_.SetText(Writer().name);
  name_.enabled = editable;
  cards_.clear();
  const std::vector<edit::Comment>& list = List();
  auto line = [](const edit::Comment& c) {
    return std::pair{(c.author.empty() ? std::wstring(T(L"Someone")) : c.author) + L" · " + When(c.created, c.utc), c.text};
  };
  for (const edit::Comment& c : list) {
    if (!c.parent.empty()) continue;
    Card card;
    card.id = c.id;
    card.resolved = c.resolved;
    card.legacy = c.legacy;
    card.lines.push_back(line(c));
    for (const edit::Comment& r : list)
      if (r.parent == c.id) card.lines.push_back(line(r));
    cards_.push_back(std::move(card));
  }
  const Card* pick = nullptr;
  for (const Card& k : cards_)
    if (k.id == picked_) pick = &k;
  if (!pick) {
    newThread_ = newThread_ || editing_.empty();
  }
  const bool replying = pick && !newThread_ && editing_.empty();
  box_.enabled = post_.enabled = editable;
  post_.text = !editing_.empty() ? T(L"Save") : replying ? T(L"Reply") : T(L"Post");
  box_.label = !editing_.empty() ? T(L"Edit comment") : replying ? T(L"Reply") : T(L"New comment");
  reply_.enabled = editable;
  edit_.enabled = delete_.enabled = editable && pick;
  resolve_.enabled = editable && pick && !pick->legacy;
  resolve_.text = pick && pick->resolved ? T(L"Reopen") : T(L"Resolve");
  if (cards_.empty() && note_.empty()) note_ = editable ? T(L"No comments on this slide.") : T(L"No comments.");
  else if (!cards_.empty() && note_ == T(L"No comments on this slide.")) note_.clear();
}

void PresentationView::CommentsPanel::PlaceCards() {
  Canvas measure;
  const float u = ui_;
  const float left = cardsRc_.left + 16 * u, right = cardsRc_.right - 16 * u;
  float y = cardsRc_.top - scroll_;
  for (Card& k : cards_) {
    float h = 10 * u;
    for (size_t i = 0; i < k.lines.size(); ++i) {
      const float indent = i == 0 ? 12 * u : 24 * u;
      h += 20 * u + measure.TextWrapped(k.lines[i].second, RectF(left + indent, 0, right - 12 * u, 1000), {}, 13 * u, 6, false) +
           6 * u;
    }
    k.rc = RectF(left, y, right, std::round(y + h + 4 * u));
    y = k.rc.bottom + 8 * u;
  }
  height_ = y + scroll_ - cardsRc_.top;
}

void PresentationView::CommentsPanel::Draw(Canvas& c) {
  edit::Editor* ed = Ed();
  if (seenSlide_ != v_.current_ || seenState_ != (ed ? ed->StateId() : 0)) {
    Refresh();
    PlaceCards();
  }
  if (focusWanted_) {
    focusWanted_->TakeFocus();
    focusWanted_ = nullptr;
  }
  const Palette& p = theme::P();
  const float u = ui_;
  for (const auto& [text, rc] : labels_) c.Text(text, rc, p.textSecondary, 12 * u);
  if (!note_.empty()) c.Text(note_, noteRc_, p.textSecondary, 12 * u);
  c.PushClip(cardsRc_);
  for (int i = 0; i < static_cast<int>(cards_.size()); ++i) {
    const Card& k = cards_[static_cast<size_t>(i)];
    if (k.rc.bottom < cardsRc_.top || k.rc.top > cardsRc_.bottom) continue;
    c.FillRound(k.rc, 6 * u, p.dark ? Rgb(0xFFFFFF, 0.05f) : Rgb(0xFFFFFF));
    if (i == hover_) c.FillRound(k.rc, 6 * u, p.hover);
    if (k.id == picked_) c.StrokeRound(Inflate(k.rc, -0.5f * u, -0.5f * u), 6 * u, p.accent, std::max(1.f, std::round(2 * u)));
    else c.StrokeRound(k.rc, 6 * u, p.inputBorder, 1);
    float y = k.rc.top + 10 * u;
    for (size_t l = 0; l < k.lines.size(); ++l) {
      const float indent = l == 0 ? 12 * u : 24 * u;
      const D2D1_RECT_F head = RectF(k.rc.left + indent, y, k.rc.right - 12 * u, y + 18 * u);
      const D2D1_COLOR_F text = k.resolved ? p.textSecondary : p.text;
      c.Text((l == 0 ? L"" : L"↳ ") + k.lines[l].first, head, text, 12 * u, DWRITE_FONT_WEIGHT_SEMI_BOLD);
      if (l == 0 && k.resolved) c.Text(T(L"Resolved"), head, p.textSecondary, 12 * u, DWRITE_FONT_WEIGHT_REGULAR, Align::Trailing);
      y += 20 * u;
      y += c.TextWrapped(k.lines[l].second, RectF(k.rc.left + indent, y, k.rc.right - 12 * u, k.rc.bottom), text, 13 * u, 6) +
           6 * u;
    }
  }
  c.PopClip();
}

int PresentationView::CommentsPanel::CardAt(float x, float y) const {
  if (!Contains(cardsRc_, x, y)) return -1;
  for (int i = 0; i < static_cast<int>(cards_.size()); ++i)
    if (Contains(cards_[static_cast<size_t>(i)].rc, x, y)) return i;
  return -1;
}

bool PresentationView::CommentsPanel::OnMouseDown(float x, float y, bool) {
  const int i = CardAt(x, y);
  if (i < 0) return false;
  picked_ = cards_[static_cast<size_t>(i)].id;
  editing_.clear();
  newThread_ = false;
  Refresh();
  if (box_.enabled) focusWanted_ = &box_;
  v_.host_.ViewStateChanged();
  v_.host_.ViewInvalidate();
  return true;
}

bool PresentationView::CommentsPanel::OnMouseMove(float x, float y) {
  const int i = CardAt(x, y);
  if (i == hover_) return false;
  hover_ = i;
  return true;
}

void PresentationView::CommentsPanel::OnMouseLeave() { hover_ = -1; }

bool PresentationView::CommentsPanel::OnWheel(int delta, float x, float y) {
  if (!Contains(cardsRc_, x, y)) return false;
  const float room = cardsRc_.bottom - cardsRc_.top;
  scroll_ = std::clamp(scroll_ - static_cast<float>(delta) / 120.0f * 48 * ui_, 0.0f, std::max(0.0f, height_ - room));
  PlaceCards();
  return true;
}

std::wstring PresentationView::CommentsPanel::TooltipAt(float x, float y) {
  return CardAt(x, y) >= 0 ? T(L"Click to reply; its marker is outlined on the slide") : L"";
}

}  // namespace fw::slide::view
