// The Word document's headers and footers edited in place, page numbers, links, bookmarks, the
// navigation pane and formatting marks (plan §6.2 "basic", milestone D3). The model's side is
// doc/edit/HeaderFooter.h and Inline.h; the panels are DocPanels.cpp.

#include <algorithm>

#include "Commands.h"
#include "core/Strings.h"
#include "doc/edit/HeaderFooter.h"
#include "doc/edit/Inline.h"
#include "ui/Gfx.h"
#include "ui/Icons.h"
#include "ui/Menu.h"
#include "ui/Theme.h"
#include "ui/Toolbar.h"
#include "word/DocPanels.h"
#include "word/DocView.h"

namespace layout = fw::doc::layout;
namespace edit = fw::doc::edit;
using fw::doc::HdrFtrType;
using fw::doc::kNone;
using namespace gfx;

namespace {

std::wstring W(std::u16string_view s) { return std::wstring(s.begin(), s.end()); }
fw::doc::Pos ToDoc(layout::TextPos p) { return {p.para, p.offset}; }

}  // namespace

// ---------------------------------------------------------------------------
// Headers and footers in place

layout::HitStories DocView::ClickStories() const {
  if (!Editing()) return layout::HitStories::All;
  return hf_ ? layout::HitStories::HeaderFooter : layout::HitStories::Body;
}

size_t DocView::CaretPage() const {
  if (!hf_ || !pagination_) return SIZE_MAX;
  return std::min(hf_->page, pagination_->PageCount() ? pagination_->PageCount() - 1 : 0);
}

int DocView::ZoneAt(int page, float py) const {
  if (page < 0 || page >= static_cast<int>(pagination_->PageCount())) return 0;
  const layout::Page& p = pagination_->GetPage(page);
  if (py < p.body.top) return -1;
  if (py >= p.body.bottom) return 1;
  return 0;
}

bool DocView::HeaderFooterClick(int x, int y, bool doubleClick) {
  if (!Editing() || placeholder_ || presentation_) return false;
  const int page = PageAtPoint(x, y);
  if (page < 0 || page >= static_cast<int>(pagination_->PageCount())) return false;
  float px, py;
  if (!ToPagePoint(page, x, y, px, py)) return false;
  const int zone = ZoneAt(page, py);
  if (!hf_) {
    if (zone == 0 || !doubleClick) return false;  // a single click there goes to the body's nearest text
    EnterHeaderFooter(page, zone > 0, std::pair{px, py});
    return true;
  }
  if (zone == 0) {
    if (doubleClick) LeaveHeaderFooter();
    return true;  // the body is dimmed: clicks there don't move the caret
  }
  if (static_cast<size_t>(page) != hf_->page || (zone > 0) != hf_->footer) {
    EnterHeaderFooter(page, zone > 0, std::pair{px, py});
    return true;
  }
  return false;  // in the one being edited: the caret goes there as in the body
}

bool DocView::EnterHeaderFooter(int page, bool footer, std::optional<std::pair<float, float>> at) {
  if (!Editing() || page < 0 || page >= static_cast<int>(pagination_->PageCount())) return false;
  const layout::Page& p = pagination_->GetPage(page);
  const bool first = page == 0 || pagination_->GetPage(page - 1).section != p.section;
  const uint32_t section = p.section;
  const HdrFtrType type = edit::PageType(*doc_, section, first, p.number);
  std::u16string why;
  const uint32_t story = edit::EnsureHeaderFooter(*editor_, section, footer, type, why);
  if (story == kNone) {
    host_.ViewMessage(BannerKind::Warning, W(why));
    return false;
  }
  if (!hf_) bodyCaret_ = focus_;
  hf_ = HeaderEdit{story, static_cast<size_t>(page), section, footer, type};
  SyncFile();  // a new header or footer is an edit
  pagination_->LayOutPages(static_cast<size_t>(page) + 1);
  TakeNewPages();
  const std::vector<uint32_t> paras = doc_->StoryParagraphs(story);
  TextPos caret{paras.empty() ? 0 : paras.front(), 0};
  if (at)
    if (auto hit = pagination_->HitTest(static_cast<size_t>(page), at->first, at->second, layout::HitStories::HeaderFooter);
        hit && StoryOf(*hit) == story)
      caret = *hit;
  anchor_ = focus_ = caret;
  goalX_ = -1;
  editor_->BreakTyping();
  CaretMoved(true);
  host_.ViewStateChanged();
  return true;
}

