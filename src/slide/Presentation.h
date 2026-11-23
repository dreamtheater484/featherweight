#pragma once

#include <pugixml.hpp>

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "draw/Drawing.h"
#include "draw/Theme.h"
#include "pkg/Package.h"
#include "pkg/Status.h"
#include "pkg/XmlDom.h"
#include "slide/Text.h"

// The presentation model and its .pptx reader (plan §8, P1): the deck, its slides, layouts and
// masters, themes, notes, with inheritance resolved and cached (§8.3). Read-only in P1.
//
//   auto deck = fw::slide::Read(std::move(package), status);
//   const fw::slide::Slide* s = deck->GetSlide(0);          // loaded on first use
//   draw the background: draw::Drawing::FromShapeTree(s->background, *deck->Resources(*s->backgroundOwner, *s))
//   for (const DrawItem& item : deck->DrawList(*s)) {       // master's, layout's, then the slide's shapes
//     auto res = deck->Resources(*item.owner, *s);
//     auto d = draw::Drawing::FromShapeTree(item.shape->drawNode, *res);
//     d->Draw(painter, d->Bounds());
//     for (const draw::TextBox& box : d->TextBoxes())
//       if (const Shape* t = item.owner->ShapeOfText(box.content))
//         lay out deck->Text(*t, *s)->paragraphs in box.rect
//   }
//
// Everything is in points (1/72 inch), as in fwdraw. Nothing the reader doesn't understand stops
// it: an element it can't read is skipped, a part it can't read leaves its slide blank, and the
// reason is listed in Problems(). Opening never changes the file.
//
// Threads: the presentation may be used from several threads; GetSlide, Text, GetTable and
// EmbeddedFonts take its lock. What they return stays valid and unchanged while the presentation
// lives, except that an edit (src/slide/edit, P2) replaces what it touched: the slides, notes and
// texts an edit::Editor reports changed are read again, and the old ones stay valid only until
// the next edit (drop them in the editor's change listener). Edits run on one thread, with nothing
// reading the presentation from another meanwhile. DrawingResources objects are each for one thread.
namespace fw::slide {

namespace edit {
class Editor;
class PartStore;
}  // namespace edit

enum class FileFormat : uint8_t { Pptx, Pptm, Ppsx, Ppsm, Potx, Potm };

// ST_PlaceholderType. A shape that isn't a placeholder has None.
enum class PlaceholderType : uint8_t {
  None, Title, CenteredTitle, Subtitle, Body, Object, Chart, Table, ClipArt, Diagram, Media, Picture, SlideImage,
  Date, Footer, Header, SlideNumber
};

struct Placeholder {
  enum class Size : uint8_t { Full, Half, Quarter };
  PlaceholderType type = PlaceholderType::None;
  uint32_t index = 0;
  bool hasIndex = false;
  Size size = Size::Full;
  bool vertical = false;       // orient="vert"
  bool customPrompt = false;   // hasCustomPrompt
  explicit operator bool() const { return type != PlaceholderType::None; }
};

struct SlideBase;

struct Shape {
  enum class Kind : uint8_t {
    Shape,      // p:sp (text boxes too)
    Picture,    // p:pic (video and audio show their poster: media)
    Connector,  // p:cxnSp
    Group,      // p:grpSp: children
    Table,      // p:graphicFrame with a:tbl: Presentation::GetTable
    Chart,      // p:graphicFrame with c:chart (fwchart draws it through Drawing)
    Diagram,    // SmartArt: its stored drawing (dsp:drawing), through Drawing
    Ole,        // an embedded object: its preview picture, never started
    Graphic,    // another p:graphicFrame: a placeholder of its size
    ContentPart,  // p:contentPart (ink): not drawn in P1
  };
  Kind kind = Kind::Shape;
  uint32_t id = 0;  // cNvPr id: what the animation timing and links target
  std::string name, description;  // description: the alt text
  bool hidden = false;
  bool textBox = false;  // cNvSpPr txBox="1"
  bool media = false;    // a video or audio (a:videoFile, a:audioFile, p14:media): shows its poster
  Placeholder placeholder;
  pugi::xml_node node;      // the element as written in its part
  // What to draw, with draw::Drawing::FromShapeTree: `node`, or for a placeholder a copy holding
  // what it inherits from its layout's and master's placeholder (position, geometry, fill, line,
  // style, body properties). Its text box's content is `textNode`.
  pugi::xml_node drawNode;
  pugi::xml_node textNode;  // the p:txBody in drawNode: TextBox::content; null without text
  draw::Rect bounds;        // in the parent's coordinates (the slide's, or the group's child space)
  float rotation = 0;       // degrees clockwise
  bool flipH = false, flipV = false;
  const Shape* inherits = nullptr;  // the layout's or master's placeholder this one inherits from
  const SlideBase* owner = nullptr;         // the part it is in
  Hyperlink click, hover;           // on the shape (cNvPr)
  std::vector<Shape> children;      // Group
};

enum class PartKind : uint8_t { Slide, Layout, Master, Notes, NotesMaster, HandoutMaster };

struct Master;

// What slides, layouts, masters and notes have in common: a shape tree on a background.
struct SlideBase {
  PartKind kind = PartKind::Slide;
  std::string part;   // the part's name ("/ppt/slides/slide1.xml")
  std::string name;   // p:cSld name (layouts' and masters' names)
  pugi::xml_node root;  // p:sld, p:sldLayout, p:sldMaster, p:notes, p:notesMaster, p:handoutMaster
  std::vector<Shape> shapes;
  std::shared_ptr<const draw::Theme> theme;  // its master's theme (never null)
  draw::ColorMap colorMap;                   // in effect: the part's override, else its master's
  // The background in effect (the part's, else its layout's, else its master's) as a rectangle
  // shape filling the slide, for draw::Drawing::FromShapeTree; its pictures are in
  // backgroundOwner's relationships. A chain without one gets PowerPoint's default, a solid bg1.
  pugi::xml_node background;
  const SlideBase* backgroundOwner = nullptr;
  bool showMasterShapes = true;               // showMasterSp
  const SlideBase* parent = nullptr;          // a slide's layout, a layout's master, notes' master
  const pkg::RelationshipSet* relationships = nullptr;  // never null once read

