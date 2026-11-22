#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "draw/Painter.h"
#include "slide/Presentation.h"
#include "slide/Text.h"

// Editing a presentation (plan §4.3 rules 4 and 5, §8.3; P2, p2-model). Headless: no window.
//
// Every change goes through an Editor (§4.3 rule 4: no code outside src/slide/edit changes the
// model). Each call is one undo step, unless inside BeginGroup/EndGroup or continuing typing, and
// changes nothing when it fails (Result says why, in plain language).
//
// How it works: the model is the parts' XML (P1's reader keeps each part as a DOM). An edit works
// on copies of the parts it touches and publishes them when it succeeds; a published part never
// changes again, so an undo step keeps the version from before it, and undo and redo swap versions
// back (they cost about what the edit did). The model then reads again only what changed: the
// slides touched, or everything after a change to a master, a layout, a theme or the slide size.
// The pointers the presentation handed out for those (Slide, Shape, TextBody, Table) stay valid
// until the next edit, so drop them in the change listener: SlideRender::Make them again.
//
// Addressing, independent of any selection:
//  - slides by their index in the deck now (Presentation::GetSlide); changes report slide ids too
//    (p:sldId id), which stay with a slide when it moves;
//  - shapes by their id on the slide (Shape::id, cNvPr id; unique on a slide);
//  - text by TextTarget (the shape, or a table cell in it) and paragraph and offset: a paragraph's
//    text is its runs' text in order, a line break (a:br) counting as one character '\v', a field
//    its text; offsets are UTF-16 code units (wchar_t), as fw::slide::TextRun holds them.
//
// Undo names are plain English, as PowerPoint shows them ("Typing", "Delete Slide").
namespace fw::slide::edit {

class Journal;
class PartStore;

// ---- Changes, for the view: which slides and thumbnails to draw again.

enum ChangeFlag : uint32_t {
  kContent = 1u << 0,   // shapes, text or pictures on the slide
  kProps = 1u << 1,     // hidden, its layout, background, transition
  kNotes = 1u << 2,     // its notes
  kAdded = 1u << 3,     // a new slide (inserted, duplicated, copied in, or back by undo)
  kRemoved = 1u << 4,   // gone (index -1)
  kNumber = 1u << 5,    // its slide number changed and it shows one
};

struct SlideChange {
  uint32_t id = 0;     // p:sldId id
  int index = -1;      // after the change; -1 when the slide is gone
  uint32_t what = 0;   // ChangeFlag bits
  std::vector<uint32_t> shapes;  // shape ids touched, when known (empty: any)
};

struct Changes {
  enum class Cause : uint8_t { Edit, Undo, Redo, Replay };
  Cause cause = Cause::Edit;
  std::u16string step;        // the undo step's name
  bool slideList = false;     // slides added, removed or moved: indexes differ
  bool allSlides = false;     // every slide may look different (a master, layout or theme, the size)
  bool slideSize = false;
  bool sections = false;      // sections (or custom shows) changed
  std::vector<SlideChange> slides;  // the slides whose look or notes changed, or that came or went
  bool Empty() const { return !slideList && !allSlides && !slideSize && !sections && slides.empty(); }
};

// The outcome of an edit. On failure nothing changed.
struct Result {
  bool ok = true;
  std::u16string message;
  explicit operator bool() const { return ok; }
  static Result Fail(std::u16string why) { return Result{false, std::move(why)}; }
};

// ---- Text addressing

struct TextTarget {
  size_t slide = 0;
  uint32_t shape = 0;       // the shape (cNvPr id); a table's graphic frame for a cell
  int row = -1, column = -1;  // a table cell; -1: the shape's own text
  bool notes = false;       // a shape on the slide's notes page (its notes body: Editor::NotesShape)
};

struct TextPos {
  size_t paragraph = 0;
  size_t offset = 0;  // UTF-16 units into the paragraph's text
  bool operator==(const TextPos&) const = default;
  bool operator<(const TextPos& o) const { return paragraph != o.paragraph ? paragraph < o.paragraph : offset < o.offset; }
};

struct TextRange {
  TextPos from, to;  // from <= to
};

// A color as text and fills take it.
struct ColorSpec {
  enum class Kind : uint8_t { Inherit, None, Rgb, Scheme, Xml };
  Kind kind = Kind::Inherit;  // Inherit: the element is removed; None: a:noFill
  uint32_t rgb = 0;           // 0xRRGGBB
  std::string scheme;         // "accent1", "tx1", "bg1"...
  int lumMod = 0, lumOff = 0;  // theme color variations, 1/1000 % (75000: 25 % darker); 0: none
  std::string xml;            // Xml: a whole fill element (a:solidFill, a:gradFill...) with its namespaces
  static ColorSpec Rgb(uint32_t v) { ColorSpec c; c.kind = Kind::Rgb; c.rgb = v; return c; }
  static ColorSpec Scheme(std::string s, int lumMod = 0, int lumOff = 0) {
    ColorSpec c; c.kind = Kind::Scheme; c.scheme = std::move(s); c.lumMod = lumMod; c.lumOff = lumOff; return c;
  }
};

// Character formatting: each set field is applied, the rest left as they are.
struct RunPatch {
  std::optional<bool> bold, italic;
  std::optional<Underline> underline;
  std::optional<Strike> strike;
  std::optional<Caps> caps;
  std::optional<float> size;      // points (1 to 4000)
  std::optional<float> baseline;  // fraction of the size: 0.3 superscript, -0.25 subscript, 0 none
  std::optional<float> spacing;   // points between characters
  std::optional<std::wstring> latin, eastAsian, complexScript;  // typefaces ("+mn-lt" the theme's); empty: inherit
  std::optional<ColorSpec> color;
  std::optional<ColorSpec> highlight;
  std::optional<std::string> lang;
};

struct BulletSpec {
  enum class Kind : uint8_t { Inherit, None, Char, AutoNumber };
  Kind kind = Kind::Inherit;
  std::wstring character = L"\x2022";
  std::wstring font;          // Char: its typeface (empty: the text's)
  std::string scheme = "arabicPeriod";  // AutoNumber
  int startAt = 1;
};

// Paragraph formatting, applied to whole paragraphs.
struct ParagraphPatch {
  std::optional<TextAlign> align;
  std::optional<int> level;            // 0-8 (the list level: Tab and Shift+Tab)
  std::optional<float> marginLeft, indent;  // points; indent is the first line's, from marginLeft
  std::optional<Spacing> lineSpacing, spaceBefore, spaceAfter;
  std::optional<BulletSpec> bullet;
  std::optional<bool> rtl;
};

// The text box (a:bodyPr).
struct BodyPatch {
  std::optional<BodyProps::Autofit> autofit;
  // Shrink: the scale Featherweight computed after an edit (fractions, 1 = none), written as
  // normAutofit fontScale and lnSpcReduction (plan §8.3).
  std::optional<float> fontScale, lineSpacingReduction;
  std::optional<BodyProps::Anchor> anchor;
  std::optional<bool> wrap;
  std::optional<std::array<float, 4>> insets;  // left, top, right, bottom, points
  std::optional<BodyProps::Vertical> vertical;
  std::optional<int> columns;
};

// Where a hyperlink goes.
struct LinkTarget {
  enum class Kind : uint8_t { Url, Slide, FirstSlide, LastSlide, NextSlide, PreviousSlide, EndShow };
  Kind kind = Kind::Url;
  std::string url;      // Url: a web or mail address, as typed
  size_t slide = 0;     // Slide: its index
  std::string tooltip;
};

// ---- Shapes

// A shape's place in its parent's coordinates (the slide's, or its group's child space), points
// (draw::Rect: left, top, right, bottom).
struct Transform {
  draw::Rect bounds;
  float rotation = 0;  // degrees clockwise
  bool flipH = false, flipV = false;
};

enum class ZOrder : uint8_t { BringToFront, BringForward, SendBackward, SendToBack };

// A shape's look: each set field replaces that part of its p:spPr (or a picture's, a connector's),
// in schema order. Fill, line and effects as DrawingML elements with their namespaces declared
// (a:solidFill..., a:ln, a:effectLst), empty to remove the shape's own (it then follows its style
// or placeholder).
struct ShapePatch {
  std::optional<std::string> fill, line, effects;
  std::optional<std::string> geometry;  // a:prstGeom or a:custGeom
  std::optional<std::wstring> name, altText;
  std::optional<bool> hidden;
};

// A picture's crop, fractions of the picture cut from each side (a:srcRect; negative: padding).
struct Crop {
  float left = 0, top = 0, right = 0, bottom = 0;
};

// Table cells' look (a:tcPr).
struct CellPatch {
  std::optional<std::string> fill;  // a fill element, empty: the table style's
  // Borders by TableCell::Edge (left, right, top, bottom, the diagonals): an a:lnL... element's
  // content as a:ln would hold it, with its namespaces; empty: the table style's.
  std::array<std::optional<std::string>, 6> borders;
  std::optional<std::array<float, 4>> margins;  // left, top, right, bottom, points
  std::optional<BodyProps::Anchor> anchor;
};

// ---- Slides and design

// Slide size changes: how the content follows (PowerPoint's Maximize and Ensure Fit).
enum class Scale : uint8_t { DontScale, Maximize, EnsureFit };

// Slide numbers, footer and date (Insert > Header & Footer).
struct HeaderFooter {
  bool slideNumber = false;
  bool footer = false;
  std::wstring footerText;
  bool date = false;
  bool dateFixed = false;   // the fixed text below, else updated automatically in `dateFormat`
  std::wstring dateText;
  std::string dateFormat = "datetime1";  // a field type: datetime1 (short date) to datetime13
  bool notOnTitle = false;  // "Don't show on title slide"
};

class Editor {
 public:
  explicit Editor(Presentation& deck);
  ~Editor();
  Editor(const Editor&) = delete;
  Editor& operator=(const Editor&) = delete;

