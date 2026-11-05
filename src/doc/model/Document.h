#pragma once

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "doc/model/Props.h"
#include "doc/model/Tree.h"
#include "pkg/Package.h"
#include "pkg/References.h"
#include "pkg/RoundTrip.h"

namespace fw::doc {

namespace docx {
class Reader;
class Writer;
}  // namespace docx
namespace edit {
struct EditorState;
class TreeEdit;
struct PictureAccess;
}  // namespace edit

// The document model (plan §6.3): a headless, read-only view of a Word document, built by the
// .docx reader (doc/docx/Docx.h) and written back by its writer.
//
// Shape of the model:
//  - Parts: the XML parts the model owns (the main document, headers, footers, notes, comments,
//    styles, numbering, settings, the font table). Every other part stays in the package as it was.
//  - Elements: every known element of those parts, in one arena (Tree.h). Code walks them with
//    Node handles; typed views and resolved properties sit on top.
//  - Stories: the main text, each header and footer, each footnote, endnote and comment, and each
//    text box. A story is a list of blocks: paragraphs, tables, and block-level content controls.
//  - Paragraphs: UTF-16 text in which every inline item that takes a place stands for one
//    character (below), with attribute runs (the w:r elements) and inline items found by walking
//    the paragraph's elements. A paragraph's index is its stable id for caches and positions.
//
// Characters in paragraph text that stand for something other than themselves:
//   U+0009 tab (w:tab, w:ptab)                  U+000B line break (w:br, w:cr)
//   U+000C page break (w:br type="page")         U+000E column break (w:br type="column")
//   U+0013 / U+0014 / U+0015 field begin, separator and end (w:fldChar)
//   U+0002 note reference or note mark           U+0005 comment reference or annotation mark
//   U+0003 / U+0004 note separators              U+00AD soft hyphen, U+2011 non-breaking hyphen
//   U+FFFC drawing, picture, object, equation, page number or date item
//   w:sym: the symbol's own character (often in the Symbol font's private use area)
// Field codes (between a field's begin and separator) are part of the text, as in Word: TextRun
// marks them, and the viewer hides them and shows the result.
//
// The model doesn't change after reading. Editing (D2) comes as operations with inverses (plan §4.3
// rule 4) applied by an editor that is a friend of this class; nothing here mutates the model.
// Identical sets of plain properties (a run's w:rPr, a paragraph's w:pPr, w:tabs, a cell's w:tcPr)
// are stored once and shared by their users, so an edit gives its element its own copy first.
// Resolution caches are filled on demand, so calls that resolve properties must come from one
// thread at a time.

inline constexpr uint32_t kNone = 0xFFFFFFFF;

enum class PartKind : uint8_t { Document, Header, Footer, Footnotes, Endnotes, Comments, Styles, Numbering, Settings, FontTable };

// A part the model owns and writes.
struct Part {
  std::string name;  // "/word/document.xml"
  PartKind kind = PartKind::Document;
  ElemId root = kNoElem;
  pkg::NodeId docNode = pkg::kNoNode;      // owns what surrounds the root: the declaration, comments
  std::array<std::string, kNsCount> prefix;  // the prefix the writer uses for each namespace
  std::array<std::string, kNsCount> uri;     // the URI the part uses for it (transitional or strict)
  std::vector<std::pair<std::string, std::string>> addDecls;  // declarations the writer adds to the root
  std::vector<std::string> mustUnderstand;  // mc:MustUnderstand namespaces the model doesn't understand
  bool Editable() const { return mustUnderstand.empty(); }
  // False when the writer can't reproduce the part (an inner element redeclares a prefix the writer
  // uses). The part is then only ever copied from the snapshot.
  bool writable = true;
};

enum class StoryKind : uint8_t { Main, Header, Footer, Footnote, Endnote, Comment, TextBox };

struct Story {
  StoryKind kind = StoryKind::Main;
  uint32_t part = kNone;  // the part it lives in (a text box: the part of its drawing)
  ElemId root = kNoElem;  // w:body, w:hdr, w:ftr, w:footnote, w:endnote, w:comment or w:txbxContent
  std::string id;         // notes and comments: w:id
  uint32_t drawing = kNone;  // text boxes: the drawing they belong to
};

// A position in a paragraph's text.
struct Pos {
  uint32_t para = kNone;
  uint32_t offset = 0;
  bool operator==(const Pos&) const = default;
};

struct Paragraph {
  ElemId elem = kNoElem;  // the w:p
  uint32_t story = kNone;
  std::u16string text;
  uint32_t version = 0;  // changes whenever the paragraph does (D2), for layout caches
};

enum class BlockKind : uint8_t { Paragraph, Table, ContentControl, CustomXml };

struct Block {
  BlockKind kind = BlockKind::Paragraph;
  Node node;               // w:p, w:tbl, w:sdt or w:customXml
  uint32_t para = kNone;   // Paragraph: its index
};

// A stretch of paragraph text inside one w:r with the same context.
struct TextRun {
  uint32_t start = 0, end = 0;  // [start, end) in the paragraph's text
  Node run;                     // the w:r; its w:rPr is the direct formatting
  Node hyperlink;               // the innermost enclosing w:hyperlink
  Node revision;                // the innermost enclosing w:ins, w:del, w:moveFrom or w:moveTo
  Node contentControl;          // the innermost enclosing inline w:sdt
  Node simpleField;             // the innermost enclosing w:fldSimple (its text is the result)
  uint8_t fieldDepth = 0;       // fields open around this text
  bool fieldCode = false;       // inside a field code: hidden in the view
  bool displayOnly = false;     // from a kept mc:AlternateContent branch
};

enum class ItemKind : uint8_t {
  Tab, PositionalTab, LineBreak, PageBreak, ColumnBreak, NoBreakHyphen, SoftHyphen, Symbol,
  FieldBegin, FieldSeparator, FieldEnd,
  FootnoteRef, EndnoteRef, CommentRef, NoteMark, AnnotationMark, Separator, ContinuationSeparator,
  PageNumber, DateText, RenderedPageBreak,
  Drawing,  // see Document::Drawings()
  BookmarkStart, BookmarkEnd, CommentStart, CommentEnd, MoveFromStart, MoveFromEnd, MoveToStart, MoveToEnd,
  PermStart, PermEnd, ProofError, Other,
};

// Something inside a paragraph that isn't plain text: a character that stands for something, a
// drawing, or a zero-length marker.
struct InlineItem {
  ItemKind kind = ItemKind::Other;
  uint32_t offset = 0, length = 0;
  Node node;
  uint32_t drawing = kNone;  // Drawing: index into Document::Drawings()
};

enum class DrawingKind : uint8_t {
  DrawingML,  // w:drawing
  Vml,        // w:pict
  Object,     // w:object: an embedded object with its VML preview
  Equation,   // m:oMath, m:oMathPara (shown from D5)
  ContentPart,
};

enum class WrapType : uint8_t { Inline, None, Square, Tight, Through, TopAndBottom };

// A drawing handle (plan §6.3): what layout needs to place it, and what fwdraw needs to draw it.
struct Drawing {
  DrawingKind kind = DrawingKind::DrawingML;
  Node node;               // the w:drawing, w:pict, w:object or m:oMath element
  Pos pos;                 // its character (U+FFFC) in the paragraph
  uint32_t part = kNone;   // the part it's in: its relationships resolve r:embed and r:id
  bool displayOnly = false;
  bool anchored = false;   // floating (wp:anchor, or VML position:absolute)
  Emu cx{0}, cy{0};        // its extent
  // Floating placement (wp:anchor; for VML, read from its style).
  std::string hRelativeFrom, vRelativeFrom, hAlign, vAlign;
  std::optional<Emu> hOffset, vOffset;
  bool simplePos = false;
  Emu simpleX{0}, simpleY{0};
  WrapType wrap = WrapType::Inline;
  std::string wrapText;    // "bothSides", "left", "right", "largest"
  Emu distT{0}, distB{0}, distL{0}, distR{0};
  Emu effectL{0}, effectT{0}, effectR{0}, effectB{0};  // wp:effectExtent
  bool behindDoc = false, layoutInCell = true, allowOverlap = true, locked = false, hidden = false;
  int64_t relativeHeight = 0;  // z-order
  // wp:docPr: id, name and alt text.
  uint32_t id = 0;
  std::string name, descr, title;
  std::vector<uint32_t> textBoxes;  // stories read from its w:txbxContent
  std::string keptXml;  // display-only drawings: their XML (the kept AlternateContent block is what's written)
  uint32_t version = 0;  // changes whenever its XML does (D3 picture edits), for the view's caches
};

// A field (plan §6.3): begin, separator and end, which may be in different paragraphs.
struct Field {
  uint32_t story = kNone;
  Pos begin, separator, end;     // separator.para is kNone when the field has none
  Node beginNode, separatorNode, endNode;  // w:fldChar elements; a w:fldSimple for all three
  bool simple = false;           // w:fldSimple
  bool locked = false, dirty = false;
  uint32_t depth = 0;            // 0 for an outermost field
  uint32_t parent = kNone;       // the enclosing field's index
  std::u16string code;           // the instruction, as written (" PAGE \* MERGEFORMAT ")
  std::u16string result;         // the last result, as shown: text between separator and end,
                                 // without nested fields' codes
  bool complete = true;          // false when the end is missing
  // The field type: the first word of the code ("PAGE", "REF", "TOC"), upper-cased by Word.
  std::u16string_view Type() const;
};

struct CommentInfo {
  uint32_t story = kNone;
  std::string_view id, author, initials, date;
  Pos start, end, reference;  // the commented range and the reference mark; para kNone when missing
  std::string_view paraId;    // the w14:paraId of its last paragraph, as commentsExtended names it
  std::string parentId;       // the comment this replies to (commentsExtended), empty for none
  bool done = false;          // resolved (commentsExtended)
};

enum class RevisionKind : uint8_t {
  Insertion, Deletion, MoveFrom, MoveTo,      // inline content (w:ins, w:del, w:moveFrom, w:moveTo)
  MarkInsertion, MarkDeletion,                // a paragraph mark (w:pPr/w:rPr/w:ins, w:del)
  RowInsertion, RowDeletion, CellInsertion, CellDeletion, CellMerge,
  RunFormat, ParagraphFormat, SectionFormat, TableFormat, RowFormat, CellFormat, TableGrid,
};

struct Revision {
  RevisionKind kind = RevisionKind::Insertion;
  Node node;  // the change element
  std::string_view id, author, date;
  uint32_t story = kNone;
  Pos start, end;  // the text it covers (inline kinds), or the paragraph it belongs to
};

// A pair of range ends found by the reference registry's rules: bookmarks, comment ranges, move
// ranges, permissions, custom XML ranges, and fields (by nesting).
struct PairInfo {
  pkg::RefKind kind = pkg::RefKind::BookmarkId;
  std::string_view id;  // empty for fields
  Node open, close;
  uint32_t story = kNone;
  Pos openPos, closePos;
};

enum class StyleType : uint8_t { Paragraph, Character, Table, Numbering };

struct Style {
  StyleType type = StyleType::Paragraph;
  std::string_view id, name;
  Node node;  // the w:style
  uint32_t basedOn = kNone, next = kNone, link = kNone;  // indices into Styles()
  bool isDefault = false;
};

// A list label, ready for display (plan §6.3 numbering).
struct ListLabel {
  std::u16string text;     // "3.", "iv)", "1.2.1", or the bullet character
  std::string_view numFmt; // the level's w:numFmt ("decimal", "bullet", ...)
  bool bullet = false;
  std::string_view suffix; // what follows the label: "tab" (the default), "space", "nothing"
  Justification jc = Justification::Start;
  int32_t level = 0;
  std::string_view numId;
  Node lvl;                // the effective w:lvl (an override's or the abstract definition's)
  RunProps props;          // the label's formatting: the paragraph mark's, then the level's w:rPr
};

// A number in a w:numFmt ("decimal", "upperRoman", "lowerLetter", "ordinal", "cardinalText",
// "chicago", ...), as list labels, page numbers and note references show it. Formats Word has but
// this doesn't (most East Asian and Indic ones) come out as decimal.
std::u16string FormatNumber(int64_t n, std::string_view numFmt);

// Settings that layout needs (word/settings.xml).
struct DocSettings {
  int32_t compatibilityMode = 0;  // w:compatSetting "compatibilityMode"; 0 when absent (Word reads 12)
  bool autoHyphenation = false;
  int32_t consecutiveHyphenLimit = 0;  // 0: no limit
  Twips hyphenationZone{360};
  bool doNotHyphenateCaps = false;
  Twips defaultTabStop{720};
  bool evenAndOddHeaders = false, mirrorMargins = false, gutterAtTop = false;
  bool bordersDoNotSurroundHeader = false, bordersDoNotSurroundFooter = false;
  bool displayBackgroundShape = false;
  std::string_view characterSpacingControl;  // "doNotCompress", "compressPunctuation", ...
  NoteProps footnotes, endnotes;
  std::vector<std::string_view> footnoteSeparators, endnoteSeparators;  // note ids named in settings
  std::string_view themeFontLang, themeFontLangEastAsia, themeFontLangBidi;
  std::string_view decimalSymbol, listSeparator;
  Node compat;  // w:compat, for the legacy flags: Compat(Name("useFELayout"))
  bool Compat(uint32_t flag) const { return compat && compat.Child(flag) && compat.Child(flag).On(); }
};

// Theme colors and fonts (word/theme/theme1.xml), read but never written.
struct Theme {
  bool present = false;
  // dk1, lt1, dk2, lt2, accent1-6, hlink, folHlink, as sRGB.
  std::array<uint32_t, 12> colors{};
  struct Fonts {
    std::string latin, eastAsia, cs;
    std::vector<std::pair<std::string, std::string>> scripts;  // a:font script -> typeface
  };
  Fonts major, minor;
};

// An entry of the font table (word/fontTable.xml).
struct FontInfo {
  std::string_view name, altName, panose, charset, family, pitch;
  bool notTrueType = false;
  std::array<uint32_t, 6> sig{};  // usb0-3, csb0-1
  struct Embedded {
    std::string_view relId, fontKey;
    bool subsetted = false;
  };
  Embedded regular, bold, italic, boldItalic;  // obfuscated fonts in the package
};

// A section (plan §6.3): its properties are stored where Word stores them, in the w:pPr of its
// last paragraph, or for the last section at the end of the body.
struct Section {
  Node sectPr;
  uint32_t endPara = kNone;  // the paragraph that holds sectPr; kNone for the last section
  SectionProps props;
  // Header and footer stories by HdrFtrType, inherited from earlier sections as Word does;
  // kNone when there is none.
  std::array<uint32_t, 3> headers{kNone, kNone, kNone};
  std::array<uint32_t, 3> footers{kNone, kNone, kNone};
};

class Document {
 public:
  ~Document();
  Document(const Document&) = delete;
  Document& operator=(const Document&) = delete;

