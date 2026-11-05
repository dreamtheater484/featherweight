// The body flow (Engine.h): the main story, page by page, in its sections and columns, with
// headers and footers, footnotes at the bottom of the page, floats text wraps around, Word's
// keep-with-next, keep-lines-together, widow and orphan control and page breaks, and tables whose
// rows split across pages under repeated header rows. Endnotes follow the body.
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <numeric>
#include <string_view>
#include <tuple>

#include "core/OpenTimings.h"
#include "doc/layout/Engine.h"

namespace fw::doc::layout {

namespace {

constexpr float kEpsilon = 0.01f;

struct Item {
  BlockKind kind = BlockKind::Paragraph;
  Node node;
  uint32_t para = kNone;
  uint32_t first = kNone;  // its first paragraph (a table's first cell's), for finding edits
  uint32_t section = 0;
  LineStory story = LineStory::Body;
};

struct Column {
  float left = 0, right = 0;
};


}  // namespace

struct Engine::Impl {
  std::vector<Item> items;
  size_t bodyItems = 0;  // of them, the body's; endnotes follow once added
  size_t next = 0;
  bool endnotesAdded = false;

  // The paragraph in progress (it may continue on the next page).
  std::shared_ptr<ParaText> pt;
  std::unique_ptr<ParaLines> breaker;
  float breakerWidth = 0;
  text::LineBreaker::State paraStart;
  uint32_t paraLines = 0;  // lines of it on earlier columns
  bool paraBegun = false;  // its space before and floats are placed
  bool breakAfter = false;  // it ended with a page break
  bool nextHard = false;    // the next page starts after a section break or a page break before
  bool hardStart = false;   // this page did
  float separatorHeight = -1;
  size_t anchorDrawings = 0, anchorZones = 0;  // where its floats start on the page

  // The table in progress (not changed once measured: checkpoints share it).
  std::shared_ptr<TableBox> table;
  size_t row = 0;
  std::vector<float> rowCut;  // the current row's cells, content already shown (a split row)
  // Vertically merged cells whose rows are being placed: their text runs on through the rows they
  // span, and on the next page when those rows do.
  struct Span {
    size_t row = 0, cell = 0;  // the restart cell
    size_t last = 0;           // the last row it spans
    float shown = 0;           // its content placed on earlier pages
    float top = -1;            // where its part on this page starts; < 0: not on this page yet
    size_t at = 0;             // where its lines go in the page's, in reading order
  };
  std::vector<Span> spans;

  // The page being laid out.
  bool havePage = false;
  Page page;
  Stack body;  // the page's body content, page coordinates
  PageGeom geom;
  PageFields fields;
  uint32_t section = kNone;
  std::vector<Column> cols;
  size_t col = 0;
  float top = 0, bottom = 0, y = 0;
  bool columnEmpty = true, pageEmpty = true;
  Spacing spacing;
  std::vector<Zone> zones;
  std::vector<uint32_t> notes;  // footnote stories on the page, in order
  float notesHeight = 0;        // with the separator
  // Notes stacked at a width (a page setup may change it, D3).
  struct NoteStackAt {
    float width = 0;
    Stack stack;
  };
  std::unordered_map<uint32_t, NoteStackAt> noteStacks;
  float noteWidth = 0;
  float separatorWidth = -1;  // what separatorHeight was measured at
  size_t hfLines = 0;  // header and footer lines first on the page, for refreshing page counts
  bool hfUsesCount = false;

  // keep-with-next: where the chain started on this page.
  struct Mark {
    size_t next = 0;
    size_t col = 0;
    float y = 0;
    Spacing spacing;
    size_t lines = 0, paints = 0, fills = 0, edges = 0, drawings = 0, cells = 0, zones = 0, notes = 0;
    float notesHeight = 0;
    bool columnEmpty = true;
    // Inside the table in progress (its rows kept together): the row it starts at.
    size_t row = SIZE_MAX;
    std::vector<float> rowCut;
    std::vector<Span> spans;
  };
  std::optional<Mark> chain;
  // Where the table in progress started, when it started on this page.
  std::optional<Mark> tableStart;

  // Page numbering.
  int32_t number = 0;
  size_t pagesInSection = 0;
  std::vector<size_t> sectionFirstPage;
  bool blankPending = false;
  std::vector<bool> pageUsesCount;
  // Per page: the NUMPAGES and SECTIONPAGES its header and footer show.
  std::vector<std::pair<std::u16string, std::u16string>> shownCounts;
  int stalled = 0;  // pages in a row that moved nothing on
  bool paginatedMarked = false;

  // ---- Laying out again after an edit (D2, plan §4)
  // The flow between two pages: all the next page starts from.
  struct Checkpoint {
    size_t next = 0;
    bool endnotesAdded = false;
    std::shared_ptr<ParaText> pt;  // the paragraph in progress, and where in it
    float breakerWidth = 0;
    text::LineBreaker::State state, paraStart;
    uint32_t paraLines = 0;
    bool paraBegun = false, breakAfter = false, nextHard = false;
    size_t anchorDrawings = 0, anchorZones = 0;
    std::shared_ptr<TableBox> table;  // the table in progress, and where in it
    size_t row = 0;
    std::vector<float> rowCut;
    std::vector<Span> spans;
    uint32_t section = kNone;
    int32_t number = 0;
    size_t pagesInSection = 0;
    bool blankPending = false;
    int stalled = 0;
    PageFields fields;
    int32_t footnoteCount = 0, endnoteCount = 0;
    size_t notes = 0;  // notes numbered (Engine::noteSeq_)
  };
  std::vector<Checkpoint> checkpoints;  // [i]: before page i; one more than there are pages
  // The pages after an edit that aren't laid out again yet: still shown, and taken back as they
  // are once a page laid out again ends in the state its old one did (the first's index is the
  // number of pages laid out).
  struct OldPage {
    Page page;
    Checkpoint after;
    bool usesCount = false;
    std::pair<std::u16string, std::u16string> shownCounts;
    std::vector<uint32_t> notes;  // numbered while laying it out
    uint32_t sectionStarts = 0;   // sections starting on it
  };
  std::deque<OldPage> old;
  bool oldDone = false;  // the old layout ended with its last page
  // What a page start must be past to take old pages back: the items the edits changed, and none
  // at all after one that changes what later paragraphs show (lists, fields, sections, headers).
  size_t convergeFrom = 0;
  bool noConverge = false;
  std::vector<size_t> seams;  // old pages that don't follow from the one before (from two layouts)
  // Paragraph lines laid out again (CachedPara), by w:p element; new ones are kept while laying
  // out the pages before `cacheUntil`.
  std::unordered_map<ElemId, std::shared_ptr<CachedPara>> lineCache;
  size_t cacheUntil = 0;
  uint32_t edits = 0;
};

// The first paragraph in a cell, content control or custom XML block.
static uint32_t FirstPara(const Document& doc, Node container) {
  for (const Block& b : doc.Blocks(container)) {
    if (b.kind == BlockKind::Paragraph) return b.para;
    uint32_t p = kNone;
    if (b.kind == BlockKind::Table) {
      for (Node tr : doc.TableRows(b.node)) {
        for (Node tc : doc.RowCells(tr))
          if ((p = FirstPara(doc, tc)) != kNone) return p;
      }
    } else if ((p = FirstPara(doc, b.node)) != kNone) {
      return p;
    }
  }
  return kNone;
}

// The body, flattened: paragraphs and tables in order, with their sections.
static void FlattenBody(const Document& doc, std::vector<Item>& items) {
  items.clear();
  const std::vector<Section>& sections = doc.Sections();
  uint32_t sec = 0;
  // A table's first paragraph, for its section.
  auto firstPara = [&](Node container) { return FirstPara(doc, container); };
  std::function<void(const std::vector<Block>&)> add = [&](const std::vector<Block>& blocks) {
    for (const Block& b : blocks) {
      if (b.kind == BlockKind::ContentControl || b.kind == BlockKind::CustomXml) {
        add(doc.Blocks(b.node));
        continue;
      }
      Item it;
      it.kind = b.kind;
      it.node = b.node;
      it.para = b.para;
      uint32_t first = kNone;
      if (b.kind == BlockKind::Paragraph) first = b.para;
      else
        for (Node tr : doc.TableRows(b.node)) {
          for (Node tc : doc.RowCells(tr))
            if ((first = firstPara(tc)) != kNone) break;
          if (first != kNone) break;
        }
      while (sec + 1 < sections.size() && sections[sec].endPara != kNone && first != kNone && first > sections[sec].endPara)
        ++sec;
      it.first = first;
      it.section = sec;
      items.push_back(it);
    }
  };
  add(doc.Blocks(doc.MainStory()));
}

Engine::Engine(const Document& doc, text::Fonts& fonts, const Options& options, OpenTimings* timings)
    : impl_(new Impl), doc_(doc), fonts_(fonts), options_(options), timings_(timings), generation_(doc.CacheGeneration()) {
  compatMode_ = doc.Settings().compatibilityMode > 0 ? doc.Settings().compatibilityMode : 12;
  sumSpacing_ = doc.Settings().Compat(Name("doNotUseHTMLParagraphAutoSpacing"));
  CountStories();
  FlattenBody(doc, impl_->items);
  impl_->bodyItems = impl_->items.size();
}

Engine::~Engine() = default;

const std::vector<Field>& Engine::StoryFields(uint32_t story) {
  auto it = fieldsCache_.find(story);
  if (it == fieldsCache_.end()) it = fieldsCache_.emplace(story, doc_.Fields(story)).first;
  return it->second;
}

namespace {

size_t LineBytes(const LineBox& l) {
  size_t n = sizeof(LineBox) + l.line.caretX_.capacity() * 4 + l.line.advance_.capacity() * 4 + l.line.flags_.capacity() +
             l.line.objects.capacity() * sizeof(text::InlineObject) + l.text.capacity() * 2;
  for (const text::GlyphPiece& g : l.line.pieces)
    n += sizeof(g) + g.glyphs.capacity() * 2 + g.advances.capacity() * 4 + g.offsets.capacity() * sizeof(DWRITE_GLYPH_OFFSET);
  return n;
}

size_t PageBytes(const Page& p) {
  size_t n = sizeof(Page) + p.paints.capacity() * sizeof(RunPaint) + p.fills.capacity() * sizeof(Fill) +
             p.edges.capacity() * sizeof(Edge) + p.drawings.capacity() * sizeof(DrawingBox) +
             p.cells.capacity() * sizeof(CellRect);
  for (const LineBox& l : p.lines) n += LineBytes(l);
  return n;
}

size_t TextBytes(const ParaText& pt) {
  return sizeof(ParaText) + pt.text.capacity() * 2 + pt.runs.capacity() * sizeof(text::TextRun) +
         pt.paints.capacity() * sizeof(RunPaint);
}

}  // namespace

size_t Engine::MemoryBytes() const {
  const Impl& f = *impl_;
  size_t n = sizeof(*this) + sizeof(Impl);
  for (const Page& p : pages_) n += PageBytes(p);
  for (const Impl::OldPage& o : f.old) n += PageBytes(o.page);
  // What the checkpoints keep: the paragraphs and tables in progress at page ends, each once.
  std::vector<const void*> seen;
  auto once = [&](const void* p) {
    if (!p || std::find(seen.begin(), seen.end(), p) != seen.end()) return false;
    seen.push_back(p);
    return true;
  };
  auto held = [&](const Impl::Checkpoint& c) {
    n += sizeof(c) + c.rowCut.capacity() * sizeof(float) + c.spans.capacity() * sizeof(Impl::Span);
    if (once(c.pt.get())) n += TextBytes(*c.pt);
    if (once(c.table.get()))
      for (const RowBox& row : c.table->rows)
        for (const CellBox& cell : row.cells)
          for (const LineBox& l : cell.content.lines) n += LineBytes(l);
  };
  for (const Impl::Checkpoint& c : f.checkpoints) held(c);
  for (const Impl::OldPage& o : f.old) held(o.after);
  for (const auto& [elem, cached] : f.lineCache) {
    if (once(cached->pt.get())) n += TextBytes(*cached->pt);
    for (const CachedPara::Made& m : cached->lines) {
      n += sizeof(m) + m.line.caretX_.capacity() * 4 + m.line.advance_.capacity() * 4 + m.line.flags_.capacity();
      for (const text::GlyphPiece& g : m.line.pieces)
        n += sizeof(g) + g.glyphs.capacity() * 2 + g.advances.capacity() * 4 + g.offsets.capacity() * sizeof(DWRITE_GLYPH_OFFSET);
    }
  }
  return n;
}

// ---- The flow

struct Flow {
  Engine& e;
  Engine::Impl& f;
  const Document& doc;

