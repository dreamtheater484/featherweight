#pragma once

// The pagination engine behind Layout (Layout.h): the body flow, a page at a time, and stacked
// layout for what doesn't break across pages by itself (headers, footers, notes, cells, text
// boxes).
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "doc/layout/Internal.h"

namespace fw::doc::layout {

// A page's geometry, from its section.
struct PageGeom {
  float width = 612, height = 792;
  float left = 72, right = 72, top = 72, bottom = 72;  // margins, after mirroring and the gutter
  float header = 36, footer = 36;                       // header and footer distances
  Rect Margins() const { return {left, top, width - right, height - bottom}; }
};

// A rectangle text wraps around (a floating drawing), page coordinates.
struct Zone {
  Rect rect;  // the drawing's box with its distances from text
  WrapType wrap = WrapType::Square;
  uint8_t side = 0;  // 0 both sides, 1 left only, 2 right only, 3 the larger side
};

// Where stacked or flowed content goes: what it's in, for floats, fields and the dump's story.
struct Frame {
  float x = 0, y = 0;  // the frame's top-left corner on the page
  float width = 0;
  LineStory story = LineStory::Body;
  const PageGeom* geom = nullptr;
  const PageFields* fields = nullptr;
  std::vector<Zone>* zones = nullptr;  // floats text wraps around (page coordinates); null: none
  bool inCell = false;
};

// Spacing carried from one paragraph to the next.
struct Spacing {
  float after = 0;  // the previous paragraph's space after
  const ParaProps* prev = nullptr;
  bool atTop = true;  // nothing above yet in this frame or column
};

class Engine {
 public:
  Engine(const Document& doc, text::Fonts& fonts, const Options& options, OpenTimings* timings);
  ~Engine();

  // Lays out one more page; false once the document is done.
  bool NextPage();
  bool Done() const { return done_; }
  // The pages laid out.
  std::vector<Page>& Pages() { return pages_; }
  const std::vector<Page>& Pages() const { return pages_; }
  // The pages shown: those laid out, then after an edit the old ones not laid out again yet.
  size_t ShownCount() const;
  const Page& Shown(size_t index) const;
  // What PAGE, NUMPAGES and SECTIONPAGES show on a shown page, for its text as drawn.
  PageFields ShownFields(size_t index) const;
  // The document changed (Layout::Edited): paragraphs [first, first + removed) of `story` were
  // replaced by [first, first + added).
  void Edited(uint32_t story, uint32_t first, uint32_t removed, uint32_t added, bool structure);
  uint32_t Revision() const { return revision_; }
  const Document& Doc() const { return doc_; }
  text::Fonts& Fonts() { return fonts_; }
  StylePool& Styles() { return styles_; }
  size_t MemoryBytes() const;

  // Text (Text.cpp).
  text::TextStyle StyleOf(const RunProps& r, bool hidden) const;
  RunPaint PaintOf(const RunProps& r, uint32_t modelRun) const;
  text::ParagraphFormat FormatOf(const ParaProps& p) const;
  // `pageBreaks`: the paragraph is the body's own, where page and column breaks break; Word
  // ignores them in table cells, text boxes, headers, footers and notes. `numberNotes` false (for
  // mapping offsets): note references the flow hasn't reached keep no number rather than take one.
  void BuildText(uint32_t para, const PageFields& fields, ParaText& out, bool pageBreaks = false,
                 bool numberNotes = true);
  std::u16string NoteMark(uint32_t story) const;
  // A note's reference mark, numbered in the order references are laid out.
  std::u16string NoteReference(uint32_t story);
  void RefreshPageCounts();
  float EquationWidth(uint32_t drawing, float size);

  // Stacked layout (Stack.cpp).
  Stack StackStory(uint32_t story, const Frame& frame);
  void StackBlocks(const std::vector<Block>& blocks, const Frame& frame, Stack& out, Spacing& spacing);