  Presentation& Deck() { return deck_; }
  const Presentation& Deck() const { return deck_; }
  // False, with the reason, when the deck is shown but not edited (a part Featherweight must not
  // change: mc:MustUnderstand it doesn't, or a damaged presentation part).
  bool CanEdit(std::u16string* why = nullptr) const;

  // ---- Slides (through the reference registry: the slide list, sections, custom shows, links
  // between slides, notes and relationships follow).
  // A new slide at `at` (0..SlideCount) on layout `layout` (an index into Presentation::Layouts()),
  // with the layout's placeholders, as PowerPoint's New Slide makes it. `*id` gets its slide id.
  Result NewSlide(size_t at, size_t layout, uint32_t* id = nullptr);
  // Copies of `slides`, each right after its original (PowerPoint's Duplicate: Ctrl+D), with their
  // notes, charts and SmartArt copied and pictures shared. `ids` gets the new slides' ids.
  Result DuplicateSlides(const std::vector<size_t>& slides, std::vector<uint32_t>* ids = nullptr);
  // Links to a deleted slide stop working (as PowerPoint leaves them), custom shows and sections
  // lose it; its parts leave the file on save when nothing else uses them.
  Result DeleteSlides(const std::vector<size_t>& slides);
  // `slides` (in deck order) moved so the first of them lands at index `to` of the list without
  // them (0..SlideCount - slides.size()). They join the section they land in.
  Result MoveSlides(const std::vector<size_t>& slides, size_t to);
  Result HideSlides(const std::vector<size_t>& slides, bool hidden);
  // Placeholders map onto the new layout's as PowerPoint maps them (by type and index): matched
  // ones keep their text and follow the new layout; unmatched empty ones go, unmatched ones with
  // content stay where they are; the new layout's unmatched placeholders are added empty.
  Result SetLayout(const std::vector<size_t>& slides, size_t layout);
  // The slide size in points (16:9 is 960 x 540, 4:3 720 x 540), every slide, layout and master's
  // shapes scaled as `scale` says, centered.
  Result SetSlideSize(draw::Size size, Scale scale);
  // The slide's notes as plain text ('\n' between paragraphs, '\v' a line break), made on the notes
  // master when the slide has none.
  Result SetNotesText(size_t slide, std::wstring_view text);
  // The notes page's body placeholder (for rich notes editing through TextTarget{.notes = true});
  // 0 when the slide has no notes page yet (SetNotesText makes one).
  uint32_t NotesShape(size_t slide);