  explicit Flow(Engine& engine) : e(engine), f(*engine.impl_), doc(engine.doc_) {}

  using TableBox = Engine::TableBox;
  using RowBox = Engine::RowBox;
  using CellBox = Engine::CellBox;
  using Span = Engine::Impl::Span;

  const SectionProps& Props(uint32_t s) const {
    static const SectionProps none;
    return s < doc.Sections().size() ? doc.Sections()[s].props : none;
  }

  PageGeom GeomOf(uint32_t s, int32_t number) const {
    const SectionProps& p = Props(s);
    PageGeom g;
    g.width = Pt(p.pageWidth);
    g.height = Pt(p.pageHeight);
    g.left = Pt(p.marginStart);
    g.right = Pt(p.marginEnd);
    g.top = std::abs(Pt(p.marginTop));
    g.bottom = std::abs(Pt(p.marginBottom));
    g.header = Pt(p.headerDistance);
    g.footer = Pt(p.footerDistance);
    const DocSettings& set = doc.Settings();
    if (set.mirrorMargins && number % 2 == 0) std::swap(g.left, g.right);
    float gutter = Pt(p.gutter);
    if (set.gutterAtTop) g.top += gutter;
    else if (set.mirrorMargins && number % 2 == 0) g.right += gutter;
    else g.left += gutter;
    return g;
  }

  std::vector<Column> ColumnsOf(uint32_t s, float left, float right) const {
    const Columns& c = Props(s).columns;
    std::vector<Column> cols;
    float width = right - left;
    if (c.count <= 1 || width <= 0) return {{left, right}};
    if (!c.equalWidth && c.cols.size() >= static_cast<size_t>(c.count)) {
      float x = left;
      for (const Columns::Col& k : c.cols) {
        cols.push_back({x, x + Pt(k.width)});
        x += Pt(k.width) + Pt(k.space);
      }
      return cols;
    }
    float space = Pt(c.space);
    float w = (width - space * static_cast<float>(c.count - 1)) / static_cast<float>(c.count);
    for (int32_t i = 0; i < c.count; ++i) {
      float x = left + static_cast<float>(i) * (w + space);
      cols.push_back({x, x + w});
    }
    return cols;
  }

  std::u16string NumberText(uint32_t s, int32_t n) const {
    std::string_view fmt = Props(s).pageNumberFormat;
    return FormatNumber(n, fmt.empty() ? "decimal" : fmt);
  }

  // ---- Pages

  void StartPage() {
    uint32_t s = f.next < f.items.size() ? f.items[f.next].section
                 : f.section != kNone   ? f.section
                                        : 0;
    if (doc.Sections().empty()) s = 0;
    const bool newSection = s != f.section;
    if (newSection) {
      const SectionProps& p = Props(s);
      if (p.pageNumberStart) f.number = *p.pageNumberStart - 1;
      // Even and odd starts leave a blank page when the parity is wrong.
      if (f.section != kNone && !f.blankPending &&
          ((p.start == SectionStart::EvenPage && (f.number + 1) % 2 != 0) ||
           (p.start == SectionStart::OddPage && (f.number + 1) % 2 == 0))) {
        f.blankPending = true;
      }
      f.section = s;
      f.pagesInSection = 0;
      f.sectionFirstPage.push_back(e.pages_.size());
    }
    ++f.number;
    ++f.pagesInSection;
    f.page = Page();
    f.body = Stack();
    f.geom = GeomOf(s, f.number);
    f.page.width = f.geom.width;
    f.page.height = f.geom.height;
    f.page.section = s;
    f.page.number = f.number;
    f.page.numberText = NumberText(s, f.number);
    f.fields.page = f.page.numberText;
    f.zones.clear();
    f.notes.clear();
    f.notesHeight = 0;
    f.chain.reset();
    f.tableStart.reset();
    f.hfLines = 0;
    f.hfUsesCount = false;
    LayOutHeaderFooter();
    const SectionProps& p = Props(s);
    f.cols = ColumnsOf(s, f.geom.left, f.geom.width - f.geom.right);
    f.col = 0;
    f.y = f.top;
    f.columnEmpty = true;
    f.pageEmpty = true;
    f.spacing = Spacing();
    f.noteWidth = f.geom.width - f.geom.left - f.geom.right;
    (void)p;
    f.hardStart = f.nextHard || e.pages_.empty();
    f.nextHard = false;
    f.havePage = true;
  }

  // The header and footer for this page: first page, even page or default.
  void LayOutHeaderFooter() {
    float top = f.geom.top, bottom = f.geom.height - f.geom.bottom;
    if (!doc.Sections().empty()) {
      const Section& sec = doc.Sections()[f.section];
      const bool first = sec.props.titlePage && f.pagesInSection == 1;
      const bool even = doc.Settings().evenAndOddHeaders && f.number % 2 == 0;
      auto pick = [&](const std::array<uint32_t, 3>& set) {
        if (first) return set[static_cast<int>(HdrFtrType::First)];
        if (even) return set[static_cast<int>(HdrFtrType::Even)];
        return set[static_cast<int>(HdrFtrType::Default)];
      };
      Frame frame;
      frame.geom = &f.geom;
      frame.fields = &f.fields;
      frame.x = f.geom.left;
      frame.width = f.geom.width - f.geom.left - f.geom.right;
      std::vector<Zone> hfZones;
      frame.zones = &hfZones;
      uint32_t header = pick(sec.headers);
      if (header != kNone) {
        frame.y = f.geom.header;
        frame.story = LineStory::Header;
        Stack h = e.StackStory(header, frame);
        if (h.usesPageCount) f.hfUsesCount = true;
        if (Pt(sec.props.marginTop) >= 0) top = std::max(top, f.geom.header + h.height);
        f.body.Append(std::move(h), frame.x, frame.y);
      }
      uint32_t footer = pick(sec.footers);
      if (footer != kNone) {
        frame.story = LineStory::Footer;
        frame.y = 0;
        const size_t zones = hfZones.size();
        Stack ft = e.StackStory(footer, frame);
        float y = f.geom.height - f.geom.footer - ft.height;
        if (ft.placedOnPage) {
          // Again where it goes, for what's placed by the page or wraps text.
          hfZones.resize(zones);
          frame.y = y;
          ft = e.StackStory(footer, frame);
        }
        if (ft.usesPageCount) f.hfUsesCount = true;
        if (Pt(sec.props.marginBottom) >= 0) bottom = std::min(bottom, y);
        f.body.Append(std::move(ft), frame.x, y);
      }
      // Header and footer floats that wrap text push the body's text aside too.
      for (const Zone& z : hfZones) f.zones.push_back(z);
    }
    f.hfLines = f.body.lines.size();
    f.top = top;
    f.bottom = bottom;
  }

  void FinishPage() {
    // Footnotes at the bottom of the text area.
    if (!f.notes.empty()) {
      float y = f.bottom - f.notesHeight;
      Stack sep = Separator();
      float x = f.geom.left;
      if (sep.height > 0) {
        sep.edges.push_back({SeparatorBorder(), 0, sep.height / 2, std::min(144.0f, f.noteWidth), sep.height / 2});
        float h = sep.height;
        f.body.Append(std::move(sep), x, y);
        y += h;
      }
      for (uint32_t n : f.notes) {
        Stack s = NoteStack(n);
        float h = s.height;
        f.body.Append(std::move(s), x, y);
        y += h;
      }
    }
    Page& p = f.page;
    p.body = {f.geom.left, f.top, f.geom.width - f.geom.right, f.bottom};
    p.lines = std::move(f.body.lines);
    p.paints = std::move(f.body.paints);
    p.fills = std::move(f.body.fills);
    p.edges = std::move(f.body.edges);
    p.drawings = std::move(f.body.drawings);
    p.cells = std::move(f.body.cells);
    std::stable_sort(p.drawings.begin(), p.drawings.end(), [](const DrawingBox& a, const DrawingBox& b) {
      if (a.behindText != b.behindText) return a.behindText;
      if (a.floating != b.floating) return !a.floating;
      return a.z < b.z;
    });
    for (const LineBox& l : p.lines)
      if (l.story == LineStory::Body || l.story == LineStory::Cell || l.story == LineStory::TextBox) {
        p.first = {l.para, 0};
        break;
      }
    p.lines.shrink_to_fit();
    p.paints.shrink_to_fit();
    e.pages_.push_back(std::move(p));
    f.pageUsesCount.push_back(f.hfUsesCount);
    f.shownCounts.push_back({f.fields.pages, f.fields.sectionPages});
    f.havePage = false;
    if (e.pages_.size() == 1 && e.timings_) e.timings_->Mark(OpenTimings::FirstPage);
  }

  Border SeparatorBorder() const {
    Border b;
    b.style = BorderStyle::Single;
    b.width = EighthPoints(4);
    return b;
  }

  // The separator note above footnotes (or endnotes): its height, without its text (the line is
  // drawn as an edge).
  Stack Separator(bool endnote = false) {
    const std::vector<std::string_view>& ids = endnote ? doc.Settings().endnoteSeparators : doc.Settings().footnoteSeparators;
    Frame frame;
    frame.geom = &f.geom;
    frame.fields = &f.fields;
    frame.width = endnote ? ColumnRight() - ColumnLeft() : f.noteWidth;
    frame.story = endnote ? LineStory::Endnote : LineStory::Footnote;
    for (std::string_view id : ids) {
      uint32_t s = doc.NoteStory(endnote ? StoryKind::Endnote : StoryKind::Footnote, id);
      if (s == kNone) continue;
      std::string_view type = doc.NodeOf(doc.Stories()[s].root).AttrOr(Name("type"));
      if (type != "separator") continue;
      Stack st = e.StackStory(s, frame);
      st.lines.clear();  // the separator character draws a line, not text
      return st;
    }
    Stack st;
    st.height = 12;
    return st;
  }