  // Frames and floating tables: blocks that leave the flow and are placed like a floating
  // drawing, with text wrapping around them. A paragraph with w:framePr is in a frame (with the
  // paragraphs after it that have the same one), and so is a table whose first paragraph is (a
  // table in a frame); a table with w:tblpPr floats.
  struct Placement {
    std::optional<FrameProps> frame;
    std::optional<TableFloat> table;
    explicit operator bool() const { return frame || table; }
  };
  Placement PlacementOf(const Block& b) const;
  // Whether `b` goes in the frame `place` began: it has the same w:framePr.
  bool JoinsPlaced(const Placement& place, const Block& b) const;
  struct Placed {
    Stack content;  // relative to the box's top-left corner
    Rect box;       // page coordinates
    Zone zone;
  };
  // Lays out such blocks and places them. `anchorTop`: where the flow is on the page, for those
  // placed relative to the text.
  Placed PlaceBlocks(const std::vector<Block>& group, const Placement& place, const Frame& frame, float anchorTop,
                     float columnLeft, float columnRight);
  void StackParagraph(uint32_t para, const Frame& frame, Stack& out, Spacing& spacing);
  void StackTable(Node tbl, const Frame& frame, Stack& out, Spacing& spacing);

  // After an edit (D2): the text and lines of `para` laid out before at `width`, or null; and the
  // cache entry to keep a paragraph's text and lines in, when it's laid out on the few pages after
  // an edit and its text doesn't depend on its page (null otherwise).
  std::shared_ptr<CachedPara> LinesOf(uint32_t para, float width, bool breaks);
  std::shared_ptr<CachedPara> KeepLines(uint32_t para, float width, bool breaks, const std::shared_ptr<ParaText>& pt);

  // Paragraph pieces shared by the flow and stacks.
  float TextHeight(const ParaText& pt, const text::Line& line);
  float LineHeight(const ParaText& pt, const text::Line& line);
  float BaselineIn(const ParaText& pt, const text::Line& line, float height) const;
  // What of a line's height may hang below the bottom of the page: proportional spacing's extra.
  float Hang(const ParaText& pt, const text::Line& line, float height) const;
  float SpaceBefore(const ParaProps& p, const Spacing& s) const;
  float SpaceAfter(const ParaProps& p) const;
  // The room paragraph borders take above a paragraph: its top border, or the between border inside
  // a group of paragraphs with the same borders, and the bottom border of a group just ended.
  float BorderAbove(const ParaProps& p, const Spacing& s) const;
  float BorderBelow(const Spacing& s) const;  // what a group ending the frame still takes
  bool Contextual(const ParaProps& a, const ParaProps& b) const;
  // A floating drawing's box on the page, for a paragraph whose top is at `paraTop`.
  Rect PlaceFloat(const Drawing& d, const Frame& frame, float paraTop, float columnLeft, float columnRight) const;
  Zone ZoneOf(const Drawing& d, const Rect& box) const;
  text::LineSpace SpaceBeside(const std::vector<Zone>& zones, float top, float height, float left, float right) const;
  // Adds a drawing (and its text boxes' lines) to a stack, at `box` (frame coordinates).
  void AddDrawing(uint32_t drawing, const Rect& box, bool floating, const Frame& frame, Stack& out);
  // A placed line, with its inline drawings, into `out`.
  void AddLine(const ParaText& pt, text::Line&& line, float x, float top, float height, const Frame& frame,
               uint32_t paintBase, Stack& out);