void DocView::LeaveHeaderFooter() {
  if (!hf_) return;
  hf_.reset();
  const std::vector<uint32_t> main = doc_->StoryParagraphs(doc_->MainStory());
  TextPos to = bodyCaret_;
  if (to.para >= doc_->Paragraphs().size() || StoryOf(to) != doc_->MainStory() ||
      to.offset > doc_->Para(to.para).text.size())
    to = {main.empty() ? 0 : main.front(), 0};
  anchor_ = focus_ = to;
  goalX_ = -1;
  if (editor_) editor_->BreakTyping();
  CaretMoved(true);
  host_.ViewStateChanged();
}

void DocView::CheckHeaderFooter() {
  if (!hf_) return;
  const auto& sections = doc_->Sections();
  bool still = hf_->section < sections.size();
  if (still) {
    const fw::doc::Section& s = sections[hf_->section];
    still = (hf_->footer ? s.footers : s.headers)[static_cast<size_t>(hf_->type)] == hf_->story;
  }
  if (!still || StoryOf(focus_) != hf_->story) {
    // Undone back to before the header was made (or the caret went elsewhere): the body again.
    if (StoryOf(focus_) == doc_->MainStory()) bodyCaret_ = focus_;
    LeaveHeaderFooter();
  }
}

std::wstring DocView::HeaderFooterLabel(int page, bool footer) const {
  const layout::Page& p = pagination_->GetPage(page);
  const bool first = page == 0 || pagination_->GetPage(page - 1).section != p.section;
  const HdrFtrType type = edit::PageType(*doc_, p.section, first, p.number);
  std::wstring label = type == HdrFtrType::First ? L"First page "
                       : type == HdrFtrType::Even ? L"Even page "
                       : doc_->Settings().evenAndOddHeaders ? L"Odd page "
                                                            : L"";
  label += footer ? (label.empty() ? L"Footer" : L"footer") : (label.empty() ? L"Header" : L"header");
  if (doc_->Sections().size() > 1) label += L" – Section " + std::to_wstring(p.section + 1);
  return label;
}

void DocView::DrawHeaderFooterMode(Canvas& c, int page) {
  if (!Editing() || presentation_ || placeholder_ || page >= static_cast<int>(pagination_->PageCount())) return;
  const Palette& pal = theme::P();
  const layout::Page& p = pagination_->GetPage(page);
  const D2D1_COLOR_F veil = Alpha(host_.ViewInverted() ? pal.paperInverted : pal.paper, 0.5f);
  if (!hf_) {
    // The headers and footers are dimmed while the body is edited, as in Word.
    c.Fill(PageRectToScreen(page, {0, 0, p.width, p.body.top}), veil);
    c.Fill(PageRectToScreen(page, {0, p.body.bottom, p.width, p.height}), veil);
    return;
  }
  c.Fill(PageRectToScreen(page, {0, p.body.top, p.width, p.body.bottom}), veil);
  const float u = ui_;
  for (bool footer : {false, true}) {
    const float y = footer ? p.body.bottom : p.body.top;
    const D2D1_RECT_F line = PageRectToScreen(page, {0, y, p.width, y});
    const float t = std::max(1.0f, std::round(u));
    for (float x = line.left; x < line.right; x += 6 * u) c.Fill(RectF(x, line.top, std::min(x + 3 * u, line.right), line.top + t), pal.accent);
    std::wstring label = HeaderFooterLabel(page, footer);
    const float w = c.MeasureText(label, 11 * u) + 12 * u, h = 18 * u;
    const float top = footer ? line.top - h : line.top + t;
    const D2D1_RECT_F box = RectF(line.left + 8 * u, top, line.left + 8 * u + w, top + h);
    c.FillRound(box, 3 * u, pal.accent);
    c.Text(label, RectF(box.left + 6 * u, box.top, box.right, box.bottom), pal.onAccent, 11 * u);
    if (p.section > 0 && edit::LinkedToPrevious(*doc_, p.section, footer, hf_->type)) {
      const std::wstring same = L"Same as previous";
      const float sw = c.MeasureText(same, 11 * u) + 12 * u;
      const D2D1_RECT_F sb = RectF(line.right - 8 * u - sw, top, line.right - 8 * u, top + h);
      c.FillRound(sb, 3 * u, pal.accent);
      c.Text(same, RectF(sb.left + 6 * u, sb.top, sb.right, sb.bottom), pal.onAccent, 11 * u);
    }
  }
}

// ---------------------------------------------------------------------------
// Formatting marks and panels