  Stack NoteStack(uint32_t story) {
    auto it = f.noteStacks.find(story);
    if (it != f.noteStacks.end() && it->second.width == f.noteWidth) {
      Stack copy = it->second.stack;
      return copy;
    }
    Frame frame;
    frame.geom = &f.geom;
    frame.fields = &f.fields;
    frame.width = f.noteWidth;
    frame.story = doc.Stories()[story].kind == StoryKind::Endnote ? LineStory::Endnote : LineStory::Footnote;
    Stack s = e.StackStory(story, frame);
    return f.noteStacks.insert_or_assign(story, Engine::Impl::NoteStackAt{f.noteWidth, s}).first->second.stack;
  }

  float NoteHeight(uint32_t story) {
    auto it = f.noteStacks.find(story);
    return it != f.noteStacks.end() && it->second.width == f.noteWidth ? it->second.stack.height : NoteStack(story).height;
  }

  // Footnotes the cells of a row refer to between cutFrom and cutTo (null: from the top, to the
  // end), not on the page yet: their height (with the separator for the first) and stories.
  float RowNotes(const RowBox& row, const std::vector<float>* cutFrom, const std::vector<float>* cutTo,
                 std::vector<uint32_t>& add) {
    add.clear();
    float h = 0;
    for (size_t i = 0; i < row.cells.size(); ++i) {
      const float from = cutFrom ? (*cutFrom)[i] : -1, to = cutTo ? (*cutTo)[i] : FLT_MAX;
      for (const auto& [y, story] : row.cells[i].content.notes) {
        if (y <= from + kEpsilon || y > to + kEpsilon) continue;
        if (std::find(f.notes.begin(), f.notes.end(), story) != f.notes.end()) continue;
        if (std::find(add.begin(), add.end(), story) != add.end()) continue;
        if (f.notes.empty() && add.empty()) h += SeparatorHeight();
        h += NoteHeight(story);
        add.push_back(story);
      }
    }
    return h;
  }

  float SeparatorHeight() {
    if (f.separatorHeight < 0 || f.separatorWidth != f.noteWidth) {
      f.separatorHeight = Separator().height;
      f.separatorWidth = f.noteWidth;
    }
    return f.separatorHeight;
  }

  // Footnotes a line brings to the page: their height (with the separator for the first).
  float NotesFor(const ParaText& pt, const text::Line& line, std::vector<uint32_t>* add) {
    float h = 0;
    bool first = f.notes.empty();
    for (const auto& [offset, story] : pt.notes) {
      if (offset < line.start || offset >= std::max(line.end, line.start + 1)) continue;
      if (doc.Stories()[story].kind != StoryKind::Footnote) continue;
      if (std::find(f.notes.begin(), f.notes.end(), story) != f.notes.end()) continue;
      if (add && std::find(add->begin(), add->end(), story) != add->end()) continue;
      if (first) {
        h += SeparatorHeight();
        first = false;
      }
      h += NoteHeight(story);
      if (add) add->push_back(story);
    }
    return h;
  }

  float Bottom() const { return f.bottom - f.notesHeight; }

  // Whether a line shows nothing (it may hold a break, hidden text or markers).
  static bool Blank(const ParaText& pt, const text::Line& line) {
    for (char16_t c : pt.Shown(line.start, line.end, false))
      if (c != u' ' && c != u'\t') return false;
    return line.objects.empty();
  }
  float ColumnLeft() const { return f.cols[f.col].left; }
  float ColumnRight() const { return f.cols[f.col].right; }

  // Moves on to the next column; false when the page is full.
  bool NextColumn() {
    if (f.col + 1 < f.cols.size()) {
      ++f.col;
      f.y = f.top;
      f.columnEmpty = true;
      f.spacing = Spacing();
      return true;
    }
    return false;
  }

  Engine::Impl::Mark MarkNow() const {
    Engine::Impl::Mark m;
    m.next = f.next;
    m.col = f.col;
    m.y = f.y;
    m.spacing = f.spacing;
    m.lines = f.body.lines.size();
    m.paints = f.body.paints.size();
    m.fills = f.body.fills.size();
    m.edges = f.body.edges.size();
    m.drawings = f.body.drawings.size();
    m.cells = f.body.cells.size();
    m.zones = f.zones.size();
    m.notes = f.notes.size();
    m.notesHeight = f.notesHeight;
    m.columnEmpty = f.columnEmpty;
    if (f.table) {
      m.row = f.row;
      m.rowCut = f.rowCut;
      m.spans = f.spans;
    }
    return m;
  }

  void Rollback(const Engine::Impl::Mark& m) {
    f.next = m.next;
    f.col = m.col;
    f.y = m.y;
    f.spacing = m.spacing;
    f.body.lines.resize(m.lines);
    f.body.paints.resize(m.paints);
    f.body.fills.resize(m.fills);
    f.body.edges.resize(m.edges);
    f.body.drawings.resize(m.drawings);
    f.body.cells.resize(m.cells);
    f.zones.resize(m.zones);
    f.notes.resize(m.notes);
    f.notesHeight = m.notesHeight;
    f.columnEmpty = m.columnEmpty;
    f.pt.reset();
    f.breaker.reset();
    if (m.row == SIZE_MAX) {
      f.table.reset();
      return;
    }
    // Back to a row of a table: the table again (measured anew if it had ended), at that row.
    const Node tbl = f.items[m.next].node;
    if (!f.table || !(f.table->tbl == tbl)) f.table = std::make_shared<TableBox>(e.MeasureTable(tbl, BodyFrame()));
    f.row = m.row;
    f.rowCut = m.rowCut;
    f.spans = m.spans;
  }

  Frame BodyFrame() {
    Frame fr;
    fr.x = ColumnLeft();
    fr.y = 0;
    fr.width = ColumnRight() - ColumnLeft();
    fr.story = f.next < f.items.size() ? f.items[f.next].story : LineStory::Body;
    fr.geom = &f.geom;
    fr.fields = &f.fields;
    fr.zones = &f.zones;
    return fr;
  }

  enum class Step { Continue, PageFull };

  // Keeps a keep-with-next chain: true when the content that just failed to start belongs after
  // a chain begun on this page, which then moves to the next column or page as a whole.
  bool BreakChain() {
    if (!f.chain || f.chain->columnEmpty) return false;
    Engine::Impl::Mark m = *f.chain;
    Rollback(m);
    f.chain.reset();
    f.columnEmpty = false;  // the chain moves: don't start it again in this column
    return true;
  }

  Step Paragraph(const Item& item) {
    Frame frame = BodyFrame();
    if (!f.pt) {
      // After an edit, paragraphs laid out again before come from the cache.
      std::shared_ptr<CachedPara> cached = e.LinesOf(item.para, frame.width, true);
      if (cached) {
        f.pt = cached->pt;
      } else {
        f.pt = std::make_shared<ParaText>();
        e.BuildText(item.para, f.fields, *f.pt, true);
        cached = e.KeepLines(item.para, frame.width, true, f.pt);
      }
      // A paragraph nothing of which shows takes no room, and nor does an empty one that only
      // holds a section break (Word gives it no line and no spacing).
      if (f.pt->empty || (EndsSection() && f.pt->inlines.empty() && f.pt->anchors.empty() &&
                          f.pt->Shown(0, static_cast<uint32_t>(f.pt->text.size()), false).empty())) {
        f.pt.reset();
        ++f.next;
        return Step::Continue;
      }
      f.breaker = std::make_unique<ParaLines>(e.fonts_, f.pt, frame.width, std::move(cached));
      f.breakerWidth = frame.width;
      f.paraStart = f.breaker->Save();
      f.paraLines = 0;
      f.paraBegun = false;
      if (f.pt->props->pageBreakBefore && !f.pageEmpty) {
        f.nextHard = true;
        return Step::PageFull;
      }
    }
    ParaText& pt = *f.pt;
    const ParaProps& p = *pt.props;
    if (p.keepNext && !f.chain && f.paraLines == 0 && !f.paraBegun) f.chain = MarkNow();
    const bool columnWasEmpty = f.columnEmpty;
    float paraTop = f.y;
    if (!f.paraBegun) {
      f.anchorDrawings = f.body.drawings.size();
      f.anchorZones = f.zones.size();
      paraTop = f.y + (f.paraLines == 0 ? e.SpaceBefore(p, f.spacing) + e.BorderAbove(p, f.spacing) : 0);
      if (f.columnEmpty && f.spacing.prev == nullptr && f.paraLines == 0) {
        // At the top of a page Word keeps the space before only after a section break or a page
        // break before (w:pageBreakBefore), not after a page break character or where the text
        // just flowed on; at the top of a later column it keeps it (measured on the corpus:
        // dropping it there too, or always keeping it, matches fewer lines and pages).
        const bool keep = f.hardStart || f.col > 0;
        paraTop = f.y + (keep ? e.SpaceBefore(p, Spacing()) : 0.0f) + e.BorderAbove(p, Spacing());
      }
      for (uint32_t a : pt.anchors) {
        const Drawing& d = doc.Drawings()[a];
        Rect box = e.PlaceFloat(d, frame, paraTop, ColumnLeft(), ColumnRight());
        if (!d.behindDoc && d.wrap != WrapType::None) f.zones.push_back(e.ZoneOf(d, box));
        e.AddDrawing(a, box, true, frame, f.body);
      }
      f.paraBegun = true;
      f.y = paraTop;
    }
    uint32_t paintBase = static_cast<uint32_t>(f.body.paints.size());
    f.body.paints.insert(f.body.paints.end(), pt.paints.begin(), pt.paints.end());

    struct Placed {
      size_t lines, drawings, notes;
      float notesHeight;
      text::LineBreaker::State state;
      float y;
    };
    std::vector<Placed> placed;
    bool overflow = false;
    float estimate = 12;
    ParaLines& br = *f.breaker;
    while (!br.Done()) {
      text::LineBreaker::State state = br.Save();
      text::LineSpace space{f.y, ColumnLeft(), ColumnRight()};
      const bool wrap = !f.zones.empty();
      if (wrap) space = e.SpaceBeside(f.zones, f.y, estimate, ColumnLeft(), ColumnRight());
      text::Line line = br.Next(space.left - ColumnLeft(), space.right - ColumnLeft());
      float h = e.LineHeight(pt, line);
      if (wrap && h > estimate + kEpsilon) {
        text::LineSpace again = e.SpaceBeside(f.zones, f.y, h, ColumnLeft(), ColumnRight());
        if (again.left != space.left || again.right != space.right || again.top != space.top) {
          br.Restore(state);
          space = again;
          line = br.Next(space.left - ColumnLeft(), space.right - ColumnLeft());
          h = e.LineHeight(pt, line);
        }
      }
      estimate = h;
      std::vector<uint32_t> newNotes;
      float noteH = pt.notes.empty() ? 0 : NotesFor(pt, line, &newNotes);
      // A line with nothing but a page break goes on this page even when it's full: Word never
      // starts a page just for the break.
      const bool breakOnly = line.ending == text::LineEnd::PageBreak && Blank(pt, line);
      const float hang = e.Hang(pt, line, h);
      if (space.top + h - hang > Bottom() - noteH + kEpsilon && !breakOnly && !(f.columnEmpty && placed.empty())) {
        br.Restore(state);
        overflow = true;
        break;
      }
      placed.push_back({f.body.lines.size(), f.body.drawings.size(), f.notes.size(), f.notesHeight, state, f.y});
      for (uint32_t n : newNotes) f.notes.push_back(n);
      f.notesHeight += noteH;
      e.AddLine(pt, std::move(line), ColumnLeft(), space.top, h, frame, paintBase, f.body);
      if (f.body.lines.back().line.ending == text::LineEnd::PageBreak) {
        f.y = space.top + h;
        f.columnEmpty = false;
        f.pageEmpty = false;
        // (A page break here doesn't keep the space before of what starts the next page.)
        if (!br.Done() && e.compatMode_ >= 15) {
          // Word 2013 and later keep the paragraph mark after a page break on the break's line.
          text::LineBreaker::State rest = br.Save();
          text::Line tail = br.Next(0, ColumnRight() - ColumnLeft());
          if (!(br.Done() && Blank(pt, tail) && tail.objects.empty())) br.Restore(rest);
        }
        if (!br.Done()) {
          f.paraLines += static_cast<uint32_t>(placed.size());
          return PageBreakInParagraph();
        }
        f.breakAfter = true;
        break;
      }
      if (f.body.lines.back().line.ending == text::LineEnd::ColumnBreak && !br.Done()) {
        f.y = space.top + h;
        f.columnEmpty = false;
        f.pageEmpty = false;
        f.paraLines += static_cast<uint32_t>(placed.size());
        if (!NextColumn()) return Step::PageFull;
        return Step::Continue;
      }
      f.y = space.top + h;
      f.columnEmpty = false;
      f.pageEmpty = false;
    }
    if (!overflow) {
      if (p.shading.Present() && !p.shading.fill.automatic)
        f.body.fills.push_back({{ColumnLeft() + Pt(p.indStart), paraTop, ColumnRight() - Pt(p.indEnd), f.y}, p.shading.fill.rgb});
      f.spacing.after = e.SpaceAfter(p);
      f.spacing.prev = &p;
      f.spacing.atTop = false;
      if (!p.keepNext) f.chain.reset();
      f.pt.reset();
      f.breaker.reset();
      ++f.next;
      if (f.breakAfter) {
        f.breakAfter = false;
        return Step::PageFull;
      }
      return Step::Continue;
    }

    // It doesn't all fit: widow and orphan control, keep lines together.
    size_t k = placed.size();
    size_t rest = 0;
    {
      text::LineBreaker::State s = br.Save();
      while (!br.Done() && rest < 3) {
        br.Next(0, ColumnRight() - ColumnLeft());
        ++rest;
      }
      br.Restore(s);
    }
    size_t remove = 0;
    const uint32_t before = f.paraLines;
    if (p.keepLines && before == 0) remove = k;
    else if (p.widowControl) {
      if (before == 0 && k == 1) remove = 1;
      else if (rest == 1 && k >= 1) {
        remove = 1;
        if (before == 0 && k - remove == 1) remove = k;
      }
    }
    if (remove == k && before == 0 && columnWasEmpty && !f.chain) remove = 0;  // moving wouldn't help
    if (remove > 0) {
      const Placed& m = placed[k - remove];
      f.body.lines.resize(m.lines);
      f.body.drawings.resize(m.drawings);
      f.notes.resize(m.notes);
      f.notesHeight = m.notesHeight;
      br.Restore(m.state);
      f.y = m.y;
    }
    const size_t kept = k - remove;
    if (kept == 0 && before == 0) {
      // None of it stays here: it starts again in the next column, floats and all.
      f.body.drawings.resize(std::min(f.body.drawings.size(), f.anchorDrawings));
      f.zones.resize(std::min(f.zones.size(), f.anchorZones));
      f.body.paints.resize(paintBase);
      br.Restore(f.paraStart);
      f.paraBegun = false;
      if (f.chain && BreakChain()) return NextColumn() ? Step::Continue : Step::PageFull;
      if (f.chain && f.chain->next == f.next) f.chain.reset();
    }
    f.paraLines += static_cast<uint32_t>(kept);
    if (kept > 0) f.chain.reset();
    return NextColumn() ? Step::Continue : Step::PageFull;
  }

