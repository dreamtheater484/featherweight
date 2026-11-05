// The body flow (Engine.h): the main story, page by page, in its sections and columns, with
// headers and footers, footnotes at the bottom of the page, floats text wraps around, Word's
// keep-with-next, keep-lines-together, widow and orphan control and page breaks, and tables whose
// rows split across pages under repeated header rows. Endnotes follow the body.
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdlib>
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
  uint32_t section = 0;
  LineStory story = LineStory::Body;
};

struct Column {
  float left = 0, right = 0;
};

}  // namespace

struct Engine::Impl {
  std::vector<Item> items;
  size_t next = 0;
  bool endnotesAdded = false;

  // The paragraph in progress (it may continue on the next page).
  std::unique_ptr<ParaText> pt;
  std::unique_ptr<text::LineBreaker> breaker;
  text::LineBreaker::State paraStart;
  uint32_t paraLines = 0;  // lines of it on earlier columns
  bool paraBegun = false;  // its space before and floats are placed
  bool breakAfter = false;  // it ended with a page break
  bool nextHard = false;    // the next page starts after a page or section break
  bool hardStart = false;   // this page did
  float separatorHeight = -1;
  size_t anchorDrawings = 0, anchorZones = 0;  // where its floats start on the page

  // The table in progress.
  std::unique_ptr<TableBox> table;
  size_t row = 0;
  std::vector<float> rowCut;  // the current row's cells, content already shown (a split row)
  float tableX = 0;

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
  std::unordered_map<uint32_t, Stack> noteStacks;
  float noteWidth = 0;
  size_t hfLines = 0;  // header and footer lines first on the page, for refreshing page counts
  bool hfUsesCount = false;

  // keep-with-next: where the chain started on this page.
  struct Mark {
    size_t next = 0;
    size_t col = 0;
    float y = 0;
    Spacing spacing;
    size_t lines = 0, paints = 0, fills = 0, edges = 0, drawings = 0, zones = 0, notes = 0;
    float notesHeight = 0;
    bool columnEmpty = true;
  };
  std::optional<Mark> chain;

  // Page numbering.
  int32_t number = 0;
  size_t pagesInSection = 0;
  std::vector<size_t> sectionFirstPage;
  bool blankPending = false;
  std::vector<bool> pageUsesCount;
  int stalled = 0;  // pages in a row that moved nothing on
};

Engine::Engine(const Document& doc, text::Fonts& fonts, const Options& options, OpenTimings* timings)
    : impl_(new Impl), doc_(doc), fonts_(fonts), options_(options), timings_(timings) {
  compatMode_ = doc.Settings().compatibilityMode > 0 ? doc.Settings().compatibilityMode : 12;
  fieldsCount_.assign(doc.Stories().size(), 0);
  for (uint32_t s = 0; s < doc.Stories().size(); ++s) {
    StoryKind k = doc.Stories()[s].kind;
    if (k == StoryKind::Header || k == StoryKind::Footer || k == StoryKind::Main) fieldsCount_[s] = 1;
  }
  // The body, flattened: paragraphs and tables in order, with their sections.
  const std::vector<Section>& sections = doc.Sections();
  uint32_t sec = 0;
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
      uint32_t first = b.kind == BlockKind::Paragraph ? b.para : doc.ParagraphOf(b.node);
      while (sec + 1 < sections.size() && sections[sec].endPara != kNone && first != kNone && first > sections[sec].endPara)
        ++sec;
      it.section = sec;
      impl_->items.push_back(it);
    }
  };
  add(doc.Blocks(doc.MainStory()));
}

Engine::~Engine() = default;

const std::vector<Field>& Engine::StoryFields(uint32_t story) {
  auto it = fieldsCache_.find(story);
  if (it == fieldsCache_.end()) it = fieldsCache_.emplace(story, doc_.Fields(story)).first;
  return it->second;
}