  // ---- The package and the parts the model owns
  const pkg::Package& Package() const { return *package_; }
  pkg::Package& Package() { return *package_; }
  const std::vector<Part>& Parts() const { return parts_; }
  const Part* FindPart(PartKind kind) const;
  // False when any part has an mc:MustUnderstand namespace the model doesn't understand: the
  // document is shown but not edited (plan §4.3 rule 1).
  bool Editable() const;
  const pkg::RoundTripStore& Store() const { return store_; }
  pkg::RoundTripStore& Store() { return store_; }
  const Tree& GetTree() const { return tree_; }
  Node NodeOf(ElemId id) const { return Node(&tree_, id); }
  Node RootOf(const Part& part) const { return Node(&tree_, part.root); }

  // ---- Stories and blocks
  const std::vector<Story>& Stories() const { return stories_; }
  uint32_t MainStory() const { return main_; }
  // The note or comment with this id; kNone when there is none.
  uint32_t NoteStory(StoryKind kind, std::string_view id) const;
  // The blocks directly inside a story, a table cell (w:tc), a block-level content control or a
  // custom XML block, in order. Range markers between blocks aren't blocks.
  std::vector<Block> Blocks(uint32_t story) const;
  std::vector<Block> Blocks(Node container) const;
  // Every paragraph of a story in document order, tables and content controls included.
  std::vector<uint32_t> StoryParagraphs(uint32_t story) const;