  // Where a split row's cell is cut, for widow and orphan control and keep lines together: the
  // paragraph the cut goes through keeps two lines on each side of it, or all of them together.
  float KeepTogether(const Stack& s, float start, float to) const {
    const LineBox* next = nullptr;
    for (const LineBox& l : s.lines)
      if (l.top >= to - kEpsilon && (!next || l.top < next->top)) next = &l;
    if (!next || next->para == kNone) return to;
    const uint32_t para = next->para;
    std::vector<float> above;
    size_t below = 0;
    for (const LineBox& l : s.lines) {
      if (l.para != para) continue;
      if (l.top >= to - kEpsilon) ++below;
      else if (l.top >= start - kEpsilon) above.push_back(l.top);
    }
    if (above.empty()) return to;
    const ParaProps& p = doc.ResolvedPara(para);
    size_t keep = above.size();
    if (p.keepLines) keep = 0;
    else if (p.widowControl) {
      if (below == 1) --keep;    // its last line doesn't go on alone
      if (keep == 1) keep = 0;   // nor does its first stay alone
    }
    if (keep == above.size()) return to;
    std::sort(above.begin(), above.end());
    return std::max(start, above[keep]);
  }

  // Whether the current item is the last of its section: the paragraph that holds its w:sectPr.
  bool EndsSection() const {
    return f.next + 1 < f.items.size() && f.items[f.next + 1].section != f.items[f.next].section;
  }

  // A page break inside a paragraph: the rest of it starts the next page.
  Step PageBreakInParagraph() {
    f.paraBegun = true;
    return Step::PageFull;
  }

  // What a merged cell's text still needs below where its part on this page starts.
  float SpanNeed(const Span& s) const {
    const CellBox& c = f.table->rows[s.row].cells[s.cell];
    return c.content.height - s.shown + c.marginTop + c.marginBottom;
  }

  // How far a merged cell's text goes on this page when its part there, starting at `top`, ends
  // at `bottom`.
  float SpanCut(const Span& s, float top, float bottom) const {
    const CellBox& c = f.table->rows[s.row].cells[s.cell];
    const float limit = s.shown + (bottom - top) - c.marginTop - c.marginBottom;
    float to = s.shown;
    for (const LineBox& l : c.content.lines)
      if (l.top >= s.shown - kEpsilon && l.top + l.height <= limit + kEpsilon) to = std::max(to, l.top + l.height);
    return KeepTogether(c.content, s.shown, to);
  }

  // Places the text of a merged cell's part on this page: what fits above `bottom`, or with
  // `bottom` < 0 all the rest, its rows having ended at f.y.
  void FlushSpan(Span& s, float x, float bottom) {
    if (s.top < 0) return;
    const CellBox& c = f.table->rows[s.row].cells[s.cell];
    float to = 1e30f, vshift = 0;
    if (bottom >= 0) {
      to = SpanCut(s, s.top, bottom);
    } else if (s.shown <= 0) {
      vshift = Engine::VShift(c, f.y - s.top - c.marginTop - c.marginBottom);
    }
    // Its lines go where the row's cells to its left end, in reading order.
    const size_t old = f.body.lines.size();
    Engine::EmitCell(c, x, s.top, (bottom >= 0 ? bottom : f.y) - s.top, s.shown, to, vshift, nullptr, f.body);
    const size_t added = f.body.lines.size() - old;
    const size_t at = std::min(s.at, old);
    std::rotate(f.body.lines.begin() + static_cast<std::ptrdiff_t>(at), f.body.lines.begin() + static_cast<std::ptrdiff_t>(old),
                f.body.lines.end());
    for (Span& other : f.spans)
      if (&other != &s && other.top >= 0 &&
          (other.at > at || (other.at == at && f.table->rows[other.row].cells[other.cell].x > c.x)))
        other.at += added;
    s.shown = to;
    s.top = -1;
  }

  // Where a merged cell's lines go among those of the row that starts its part on this page: after
  // the row's cells to its left, which EmitRow placed from line `rowStart` on.
  size_t SpanAt(const Span& s, float x, size_t rowStart) const {
    const float left = x + f.table->rows[s.row].cells[s.cell].x - 0.5f;
    size_t at = rowStart;
    while (at < f.body.lines.size() && f.body.lines[at].x < left) ++at;
    return at;
  }

  // Row f.row, placed at `top` from line `rowStart` on: merged cells already open start their
  // part on this page there, and on its first part, the merged cells that start in it open.
  void StartSpans(float x, float top, size_t rowStart, bool firstPart) {
    for (Span& s : f.spans)
      if (s.top < 0) {
        s.top = top;
        s.at = SpanAt(s, x, rowStart);
      }
    if (!firstPart) return;
    const TableBox& t = *f.table;
    const RowBox& row = t.rows[f.row];
    for (size_t i = 0; i < row.cells.size(); ++i) {
      const CellBox& c = row.cells[i];
      if (c.continued || c.rowSpan <= 1) continue;
      Span s{f.row, i, std::min(t.rows.size() - 1, f.row + static_cast<size_t>(c.rowSpan) - 1), 0, top};
      s.at = SpanAt(s, x, rowStart);
      f.spans.push_back(s);
    }
  }

  // Row f.row placed, whole or its last part, from `top` to f.y and from line `rowStart` on:
  // merged cells start in it, and those that end in it place the rest of their text.
  void RowPlaced(float x, float top, size_t rowStart, bool firstPart) {
    StartSpans(x, top, rowStart, firstPart);
    for (size_t k = 0; k < f.spans.size();) {
      if (f.spans[k].last <= f.row) {
        FlushSpan(f.spans[k], x, -1);
        f.spans.erase(f.spans.begin() + static_cast<std::ptrdiff_t>(k));
      } else {
        ++k;
      }
    }
  }

  // The page or column ends inside the table: merged cells place what fits of their text above f.y.
  void FlushSpans(float x) {
    for (Span& s : f.spans) FlushSpan(s, x, f.y);
  }

  // The table as it was measured before an edit before it: an old page's flow went on in it.
  std::shared_ptr<TableBox> OldTable(Node tbl) const {
    for (const OldPage& o : f.old)
      if (o.after.table && o.after.table->tbl == tbl) return o.after.table;
    return nullptr;
  }

