#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "doc/edit/Editor.h"
#include "doc/edit/Pictures.h"
#include "pkg/Package.h"
#include "pkg/RoundTrip.h"
#include "pkg/XmlDom.h"

namespace draw {
class Bitmap;
}

// Internal to the drawing tools (Pictures.cpp, Shapes.cpp, Watermark.cpp): a drawing's DrawingML
// edited in place, and the undo steps that put it and the package back exactly.
namespace fw::doc::edit {

// The document's drawing handles and stories, which only the model's friends change.
struct PictureAccess {
  static Drawing& At(Document& d, uint32_t i) { return d.drawings_[i]; }
  static std::vector<Drawing>& Drawings(Document& d) { return d.drawings_; }
  static std::vector<Story>& Stories(Document& d) { return d.stories_; }
  static std::vector<Paragraph>& Paras(Document& d) { return d.paras_; }
  static Tree& Elems(Document& d) { return d.tree_; }
  // After a story is added: the model's indexes again.
  static void Refresh(Document& d) {
    d.ResetCaches();
    d.BuildIndexes();
  }
};

namespace dx {

inline constexpr std::string_view kWp = "http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing";
inline constexpr std::string_view kA = "http://schemas.openxmlformats.org/drawingml/2006/main";
inline constexpr std::string_view kPic = "http://schemas.openxmlformats.org/drawingml/2006/picture";
inline constexpr std::string_view kR = "http://schemas.openxmlformats.org/officeDocument/2006/relationships";
inline constexpr std::string_view kWp14 = "http://schemas.microsoft.com/office/word/2010/wordprocessingDrawing";
inline constexpr std::string_view kWps = "http://schemas.microsoft.com/office/word/2010/wordprocessingShape";
inline constexpr std::string_view kAsvg = "http://schemas.microsoft.com/office/drawing/2016/SVG/main";
inline constexpr std::string_view kV = "urn:schemas-microsoft-com:vml";
inline constexpr std::string_view kO = "urn:schemas-microsoft-com:office:office";
inline constexpr std::string_view kW10 = "urn:schemas-microsoft-com:office:word";
inline constexpr std::string_view kImageRel = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/image";
inline constexpr const char* kSvgExt = "{96DAC541-7B7A-43D3-8B79-37D633B846F1}";
inline constexpr int64_t kEmuPerPoint = 12700;
// Word's limits for a drawing's size: 1/100 inch to 22 inches.
inline constexpr int64_t kMinEmu = 9144, kMaxEmu = 22 * 914400;

std::u16string Msg(const char* s);
Result Fail(std::u16string why);
extern uint32_t g_version;  // Drawing::version: every change gets a new one

// One drawing's wp:inline or wp:anchor, parsed for a change, with the fragments it's in.
struct Frame {
  pkg::XmlDom dom;      // the fragment: wp:inline or wp:anchor, or the mc:AlternateContent holding it
  pugi::xml_node root;  // wp:inline or wp:anchor
  pugi::xml_node pic;   // pic:pic (a picture)
  pugi::xml_node shape; // wps:wsp (a shape or text box)
  size_t index = 0;     // which of the fragments it is
  std::vector<pkg::Fragment> fragments;
  std::string wp, a;    // the prefixes it uses for them, with the colon ("wp:"), or empty