  // ---- Design (p2-slides)
  // The background of `slides`: a p:bg element with its namespaces (p:bgPr with a fill, or
  // p:bgRef); empty: the layout's again. Pictures in it name relationships of the slide (AddPart).
  Result SetBackground(const std::vector<size_t>& slides, std::string_view bgXml);
  // A master's own background (Apply to All also clears the slides' own: call SetBackground first).
  Result SetMasterBackground(size_t master, std::string_view bgXml);
  // A master's theme replaced by `themeXml` (an a:theme part: colors, fonts and effects), every
  // slide on it following. Layouts and masters that share the theme part change together.
  Result SetTheme(size_t master, std::string_view themeXml);
  // Slide numbers, footer and date on `slides` (empty: every slide; PowerPoint's Apply to All also
  // sets the layouts'), as PowerPoint writes them: the placeholders made on the slides from their
  // layout, or removed.
  Result SetHeaderFooter(const std::vector<size_t>& slides, const HeaderFooter& hf);

  // ---- Copying slides between decks (Reuse Slides, paste from another deck). Copies `slides` of
  // `source` to `at`. keepSourceDesign: the layout, master and theme come too (shared with an equal
  // one already here); else each slide takes the layout here with the same type and name (else the
  // same type, else the first) and its placeholders map onto it.
  Result InsertSlides(Presentation& source, const std::vector<size_t>& slides, size_t at, bool keepSourceDesign,
                      std::vector<uint32_t>* ids = nullptr);