  Step Table(const Item& item) {
    Frame frame = BodyFrame();
    if (!f.table) {
      f.tableStart = MarkNow();
      // Laid out again after an edit before it: the table measured before, as it was when nothing
      // in it changed, or with the cells that didn't change kept (cells with list labels are
      // measured again: an edit before them may number them again).
      const std::shared_ptr<TableBox> before = OldTable(item.node);
      if (before && e.BringUpToDate(*before, frame.width, frame.story)) f.table = before;
      else
        f.table = std::make_shared<TableBox>(
            e.MeasureTable(item.node, frame, before && before->frameWidth == frame.width ? before.get() : nullptr));
      f.row = 0;
      f.rowCut.clear();
      f.spans.clear();
      // The space after the paragraph before it (and its bottom border) stay above the table.
      f.y += f.spacing.after + e.BorderBelow(f.spacing);
      f.spacing = Spacing();
      f.spacing.atTop = false;
    }
    const TableBox& t = *f.table;
    const float x = ColumnLeft() + t.left;
    bool firstOnPage = f.columnEmpty;
    bool fresh = f.columnEmpty;  // nothing above but repeated header rows
    // Repeated header rows at the top of a column the table continues in.
    if (f.columnEmpty && f.row > 0 && f.row >= t.headerRows && t.headerRows > 0) {
      for (size_t r = 0; r < t.headerRows; ++r) {
        e.EmitRow(t, r, x, f.y, t.rows[r].height, nullptr, nullptr, f.body, r == 0, false);
        f.y += t.rows[r].height;
      }
      firstOnPage = false;
      f.columnEmpty = false;
    }
    while (f.row < t.rows.size()) {
      const RowBox& row = t.rows[f.row];
      if (row.props.hidden) {
        ++f.row;
        continue;
      }
      const bool firstPart = f.rowCut.empty();
      // A row whose first paragraph has a page break before starts a page, as that paragraph
      // would outside a table (measured on the corpus: Word does it for the row's first cell).
      if (firstPart && row.breakBefore && !f.pageEmpty) {
        FlushSpans(x);
        f.nextHard = true;
        return Step::PageFull;
      }
      float h = row.height - row.grown;
      if (!firstPart) {
        // The rest of a split row: its tallest remaining cell.
        h = 0;
        for (size_t i = 0; i < row.cells.size(); ++i) {
          const CellBox& c = row.cells[i];
          if (c.continued || c.rowSpan > 1 || c.turn) continue;
          h = std::max(h, c.content.height - f.rowCut[i] + c.marginTop + c.marginBottom);
        }
        if (row.props.heightRule == "exact") h = std::max(0.0f, row.height - f.rowCut.front());
      }
      // Merged cells that end in this row make it tall enough for the rest of their text.
      if (row.props.heightRule != "exact")
        for (const Span& s : f.spans)
          if (s.last == f.row) h = std::max(h, SpanNeed(s) - (s.top >= 0 ? f.y - s.top : 0.0f));
      const bool last = f.row + 1 == t.rows.size();
      const std::vector<float>* from = f.rowCut.empty() ? nullptr : &f.rowCut;
      std::vector<uint32_t> notes;
      float notesH = RowNotes(row, from, nullptr, notes);
      auto takeNotes = [&]() {
        f.notes.insert(f.notes.end(), notes.begin(), notes.end());
        f.notesHeight += notesH;
      };
      if (f.y + h <= Bottom() - notesH + kEpsilon || ((f.columnEmpty || fresh) && (row.props.cantSplit || h <= 0))) {
        takeNotes();
        const size_t rowStart = f.body.lines.size();
        // A row kept with the next starts a chain, if none runs yet; header rows, kept rows and
        // what precedes them stay together with the first row that isn't kept.
        // (Not while merged cells are open: their text goes in among earlier lines, which a
        // rollback to the chain's start couldn't take back.)
        if (row.keepNext && !f.chain && firstPart && f.spans.empty()) {
          f.chain = MarkNow();
          if (fresh) f.chain->columnEmpty = true;  // only repeated header rows above: moving won't help
        }
        e.EmitRow(t, f.row, x, f.y, h, f.rowCut.empty() ? nullptr : &f.rowCut, nullptr, f.body, firstOnPage, last, false);
        const float rowTop = f.y;
        f.y += h;
        RowPlaced(x, rowTop, rowStart, firstPart);
        f.columnEmpty = false;
        f.pageEmpty = false;
        firstOnPage = false;
        f.rowCut.clear();
        ++f.row;
        if (f.row > t.headerRows && !row.keepNext) f.chain.reset();
        fresh = false;
        continue;
      }
      // It doesn't fit. Rows that can't split, and rows of which nothing fits, move on.
      float room = Bottom() - f.y;
      std::vector<float> cutTo(row.cells.size(), 0);
      bool any = false, allDone = true;
      auto cut = [&]() {
        any = false;
        allDone = true;
        bool starved = false;
        for (size_t i = 0; i < row.cells.size(); ++i) {
          const CellBox& c = row.cells[i];
          if (c.continued || c.rowSpan > 1 || c.turn) continue;  // vertical text doesn't split
          float start = f.rowCut.empty() ? 0 : f.rowCut[i];
          float limit = start + room - c.marginTop - c.marginBottom;
          float to = start;
          for (const LineBox& l : c.content.lines)
            if (l.top >= start - kEpsilon && l.top + l.height <= limit + kEpsilon) to = std::max(to, l.top + l.height);
          to = KeepTogether(c.content, start, to);
          cutTo[i] = to;
          if (to > start + kEpsilon) any = true;
          bool rest = false;
          for (const LineBox& l : c.content.lines)
            if (l.top >= to - kEpsilon) rest = true;
          if (rest) allDone = false;
          if (rest && to <= start + kEpsilon) starved = true;
        }
        // Word splits a row only where every cell with text left keeps some of it on this page.
        if (starved) any = false;
        // Merged cells ending in this row whose text doesn't all fit go on too, and the row
        // splits for them when some of it fits.
        for (const Span& s : f.spans) {
          if (s.last != f.row || SpanNeed(s) <= (s.top >= 0 ? f.y - s.top : 0.0f) + room + kEpsilon) continue;
          allDone = false;
          if (!starved && SpanCut(s, s.top >= 0 ? s.top : f.y, f.y + room) > s.shown + kEpsilon) any = true;
        }
      };
      if (!row.props.cantSplit || f.columnEmpty || fresh) {
        cut();
        // The footnotes of what fits take room from it: cut again above them.
        if (any && !allDone && (notesH = RowNotes(row, from, &cutTo, notes)) > 0) {
          room -= notesH;
          cut();
          notesH = RowNotes(row, from, &cutTo, notes);
        }
      }
      if (!any || allDone) {
        if (f.columnEmpty || fresh) {
          // Taller than a page and nothing splits: it goes here anyway.
          notesH = RowNotes(row, from, nullptr, notes);
          takeNotes();
          const size_t rowStart = f.body.lines.size();
          e.EmitRow(t, f.row, x, f.y, h, f.rowCut.empty() ? nullptr : &f.rowCut, nullptr, f.body, firstOnPage, last, false);
          const float rowTop = f.y;
          f.y += h;
          RowPlaced(x, rowTop, rowStart, firstPart);
          f.columnEmpty = false;
          f.pageEmpty = false;
          f.rowCut.clear();
          ++f.row;
          continue;
        }
        if (f.rowCut.empty() && BreakChain()) return NextColumn() ? Step::Continue : Step::PageFull;
        // Word doesn't leave a table's header rows alone at the foot of a page: the table starts
        // on the next one, with what's kept with it.
        if (f.rowCut.empty() && f.row > 0 && f.row <= t.headerRows && f.tableStart && !f.tableStart->columnEmpty) {
          if (BreakChain()) return NextColumn() ? Step::Continue : Step::PageFull;
          Rollback(*f.tableStart);
          f.tableStart.reset();
          f.columnEmpty = false;
          return NextColumn() ? Step::Continue : Step::PageFull;
        }
        FlushSpans(x);
        return NextColumn() ? Step::Continue : Step::PageFull;
      }
      takeNotes();
      const size_t rowStart = f.body.lines.size();
      e.EmitRow(t, f.row, x, f.y, room, f.rowCut.empty() ? nullptr : &f.rowCut, &cutTo, f.body, firstOnPage, true, false);
      // The row's merged cells start here; with the others, they place what fits on this page.
      StartSpans(x, f.y, rowStart, firstPart);
      f.y += room;
      FlushSpans(x);
      f.columnEmpty = false;
      f.pageEmpty = false;
      f.rowCut = cutTo;
      return NextColumn() ? Step::Continue : Step::PageFull;
    }
    // Rows a merged cell claims to span that the table doesn't have.
    for (Span& s : f.spans) FlushSpan(s, x, -1);
    f.spans.clear();
    f.table.reset();
    ++f.next;
    f.spacing = Spacing();
    f.spacing.atTop = false;
    return Step::Continue;
  }

  // Endnotes, after the body: each note's blocks as flow items.
  void AddEndnotes() {
    f.endnotesAdded = true;
    const size_t before = f.items.size();
    AddEndnoteItems(f.section == kNone ? 0 : f.section);
    if (f.items.size() == before) return;
    // Word's separator above them where they start, after the body's last space: the separator
    // note's height, a line across its first two inches (as above footnotes).
    Stack sep = Separator(true);
    const float y = f.spacing.atTop ? f.y : f.y + f.spacing.after;
    if (sep.height <= 0 || y + sep.height > Bottom() + kEpsilon) return;
    const float x = ColumnLeft(), width = ColumnRight() - ColumnLeft();
    sep.edges.push_back({SeparatorBorder(), 0, sep.height / 2, std::min(144.0f, width), sep.height / 2});
    const float h = sep.height;
    f.body.Append(std::move(sep), x, y);
    f.y = y + h;
    f.spacing = Spacing();
    f.spacing.atTop = false;
  }

  void AddEndnoteItems(uint32_t sec) {
    std::vector<std::pair<int32_t, uint32_t>> order;
    for (uint32_t s = 0; s < doc.Stories().size(); ++s) {
      if (doc.Stories()[s].kind != StoryKind::Endnote) continue;
      std::u16string mark = e.NoteMark(s);
      if (mark.empty()) continue;
      auto it = e.noteOrder_.find(s);
      order.push_back({it == e.noteOrder_.end() ? 0 : it->second, s});
    }
    std::sort(order.begin(), order.end());
    std::function<void(const std::vector<Block>&)> add = [&](const std::vector<Block>& blocks) {
      for (const Block& b : blocks) {
        if (b.kind == BlockKind::ContentControl || b.kind == BlockKind::CustomXml) {
          add(doc.Blocks(b.node));
          continue;
        }
        Item it;
        it.kind = b.kind;
        it.node = b.node;
        it.para = b.para;
        it.section = sec;
        it.story = LineStory::Endnote;
        f.items.push_back(it);
      }
    };
    for (const auto& [n, s] : order) add(doc.Blocks(s));
  }

  // ---- Laying out again after an edit (D2, plan §4)
  //
  // Each page's checkpoint is the flow before it. An edit keeps the pages before the one ahead of
  // the change (whose end the change may move) and lays out again from there. The pages after it
  // stay as old pages, shown until replaced; once a page laid out again ends in the state its old
  // one did, past what changed, the old pages after it are taken back as they are.

  using Checkpoint = Engine::Impl::Checkpoint;
  using OldPage = Engine::Impl::OldPage;