namespace {
// All markup and Original, where the layout can show them (layout::Options::showDeletions).
template <class O>
constexpr bool kMarkupOptions = requires(O o) {
  o.showDeletions = true;
  o.hideInsertions = true;
};
template <class O>
void SetMarkupOptions(O& o, bool showDeletions, bool hideInsertions) {
  if constexpr (kMarkupOptions<O>) {
    o.showDeletions = showDeletions;
    o.hideInsertions = hideInsertions;
  }
}
}  // namespace

bool DocView::MarkupLaidOut() { return kMarkupOptions<layout::Options>; }

bool DocView::RevisionShown(fw::doc::Node revision) const {
  if (!revision) return true;
  const bool deleted = revision.Is(fw::doc::Name("del")) || revision.Is(fw::doc::Name("moveFrom"));
  const bool laidOut = MarkupLaidOut();
  if (deleted) return laidOut && (markup_ == Markup::All || markup_ == Markup::Original);
  return !(laidOut && markup_ == Markup::Original);
}

layout::Options DocView::LayoutOptions() const {
  layout::Options options;
  options.showHiddenText = marks_;
  options.formattingMarks = marks_;
  FILETIME now;
  GetSystemTimeAsFileTime(&now);  // DATE and TIME fields show it, as Word updates them on opening
  options.now = static_cast<int64_t>(static_cast<uint64_t>(now.dwHighDateTime) << 32 | now.dwLowDateTime);
  SetMarkupOptions(options, markup_ == Markup::All || markup_ == Markup::Original, markup_ == Markup::Original);
  return options;
}

void DocView::ToggleMarks() {
  if (!Ready() || !doc_) return;
  marks_ = !marks_;
  // Hidden text takes room when shown: the pages are laid out again, the first on screen first.
  pagination_ = std::make_unique<layout::Layout>(*doc_, *fonts_, LayoutOptions());
  pagination_->LayOutPages(static_cast<size_t>(std::max(0, currentPage_)) + 2);
  revision_ = pagination_->Revision();
  caretRevision_ = UINT32_MAX;
  selRevision_ = UINT32_MAX;
  for (auto& m : matches_) m.measured = false;
  for (auto& cm : comments_) cm.measured = false;
  for (auto& ch : changes_) ch.measured = false;
  squiggles_.clear();
  TakeNewPages();
  host_.ViewStateChanged();
  host_.ViewInvalidate();
}

void DocView::ShowPanel(PanelContent* panel, bool toggle) {
  if (toggle && host_.ViewShownPanel() == panel) {
    host_.ViewShowPanel(nullptr);
    return;
  }
  host_.ViewShowPanel(panel);
}

void DocView::ClosePanels() {
  PanelContent* shown = host_.ViewShownPanel();
  if (shown && (shown == linkPanel_.get() || shown == bookmarkPanel_.get() || shown == pageNumberPanel_.get() ||
                shown == navPanel_.get()))
    host_.ViewShowPanel(nullptr);
}

std::vector<DocView::Heading> DocView::Headings() const {
  std::vector<Heading> out;
  if (!doc_) return out;
  for (uint32_t p : doc_->StoryParagraphs(doc_->MainStory())) {
    const int level = doc_->ResolvedPara(p).outlineLevel;
    if (level < 0 || level > 8) continue;
    std::wstring text;
    std::vector<uint32_t> map;
    VisibleText(p, text, map);
    while (!text.empty() && (text.back() == L' ' || text.back() == L'\t')) text.pop_back();
    if (text.empty()) continue;
    out.push_back({p, level, std::move(text)});
  }
  return out;
}

void DocView::GoTo(TextPos at, TextPos to) {
  if (hf_ && StoryOf(at) != hf_->story) LeaveHeaderFooter();
  anchor_ = at;
  focus_ = to;
  goalX_ = -1;
  if (editor_) editor_->BreakTyping();
  ScrollToPos(at, to);
  CaretMoved(true);
}

// ---------------------------------------------------------------------------
// Commands