size_t Engine::MemoryBytes() const {
  size_t n = sizeof(*this) + sizeof(Impl);
  for (const Page& p : pages_) {
    n += sizeof(Page) + p.paints.capacity() * sizeof(RunPaint) + p.fills.capacity() * sizeof(Fill) +
         p.edges.capacity() * sizeof(Edge) + p.drawings.capacity() * sizeof(DrawingBox);
    for (const LineBox& l : p.lines) {
      n += sizeof(LineBox) + l.line.caretX_.capacity() * 4 + l.line.advance_.capacity() * 4 + l.line.flags_.capacity() +
           l.line.objects.capacity() * sizeof(text::InlineObject);
      for (const text::GlyphPiece& g : l.line.pieces)
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
    f.havePage = false;
    if (e.pages_.size() == 1 && e.timings_) e.timings_->Mark(OpenTimings::FirstPage);
  }

  Border SeparatorBorder() const {
    Border b;
    b.style = BorderStyle::Single;
    b.width = EighthPoints(4);
    return b;
  }

  Stack Separator() {
    const std::vector<std::string_view>& ids = doc.Settings().footnoteSeparators;
    Frame frame;
    frame.geom = &f.geom;
    frame.fields = &f.fields;
    frame.width = f.noteWidth;
    frame.story = LineStory::Footnote;
    for (std::string_view id : ids) {
      uint32_t s = doc.NoteStory(StoryKind::Footnote, id);
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
    if (it != f.noteStacks.end()) {
      Stack copy = it->second;
      return copy;
    }
    Frame frame;
    frame.geom = &f.geom;
    frame.fields = &f.fields;
    frame.width = f.noteWidth;
    frame.story = doc.Stories()[story].kind == StoryKind::Endnote ? LineStory::Endnote : LineStory::Footnote;
    Stack s = e.StackStory(story, frame);
    return f.noteStacks.emplace(story, s).first->second;
  }

  float NoteHeight(uint32_t story) {
    auto it = f.noteStacks.find(story);
    return it != f.noteStacks.end() ? it->second.height : NoteStack(story).height;
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
    if (f.separatorHeight < 0) f.separatorHeight = Separator().height;
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
    m.zones = f.zones.size();
    m.notes = f.notes.size();
    m.notesHeight = f.notesHeight;
    m.columnEmpty = f.columnEmpty;
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
    f.zones.resize(m.zones);
    f.notes.resize(m.notes);
    f.notesHeight = m.notesHeight;
    f.columnEmpty = m.columnEmpty;
    f.pt.reset();
    f.breaker.reset();
    f.table.reset();
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
      f.pt = std::make_unique<ParaText>();
      e.BuildText(item.para, f.fields, *f.pt, true);
      if (f.pt->empty) {
        f.pt.reset();
        ++f.next;
        return Step::Continue;
      }
      f.breaker = std::make_unique<text::LineBreaker>(e.fonts_, f.pt->fw, frame.width);
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
        // Word keeps the space before at the top of a page (measured on the corpus: dropping it at
        // every page top, or after hard breaks only, matches fewer pages).
        paraTop = f.y + e.SpaceBefore(p, Spacing()) + e.BorderAbove(p, Spacing());
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
    text::LineBreaker& br = *f.breaker;
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
        f.nextHard = true;
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
      // Space after may hang past the bottom of the page; the next paragraph starts below it.
      f.y += 0;
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

  // A page break inside a paragraph: the rest of it starts the next page.
  Step PageBreakInParagraph() {
    f.paraBegun = true;
    return Step::PageFull;
  }

  Step Table(const Item& item) {
    Frame frame = BodyFrame();
    if (!f.table) {
      f.table = std::make_unique<TableBox>(e.MeasureTable(item.node, frame));
      f.row = 0;
      f.rowCut.clear();
      // The space after the paragraph before it (and its bottom border) stay above the table.
      f.y += f.spacing.after + e.BorderBelow(f.spacing);
      f.spacing = Spacing();
      f.spacing.atTop = false;
    }
    TableBox& t = *f.table;
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
      float h = row.height;
      if (!f.rowCut.empty()) {
        // The rest of a split row: its tallest remaining cell.
        h = 0;
        for (size_t i = 0; i < row.cells.size(); ++i) {
          const CellBox& c = row.cells[i];
          if (c.continued) continue;
          h = std::max(h, c.content.height - f.rowCut[i] + c.marginTop + c.marginBottom);
        }
        if (row.props.heightRule == "exact") h = std::max(0.0f, row.height - f.rowCut.front());
      }
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
        e.EmitRow(t, f.row, x, f.y, h, f.rowCut.empty() ? nullptr : &f.rowCut, nullptr, f.body, firstOnPage, last);
        f.y += h;
        f.columnEmpty = false;
        f.pageEmpty = false;
        firstOnPage = false;
        f.rowCut.clear();
        ++f.row;
        f.chain.reset();
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
        for (size_t i = 0; i < row.cells.size(); ++i) {
          const CellBox& c = row.cells[i];
          if (c.continued) continue;
          float start = f.rowCut.empty() ? 0 : f.rowCut[i];
          float limit = start + room - c.marginTop - c.marginBottom;
          float to = start;
          for (const LineBox& l : c.content.lines)
            if (l.top >= start - kEpsilon && l.top + l.height <= limit + kEpsilon) to = std::max(to, l.top + l.height);
          cutTo[i] = to;
          if (to > start + kEpsilon) any = true;
          bool rest = false;
          for (const LineBox& l : c.content.lines)
            if (l.top >= to - kEpsilon) rest = true;
          if (rest) allDone = false;
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
          e.EmitRow(t, f.row, x, f.y, h, f.rowCut.empty() ? nullptr : &f.rowCut, nullptr, f.body, firstOnPage, last);
          f.y += h;
          f.columnEmpty = false;
          f.pageEmpty = false;
          f.rowCut.clear();
          ++f.row;
          continue;
        }
        if (f.row == 0 && f.rowCut.empty() && BreakChain()) return NextColumn() ? Step::Continue : Step::PageFull;
        return NextColumn() ? Step::Continue : Step::PageFull;
      }
      takeNotes();
      e.EmitRow(t, f.row, x, f.y, room, f.rowCut.empty() ? nullptr : &f.rowCut, &cutTo, f.body, firstOnPage, true);
      f.y += room;
      f.columnEmpty = false;
      f.pageEmpty = false;
      f.rowCut = cutTo;
      return NextColumn() ? Step::Continue : Step::PageFull;
    }
    f.table.reset();
    ++f.next;
    f.spacing = Spacing();
    f.spacing.atTop = false;
    return Step::Continue;
  }

  // Endnotes, after the body: each note's blocks as flow items.
  void AddEndnotes() {
    f.endnotesAdded = true;
    std::vector<std::pair<int32_t, uint32_t>> order;
    for (uint32_t s = 0; s < doc.Stories().size(); ++s) {
      if (doc.Stories()[s].kind != StoryKind::Endnote) continue;
      std::u16string mark = e.NoteMark(s);
      if (mark.empty()) continue;
      auto it = e.noteOrder_.find(s);
      order.push_back({it == e.noteOrder_.end() ? 0 : it->second, s});
    }
    std::sort(order.begin(), order.end());
    uint32_t sec = f.section == kNone ? 0 : f.section;
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
        if (p.start == SectionStart::Continuous && !f.pageEmpty) {
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

bool Engine::NextPage() {
  Flow flow(*this);
  Impl& f = *impl_;
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
  if (done_ && made) {
    if (timings_) timings_->Mark(OpenTimings::Paginated);
    RefreshPageCounts();
  }
  return made;
}

// Once the page count is known, NUMPAGES and SECTIONPAGES in headers and footers show it.
void Engine::RefreshPageCounts() {
  Impl& f = *impl_;
  bool any = false;
  for (bool b : f.pageUsesCount) any = any || b;
  if (!any) return;
  std::u16string total = FormatNumber(static_cast<int64_t>(pages_.size()), "decimal");
  Flow flow(*this);
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
    f.fields.page = page.numberText;
    f.fields.pages = total;
    f.fields.sectionPages = FormatNumber(static_cast<int64_t>(std::max<size_t>(1, sectionEnd - sectionStart)), "decimal");
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
  ++revision_;
}

}  // namespace fw::doc::layout