  // ---- Paragraphs
  const std::vector<Paragraph>& Paragraphs() const { return paras_; }
  const Paragraph& Para(uint32_t para) const { return paras_[para]; }
  Node ParaNode(uint32_t para) const { return Node(&tree_, paras_[para].elem); }
  std::string_view W14ParaId(uint32_t para) const;
  // The paragraph's attribute runs, in order, split where the field state changes.
  std::vector<TextRun> Runs(uint32_t para) const;
  // Everything in the paragraph that isn't plain text, in order.
  std::vector<InlineItem> Items(uint32_t para) const;
  // The paragraph a node is in (kNone outside paragraphs).
  uint32_t ParagraphOf(Node node) const;

  // ---- Resolved properties (cached)
  const ParaProps& ResolvedPara(uint32_t para) const;
  const RunProps& ResolvedRun(uint32_t para, Node run) const;
  const RunProps& ResolvedMark(uint32_t para) const;  // the paragraph mark (w:pPr/w:rPr)
  // What a run in this paragraph with this character style (or none) would be without direct
  // formatting, and what the paragraph would be without its own w:pPr (but still in its list).
  // Formatting compares with these to write only what differs. Not cached.
  RunProps StyleRun(uint32_t para, std::string_view charStyle = {}) const;
  ParaProps StylePara(uint32_t para) const;
  // What a style gives on its own: the document defaults, then the style and the ones it's based
  // on. Empty or unknown: the defaults alone. Modifying a style compares with its basedOn's.
  RunProps StyleChainRun(std::string_view styleId) const;
  ParaProps StyleChainPara(std::string_view styleId) const;
  std::optional<ListLabel> Label(uint32_t para) const;
  TableProps ResolvedTable(Node tbl) const;
  RowProps ResolvedRow(Node tr) const;
  CellProps ResolvedCell(Node tc) const;