  // A shape by its cNvPr id, groups included; null when there's none.
  const Shape* FindShape(uint32_t id) const;
  // The shape whose text box this is (a draw::TextBox::content of a Drawing made from one of this
  // part's drawNodes, groups included).
  const Shape* ShapeOfText(pugi::xml_node content) const;
  // The placeholder that one of a child part's placeholders inherits from (PowerPoint's matching:
  // by index, then by type). Null when none matches.
  const Shape* FindPlaceholder(const Placeholder& ph) const;
  // The first placeholder of a type, or null.
  const Shape* FindPlaceholder(PlaceholderType type) const;

  virtual ~SlideBase();

  pugi::xml_document scratch;  // the reader's: drawNodes and the background live here

 private:
  friend class Presentation;
  friend class Loader;
  friend class edit::Editor;
  // The part as read, or as the last edit left it (src/slide/edit): never changed once read.
  std::shared_ptr<const pkg::XmlDom> dom_;
  std::shared_ptr<const pkg::RelationshipSet> rels_;  // an edited part's relationships (else the package's)
  std::map<const void*, const Shape*> byText_;  // textNode's internal object -> shape
  std::map<uint32_t, const Shape*> byId_;
};

struct Layout : SlideBase {
  std::string type;  // ST_SlideLayoutType ("title", "obj", "twoObj", "blank", "cust"…)
  const Master* master = nullptr;
};

struct Master : SlideBase {
  // p:txStyles: the title, body and other text styles (a notes master's p:notesStyle is its
  // bodyStyle). Null when missing.
  pugi::xml_node titleStyle, bodyStyle, otherStyle;
  std::vector<const Layout*> layouts;
};

struct NotesSlide : SlideBase {};

struct Slide : SlideBase {
  uint32_t id = 0;       // p:sldId id: what sections and custom shows name
  size_t index = 0;      // in the deck
  int number = 1;        // as the slide number field shows it (firstSlideNum + index)
  bool hidden = false;   // show="0": skipped by the slide show
  const Layout* layout = nullptr;
  const Master* master = nullptr;
  const NotesSlide* notes = nullptr;  // null when the slide has none
  // For the slide show (p1-show parses these): p:transition and p:timing as written, or the
  // mc:AlternateContent element holding them (its branches untouched). Null when missing.
  pugi::xml_node transition, timing;

