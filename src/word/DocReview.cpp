// Review in Word documents (plan §6.2 "Review", milestone D4): comments and tracked changes. The
// model's side is doc/edit (Editor's tracked changes, Comments.h); this is the tab's: the commands,
// going from change to change, and the Review pane.
#define SECURITY_WIN32
#include <windows.h>
#include <security.h>

#include <algorithm>
#include <cmath>

#include "Commands.h"
#include "core/Strings.h"
#include "doc/edit/Comments.h"
#include "ui/Gfx.h"
#include "ui/Icons.h"
#include "ui/Menu.h"
#include "ui/Theme.h"
#include "word/DocFile.h"
#include "word/DocPanels.h"

using namespace gfx;
namespace edit = fw::doc::edit;
using fw::doc::kNone;
using fw::doc::Pos;

namespace {

std::wstring W(std::u16string_view s) { return std::wstring(s.begin(), s.end()); }
std::wstring Wide(std::string_view s) { return fw::Utf8ToWide(std::string(s)); }
std::u16string U16(std::wstring_view s) { return std::u16string(s.begin(), s.end()); }
fw::doc::layout::TextPos ToText(Pos p) { return {p.para, p.offset}; }

std::wstring Trim(std::wstring s) {
  while (!s.empty() && iswspace(s.back())) s.pop_back();
  size_t i = 0;
  while (i < s.size() && iswspace(s[i])) ++i;
  return s.substr(i);
}

int Compare(Pos a, Pos b) { return a.para != b.para ? (a.para < b.para ? -1 : 1) : a.offset < b.offset ? -1 : a.offset > b.offset ? 1 : 0; }

// "2026-11-05T10:00:00Z" -> "5 Nov 2026"
std::wstring Day(std::string_view date) {
  static const wchar_t* kMonths[] = {L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun", L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec"};
  if (date.size() < 10) return Wide(date);
  const int y = std::atoi(std::string(date.substr(0, 4)).c_str()), m = std::atoi(std::string(date.substr(5, 2)).c_str()),
            d = std::atoi(std::string(date.substr(8, 2)).c_str());
  if (m < 1 || m > 12 || d < 1) return Wide(date);
  return std::to_wstring(d) + L" " + kMonths[m - 1] + L" " + std::to_wstring(y);
}

// Where a tracked change is, to go to: its text, a paragraph mark's end, a paragraph's text.
struct Spot {
  Pos from, to;
};
std::vector<Spot> Spots(const fw::doc::Document& doc, uint32_t story) {
  using K = fw::doc::RevisionKind;
  std::vector<Spot> out;
  for (const fw::doc::Revision& r : doc.Revisions(story)) {
    Spot s{r.start, r.end};
    const uint32_t p = doc.ParagraphOf(r.node);
    if (r.kind == K::MarkInsertion || r.kind == K::MarkDeletion) {
      if (p == kNone) continue;
      const auto len = static_cast<uint32_t>(doc.Para(p).text.size());
      s = {{p, len}, {p, len}};
    } else if (r.kind == K::ParagraphFormat && p != kNone) {
      s = {{p, 0}, {p, static_cast<uint32_t>(doc.Para(p).text.size())}};
    }
    if (s.from.para == kNone || s.to.para == kNone) continue;
    if (Compare(s.to, s.from) < 0) std::swap(s.from, s.to);
    out.push_back(s);
  }
  std::stable_sort(out.begin(), out.end(), [](const Spot& a, const Spot& b) { return Compare(a.from, b.from) < 0; });
  return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// The name changes and comments are made with

std::string DocView::ReviewerName() const {
  std::wstring name = host_.ViewReviewer();
  if (name.empty() && !DocFile::Config().user.empty()) {
    // Windows' name for you ("Ann Lee"); the sign-in name when it has none. (Snapshots and the UI
    // checks have no user: changes are by "Author" there.)
    wchar_t buf[256];
    ULONG n = 256;
    name = GetUserNameExW(NameDisplay, buf, &n) && n > 0 ? std::wstring(buf, n) : DocFile::Config().user;
  }
  return fw::WideToUtf8(Trim(name));
}

// ---------------------------------------------------------------------------
// Commands

std::optional<CommandState> DocView::QueryD4(int cmd) {
  const bool edit = Editing() && editor_;
  const bool any = !changes_.empty() || allChanges_ > 0;
  CommandState s;
  switch (cmd) {
    case CmdReviewPane:
      s = {Ready(), reviewPanel_ && host_.ViewShownPanel() == reviewPanel_.get()};
      s.label = L"Comments and changes";
      break;
    case CmdNewComment: s = {edit, false}; s.label = L"New comment"; s.shortcut = L"Ctrl+Alt+M"; break;
    case CmdTrackChanges:
      s = {edit, edit && editor_->Tracking()};
      s.label = L"Track changes";
      s.shortcut = L"Ctrl+Shift+E";
      break;
    case CmdAcceptChange: s = {edit && any, false}; s.label = L"Accept change"; break;
    case CmdRejectChange: s = {edit && any, false}; s.label = L"Reject change"; break;
    case CmdAcceptAll: s = {edit && any, false}; s.label = L"Accept all changes"; break;
    case CmdRejectAll: s = {edit && any, false}; s.label = L"Reject all changes"; break;
    case CmdNextChange: s = {Ready() && any, false}; s.label = L"Next change"; break;
    case CmdPrevChange: s = {Ready() && any, false}; s.label = L"Previous change"; break;
    case CmdMarkupSimple: s = {Ready(), markup_ == Markup::Simple}; s.label = L"Simple markup"; break;
    case CmdMarkupAll: s = {Ready() && MarkupLaidOut(), markup_ == Markup::All}; s.label = L"All markup"; break;
    case CmdMarkupNone: s = {Ready(), markup_ == Markup::None}; s.label = L"No markup"; break;
    case CmdMarkupOriginal: s = {Ready() && MarkupLaidOut(), markup_ == Markup::Original}; s.label = L"Original"; break;
    case CmdReviewerName: s = {edit, false}; s.label = L"Your name for changes and comments…"; break;
    default: return std::nullopt;
  }
  return s;
}

bool DocView::ExecuteD4(int cmd) {
  const auto q = QueryD4(cmd);
  if (!q) return false;
  if (!q->enabled) return true;
  auto panel = [this] {
    if (!reviewPanel_) reviewPanel_ = std::make_unique<ReviewPanel>(*this);
    return reviewPanel_.get();
  };
  auto setMarkup = [this](Markup m) {
    const bool relayout = (m == Markup::All || m == Markup::Original || markup_ == Markup::All || markup_ == Markup::Original);
    markup_ = m;
    if (relayout) Relayout(nullptr);
    for (auto& ch : changes_) ch.measured = false;
    host_.ViewStateChanged();
    host_.ViewInvalidate();
  };
  switch (cmd) {
    case CmdReviewPane: ShowPanel(panel(), true); return true;
    case CmdNewComment:
      ShowPanel(panel(), false);
      reviewPanel_->StartComment();
      return true;
    case CmdReviewerName:
      ShowPanel(panel(), false);
      reviewPanel_->StartName();
      return true;
    case CmdTrackChanges: {
      editor_->SetAuthor(ReviewerName());
      const bool on = !editor_->Tracking();
      const edit::Result r = editor_->SetTracking(on);
      SyncFile();
      if (!r) {
        if (!r.message.empty()) host_.ViewMessage(BannerKind::Warning, W(r.message));
      } else {
        host_.ViewMessage(BannerKind::Info, on ? L"Track Changes is on: what you change is marked, with your name."
                                               : L"Track Changes is off.");
      }
      ++editStamp_;
      host_.ViewStateChanged();
      host_.ViewInvalidate();
      return true;
    }
    case CmdAcceptChange: ReviewChange(true); return true;
    case CmdRejectChange: ReviewChange(false); return true;
    case CmdAcceptAll:
    case CmdRejectAll: {
      edit::Result r = editor_->ReviewAll(cmd == CmdAcceptAll);
      if (r && r.caret.para == kNone) r.caret = SelFrom();
      if (Apply(r) && !r.message.empty()) host_.ViewMessage(BannerKind::Info, W(r.message));
      return true;
    }
    case CmdNextChange:
    case CmdPrevChange:
      if (!GoToChange(cmd == CmdPrevChange)) host_.ViewMessage(BannerKind::Info, L"There are no tracked changes.");
      return true;
    case CmdMarkupSimple: setMarkup(Markup::Simple); return true;
    case CmdMarkupAll: setMarkup(Markup::All); return true;
    case CmdMarkupNone: setMarkup(Markup::None); return true;
    case CmdMarkupOriginal: setMarkup(Markup::Original); return true;
  }
  return false;
}

bool DocView::GoToChange(bool back) {
  if (!doc_ || !Ready()) return false;
  const Pos from = SelFrom(), to = SelTo();
  // The caret's story first, then the others in order (main text, notes, headers and footers).
  std::vector<uint32_t> order;
  const uint32_t here = from.para < doc_->Paragraphs().size() ? doc_->Para(from.para).story : doc_->MainStory();
  order.push_back(here);
  const auto& stories = doc_->Stories();
  for (size_t k = 1; k <= stories.size(); ++k) {
    const uint32_t s = static_cast<uint32_t>((here + (back ? stories.size() - k : k)) % stories.size());
    if (s == here || stories[s].kind == fw::doc::StoryKind::Comment || stories[s].kind == fw::doc::StoryKind::TextBox) continue;
    order.push_back(s);
  }
  for (size_t i = 0; i < order.size(); ++i) {
    const std::vector<Spot> spots = Spots(*doc_, order[i]);
    const Spot* pick = nullptr;
    if (i > 0) {
      if (!spots.empty()) pick = back ? &spots.back() : &spots.front();
    } else if (!back) {
      for (const Spot& s : spots)
        if (Compare(s.from, to) >= 0 && !(Compare(s.from, from) == 0 && Compare(s.to, to) == 0 && !(from == to))) {
          pick = &s;
          break;
        }
    } else {
      for (auto it = spots.rbegin(); it != spots.rend() && !pick; ++it)
        if (Compare(it->from, from) < 0) pick = &*it;
    }
    if (pick) {
      GoTo(ToText(pick->from), ToText(pick->to));
      return true;
    }
  }
  // Round again in the caret's story, from its other end.
  const std::vector<Spot> spots = Spots(*doc_, here);
  if (spots.empty()) return false;
  const Spot& s = back ? spots.back() : spots.front();
  GoTo(ToText(s.from), ToText(s.to));
  return true;
}

void DocView::ReviewChange(bool accept) {
  if (!Apply(editor_->ReviewChanges(SelFrom(), SelTo(), accept))) return;
  GoToChange(false);  // as Word's Accept and Move to Next
}

void DocView::AddD4Toolbar(ToolbarModel& m) {
  ToolItem review;
  review.cmd = CmdReviewPane;
  review.glyph = icon::Comment;
  review.toggle = true;
  review.checked = reviewPanel_ && host_.ViewShownPanel() == reviewPanel_.get();
  review.priority = 3;
  review.tooltip = editor_ && editor_->Tracking() ? L"Comments and changes (Track Changes is on)"
                                                  : L"Comments and changes";
  m.left.push_back(review);
  // Insert ▾: a comment, first among what goes at the caret.
  for (ToolItem& t : m.left) {
    if (t.cmd != CmdInsertBox) continue;
    t.choices.insert(t.choices.begin(), L"Comment (Ctrl+Alt+M)");
    auto rest = t.onChange;
    t.onChange = [this, rest](const ToolValue& v) {
      if (v.index == 0) {
        Execute(CmdNewComment);
      } else if (rest) {
        ToolValue r = v;
        --r.index;
        rest(r);
      }
    };
  }
}

void DocView::FillD4Menu(HMENU m) {
  if (!Editing()) return;
  using menu::AppendItem;
  using menu::AppendSeparator;
  // The change at the caret or in the selection.
  bool change = false;
  const Pos from = SelFrom(), to = SelTo();
  if (from.para < doc_->Paragraphs().size())
    for (const Spot& s : Spots(*doc_, doc_->Para(from.para).story))
      if (from == to ? (Compare(s.from, from) <= 0 && Compare(from, s.to) <= 0)
                     : (Compare(s.from, to) < 0 && Compare(from, s.to) < 0)) {
        change = true;
        break;
      }
  if (change) {
    AppendItem(m, CmdAcceptChange, L"Accept change");
    AppendItem(m, CmdRejectChange, L"Reject change");
  }
  AppendItem(m, CmdNewComment, L"New comment\tCtrl+Alt+M");
  AppendSeparator(m);
}

// ---------------------------------------------------------------------------
// The Review pane

DocView::ReviewPanel::ReviewPanel(DocView& view) : v_(view) {
  track_.onInvoke = [this] { v_.Execute(CmdTrackChanges); };
  markup_.label = L"Show";
  markup_.items = {L"Simple markup", L"All markup", L"No markup", L"Original"};
  markup_.onChange = [this](int i) {
    static constexpr int kCmds[] = {CmdMarkupSimple, CmdMarkupAll, CmdMarkupNone, CmdMarkupOriginal};
    if (i >= 0 && i < 4) v_.Execute(kCmds[i]);
  };
  prev_.text = L"Previous";
  prev_.onInvoke = [this] { v_.Execute(CmdPrevChange); };
  next_.text = L"Next";
  next_.onInvoke = [this] { v_.Execute(CmdNextChange); };
  accept_.text = L"Accept";
  accept_.onInvoke = [this] { v_.Execute(CmdAcceptChange); };
  reject_.text = L"Reject";
  reject_.onInvoke = [this] { v_.Execute(CmdRejectChange); };
  acceptAll_.text = L"Accept all";
  acceptAll_.onInvoke = [this] { v_.Execute(CmdAcceptAll); };
  rejectAll_.text = L"Reject all";
  rejectAll_.onInvoke = [this] { v_.Execute(CmdRejectAll); };
  name_.label = L"Your name";
  name_.autoComplete = false;
  name_.tooltip = L"Changes and comments are made with this name";
  name_.onCommit = [this](const std::wstring& text, int) {
    v_.host_.ViewSetReviewer(Trim(text));
    if (v_.editor_) v_.editor_->SetAuthor(v_.ReviewerName());
    name_.SetText(Wide(v_.ReviewerName()));
  };
  box_.label = L"Comment";
  box_.autoComplete = false;
  box_.onCommit = [this](const std::wstring&, int) {};  // Enter keeps the text; the buttons post it
  post_.text = L"Comment";
  post_.primary = true;
  post_.onInvoke = [this] { Post(); };
  reply_.text = L"Reply";
  reply_.onInvoke = [this] { Reply(); };
  edit_.text = L"Edit";
  edit_.onInvoke = [this] { Edit(); };
  resolve_.text = L"Resolve";
  resolve_.onInvoke = [this] { Resolve(); };
  delete_.text = L"Delete";
  delete_.onInvoke = [this] { Delete(); };
}

std::vector<Control*> DocView::ReviewPanel::Layout(const D2D1_RECT_F& body, float ui) {
  Refresh();
  std::vector<Control*> out = LayOutRows({{L"", {&track_}},
                                          {L"Show", {&markup_}},
                                          {L"Changes", {&prev_, &next_, &accept_, &reject_}},
                                          {L"", {&acceptAll_, &rejectAll_}},
                                          {L"Your name", {&name_}},
                                          {L"Comments", {&box_}},
                                          {L"", {&post_, &reply_}},
                                          {L"", {&edit_, &resolve_, &delete_}}},
                                         body, ui);
  list_ = RectF(body.left, noteRc_.bottom + 4 * ui, body.right, body.bottom);
  PlaceCards();
  return out;
}

void DocView::ReviewPanel::Refresh() {
  seen_ = v_.editStamp_;
  const bool editing = v_.Editing() && v_.editor_;
  track_.text = std::wstring(editing && v_.editor_->Tracking() ? L"☑" : L"☐") + L" Track changes (Ctrl+Shift+E)";
  track_.enabled = editing;
  markup_.selected = static_cast<int>(v_.markup_ == Markup::Simple ? 0 : v_.markup_ == Markup::All ? 1 : v_.markup_ == Markup::None ? 2 : 3);
  const bool any = !v_.changes_.empty() || v_.allChanges_ > 0;
  for (PushButton* b : {&prev_, &next_}) b->enabled = any;
  for (PushButton* b : {&accept_, &reject_, &acceptAll_, &rejectAll_}) b->enabled = any && editing;
  if (!name_.Focused()) name_.SetText(Wide(v_.ReviewerName()));
  name_.enabled = editing;
  box_.enabled = post_.enabled = editing;

  cards_.clear();
  const auto all = v_.doc_ ? v_.doc_->Comments() : std::vector<fw::doc::CommentInfo>();
  auto lineOf = [&](const fw::doc::CommentInfo& c) {
    std::wstring head = (c.author.empty() ? std::wstring(L"Someone") : Wide(c.author)) + L" · " + Day(c.date);
    return std::pair{head, W(edit::CommentText(*v_.doc_, c))};
  };
  for (const auto& c : all) {
    if (!c.parentId.empty()) continue;
    Card card;
    card.id = std::string(c.id);
    card.done = c.done;
    card.lines.push_back(lineOf(c));
    for (const auto& r : all)
      if (r.parentId == c.id) card.lines.push_back(lineOf(r));
    cards_.push_back(std::move(card));
  }
  // Replies whose comment isn't there (a file from elsewhere): on their own.
  for (const auto& c : all) {
    if (c.parentId.empty()) continue;
    bool parent = false;
    for (const auto& p : all) parent = parent || p.id == c.parentId;
    if (parent) continue;
    Card card;
    card.id = std::string(c.id);
    card.done = c.done;
    card.lines.push_back(lineOf(c));
    cards_.push_back(std::move(card));
  }
  const bool picked = std::any_of(cards_.begin(), cards_.end(), [&](const Card& k) { return k.id == picked_; });
  if (!picked) picked_.clear();
  bool done = false;
  for (const Card& k : cards_)
    if (k.id == picked_) done = k.done;
  reply_.enabled = edit_.enabled = resolve_.enabled = delete_.enabled = editing && picked;
  resolve_.text = done ? L"Reopen" : L"Resolve";
  post_.text = editing_.empty() ? L"Comment" : L"Save";
  if (cards_.empty()) note_ = editing ? L"No comments yet. Select text and type one above." : L"No comments.";
  else if (note_.starts_with(L"No comments")) note_.clear();
}

void DocView::ReviewPanel::PlaceCards() {
  Canvas measure;
  const float u = ui_;
  const float left = list_.left + 16 * u, right = list_.right - 16 * u;
  float y = list_.top - scroll_;
  for (Card& k : cards_) {
    float h = 10 * u;
    for (size_t i = 0; i < k.lines.size(); ++i) {
      const float indent = i == 0 ? 12 * u : 24 * u;
      h += 20 * u + measure.TextWrapped(k.lines[i].second, RectF(left + indent, 0, right - 12 * u, 1000), {}, 13 * u, 4, false) +
           6 * u;
    }
    k.rc = RectF(left, y, right, std::round(y + h + 4 * u));
    y = k.rc.bottom + 8 * u;
  }
  height_ = y + scroll_ - list_.top;
}

void DocView::ReviewPanel::Draw(Canvas& c) {
  if (seen_ != v_.editStamp_) {
    Refresh();
    PlaceCards();
  }
  DrawLabels(c);
  if (focusWanted_) {
    focusWanted_->TakeFocus();
    focusWanted_ = nullptr;
  }
  const Palette& p = theme::P();
  const float u = ui_;
  c.PushClip(list_);
  for (int i = 0; i < static_cast<int>(cards_.size()); ++i) {
    const Card& k = cards_[static_cast<size_t>(i)];
    if (k.rc.bottom < list_.top || k.rc.top > list_.bottom) continue;
    const bool picked = k.id == picked_;
    c.FillRound(k.rc, 6 * u, p.dark ? Rgb(0xFFFFFF, 0.05f) : Rgb(0xFFFFFF));
    if (i == hover_) c.FillRound(k.rc, 6 * u, p.hover);
    if (picked) c.StrokeRound(Inflate(k.rc, -0.5f * u, -0.5f * u), 6 * u, p.accent, std::max(1.f, std::round(2 * u)));
    else c.StrokeRound(k.rc, 6 * u, p.inputBorder, 1);
    float y = k.rc.top + 10 * u;
    for (size_t l = 0; l < k.lines.size(); ++l) {
      const float indent = l == 0 ? 12 * u : 24 * u;
      const D2D1_RECT_F head = RectF(k.rc.left + indent, y, k.rc.right - 12 * u, y + 18 * u);
      const D2D1_COLOR_F text = k.done ? p.textSecondary : p.text;
      c.Text((l == 0 ? L"" : L"↳ ") + k.lines[l].first, head, text, 12 * u, DWRITE_FONT_WEIGHT_SEMI_BOLD);
      if (l == 0 && k.done) c.Text(L"Resolved", head, p.textSecondary, 12 * u, DWRITE_FONT_WEIGHT_REGULAR, Align::Trailing);
      y += 20 * u;
      const float h = c.TextWrapped(k.lines[l].second, RectF(k.rc.left + indent, y, k.rc.right - 12 * u, k.rc.bottom), text,
                                    13 * u, 4);
      y += h + 6 * u;
    }
  }
  c.PopClip();
}

int DocView::ReviewPanel::CardAt(float x, float y) const {
  if (!Contains(list_, x, y)) return -1;
  for (int i = 0; i < static_cast<int>(cards_.size()); ++i)
    if (Contains(cards_[static_cast<size_t>(i)].rc, x, y)) return i;
  return -1;
}

bool DocView::ReviewPanel::OnMouseDown(float x, float y, bool) {
  const int i = CardAt(x, y);
  if (i < 0) return false;
  Pick(i);
  return true;
}

bool DocView::ReviewPanel::OnMouseMove(float x, float y) {
  const int i = CardAt(x, y);
  if (i == hover_) return false;
  hover_ = i;
  return true;
}

void DocView::ReviewPanel::OnMouseLeave() { hover_ = -1; }

bool DocView::ReviewPanel::OnWheel(int delta, float x, float y) {
  if (!Contains(list_, x, y)) return false;
  const float room = list_.bottom - list_.top;
  scroll_ = std::clamp(scroll_ - static_cast<float>(delta) / 120.0f * 48 * ui_, 0.0f, std::max(0.0f, height_ - room));
  PlaceCards();
  return true;
}

std::wstring DocView::ReviewPanel::TooltipAt(float x, float y) {
  return CardAt(x, y) >= 0 ? L"Click to go to the text it's about" : L"";
}

void DocView::ReviewPanel::Pick(int card) {
  if (card < 0 || card >= static_cast<int>(cards_.size())) return;
  picked_ = cards_[static_cast<size_t>(card)].id;
  editing_.clear();
  Refresh();
  for (const auto& c : v_.doc_->Comments())
    if (c.id == picked_) {
      const Pos from = c.start.para != kNone ? c.start : c.reference;
      const Pos to = c.end.para != kNone ? c.end : from;
      if (from.para != kNone) v_.GoTo(ToText(from), ToText(to));
    }
  v_.host_.ViewInvalidate();
}

void DocView::ReviewPanel::StartComment() {
  editing_.clear();
  Refresh();
  box_.SetText(L"");
  focusWanted_ = &box_;
  v_.host_.ViewInvalidate();
}

void DocView::ReviewPanel::StartName() {
  Refresh();
  focusWanted_ = &name_;
  v_.host_.ViewInvalidate();
}

void DocView::ReviewPanel::Done(const edit::Result& r, std::wstring ok) {
  v_.SyncFile();
  note_ = r ? std::move(ok) : W(r.message);
  ++v_.editStamp_;
  v_.annotationsStale_ = true;
  v_.TakeNewPages();
  v_.host_.ViewStateChanged();
  Refresh();
  PlaceCards();
  v_.host_.ViewInvalidate();
}

void DocView::ReviewPanel::Post() {
  if (!v_.editor_) return;
  const std::wstring text = Trim(box_.Text());
  v_.editor_->SetAuthor(v_.ReviewerName());
  if (!editing_.empty()) {
    const edit::Result r = edit::EditComment(*v_.editor_, editing_, U16(text));
    if (r) {
      editing_.clear();
      box_.SetText(L"");
    }
    Done(r, L"Saved.");
    return;
  }
  const edit::Result r = edit::AddComment(*v_.editor_, v_.SelFrom(), v_.SelTo(), U16(text));
  if (r) box_.SetText(L"");
  Done(r, L"Comment added.");
  if (r) {
    const auto all = v_.doc_->Comments();
    if (!all.empty()) picked_ = std::string(all.back().id);
    Refresh();
  }
}

void DocView::ReviewPanel::Reply() {
  if (!v_.editor_ || picked_.empty()) return;
  v_.editor_->SetAuthor(v_.ReviewerName());
  const edit::Result r = edit::ReplyToComment(*v_.editor_, picked_, U16(Trim(box_.Text())));
  if (r) box_.SetText(L"");
  Done(r, L"Reply added.");
}

void DocView::ReviewPanel::Edit() {
  if (picked_.empty()) return;
  for (const auto& c : v_.doc_->Comments())
    if (c.id == picked_) box_.SetText(W(edit::CommentText(*v_.doc_, c)));
  editing_ = picked_;
  Refresh();
  focusWanted_ = &box_;
  v_.host_.ViewInvalidate();
}

void DocView::ReviewPanel::Resolve() {
  if (!v_.editor_ || picked_.empty()) return;
  bool done = false;
  for (const Card& k : cards_)
    if (k.id == picked_) done = k.done;
  Done(edit::ResolveComment(*v_.editor_, picked_, !done), done ? L"Open again." : L"Resolved.");
}

void DocView::ReviewPanel::Delete() {
  if (!v_.editor_ || picked_.empty()) return;
  const edit::Result r = edit::DeleteComment(*v_.editor_, picked_);
  if (r) picked_.clear();
  Done(r, L"Comment deleted.");
}