  // ---- Tables
  std::vector<Twips> TableGrid(Node tbl) const;  // w:gridCol widths
  std::vector<Node> TableRows(Node tbl) const;   // w:tr, also inside row-level content controls
  std::vector<Node> RowCells(Node tr) const;     // w:tc, also inside cell-level content controls
  int32_t CellGridColumn(Node tc) const;         // the first grid column the cell covers
  int32_t CellRowSpan(Node tc) const;            // rows a vMerge="restart" cell spans (1 otherwise)

  // ---- Sections, settings, styles, theme, fonts
  const std::vector<Section>& Sections() const { return sections_; }
  const DocSettings& Settings() const { return settings_; }
  const std::vector<Style>& Styles() const { return styles_; }
  const Style* FindStyle(std::string_view id) const;
  const Style* DefaultStyle(StyleType type) const;
  const Theme& GetTheme() const { return theme_; }
  const std::vector<FontInfo>& Fonts() const { return fonts_; }
  // A theme font reference ("minorHAnsi", "majorEastAsia", ...) resolved to a name.
  std::string_view ThemeFont(std::string_view ref) const;
  // A theme color name ("accent1", "text1", "background2", ...) with tint and shade (00-FF, -1
  // for none), mapped through w:clrSchemeMapping.
  std::optional<uint32_t> ThemeColor(std::string_view name, int tint = -1, int shade = -1) const;
  // The effective w:lvl for a list level: the w:num's override if it has one, else the
  // abstract definition's (following w:numStyleLink).
  Node NumberingLevel(std::string_view numId, int32_t ilvl) const;