std::optional<CommandState> DocView::QueryD3(int cmd) {
  CommandState s;
  const bool edit = Editing();
  switch (cmd) {
    case CmdEditHeader: s = {edit && !hf_, false}; s.label = L"Edit header"; break;
    case CmdEditFooter: s = {edit && !hf_, false}; s.label = L"Edit footer"; break;
    case CmdCloseHeaderFooter: s = {edit && hf_.has_value(), false}; s.label = L"Close header and footer"; s.shortcut = L"Esc"; break;
    case CmdDifferentFirstPage:
      s = {edit && hf_.has_value(), hf_ && edit::DifferentFirstPage(*doc_, hf_->section)};
      s.label = L"Different first page";
      break;
    case CmdDifferentOddEven:
      s = {edit && hf_.has_value(), doc_ && doc_->Settings().evenAndOddHeaders};
      s.label = L"Different odd and even pages";
      break;
    case CmdLinkToPrevious:
      s = {edit && hf_ && hf_->section > 0, hf_ && edit::LinkedToPrevious(*doc_, hf_->section, hf_->footer, hf_->type)};
      s.label = L"Link to previous";
      break;
    case CmdPageNumber: s = {edit, false}; s.label = L"Page numbers…"; break;
    case CmdInsertLink:
      s = {edit, false};
      s.label = edit && edit::LinkAt(*doc_, ToDoc(focus_)) ? L"Edit link…" : L"Insert link…";
      s.shortcut = L"Ctrl+K";
      break;
    case CmdRemoveLink: s = {edit && edit::LinkAt(*doc_, ToDoc(focus_)).has_value(), false}; s.label = L"Remove link"; break;
    case CmdOpenLink: s = {Ready() && doc_ && edit::LinkAt(*doc_, ToDoc(focus_)).has_value(), false}; s.label = L"Open link"; break;
    case CmdBookmark: s = {edit, false}; s.label = L"Bookmarks…"; s.shortcut = L"Ctrl+Shift+F5"; break;
    case CmdNavigationPane:
      s = {Ready(), navPanel_ && host_.ViewShownPanel() == navPanel_.get()};
      s.label = L"Navigation pane";
      break;
    case CmdShowMarks: s = {Ready(), marks_}; s.label = L"Show formatting marks"; s.shortcut = L"Ctrl+Shift+8"; break;
    default: return std::nullopt;
  }
  return s;
}

bool DocView::ExecuteD3(int cmd) {
  if (!QueryD3(cmd)) return false;
  if (!QueryD3(cmd)->enabled) return true;
  auto apply = [this](const edit::Result& r) {
    if (!r && !r.message.empty()) host_.ViewMessage(BannerKind::Warning, W(r.message));
    SyncFile();
    caretRevision_ = UINT32_MAX;
    TakeNewPages();
    host_.ViewStateChanged();
    host_.ViewInvalidate();
  };
  switch (cmd) {
    case CmdEditHeader:
    case CmdEditFooter: {
      int page = currentPage_;
      if (const auto& c = CaretBox()) page = static_cast<int>(c->page);
      EnterHeaderFooter(page, cmd == CmdEditFooter);
      return true;
    }
    case CmdCloseHeaderFooter: LeaveHeaderFooter(); return true;
    case CmdDifferentFirstPage:
      apply(edit::SetDifferentFirstPage(*editor_, hf_->section, !edit::DifferentFirstPage(*doc_, hf_->section)));
      CheckHeaderFooter();
      return true;
    case CmdDifferentOddEven:
      apply(edit::SetDifferentOddEven(*editor_, !doc_->Settings().evenAndOddHeaders));
      CheckHeaderFooter();
      return true;
    case CmdLinkToPrevious: {
      const bool linked = edit::LinkedToPrevious(*doc_, hf_->section, hf_->footer, hf_->type);
      apply(edit::SetLinkToPrevious(*editor_, hf_->section, hf_->footer, hf_->type, !linked));
      // The caret follows to what the section shows now.
      const fw::doc::Section& s = doc_->Sections()[hf_->section];
      const uint32_t story = (hf_->footer ? s.footers : s.headers)[static_cast<size_t>(hf_->type)];
      if (story != kNone && story != hf_->story) {
        hf_->story = story;
        const auto paras = doc_->StoryParagraphs(story);
        anchor_ = focus_ = {paras.empty() ? 0 : paras.front(), 0};
        CaretMoved(true);
      }
      return true;
    }
    case CmdPageNumber:
      if (!pageNumberPanel_) pageNumberPanel_ = std::make_unique<PageNumberPanel>(*this);
      pageNumberPanel_->Open();
      ShowPanel(pageNumberPanel_.get(), false);
      return true;
    case CmdInsertLink:
      if (!linkPanel_) linkPanel_ = std::make_unique<LinkPanel>(*this);
      linkPanel_->Open();
      ShowPanel(linkPanel_.get(), false);
      return true;
    case CmdRemoveLink: Apply(edit::RemoveLink(*editor_, ToDoc(focus_))); return true;
    case CmdOpenLink:
      if (auto link = edit::LinkAt(*doc_, ToDoc(focus_))) FollowLink(fw::Utf8ToWide(link->target));
      return true;
    case CmdBookmark:
      if (!bookmarkPanel_) bookmarkPanel_ = std::make_unique<BookmarkPanel>(*this);
      bookmarkPanel_->Open();
      ShowPanel(bookmarkPanel_.get(), false);
      return true;
    case CmdNavigationPane:
      if (!navPanel_) navPanel_ = std::make_unique<NavPanel>(*this);
      navPanel_->ShowResults(false);
      ShowPanel(navPanel_.get(), true);
      return true;
    case CmdShowMarks: ToggleMarks(); return true;
  }
  return false;
}