  // The speaker notes as plain text: paragraphs separated by '\n'.
  std::wstring NotesText() const;
};

// A table cell's or table's fill, resolved.
struct CellFill {
  enum class Kind : uint8_t { None, Solid, Other };
  Kind kind = Kind::None;
  draw::Color color;     // Solid; Other: its first color
  pugi::xml_node node;   // Other: the gradFill, blipFill or pattFill element (blip r:embeds are the
                         // slide's), its colors against Table::colors
  pugi::xml_node shape;  // Other: a rectangle (p:sp) filled so, the cell's size in the grid, for
                         // fwdraw to draw into the cell (scaled when its row grows)
};

// One edge of a cell, resolved.
struct CellBorder {
  bool visible = false;
  float width = 1;       // points
  draw::Color color;
  std::string dash;      // a:prstDash ("dash", "sysDot"…); empty: solid
  pugi::xml_node line;   // the a:ln as written, for compound lines and line ends; null from a theme line
};

// One cell of a table, with the table style applied.
struct TableCell {
  enum Edge { Left, Right, Top, Bottom, TopLeftToBottomRight, BottomLeftToTopRight };
  pugi::xml_node node;  // a:tc
  int row = 0, column = 0;
  int rowSpan = 1, columnSpan = 1;
  bool covered = false;  // part of another cell's span (hMerge, vMerge): not drawn
  const TextBody* text = nullptr;  // null when the cell has no text body
  float marginLeft = 7.2f, marginRight = 7.2f, marginTop = 3.6f, marginBottom = 3.6f;  // points
  BodyProps::Anchor anchor = BodyProps::Anchor::Top;
  BodyProps::Vertical vertical = BodyProps::Vertical::Horizontal;
  CellFill fill;                       // the cell's own, else the table style's
  std::array<CellBorder, 6> borders;   // by Edge
  std::vector<pugi::xml_node> textStyles;  // the table style's a:tcTxStyle for it, lowest first
};

struct Table {
  std::vector<float> columns, rows;  // widths and heights, points
  std::vector<TableCell> cells;      // row by row, every grid cell
  CellFill background;               // the table style's tblBg, under every cell
  draw::ColorContext colors;         // what fills' and borders' colors resolve against
  pugi::xml_node node;               // a:tbl
  const SlideBase* owner = nullptr;  // the part the table is in: its fills' pictures are its
  bool rtl = false;                  // columns run right to left
  const TableCell* Cell(int row, int column) const;
};

// p14:sectionLst: named runs of slides.
struct Section {
  std::string name;
  std::vector<size_t> slides;  // indexes
};

// p:custShowLst, kept as data for links and the show.
struct CustomShow {
  uint32_t id = 0;
  std::string name;
  std::vector<size_t> slides;  // indexes, in the custom show's order
};

// presProps' p:showPr.
struct ShowSettings {
  bool loop = false;           // loop until Esc
  bool kiosk = false;          // browsed at a kiosk
  bool browse = false;         // in a window
  bool narration = true, animations = true;  // showNarration, showAnimation
  bool useTimings = true;      // advance on the slides' timings
  int firstSlide = -1, lastSlide = -1;  // p:sldRg, indexes; -1: all
  uint32_t customShow = 0;     // p:custShow id; 0: none
  bool hasCustomShow = false;
};

// A font the deck carries (p:embeddedFontLst), decoded from Embedded OpenType.
struct EmbeddedFont {
  std::wstring typeface;  // as the deck names it
  bool bold = false, italic = false;
  std::vector<uint8_t> sfnt;  // TrueType or OpenType: for fw::text::Fonts::UseDocumentFonts
};

// A shape to draw on a slide, and the part it comes from.
struct DrawItem {
  const Shape* shape = nullptr;
  const SlideBase* owner = nullptr;
};

class Presentation {
 public:
  ~Presentation();
  Presentation(const Presentation&) = delete;
  Presentation& operator=(const Presentation&) = delete;