  // ---- Fields, comments, revisions, drawings, pairs, links
  std::vector<Field> Fields(uint32_t story) const;
  std::vector<CommentInfo> Comments() const;
  std::vector<Revision> Revisions(uint32_t story) const;
  const std::vector<Drawing>& Drawings() const { return drawings_; }
  // The drawing's own XML, self-contained (it declares every prefix it uses): the w:drawing's
  // child (wp:inline or wp:anchor), the w:pict's or w:object's children, or the equation.
  std::string DrawingXml(const Drawing& drawing) const;
  const pkg::RelationshipSet* RelationshipsOf(uint32_t part) const;
  // A relationship's target part read from the package (images, embedded objects, fonts).
  pkg::Status ReadTarget(uint32_t part, std::string_view relId, std::vector<uint8_t>& out) const;
  const std::vector<PairInfo>& Pairs() const { return pairs_; }
  // The external URL of a w:hyperlink (from its relationship), or "#anchor".
  std::string HyperlinkTarget(Node hyperlink) const;

  // ---- The reference registry (plan §4.3 rule 2)
  // Renames a reference everywhere the registry's rules say it can live: in the model's elements,
  // and in the store's fragments and attribute sets. Returns how many places changed.
  // (Bookmark names inside field codes, which are paragraph text, are renamed by D2's editor.)
  size_t RenameReference(pkg::RefKind kind, std::string_view from, std::string_view to);
  // Hooks the model into a registry, so that registry.Rename() (pasting, merging documents)
  // updates the model's elements too. Safe if the registry outlives the document.
  void AttachTo(pkg::ReferenceRegistry& registry);
  const pkg::ReferenceRegistry& Registry() const { return *registry_; }