void DocView::AddD3Toolbar(ToolbarModel& m) {
  // Insert: links, bookmarks, headers, footers and page numbers before the date and time.
  static constexpr int kInsert[] = {CmdInsertLink, CmdBookmark, CmdEditHeader, CmdEditFooter, CmdPageNumber};
  for (ToolItem& t : m.left) {
    if (t.cmd != CmdInsertBox) continue;
    std::vector<std::wstring> mine = {L"Link… (Ctrl+K)", L"Bookmark…", L"Header", L"Footer", L"Page numbers…"};
    const auto n = static_cast<int>(mine.size());
    t.choices.insert(t.choices.begin(), mine.begin(), mine.end());
    t.tooltip = L"Insert a link, bookmark, header, footer, page numbers, the date or the time";
    auto dates = t.onChange;
    t.onChange = [this, dates, n](const ToolValue& v) {
      if (v.index >= 0 && v.index < n) {
        Execute(kInsert[v.index]);
      } else if (dates) {
        ToolValue d = v;
        d.index -= n;
        dates(d);
      }
    };
  }
  if (hf_) {
    // Header & Footer, while one is edited: its options and Close.
    ToolItem hf;
    hf.kind = ToolKind::Dropdown;
    hf.cmd = CmdCloseHeaderFooter;
    hf.width = 132;
    hf.priority = 8;
    hf.text = L"Header & footer";
    hf.tooltip = L"Header and footer options";
    hf.sepBefore = true;
    static constexpr int kItems[] = {CmdDifferentFirstPage, CmdDifferentOddEven, CmdLinkToPrevious, CmdPageNumber,
                                     CmdCloseHeaderFooter};
    for (int cmd : kItems) {
      const CommandState s = *QueryD3(cmd);
      const bool toggle = cmd == CmdDifferentFirstPage || cmd == CmdDifferentOddEven || cmd == CmdLinkToPrevious;
      hf.choices.push_back((toggle ? (s.checked ? L"☑ " : L"☐ ") : L"") + s.label + (s.shortcut.empty() ? L"" : L" (" + s.shortcut + L")"));
    }
    hf.onChange = [this](const ToolValue& v) {
      if (v.index >= 0 && v.index < static_cast<int>(std::size(kItems))) Execute(kItems[v.index]);
    };
    m.left.push_back(hf);
  }
  ToolItem marks;
  marks.cmd = CmdShowMarks;
  marks.glyph = 0x00B6;  // ¶
  marks.toggle = true;
  marks.checked = marks_;
  marks.priority = 2;
  marks.tooltip = L"Show formatting marks (Ctrl+Shift+8)";
  m.left.push_back(marks);
  ToolItem nav;
  nav.cmd = CmdNavigationPane;
  nav.glyph = 0xE700;  // the navigation button's lines
  nav.toggle = true;
  nav.checked = navPanel_ && host_.ViewShownPanel() == navPanel_.get();
  nav.priority = 2;
  nav.tooltip = L"Navigation pane: headings and search results";
  m.left.push_back(nav);
}

void DocView::FillD3Menu(HMENU m, int x, int y) {
  using menu::AppendItem;
  using menu::AppendSeparator;
  (void)x;
  (void)y;
  if (Editing()) {
    const bool link = edit::LinkAt(*doc_, ToDoc(focus_)).has_value();
    if (link) {
      AppendItem(m, CmdInsertLink, L"Edit link…\tCtrl+K");
      AppendItem(m, CmdOpenLink, L"Open link");
      AppendItem(m, CmdRemoveLink, L"Remove link");
    } else {
      AppendItem(m, CmdInsertLink, L"Link…\tCtrl+K");
    }
    if (hf_) AppendItem(m, CmdCloseHeaderFooter, L"Close header and footer\tEsc");
    AppendSeparator(m);
  }
}