  FileFormat Format() const { return format_; }
  bool StartsInShow() const { return format_ == FileFormat::Ppsx || format_ == FileFormat::Ppsm; }
  // A template (.potx, .potm): it opens as a new, untitled presentation.
  bool IsTemplate() const { return format_ == FileFormat::Potx || format_ == FileFormat::Potm; }
  bool HasMacros() const { return hasMacros_; }  // kept, never run
  const pkg::Package& GetPackage() const { return *package_; }

  draw::Size SlideSize() const { return slideSize_; }
  draw::Size NotesSize() const { return notesSize_; }
  int FirstSlideNumber() const { return firstSlideNumber_; }
  bool RightToLeft() const { return rtl_; }

  size_t SlideCount() const { return slideRefs_.size(); }
  uint32_t SlideId(size_t index) const;
  int SlideIndexOfId(uint32_t id) const;   // -1 when there's no such slide
  int SlideIndexOfPart(std::string_view part) const;
  // The slide, read on first use. Never null for an index below SlideCount(): a slide whose part
  // can't be read is an empty slide on the first layout.
  const Slide* GetSlide(size_t index);
  // Reads every slide now (tools, tests, fuzzing; the view does it at idle).
  void LoadAll();

  const std::vector<std::unique_ptr<Master>>& Masters() const { return masters_; }
  const std::vector<std::unique_ptr<Layout>>& Layouts() const { return layouts_; }
  const Master* NotesMaster() const { return notesMaster_.get(); }
  const std::vector<Section>& Sections() const { return sections_; }
  const std::vector<CustomShow>& CustomShows() const { return customShows_; }
  const ShowSettings& Show() const { return show_; }

  // The shapes drawn on a slide, bottom to top: its master's and layout's own shapes (not their
  // placeholders) when showMasterShapes allows, then its own.
  std::vector<DrawItem> DrawList(const SlideBase& slide) const;
  // Pictures, charts and SmartArt for drawing `owner`'s shapes on `shownOn` (the theme and color
  // map are shownOn's, the relationships owner's). One per thread.
  std::unique_ptr<draw::DrawingResources> Resources(const SlideBase& owner, const SlideBase& shownOn) const;
  // A shape's text, resolved for the slide it's shown on. Null when the shape has no text body.
  const TextBody* Text(const Shape& shape, const SlideBase& shownOn);
  // Any other text body a Drawing of `owner`'s shapes finds (SmartArt's dsp:txBody, whose shapes
  // aren't the model's): resolved as a shape's that isn't a placeholder (the default text style,
  // its parent's style fontRef, its own list style), cached the same way. Null for a null body.
  const TextBody* TextOf(pugi::xml_node txBody, const SlideBase& owner, const SlideBase& shownOn);
  // A table, with its style applied. Null when the shape isn't a table.
  const Table* GetTable(const Shape& shape, const SlideBase& shownOn);
  // Text being edited (P2): a working copy of a shape's text body, or of a table cell's, resolved as
  // the shape's or cell's own would be, for the slide shown. Not cached; changes nothing.
  std::unique_ptr<TextBody> ResolveTextBody(const Shape& shape, pugi::xml_node txBody, const SlideBase& shownOn) const;
  std::unique_ptr<TextBody> ResolveCellText(const Shape& table, const TableCell& cell, pugi::xml_node txBody,
                                            const SlideBase& shownOn) const;
  // The fonts the deck embeds, decoded on first use. Those that can't be decoded are left out.
  const std::vector<EmbeddedFont>& EmbeddedFonts();