  Checkpoint Save() const {
    Checkpoint c;
    c.next = f.next;
    c.endnotesAdded = f.endnotesAdded;
    c.pt = f.pt;
    if (f.pt) {
      c.breakerWidth = f.breakerWidth;
      c.state = f.breaker->Save();
      c.paraStart = f.paraStart;
      c.paraLines = f.paraLines;
      c.paraBegun = f.paraBegun;
      c.breakAfter = f.breakAfter;
      c.anchorDrawings = f.anchorDrawings;
      c.anchorZones = f.anchorZones;
    }
    c.nextHard = f.nextHard;
    c.table = f.table;
    if (f.table) {
      c.row = f.row;
      c.rowCut = f.rowCut;
      c.spans = f.spans;
    }
    c.section = f.section;
    c.number = f.number;
    c.pagesInSection = f.pagesInSection;
    c.blankPending = f.blankPending;
    c.stalled = f.stalled;
    c.fields = f.fields;
    c.footnoteCount = e.footnoteCount_;
    c.endnoteCount = e.endnoteCount_;
    c.notes = e.noteSeq_.size();
    return c;
  }

  void Restore(const Checkpoint& c) {
    // Notes numbered after it are numbered again when they come.
    while (e.noteSeq_.size() > c.notes) {
      const uint32_t s = e.noteSeq_.back();
      e.noteSeq_.pop_back();
      e.noteNumbers_.erase(s);
      e.noteOrder_.erase(s);
      f.noteStacks.erase(s);
    }
    e.footnoteCount_ = c.footnoteCount;
    e.endnoteCount_ = c.endnoteCount;
    f.next = c.next;
    f.endnotesAdded = c.endnotesAdded;
    if (!c.endnotesAdded) f.items.resize(f.bodyItems);
    else if (f.items.size() == f.bodyItems) AddEndnoteItems(c.section == kNone ? 0 : c.section);
    f.pt = c.pt;
    f.breaker.reset();
    if (c.pt) {
      std::shared_ptr<CachedPara> cached;
      if (auto it = f.lineCache.find(doc.Para(c.pt->para).elem); it != f.lineCache.end() && it->second->pt == c.pt)
        cached = it->second;
      f.breaker = std::make_unique<ParaLines>(e.fonts_, c.pt, c.breakerWidth, std::move(cached));
      f.breaker->Restore(c.state);
      f.breakerWidth = c.breakerWidth;
      f.paraStart = c.paraStart;
      f.paraLines = c.paraLines;
      f.paraBegun = c.paraBegun;
      f.breakAfter = c.breakAfter;
      f.anchorDrawings = c.anchorDrawings;
      f.anchorZones = c.anchorZones;
    } else {
      f.paraLines = 0;
      f.paraBegun = f.breakAfter = false;
    }
    f.nextHard = c.nextHard;
    f.table = c.table;
    f.row = c.table ? c.row : 0;
    f.rowCut = c.table ? c.rowCut : std::vector<float>();
    f.spans = c.table ? c.spans : std::vector<Span>();
    f.section = c.section;
    f.number = c.number;
    f.pagesInSection = c.pagesInSection;
    f.blankPending = c.blankPending;
    f.stalled = c.stalled;
    f.fields = c.fields;
    f.havePage = false;
    f.chain.reset();
    f.tableStart.reset();
  }

  // Whether the flow is in the same state: the next page would come out the same.
  static bool Same(const Checkpoint& a, const Checkpoint& b) {
    if (a.next != b.next || a.endnotesAdded != b.endnotesAdded || a.nextHard != b.nextHard || !a.pt != !b.pt ||
        !a.table != !b.table)
      return false;
    if (a.pt && (a.pt->para != b.pt->para || a.breakerWidth != b.breakerWidth || !SameState(a.state, b.state) ||
                 !SameState(a.paraStart, b.paraStart) || a.paraLines != b.paraLines || a.paraBegun != b.paraBegun ||
                 a.breakAfter != b.breakAfter || a.anchorDrawings != b.anchorDrawings || a.anchorZones != b.anchorZones))
      return false;
    if (a.table) {
      if (!(a.table->tbl == b.table->tbl) || a.row != b.row || a.rowCut != b.rowCut || a.spans.size() != b.spans.size())
        return false;
      for (size_t i = 0; i < a.spans.size(); ++i) {
        const Span &x = a.spans[i], &y = b.spans[i];
        if (x.row != y.row || x.cell != y.cell || x.last != y.last || x.shown != y.shown || x.top != y.top || x.at != y.at)
          return false;
      }
    }
    return a.section == b.section && a.number == b.number && a.pagesInSection == b.pagesInSection &&
           a.blankPending == b.blankPending && a.stalled == b.stalled && a.fields.page == b.fields.page &&
           a.fields.pages == b.fields.pages && a.fields.sectionPages == b.fields.sectionPages &&
           a.footnoteCount == b.footnoteCount && a.endnoteCount == b.endnoteCount && a.notes == b.notes;
  }

  // Whether the flow has begun `item`, or passed it.
  static bool Touched(const Checkpoint& c, size_t item) { return c.next > item || (c.next == item && (c.pt || c.table)); }

  void ForgetOld() {
    f.old.clear();
    f.oldDone = false;
    f.convergeFrom = 0;
    f.noConverge = false;
    f.seams.clear();
  }

  // After a page is laid out again: the old one goes, and when the flow ends that page as the old
  // one ended, the old pages after it come back as they are (up to where two old layouts meet).
  void Converge() {
    if (f.old.empty()) return;
    const size_t next = e.pages_.size();
    while (!f.seams.empty() && f.seams.front() < next) f.seams.erase(f.seams.begin());
    const Checkpoint& now = f.checkpoints.back();
    const bool same = !e.done_ && !f.noConverge && now.next >= f.convergeFrom && Same(now, f.old.front().after);
    f.old.pop_front();
    if (e.done_ || (!same && f.old.empty())) {
      ForgetOld();
      return;
    }
    if (!same) return;
    const size_t stop = f.seams.empty() ? SIZE_MAX : f.seams.front();
    bool taken = false;
    while (!f.old.empty() && e.pages_.size() < stop) {
      OldPage& o = f.old.front();
      for (uint32_t i = 0; i < o.sectionStarts; ++i) f.sectionFirstPage.push_back(e.pages_.size());
      e.pages_.push_back(std::move(o.page));
      f.pageUsesCount.push_back(o.usesCount);
      f.shownCounts.push_back(std::move(o.shownCounts));
      for (uint32_t s : o.notes) e.NoteReference(s);
      f.checkpoints.push_back(std::move(o.after));
      f.old.pop_front();
      taken = true;
    }
    if (taken) Restore(f.checkpoints.back());
    if (f.old.empty()) {
      if (taken) e.done_ = f.oldDone;
      ForgetOld();
    }
  }

  // Pages from `r` on become old pages, and the flow goes back to before page r (restore the
  // checkpoint returned once the items are up to date).
  Checkpoint MoveToOld(size_t r) {
    const size_t n = e.pages_.size();
    std::deque<OldPage> older = std::move(f.old);
    f.old.clear();
    if (older.empty()) f.oldDone = e.done_;
    for (size_t p = r; p < n; ++p) {
      OldPage o;
      o.page = std::move(e.pages_[p]);
      o.after = std::move(f.checkpoints[p + 1]);
      o.usesCount = p < f.pageUsesCount.size() && f.pageUsesCount[p];
      if (p < f.shownCounts.size()) o.shownCounts = f.shownCounts[p];
      o.notes.assign(e.noteSeq_.begin() + static_cast<std::ptrdiff_t>(f.checkpoints[p].notes),
                     e.noteSeq_.begin() + static_cast<std::ptrdiff_t>(f.checkpoints[p + 1].notes));
      o.sectionStarts = static_cast<uint32_t>(std::count(f.sectionFirstPage.begin(), f.sectionFirstPage.end(), p));
      f.old.push_back(std::move(o));
    }
    if (!older.empty()) {
      if (r < n) f.seams.insert(f.seams.begin(), n);
      for (OldPage& o : older) f.old.push_back(std::move(o));
    }
    Checkpoint c = f.checkpoints[r];
    e.pages_.resize(r);
    f.checkpoints.resize(r + 1);
    f.pageUsesCount.resize(std::min(f.pageUsesCount.size(), r));
    f.shownCounts.resize(std::min(f.shownCounts.size(), r));
    while (!f.sectionFirstPage.empty() && f.sectionFirstPage.back() >= r) f.sectionFirstPage.pop_back();
    e.done_ = false;
    return c;
  }

  // The flow before shown page p (laid out or old).
  const Checkpoint& Before(size_t p) const {
    const size_t n = e.pages_.size();
    return p <= n ? f.checkpoints[p] : f.old[p - n - 1].after;
  }

  // The last body item that starts at or before paragraph `para`.
  size_t ItemOf(uint32_t para) const {
    auto end = f.items.begin() + static_cast<std::ptrdiff_t>(f.bodyItems);
    auto it = std::upper_bound(f.items.begin(), end, para, [](uint32_t p, const Item& i) { return p < i.first; });
    return it == f.items.begin() ? 0 : static_cast<size_t>(it - f.items.begin()) - 1;
  }