  // Tables (Table.cpp).
  struct CellBox {
    Node tc;
    CellProps props;
    int32_t col = 0, span = 1, rowSpan = 1;
    bool continued = false;  // vMerge continue: drawn as part of the cell above
    float x = 0, width = 0;  // the cell's box, relative to the table's left edge
    float marginTop = 0, marginBottom = 0, marginLeft = 0, marginRight = 0;
    // Vertical text (w:textDirection btLr, tbRl): quarter turns clockwise (3 runs up, 1 down),
    // and how long its longest line is, which is its height in the row.
    uint8_t turn = 0;
    float turnedLength = 0;
    uint64_t key = 0;  // its paragraphs' versions: measured again only when they change (D2)
    Stack content;
  };
  struct RowBox {
    Node tr;
    RowProps props;
    std::vector<CellBox> cells;
    float height = 0;
    float grown = 0;  // of the height: what merged cells ending in it added
    bool header = false;
    bool breakBefore = false;  // its first cell starts with a paragraph that has w:pageBreakBefore
    bool keepNext = false;     // all its paragraphs have w:keepNext: it stays with the next row
  };
  struct TableBox {
    Node tbl;
    TableProps props;
    // What it was measured in, to measure it again after an edit (D2).
    PageGeom geom;
    PageFields fields;
    float frameWidth = 0;
    LineStory story = LineStory::Body;
    float left = 0;  // relative to the frame or column
    float width = 0;
    std::vector<RowBox> rows;
    size_t headerRows = 0;
    bool labels = false;  // its cells show list labels
  };
  // `reuse`: the table measured before an edit, whose cells that didn't change are kept.
  TableBox MeasureTable(Node tbl, const Frame& frame, const TableBox* reuse = nullptr);
  // What a cell's measure depends on: its paragraphs and their versions, nested tables' included.
  uint64_t CellKey(Node tc) const;
  // Brings `t`, measured before an edit, up to date for a frame this wide: true when it has the same
  // rows and cells, and the rows whose text changed (or depends on the page, or shows list labels)
  // were measured again in place; false when the table must be measured anew (D3: an edit before
  // or in a big table doesn't measure all of it again).
  bool BringUpToDate(TableBox& t, float frameWidth, LineStory story);
  // After an edit in rows [from, to] of `t` (D2): those rows measured again in place, with their
  // cells that didn't change kept. False when they can't be on their own (merged cells over them,
  // rows added or removed, list labels in other rows that a `structure` edit may number again):
  // then the table is measured again.
  bool MeasureRowsAgain(TableBox& t, size_t from, size_t to, bool structure);
  // Rows [from, to] of `t`: added to it when it has none, else measured again in place.
  void MeasureRows(TableBox& t, const Frame& frame, const TableBox* reuse, size_t from, size_t to);
  // Places row `r` at `top` (frame coordinates), `height` tall, into `out`. A row split across
  // pages shows each cell's content from cutFrom[i] to cutTo[i] (null: from the top, to the end).
  // With `spans` false, the text of cells merged with the rows below is left to the caller.
  void EmitRow(const TableBox& t, size_t r, float x, float top, float height, const std::vector<float>* cutFrom,
               const std::vector<float>* cutTo, Stack& out, bool firstOnPage, bool lastOnPage, bool spans = true);
  // A cell's content from `from` to `to` (content coordinates), its part starting at `top` in a
  // table at `x`, `height` tall; `clip` (optional) where its lines may be drawn.
  static void EmitCell(const CellBox& c, float x, float top, float height, float from, float to, float vshift,
                       const Rect* clip, Stack& out);
  // How far a cell's content moves down for its vertical alignment, in `room`.
  static float VShift(const CellBox& c, float room);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;

  friend struct Flow;
  const Document& doc_;
  text::Fonts& fonts_;
  Options options_;
  OpenTimings* timings_;
  // The model's caches the paragraph text kept points into (Document::CacheGeneration), and
  // pointing it at the new ones after an edit made them again.
  uint64_t generation_ = 0;
  void Rebase();
  StylePool styles_;
  std::vector<Page> pages_;
  uint32_t revision_ = 0;
  bool done_ = false;
  int32_t compatMode_ = 15;
  bool sumSpacing_ = false;  // w:doNotUseHTMLParagraphAutoSpacing: space after and before add up
  std::vector<uint32_t> fieldsCount_;  // per story: fields in it (FieldsIn)
  void CountStories();
  std::unordered_map<uint32_t, std::vector<Field>> fieldsCache_;
  std::unordered_map<uint32_t, std::u16string> noteNumbers_;  // note story -> its mark
  std::unordered_map<uint32_t, int32_t> noteOrder_;           // note story -> its number
  std::vector<uint32_t> noteSeq_;                             // note stories in the order numbered
  int32_t footnoteCount_ = 0, endnoteCount_ = 0;

 public:
  const std::vector<Field>& StoryFields(uint32_t story);
  // Whether a story may have fields pagination computes. Stories made after the engine (a new
  // header, a new text box) are looked up as they come.
  bool FieldsIn(uint32_t story);
};

}  // namespace fw::doc::layout