  std::string Wp(std::string_view local) const { return wp + std::string(local); }
  std::string A(std::string_view local) const { return a + std::string(local); }
};

pugi::xml_node Child(pugi::xml_node parent, std::string_view ns, std::string_view local);
std::string PrefixOf(pugi::xml_node element);
// The prefix `ns` has where `node` is, declaring `preferred` on `root` when it has none.
std::string PrefixFor(pugi::xml_node node, pugi::xml_node root, std::string_view ns, const char* preferred);
const Part* PartOf(const Document& doc, uint32_t part);
bool Strict(const Part& part);
// The handle a frame makes, keeping what isn't its placement from `was`.
Drawing HandleFor(const Drawing& was, pugi::xml_node root);
std::string Lower(std::string s);

int64_t ClampEmu(int64_t v);
int64_t AttrInt(pugi::xml_node n, const char* name, int64_t fallback = 0);
void SetInt(pugi::xml_node n, const char* name, int64_t v);
// The picture's or shape's spPr's a:xfrm, made with a:off and a:ext when missing (`create`).
pugi::xml_node Xfrm(Frame& f, bool create);
int64_t RotationOf(Frame& f);
pugi::xml_node InsertAfterFirst(pugi::xml_node parent, const std::string& name,
                                std::initializer_list<std::pair<std::string_view, std::string_view>> after);
// Sets the size and turn, keeping wp:extent, the drawing's a:ext and the room around it in step.
void SetGeometry(Frame& f, int64_t cx, int64_t cy, int64_t rot);
void SetWrapXml(Frame& f, Wrap wrap);
// The wp:anchor's position on its page: page-relative offsets.
void SetPosition(Frame& f, int64_t x, int64_t y);
int64_t TopZ(const Document& doc);
// In line to floating: wp:inline becomes wp:anchor, placed at (x, y) on the page.
void ToAnchor(Frame& f, const Document& doc, Wrap wrap, int64_t x, int64_t y);
// Floating to in line; false when the anchor holds something an inline drawing can't.
bool ToInline(Frame& f, std::u16string& why);
Wrap WrapOf(const Drawing& d);

// A change to a drawing's XML (the fragments of `node`) and its handle. Applied when made; undo
// and redo swap. A picture keeps its stories and kept XML; with `whole`, the handle given is all
// of it but its place (a shape's kept XML and text boxes change with it).
struct DrawingStep : CustomStep {
  Document* doc = nullptr;
  uint32_t drawing = 0;
  pkg::NodeId node = pkg::kNoNode;
  std::vector<pkg::Fragment> before, after;
  Drawing handleBefore, handleAfter;
  bool whole = false;

  void Apply(const std::vector<pkg::Fragment>& frags, const Drawing& handle) {
    doc->Store().SetFragments(node, frags);
    Drawing& h = PictureAccess::At(*doc, drawing);
    Drawing keep = h;
    h = handle;
    h.node = keep.node;
    h.pos = keep.pos;
    h.part = keep.part;
    h.kind = keep.kind;
    if (!whole) {
      h.textBoxes = keep.textBoxes;
      h.keptXml = keep.keptXml;
      h.displayOnly = keep.displayOnly;
      h.kept = keep.kept;
      h.boxesWritable = keep.boxesWritable;
    }
    h.version = ++g_version;
  }
  void Undo() override { Apply(before, handleBefore); }
  void Redo() override { Apply(after, handleAfter); }
};

// What an insert adds to the package: its media parts, its relationships from the picture's part
// and its content types. Undo takes away exactly those and redo adds them again, keeping what else
// changed in the meantime: a new header or footer part gets its relationship and content type
// outside undo (torture finding 25: putting back whole sets lost them).
struct PackageStep : CustomStep {
  Document* doc = nullptr;
  struct PartState {
    std::string name;
    std::optional<std::vector<uint8_t>> before, after;  // nothing: no such part
  };
  std::vector<PartState> partStates;
  std::string relsSource;                    // the picture's part
  bool relsExisted = true;                   // it had a relationships part before
  std::vector<pkg::Relationship> relsAdded;  // from relsSource
  std::vector<std::pair<std::string, std::string>> defaultsAdded, overridesAdded;  // content types

  bool Any() const {
    return !partStates.empty() || !relsAdded.empty() || !defaultsAdded.empty() || !overridesAdded.empty();
  }
  void Apply(bool undo);
  void Undo() override { Apply(true); }
  void Redo() override { Apply(false); }
};

struct Media {
  std::vector<uint8_t> bytes;
  std::string ext, type;
};
// Where a picture's part goes (an identical one already there is shared), and the relationship to
// it from `source`, added to `step`: the relationship's id.
std::string PlaceMedia(Document& doc, const Part& source, const Media& m, PackageStep& step);
// PNG bytes for premultiplied BGRA pixels; empty when WIC can't.
std::vector<uint8_t> EncodePng(const draw::Bitmap& bmp);

std::string Escape(std::string_view s);
std::string Utf8(std::u16string_view s);
std::u16string Utf16(std::string_view s);

}  // namespace dx
}  // namespace fw::doc::edit