  void Edited(uint32_t story, uint32_t first, uint32_t removed, uint32_t added, bool structure) {
    ++e.revision_;
    const int32_t shift = static_cast<int32_t>(added) - static_cast<int32_t>(removed);
    const StoryKind kind = story < doc.Stories().size() ? doc.Stories()[story].kind : StoryKind::Main;
    auto changed = [&](uint32_t p) { return p != kNone && p >= first && p < first + removed; };
    // What's cached from the paragraphs that changed.
    if (structure || shift != 0) {
      e.fieldsCache_.clear();
    } else if (auto it = e.fieldsCache_.find(story); it != e.fieldsCache_.end()) {
      for (const Field& fl : it->second)
        if (changed(fl.begin.para) || changed(fl.separator.para) || changed(fl.end.para)) {
          e.fieldsCache_.erase(it);
          break;
        }
    }
    if (kind == StoryKind::Footnote || kind == StoryKind::Endnote) {
      f.noteStacks.erase(story);
      f.separatorHeight = -1;
    }
    if (shift != 0) f.noteStacks.clear();
    // Lines kept from earlier edits: not after one that may change list labels or fields
    // anywhere, nor ones not used for a while.
    ++f.edits;
    if (structure) f.lineCache.clear();
    for (auto it = f.lineCache.begin(); it != f.lineCache.end();)
      it = it->second->used + 8 < f.edits ? f.lineCache.erase(it) : std::next(it);

    const size_t n = e.pages_.size();
    const bool laidOut = !f.checkpoints.empty();
    size_t restart = SIZE_MAX;        // the page to lay out again from, if any
    bool rebuild = structure || shift != 0;  // the body's items are flattened again
    size_t itemFrom = 0, itemTo = 0;  // the body items that changed, [itemFrom, itemTo) before the edit
    size_t tableRow = SIZE_MAX, tableRowLast = SIZE_MAX;  // a change in one table: the rows it's in
    if (kind == StoryKind::Main) {
      itemFrom = ItemOf(first);
      size_t tail = 0;
      while (tail < f.bodyItems - itemFrom && f.items[f.bodyItems - 1 - tail].first != kNone &&
             f.items[f.bodyItems - 1 - tail].first >= first + removed)
        ++tail;
      itemTo = f.bodyItems - tail;
      for (size_t i = itemFrom; i < itemTo; ++i)
        if (f.items[i].kind == BlockKind::Paragraph && !changed(f.items[i].para)) rebuild = true;
      // In a table: the change is in a cell of a row, and pages that only show rows above it, or
      // of a row split across pages only what's above the change, stay (not when it's in its
      // repeated header rows, nor with merged cells running on).
      size_t tableCell = SIZE_MAX;
      float changeTop = FLT_MAX;  // where the change starts in the cell's content as it was
      if (itemTo == itemFrom + 1 && f.items[itemFrom].kind == BlockKind::Table) {
        const Node tbl = f.items[itemFrom].node;
        const uint32_t lastPara = added ? first + added - 1 : first;
        size_t r = 0;
        for (Node tr : doc.TableRows(tbl)) {
          uint32_t p = kNone;
          for (Node tc : doc.RowCells(tr))
            if ((p = FirstPara(doc, tc)) != kNone) break;
          if (p != kNone && p > lastPara) break;
          if (p != kNone && p <= first) tableRow = r;
          if (p != kNone) tableRowLast = r;
          ++r;
        }
        if (tableRow != SIZE_MAX) {
          size_t i = 0;
          for (Node tc : doc.RowCells(doc.TableRows(tbl)[tableRow])) {
            const uint32_t p = FirstPara(doc, tc);
            if (p != kNone && p > first) break;
            if (p != kNone) tableCell = i;
            ++i;
          }
          // The table as measured before the edit, kept by the checkpoint at the end of the layout.
          const TableBox* was = nullptr;
          for (size_t k = f.checkpoints.size(); k-- > 0 && !was;)
            if (f.checkpoints[k].table && f.checkpoints[k].table->tbl == tbl) was = f.checkpoints[k].table.get();
          if (was && tableRow < was->rows.size() && tableCell < was->rows[tableRow].cells.size())
            for (const LineBox& l : was->rows[tableRow].cells[tableCell].content.lines)
              if (l.para != kNone && l.para >= first && l.top < changeTop) changeTop = l.top;
        }
      }
      auto touched = [&](const Checkpoint& c) {
        if (tableRow == SIZE_MAX || c.next != itemFrom || !c.table) return Touched(c, itemFrom);
        if (c.row > tableRow || !c.spans.empty() || tableRow < c.table->headerRows) return true;
        if (c.row < tableRow || c.rowCut.empty()) return false;
        return tableCell >= c.rowCut.size() || c.rowCut[tableCell] + kEpsilon >= changeTop;
      };
      if (laidOut) {
        // The first page the change can move: the one before the page where its item (or row)
        // begins.
        size_t k = n;
        while (k > 0 && touched(f.checkpoints[k])) --k;
        size_t from = k > 0 ? k - 1 : 0;
        // A change to how a section starts (on the same page now, or without the blank page before
        // it) can bring it back to the page where what's before it ends, and keep-with-next the
        // page before that one too.
        bool back = false;
        if (structure && tableRow == SIZE_MAX) {
          // The first page that starts with what's before it laid out, or in the section it's in
          // (the flow enters a section before its first item).
          const uint32_t sec = itemFrom < f.items.size() ? f.items[itemFrom].section : kNone;
          auto before = [&](const Checkpoint& c) { return c.next < itemFrom && (c.section == kNone || c.section < sec); };
          size_t done = 0;
          while (done <= k && before(f.checkpoints[done])) ++done;
          if (const size_t page = done >= 2 ? done - 2 : 0; done <= k && page < from) {
            from = page;
            back = true;
          }
        }
        if (k < n || f.old.empty() || back) {
          restart = from;
        } else {
          // Not reached yet: it's on an old page, which then isn't taken back as it is.
          size_t q = n;
          while (q - n < f.old.size() && !touched(f.old[q - n].after)) ++q;
          if (q == n) restart = n > 0 ? n - 1 : 0;
        }
      }
      if (structure) f.noConverge = true;
    } else if (kind != StoryKind::Comment && laidOut) {
      // The first page that shows the changed paragraphs (a text box: or its drawing). A footnote
      // separator shows no text of its own, but it's above the notes of every page that has some.
      const uint32_t drawing = kind == StoryKind::TextBox ? doc.Stories()[story].drawing : kNone;
      const bool separator = kind == StoryKind::Footnote && !doc.NodeOf(doc.Stories()[story].root).AttrOr(Name("type")).empty();
      // A header or footer is on the pages of the sections that have it, whatever it shows.
      auto has = [&](uint32_t section) {
        if (section >= doc.Sections().size()) return false;
        const Section& sec = doc.Sections()[section];
        return std::find(sec.headers.begin(), sec.headers.end(), story) != sec.headers.end() ||
               std::find(sec.footers.begin(), sec.footers.end(), story) != sec.footers.end();
      };
      auto shows = [&](const Page& pg) {
        if ((kind == StoryKind::Header || kind == StoryKind::Footer) && has(pg.section)) return true;
        for (const LineBox& l : pg.lines)
          if (changed(l.para) || (separator && l.story == LineStory::Footnote)) return true;
        if (drawing != kNone)
          for (const DrawingBox& d : pg.drawings)
            if (d.drawing == drawing) return true;
        return false;
      };
      size_t s = SIZE_MAX;
      for (size_t p = 0; p < n + f.old.size() && s == SIZE_MAX; ++p)
        if (shows(p < n ? e.pages_[p] : f.old[p - n].page)) s = p;
      if (s != SIZE_MAX) {
        // A header or footer is on many pages, and so is what an edit to it moves; endnotes are
        // items, flattened again from the restart.
        const bool hf = kind == StoryKind::Header || kind == StoryKind::Footer || separator;
        if (hf || kind == StoryKind::Endnote || structure) f.noConverge = true;
        else f.convergeFrom = std::max(f.convergeFrom, Before(s + 1).next + 1);
        const size_t r = hf ? s : (s > 0 ? s - 1 : 0);
        if (r < n) restart = r;
      }
    }

    // Laying out again from `restart`: its pages become old pages, and the lines of the next few
    // are kept for the next keystroke.
    Checkpoint from;
    if (restart != SIZE_MAX) {
      from = MoveToOld(restart);
      f.cacheUntil = restart + 4;
    }

    // The items, and where the old pages' checkpoints are in them.
    if (kind == StoryKind::Main) {
      const size_t oldBody = f.bodyItems;
      if (rebuild) {
        const bool endnotes = f.endnotesAdded && restart == SIZE_MAX;
        FlattenBody(doc, f.items);
        f.bodyItems = f.items.size();
        if (endnotes) AddEndnoteItems(f.section == kNone ? 0 : f.section);
      } else {
        for (size_t i = itemFrom; i < itemTo; ++i)
          if (f.items[i].kind == BlockKind::Paragraph) f.items[i].node = doc.ParaNode(f.items[i].para);
      }
      const ptrdiff_t delta = static_cast<ptrdiff_t>(f.bodyItems) - static_cast<ptrdiff_t>(oldBody);
      auto moved = [&](size_t i) { return static_cast<size_t>(static_cast<ptrdiff_t>(i) + delta); };
      const size_t newTo = moved(itemTo);
      for (OldPage& o : f.old) {
        size_t& next = o.after.next;
        if (next >= itemTo) next = moved(next);
        else if (next > itemFrom || (next == itemFrom && (o.after.pt || o.after.table))) next = SIZE_MAX / 2;  // in what changed
      }
      if (f.convergeFrom >= itemTo) f.convergeFrom = moved(f.convergeFrom);
      else if (f.convergeFrom > itemFrom) f.convergeFrom = newTo;
      f.convergeFrom = std::max(f.convergeFrom, newTo);
    } else if (kind == StoryKind::Endnote && f.endnotesAdded && restart == SIZE_MAX) {
      f.items.resize(f.bodyItems);
      AddEndnoteItems(f.section == kNone ? 0 : f.section);
    }

    // Paragraphs after the change moved by `shift`.
    if (shift != 0) ShiftParagraphs(first, removed, added, kind == StoryKind::Main && rebuild);

    // In the table the flow goes on in, from where it lays out again or (the change not reached
    // yet) from where it is: the rows changed are measured again, or else the whole table, in what
    // it was measured in, with the cells that didn't change kept (not after a change that may
    // number list paragraphs again).
    Checkpoint* in = restart != SIZE_MAX ? &from : laidOut ? &f.checkpoints.back() : nullptr;
    if (tableRow != SIZE_MAX && in && in->next == itemFrom && in->table &&
        !e.MeasureRowsAgain(*in->table, tableRow, tableRowLast, structure)) {
      const std::shared_ptr<TableBox> stale = in->table;
      Frame frame;
      frame.geom = &stale->geom;
      frame.fields = &stale->fields;
      frame.width = stale->frameWidth;
      frame.story = stale->story;
      auto measured = std::make_shared<TableBox>(e.MeasureTable(stale->tbl, frame, structure ? nullptr : stale.get()));
      for (Checkpoint& c : f.checkpoints)
        if (c.table == stale) c.table = measured;  // the pages before show the same rows of it
      if (f.table == stale) f.table = measured;
      in->table = measured;
    }
    if (restart != SIZE_MAX) {
      if (kind == StoryKind::Endnote) f.items.resize(f.bodyItems);
      Restore(from);
    }
    if (f.old.empty()) ForgetOld();
  }

  // Moves paragraph indices from `first + removed` on by added - removed in everything laid out;
  // lines of the paragraphs replaced go to the ones that replaced them.
  void ShiftParagraphs(uint32_t first, uint32_t removed, uint32_t added, bool itemsDone) {
    const int64_t shift = static_cast<int64_t>(added) - static_cast<int64_t>(removed);
    auto map = [&](uint32_t& p) {
      if (p == kNone || p < first) return;
      if (p >= first + removed) p = static_cast<uint32_t>(p + shift);
      else p = added == 0 ? kNone : first + std::min(p - first, added - 1);
    };
    auto page = [&](Page& pg) {
      for (LineBox& l : pg.lines) map(l.para);
      map(pg.first.para);
      if (pg.first.para == kNone) pg.first = {};
    };
    for (Page& pg : e.pages_) page(pg);
    for (OldPage& o : f.old) page(o.page);
    std::vector<const void*> seen;
    auto once = [&](const void* p) {
      if (!p || std::find(seen.begin(), seen.end(), p) != seen.end()) return false;
      seen.push_back(p);
      return true;
    };
    auto inProgress = [&](ParaText* pt, TableBox* t) {
      if (once(pt)) map(pt->para);
      if (once(t))
        for (RowBox& row : t->rows)
          for (CellBox& c : row.cells)
            for (LineBox& l : c.content.lines) map(l.para);
    };
    for (Checkpoint& c : f.checkpoints) inProgress(c.pt.get(), c.table.get());
    for (OldPage& o : f.old) inProgress(o.after.pt.get(), o.after.table.get());
    inProgress(f.pt.get(), f.table.get());
    for (auto& [elem, cached] : f.lineCache) inProgress(cached->pt.get(), nullptr);
    if (!itemsDone)
      for (Item& it : f.items) {
        map(it.para);
        map(it.first);
      }
  }