  // ---- Shapes on a slide. New shapes get ids unused on the slide.
  // A shape element (p:sp, p:pic, p:cxnSp, p:graphicFrame, p:grpSp) as XML declaring the prefixes
  // it uses, put at z position `z` (0 the bottom; -1 on top). Its ids are renewed; the relationships
  // it names must exist (AddPart, AddRelationship in the same group).
  Result InsertShape(size_t slide, std::string_view xml, uint32_t* id = nullptr, int z = -1);
  // A text box (Insert > Text Box) and a preset shape with the theme's style ("rect", "ellipse",
  // "rightArrow"...), as PowerPoint inserts them.
  Result InsertTextBox(size_t slide, const draw::Rect& r, std::wstring_view text, uint32_t* id = nullptr);
  Result InsertAutoShape(size_t slide, std::string_view preset, const draw::Rect& r, uint32_t* id = nullptr);
  // A picture (PNG, JPEG, GIF, BMP, TIFF, EMF, WMF or SVG; the type read from its bytes) stored once
  // per deck (the same bytes share a part), its aspect ratio locked, with its alt text.
  Result InsertPicture(size_t slide, std::vector<uint8_t> bytes, const draw::Rect& r, std::wstring_view altText,
                       uint32_t* id = nullptr);
  // A table of `rows` by `columns` with PowerPoint's default style, as Insert > Table makes it.
  Result InsertTable(size_t slide, int rows, int columns, const draw::Rect& r, uint32_t* id = nullptr);
  // Deleted, with what names them: animations of them, connections to them, relationships only
  // they used (their pictures, charts and media leave the file on save when nothing else uses them).
  Result DeleteShapes(size_t slide, const std::vector<uint32_t>& ids);
  // Move, resize, rotate, flip; several shapes as one step.
  Result SetTransform(size_t slide, uint32_t id, const Transform& t);
  Result SetTransforms(size_t slide, const std::vector<std::pair<uint32_t, Transform>>& shapes);
  Result Arrange(size_t slide, const std::vector<uint32_t>& ids, ZOrder order);
  // Group (Ctrl+G) shapes of one parent: the group takes the topmost's place. Ungroup
  // (Ctrl+Shift+G): the children take the group's place, transformed into its parent's space.
  Result Group(size_t slide, const std::vector<uint32_t>& ids, uint32_t* groupId = nullptr);
  Result Ungroup(size_t slide, uint32_t groupId, std::vector<uint32_t>* ids = nullptr);
  Result SetShapeProps(size_t slide, const std::vector<uint32_t>& ids, const ShapePatch& patch);
  Result SetCrop(size_t slide, uint32_t id, const Crop& crop);
  // A shape's element replaced by `xml` (same rules as InsertShape; it keeps its id).
  Result SetShapeXml(size_t slide, uint32_t id, std::string_view xml, std::u16string stepName);
  // The shape's element as XML declaring its prefixes (copy, the clipboard); empty when none.
  std::string ShapeXml(size_t slide, uint32_t id) const;
  // A hyperlink on the shape itself (clicking it in the show); nullopt removes it.
  Result SetShapeLink(size_t slide, uint32_t id, const std::optional<LinkTarget>& link);