  struct Caches;  // the model's own (Internal.h)
  // Changes each time the caches are made again (an edit that changes styles, sections or
  // formatting in place, edit::TreeEdit): what ResolvedPara and ResolvedRun gave before is gone,
  // and whoever kept a reference to it looks it up again.
  uint64_t CacheGeneration() const { return cacheGeneration_; }

  // ---- Memory (plan §11: the store stays under a quarter of the model)
  size_t ModelBytes() const;

 private:
  friend class docx::Reader;
  friend class docx::Writer;
  friend struct edit::EditorState;
  friend class edit::TreeEdit;
  friend struct edit::PictureAccess;
  Document();

  Pos PosOf(Node node) const;  // where an element is: its paragraph and offset
  void BuildIndexes();
  void BuildSections();  // part of BuildIndexes, after a section break goes or comes back
  size_t RenameInModel(pkg::RefKind kind, std::string_view from, std::string_view to);
  bool attached_ = false;

  std::unique_ptr<pkg::Package> package_;
  const pkg::ReferenceRegistry* registry_ = nullptr;
  std::unique_ptr<pkg::ReferenceRegistry> ownRegistry_;
  Tree tree_;
  pkg::RoundTripStore store_;
  std::vector<Part> parts_;
  std::vector<Story> stories_;
  uint32_t main_ = kNone;
  std::vector<Paragraph> paras_;
  std::vector<Drawing> drawings_;
  std::vector<PairInfo> pairs_;
  std::vector<Style> styles_;
  std::vector<Section> sections_;
  DocSettings settings_;
  Theme theme_;
  std::vector<FontInfo> fonts_;
  struct CommentEx {
    std::string paraId, parentParaId;
    bool done = false;
  };
  std::vector<CommentEx> commentsEx_;  // word/commentsExtended.xml, read but never written
  std::unique_ptr<Caches> caches_;
  uint64_t cacheGeneration_ = 0;
  void ResetCaches();
  std::shared_ptr<bool> alive_;  // false once the document is gone, for the registry hook
};

}  // namespace fw::doc