  // Lays out one page. False once the document is done (no page was made).
  bool MakePage() {
    if (e.done_) return false;
    if (!f.havePage) StartPage();
    if (f.blankPending) {
      f.blankPending = false;
      FinishPage();
      return true;
    }
    for (int guard = 0; guard < 1000000; ++guard) {
      if (f.next >= f.items.size() && !f.pt && !f.table) {
        if (!f.endnotesAdded) {
          AddEndnotes();
          if (f.next < f.items.size()) continue;
        }
        FinishPage();
        e.done_ = true;
        return true;
      }
      const Item& item = f.items[f.next];
      // A new section: on this page (continuous), in the next column, or on a new page.
      if (!f.pt && !f.table && item.section != f.section && !doc.Sections().empty()) {
        const SectionProps& p = Props(item.section);
        // A continuous break to another page size or orientation starts a new page all the same.
        const bool samePage = Pt(p.pageWidth) == f.geom.width && Pt(p.pageHeight) == f.geom.height;
        if (p.start == SectionStart::Continuous && !f.pageEmpty && samePage) {
          float low = f.y;
          for (size_t k = f.hfLines; k < f.body.lines.size(); ++k) {
            const LineBox& l = f.body.lines[k];
            if (l.story == LineStory::Body || l.story == LineStory::Cell) low = std::max(low, l.top + l.height);
          }
          f.section = item.section;
          f.pagesInSection = 1;
          f.cols = ColumnsOf(item.section, f.geom.left, f.geom.width - f.geom.right);
          f.col = 0;
          f.top = std::max(f.top, low + f.spacing.after);
          f.y = f.top;
          f.columnEmpty = true;
          f.spacing = Spacing();
          f.sectionFirstPage.push_back(e.pages_.size());
          if (p.pageNumberStart) f.number = *p.pageNumberStart;
        } else if (p.start == SectionStart::NextColumn && !f.pageEmpty) {
          f.section = item.section;
          if (!NextColumn()) {
            FinishPage();
            return true;
          }
        } else if (!f.pageEmpty) {
          f.nextHard = true;
          FinishPage();
          return true;
        } else {
          // The page is still empty: start it again as the new section's.
          f.havePage = false;
          f.section = f.section;  // StartPage sees the change
          --f.number;
          if (!f.sectionFirstPage.empty() && f.sectionFirstPage.back() == e.pages_.size()) f.sectionFirstPage.pop_back();
          StartPage();
          if (f.blankPending) {
            f.blankPending = false;
            FinishPage();
            return true;
          }
          continue;
        }
      }
      if (!f.pt && !f.table) {
        Engine::Placement place = e.PlacementOf({item.kind, item.node, item.para});
        if (place && Place(item, place)) continue;
      }
      Step s = item.kind == BlockKind::Table ? Table(item) : Paragraph(item);
      if (s == Step::PageFull) {
        FinishPage();
        return true;
      }
    }
    FinishPage();
    e.done_ = true;
    return true;
  }

  // A frame or floating table, from the current item: placed on this page, where the flow goes on
  // beside or below it. False for a floating table that doesn't fit below the text it's anchored
  // to: Word lets it break across pages, as tables in the flow do.
  bool Place(const Item& item, const Engine::Placement& place) {
    std::vector<Block> group{{item.kind, item.node, item.para}};
    while (f.next + group.size() < f.items.size()) {
      const Item& it = f.items[f.next + group.size()];
      if (it.section != item.section || it.story != item.story || !e.JoinsPlaced(place, {it.kind, it.node, it.para})) break;
      group.push_back({it.kind, it.node, it.para});
    }
    Engine::Placed p = e.PlaceBlocks(group, place, BodyFrame(), f.y, ColumnLeft(), ColumnRight());
    if (place.table && place.table->vertAnchor != "page" && p.box.bottom > Bottom() + kEpsilon) return false;
    if (p.zone.wrap != WrapType::None) f.zones.push_back(p.zone);
    f.body.Append(std::move(p.content), p.box.left, p.box.top);
    f.pageEmpty = false;
    f.next += group.size();
    return true;
  }
};

std::shared_ptr<CachedPara> Engine::LinesOf(uint32_t para, float width, bool breaks) {
  Impl& f = *impl_;
  if (f.lineCache.empty()) return nullptr;
  const Paragraph& model = doc_.Para(para);
  auto it = f.lineCache.find(model.elem);
  if (it == f.lineCache.end()) return nullptr;
  const std::shared_ptr<CachedPara>& c = it->second;
  if (c->version != model.version || c->width != width || c->breaks != breaks || c->pt->para != para) {
    f.lineCache.erase(it);
    return nullptr;
  }
  c->used = f.edits;
  return c;
}

std::shared_ptr<CachedPara> Engine::KeepLines(uint32_t para, float width, bool breaks, const std::shared_ptr<ParaText>& pt) {
  Impl& f = *impl_;
  // Not text that depends on the page it's on: page fields and note numbers.
  if (pages_.size() >= f.cacheUntil || pt->empty || pt->usesPage || pt->usesPageCount || !pt->notes.empty()) return nullptr;
  auto c = std::make_shared<CachedPara>();
  c->version = doc_.Para(para).version;
  c->width = width;
  c->breaks = breaks;
  c->pt = pt;
  c->used = f.edits;
  f.lineCache[doc_.Para(para).elem] = c;
  return c;
}

void Engine::Rebase() {
  if (doc_.CacheGeneration() == generation_) return;
  generation_ = doc_.CacheGeneration();
  Impl& f = *impl_;
  f.lineCache.clear();  // every paragraph's version changed too
  const size_t n = doc_.Paragraphs().size();
  auto fix = [&](const std::shared_ptr<ParaText>& pt) {
    if (pt && pt->para < n) pt->props = &doc_.ResolvedPara(pt->para);
  };
  fix(f.pt);
  for (auto& c : f.checkpoints) fix(c.pt);
  for (auto& o : f.old) fix(o.after.pt);
}

bool Engine::NextPage() {
  Rebase();
  Flow flow(*this);
  Impl& f = *impl_;
  if (f.checkpoints.empty()) f.checkpoints.push_back(flow.Save());
  // Every page must move the flow on. Content that can't be placed anywhere (a bug, or input
  // nothing could lay out) is skipped after a few tries, and no document gets more pages than its
  // paragraphs could fill, so pagination always ends.
  // A split row moves on when any of its cells does.
  auto cut = [&f]() { return std::accumulate(f.rowCut.begin(), f.rowCut.end(), 0.0f); };
  const auto progress = std::make_tuple(f.next, f.paraLines, f.pt ? f.breaker->Save().position : 0u, f.row, cut(),
                                        f.items.size());
  bool made = flow.MakePage();
  const auto now = std::make_tuple(f.next, f.paraLines, f.pt ? f.breaker->Save().position : 0u, f.row, cut(),
                                   f.items.size());
  f.stalled = now == progress ? f.stalled + 1 : 0;
  if (f.stalled >= 3 && !done_) {
    f.pt.reset();
    f.breaker.reset();
    f.table.reset();
    f.rowCut.clear();
    ++f.next;
    f.stalled = 0;
  }
  if (!done_ && pages_.size() > 4 * doc_.Paragraphs().size() + 1000) done_ = true;
  if (made) {
    f.checkpoints.push_back(flow.Save());
    if (!f.old.empty()) {
      ++revision_;  // an old page shown is replaced
      flow.Converge();
    }
  }
  if (done_ && made) {
    if (timings_ && !f.paginatedMarked) timings_->Mark(OpenTimings::Paginated);
    f.paginatedMarked = true;
    RefreshPageCounts();
  }
  return made;
}

// Which stories' page fields are worked out: the body's, headers' and footers'. For stories an edit
// adds too (a note, a header).
void Engine::CountStories() {
  for (auto s = static_cast<uint32_t>(fieldsCount_.size()); s < doc_.Stories().size(); ++s) {
    const StoryKind k = doc_.Stories()[s].kind;
    fieldsCount_.push_back(k == StoryKind::Header || k == StoryKind::Footer || k == StoryKind::Main ? 1 : 0);
  }
}

void Engine::Edited(uint32_t story, uint32_t first, uint32_t removed, uint32_t added, bool structure) {
  CountStories();
  Rebase();
  Flow(*this).Edited(story, first, removed, added, structure);
}

size_t Engine::ShownCount() const { return pages_.size() + impl_->old.size(); }

const Page& Engine::Shown(size_t index) const {
  return index < pages_.size() ? pages_[index] : impl_->old[index - pages_.size()].page;
}

PageFields Engine::ShownFields(size_t index) const {
  PageFields fields;
  fields.page = Shown(index).numberText;
  const auto* counts = index < pages_.size() ? (index < impl_->shownCounts.size() ? &impl_->shownCounts[index] : nullptr)
                                             : &impl_->old[index - pages_.size()].shownCounts;
  if (counts) std::tie(fields.pages, fields.sectionPages) = *counts;
  return fields;
}

// Once the page count is known, NUMPAGES and SECTIONPAGES in headers and footers show it.
void Engine::RefreshPageCounts() {
  Impl& f = *impl_;
  bool any = false;
  for (bool b : f.pageUsesCount) any = any || b;
  if (!any) return;
  std::u16string total = FormatNumber(static_cast<int64_t>(pages_.size()), "decimal");
  Flow flow(*this);
  bool changed = false;
  f.shownCounts.resize(pages_.size());
  for (size_t i = 0; i < pages_.size(); ++i) {
    if (i >= f.pageUsesCount.size() || !f.pageUsesCount[i]) continue;
    Page& page = pages_[i];
    // The header and footer lines come first on the page; lay them out again.
    size_t sectionStart = 0, sectionEnd = pages_.size();
    for (size_t k = 0; k < f.sectionFirstPage.size(); ++k)
      if (f.sectionFirstPage[k] <= i) {
        sectionStart = f.sectionFirstPage[k];
        sectionEnd = k + 1 < f.sectionFirstPage.size() ? f.sectionFirstPage[k + 1] : pages_.size();
      }
    std::u16string sectionPages =
        FormatNumber(static_cast<int64_t>(std::max<size_t>(1, sectionEnd - sectionStart)), "decimal");
    // Pages taken back after an edit already show the counts.
    if (f.shownCounts[i].first == total && f.shownCounts[i].second == sectionPages) continue;
    f.shownCounts[i] = {total, sectionPages};
    changed = true;
    f.fields.page = page.numberText;
    f.fields.pages = total;
    f.fields.sectionPages = std::move(sectionPages);
    f.geom = flow.GeomOf(page.section, page.number);
    f.section = page.section;
    f.number = page.number;
    f.pagesInSection = i - sectionStart + 1;
    f.body = Stack();
    flow.LayOutHeaderFooter();
    size_t old = 0;
    for (const LineBox& l : page.lines)
      if (l.story == LineStory::Header || l.story == LineStory::Footer) ++old;
      else break;
    uint32_t paintShift = static_cast<uint32_t>(f.body.paints.size());
    std::vector<LineBox> lines = std::move(f.body.lines);
    for (size_t k = old; k < page.lines.size(); ++k) {
      page.lines[k].paint += paintShift;
      lines.push_back(std::move(page.lines[k]));
    }
    std::vector<RunPaint> paints = std::move(f.body.paints);
    paints.insert(paints.end(), page.paints.begin(), page.paints.end());
    page.lines = std::move(lines);
    page.paints = std::move(paints);
  }
  if (changed) ++revision_;
}

}  // namespace fw::doc::layout