  // ---- Tables (a graphic frame's a:tbl)
  Result InsertTableRows(size_t slide, uint32_t id, int at, int count);
  Result DeleteTableRows(size_t slide, uint32_t id, int first, int count);
  Result InsertTableColumns(size_t slide, uint32_t id, int at, int count);
  Result DeleteTableColumns(size_t slide, uint32_t id, int first, int count);
  Result MergeTableCells(size_t slide, uint32_t id, int row0, int col0, int row1, int col1);
  Result SplitTableCell(size_t slide, uint32_t id, int row, int col);
  Result SetTableColumnWidth(size_t slide, uint32_t id, int column, float points);
  Result SetTableRowHeight(size_t slide, uint32_t id, int row, float points);
  Result SetCellProps(size_t slide, uint32_t id, int row0, int col0, int row1, int col1, const CellPatch& patch);

  // ---- Text
  size_t ParagraphCount(const TextTarget& t) const;  // 0 when the target has no text body
  std::wstring ParagraphText(const TextTarget& t, size_t paragraph) const;
  // `text` at `at`; '\n' (or "\r\n") starts a new paragraph, '\v' a line break. It takes the
  // formatting of the text before it (at a paragraph's start, after it). `end` gets the position
  // after it. Consecutive typing in one text body is one undo step ("Typing") until EndTyping.
  Result InsertText(const TextTarget& t, TextPos at, std::wstring_view text, TextPos* end = nullptr);
  // Paragraphs between the ends are joined; the first keeps its formatting.
  Result DeleteText(const TextTarget& t, TextRange r);
  // Replace (find and replace, paste as text): one step.
  Result ReplaceText(const TextTarget& t, TextRange r, std::wstring_view text, TextPos* end = nullptr);
  // A shape's text as a whole (a placeholder made from nothing gets its text body).
  Result SetText(const TextTarget& t, std::wstring_view text);
  Result SetRunProps(const TextTarget& t, TextRange r, const RunPatch& patch);
  Result SetParagraphProps(const TextTarget& t, size_t first, size_t last, const ParagraphPatch& patch);
  Result SetBodyProps(const TextTarget& t, const BodyPatch& patch);
  // A link on text (Ctrl+K); nullopt removes the links in `r`.
  Result SetTextLink(const TextTarget& t, TextRange r, const std::optional<LinkTarget>& link);
  // The text body (p:txBody or a:txBody's content) replaced by `xml`: a txBody element declaring its
  // prefixes (rich paste). Hyperlinks in it must name relationships of the slide.
  Result SetTextBodyXml(const TextTarget& t, std::string_view xml);
  // Typing ends: the next text edit starts a new undo step (the caret moved, the shape lost focus).
  void EndTyping();