  // What the reader skipped, in plain words, for diagnostics (fwtool, tests).
  std::vector<std::string> Problems() const;

  // The text style a shape's text starts from (presentation.xml's p:defaultTextStyle).
  pugi::xml_node DefaultTextStyle() const { return defaultTextStyle_; }

  // ---- Editing (P2: src/slide/edit). Parts as the edits left them: what the model reads. Without
  // edits, the package's.
  // A part's relationships (null when it has none).
  const pkg::RelationshipSet* RelationshipsOf(std::string_view part) const;
  // A part's bytes (an XML part edited as a DOM is serialized). False when there's no such part.
  bool ReadPart(std::string_view part, std::vector<uint8_t>& out) const;
  // The presentation part's name ("/ppt/presentation.xml").
  const std::string& PresentationPart() const { return presentationPart_; }
  // Edits since opening (src/slide/edit holds them); null before the first.
  const edit::PartStore* Edits() const { return store_.get(); }

 private:
  Presentation();
  friend class Loader;
  friend class edit::Editor;
  friend class edit::PartStore;
  friend std::unique_ptr<Presentation> Read(std::unique_ptr<pkg::Package>, pkg::Status&);

  struct SlideRef {
    uint32_t id = 0;
    std::string part;
    std::unique_ptr<Slide> slide;
  };

  std::unique_ptr<pkg::Package> package_;
  FileFormat format_ = FileFormat::Pptx;
  bool hasMacros_ = false;
  draw::Size slideSize_{720, 540}, notesSize_{540, 720};
  int firstSlideNumber_ = 1;
  bool rtl_ = false;
  std::shared_ptr<const pkg::XmlDom> presentationDom_;
  std::unique_ptr<edit::PartStore> store_;  // the edits (src/slide/edit); null until the first
  std::string presentationPart_;
  pugi::xml_node defaultTextStyle_;
  std::vector<SlideRef> slideRefs_;
  std::vector<std::unique_ptr<Master>> masters_;
  std::vector<std::unique_ptr<Layout>> layouts_;
  std::unique_ptr<Master> notesMaster_;
  std::vector<std::unique_ptr<NotesSlide>> notes_;
  std::vector<Section> sections_;
  std::vector<CustomShow> customShows_;
  ShowSettings show_;
  std::shared_ptr<draw::ImageCache> images_;
  std::map<std::string, std::shared_ptr<const draw::Theme>> themes_;  // by part key
  std::vector<std::pair<std::wstring, std::array<std::string, 4>>> fontParts_;  // typeface, regular/bold/italic/boldItalic parts
  bool fontsDecoded_ = false;
  std::vector<EmbeddedFont> fonts_;
  // Resolved text and tables, by (txBody or a:tbl, the part shown on).
  std::map<std::pair<const void*, const void*>, std::unique_ptr<TextBody>> texts_;
  std::map<std::pair<const void*, const void*>, std::unique_ptr<Table>> tables_;
  bool tableStylesRead_ = false;
  std::shared_ptr<const pkg::XmlDom> tableStyles_;  // ppt/tableStyles.xml
  // The built-in styles decks name without defining, and the rectangles of cells' other fills.
  std::unique_ptr<pugi::xml_document> tableScratch_;
  std::vector<std::unique_ptr<TextBody>> cellTexts_;
  mutable std::recursive_mutex mutex_;
  std::vector<std::string> problems_;
};

// Reads a presentation (.pptx, .pptm, .ppsx, .ppsm, .potx, .potm) from an opened package: the
// presentation part, its masters, layouts and themes. Slides are read on first use. The
// presentation keeps the package. Fails with a plain message when the package isn't a
// presentation, or when the presentation part isn't well-formed or exceeds the package's limits.
std::unique_ptr<Presentation> Read(std::unique_ptr<pkg::Package> package, pkg::Status& status);

}  // namespace fw::slide