  // ---- Package parts, for operations that add parts (charts, media): inside a group (Run, or
  // BeginGroup/EndGroup); undo and redo take them back with its step. Empty or false when they
  // can't: no group open, no such part.
  // A new part with its content type and a relationship of type `relType` (the full URI) from
  // `source` (a part name, "/" the package): the relationship's id.
  std::string AddPart(std::string_view source, std::string_view relType, std::string_view name,
                      std::string_view contentType, std::vector<uint8_t> bytes);
  // A relationship from `source` to an existing part (internal) or an address (external): its id.
  std::string AddRelationship(std::string_view source, std::string_view relType, std::string_view target,
                              bool external = false);
  bool WritePart(std::string_view name, std::vector<uint8_t> bytes);
  // The first "<stem><n><ext>" no part has, n from 1: NewPartName("/ppt/charts/chart", ".xml").
  std::string NewPartName(std::string_view stem, std::string_view ext) const;
  bool ReadPart(std::string_view name, std::vector<uint8_t>& out) const;
  // The part of slide `slide` ("/ppt/slides/slide3.xml"); empty past the end.
  std::string SlidePart(size_t slide) const;

  // ---- Steps
  // Groups the edits until EndGroup into one undo step named `name`. Groups nest; the outermost
  // names the step.
  void BeginGroup(std::u16string name);
  void EndGroup();
  // Ends the group BeginGroup opened last, undoing what was done in it.
  void CancelGroup();
  // Runs `fn` as one step: kept when it returns true, undone when false.
  Result Run(std::u16string name, const std::function<Result()>& fn);

  bool CanUndo() const;
  bool CanRedo() const;
  std::u16string UndoName() const;
  std::u16string RedoName() const;
  std::vector<std::u16string> UndoNames() const;  // newest first
  std::vector<std::u16string> RedoNames() const;
  Result Undo(size_t steps = 1);
  Result Redo(size_t steps = 1);
  static constexpr size_t kMaxSteps = 100;

  // ---- Notifications and state
  using Listener = std::function<void(const Changes&)>;
  // Called after every edit, undo and redo, with everything it changed (after the model read the
  // changed slides again).
  void OnChange(Listener listener) { listeners_.push_back(std::move(listener)); }
  // Changed since opening or the last save; undoing back to the saved state clears it.
  bool Modified() const;
  // The current state counts as saved (tools and tests; saving uses CommitSave).
  void MarkSaved();
  // Which state the deck is in (changes with every edit, undo and redo); MarkSavedAt marks one as
  // the file's (a save that ran on a worker thread while editing went on).
  uint64_t StateId() const;
  void MarkSavedAt(uint64_t state);

  // ---- Crash recovery (Journal.h)
  void SetJournal(Journal* journal);
  Journal* GetJournal() const { return journal_; }

  // ---- Internal (src/slide/edit): the operations' own tools.
  struct Impl;
  Impl& Internals() { return *impl_; }
  // The model's DOM of a part it has read (no parsing twice); null when it hasn't.
  std::shared_ptr<const pkg::XmlDom> ModelDom(std::string_view part) const;

 private:
  Presentation& deck_;
  std::unique_ptr<Impl> impl_;
  Journal* journal_ = nullptr;
  std::vector<Listener> listeners_;
  friend struct Impl;
};

// A group (Editor::BeginGroup) for as long as it lives: ended when `done`, else cancelled.
struct EditGroup {
  EditGroup(Editor& editor, std::u16string name) : ed(editor) { ed.BeginGroup(std::move(name)); }
  ~EditGroup() { done ? ed.EndGroup() : ed.CancelGroup(); }
  EditGroup(const EditGroup&) = delete;
  EditGroup& operator=(const EditGroup&) = delete;
  Editor& ed;
  bool done = false;
};

}  // namespace fw::slide::edit
