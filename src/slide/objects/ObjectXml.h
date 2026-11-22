#pragma once

#include <pugixml.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "draw/Painter.h"
#include "slide/objects/Frame.h"

// Objects as PresentationML writes them (P2, p2-objects; plan §8.2 "Objects"): the transform of a
// shape, picture, connector, graphic frame or group; its fill, outline, shadow and crop; new
// objects as PowerPoint 2021 inserts them; grouping and ungrouping; a table's rows and columns.
//
// Every function works on one object's element (p:sp, p:pic, p:cxnSp, p:graphicFrame, p:grpSp) in
// whatever document holds it: the edit layer hands over a copy, these change it, and the edit
// layer records the change as one operation (§4.3 rule 4). Only what's asked changes: the rest of
// the element, unknown children and attributes included, stays as it was, and new children go
// where the schema puts them. Prefixes are the ones in scope for the namespaces, else p:, a: and r:.
namespace fw::slide::objects {

inline constexpr int64_t kEmuPerPoint = 12700;
inline int64_t ToEmu(float points) { return static_cast<int64_t>(points * static_cast<double>(kEmuPerPoint) + (points < 0 ? -0.5 : 0.5)); }
inline float ToPoints(int64_t emu) { return static_cast<float>(static_cast<double>(emu) / kEmuPerPoint); }

// a:xfrm (p:xfrm on a graphic frame) as written: EMUs, rotation in 60000ths of a degree. A group
// also has its children's space (chOff, chExt).
struct Xfrm {
  int64_t x = 0, y = 0, cx = 0, cy = 0;
  int32_t rot = 0;
  bool flipH = false, flipV = false;
  int64_t chX = 0, chY = 0, chCx = 0, chCy = 0;
  bool child = false;  // chOff/chExt present
};

// What kind of object an element is.
enum class ObjectKind : uint8_t { Shape, Picture, Connector, GraphicFrame, Group, Other };
ObjectKind KindOf(pugi::xml_node object);

// The transform element: spPr/a:xfrm, p:xfrm, grpSpPr/a:xfrm (or a:xfrm inside them). Null when
// the object has none (a placeholder taking its layout's place).
pugi::xml_node XfrmNode(pugi::xml_node object);
bool ReadXfrm(pugi::xml_node object, Xfrm& out);
// Writes the transform, making the element when there's none. Attributes at their defaults
// (rot 0, no flips) are removed, as PowerPoint writes them.
void WriteXfrm(pugi::xml_node object, const Xfrm& x);

// Points and degrees <-> EMUs and 60000ths, in the object's parent's coordinates.
Frame ToFrame(const Xfrm& x);
Xfrm FromFrame(const Frame& f, const Xfrm& like);  // keeps `like`'s children's space

// A group's children's space in slide points: `groupToSlide` maps the group's own box (0, 0)-(ext)
// to the slide (its frame's Transform, or render::ShapeInfo::transform).
draw::Matrix ChildSpace(const Xfrm& group, const draw::Matrix& groupToSlide);
// A frame in a parent's coordinates (mapped to the slide by `parentToSlide`) as seen on the slide,
// and back. Identity for objects on the slide itself.
Frame FrameOnSlide(const Frame& local, const draw::Matrix& parentToSlide);
Frame FrameInParent(const Frame& onSlide, const draw::Matrix& parentToSlide);
// A move by (dx, dy) slide points, in the parent's coordinates.
draw::Point DeltaInParent(draw::Point delta, const draw::Matrix& parentToSlide);
// Moves the object by EMUs, leaving its size exactly as it was.
void MoveBy(pugi::xml_node object, int64_t dx, int64_t dy);

// ---- Identity
uint32_t ObjectId(pugi::xml_node object);  // cNvPr id; 0 when missing
void SetObjectId(pugi::xml_node object, uint32_t id);
std::string ObjectName(pugi::xml_node object);
void SetObjectName(pugi::xml_node object, std::string_view name);
std::string AltText(pugi::xml_node object);  // cNvPr descr
void SetAltText(pugi::xml_node object, std::string_view text);
bool IsPlaceholder(pugi::xml_node object);
// The highest cNvPr id under `tree` (a p:spTree, groups included).
uint32_t MaxObjectId(pugi::xml_node tree);
// PowerPoint's name for a new object: "<base> <id - 1>".
std::string NewName(std::string_view base, uint32_t id);

// ---- New objects, appended to `parent` (a p:spTree or p:grpSp) as PowerPoint 2021 inserts them.
// Shapes take the theme's style (accent 1 fill, a darker accent 1 outline, light text centred).
pugi::xml_node AppendShape(pugi::xml_node parent, uint32_t id, std::string_view preset, const Xfrm& at,
                           std::string_view name);
// A text box: no fill or outline, one empty paragraph, the box growing with its text.
pugi::xml_node AppendTextBox(pugi::xml_node parent, uint32_t id, const Xfrm& at);
// A line or an arrow (a straight connector; `arrow` puts a triangle at its end).
pugi::xml_node AppendLine(pugi::xml_node parent, uint32_t id, const Xfrm& at, bool arrow);
// A picture `relId` (the slide's relationship to the media part), stretched, its aspect locked.
pugi::xml_node AppendPicture(pugi::xml_node parent, uint32_t id, std::string_view relId, const Xfrm& at,
                             std::string_view name, std::string_view altText);
// A table of `rows` x `columns` empty cells filling `at`, with a table style (PowerPoint's
// default, Medium Style 2 - Accent 1, when empty), header row and banded rows.
pugi::xml_node AppendTable(pugi::xml_node parent, uint32_t id, const Xfrm& at, int rows, int columns,
                           std::string_view styleId = {});
// A chart: the graphic frame naming chart part `relId`.
pugi::xml_node AppendChart(pugi::xml_node parent, uint32_t id, std::string_view relId, const Xfrm& at);
// An empty group whose box and children's space are both `at`.
pugi::xml_node AppendGroup(pugi::xml_node parent, uint32_t id, const Xfrm& at);

// Where a new picture of `natural` size (points) goes on a slide of `slide` size: as large as it
// is, scaled down to fit the slide if it's larger, centred.
draw::Rect PlacePicture(draw::Size natural, draw::Size slide);

// ---- Grouping. A group's box is the box around its children (turned ones' bounds); the children
// keep their coordinates, which are the group's children's space at first.
Xfrm GroupBox(const std::vector<Xfrm>& children);
// A child of `group` taken out of it: its transform in the group's parent's coordinates (moved,
// scaled and turned as the group shows it). A child group's own children's space stays.
Xfrm Ungrouped(const Xfrm& group, const Xfrm& child);

// ---- Format
struct Paint {
  enum class Kind : uint8_t {
    Automatic,  // none written: the shape's style (p:style) decides
    None,       // a:noFill
    Solid,      // a:solidFill with an sRGB color
    Gradient,   // a:gradFill, linear, from `rgb` to `rgb2`
    Picture,    // a:blipFill of picture `relId`, stretched
    Other,      // read only: a theme color, a pattern, a gradient of more stops...
  };
  Kind kind = Kind::Automatic;
  uint32_t rgb = 0, rgb2 = 0xFFFFFF;  // 0xRRGGBB
  float angle = 90;                   // Gradient: degrees clockwise from left to right
  std::string relId;                  // Picture
};
// The object's fill, outline color, width and dash as written (spPr): what the Format panel shows.
struct Format {
  bool fillable = false;   // has a fill (shapes, pictures' background, not lines or frames)
  bool lineable = false;   // has an outline (not graphic frames or groups)
  Paint fill, line;
  float lineWidth = 0;     // points; 0 when not written
  std::string dash;        // a:prstDash; empty: solid or not written
  bool shadow = false;     // an outer shadow in a:effectLst
  bool picture = false;    // a p:pic: can be cropped
};
Format ReadFormat(pugi::xml_node object);

// Each returns false (changing nothing) when the object has no such property: a graphic frame
// has no fill, a group no outline.
bool SetFill(pugi::xml_node object, const Paint& paint);
bool SetLine(pugi::xml_node object, const Paint& paint);  // Solid or None or Automatic (Gradient: its first color)
bool SetLineWidth(pugi::xml_node object, float points);
bool SetLineDash(pugi::xml_node object, std::string_view preset);  // "" or "solid": solid
// PowerPoint's "Offset: Bottom Right" outer shadow, or none (other effects stay).
bool SetShadow(pugi::xml_node object, bool on);

// A picture's crop: fractions cut from each side (a:srcRect, in 1/100000), negative to pad.
struct Crop {
  float left = 0, top = 0, right = 0, bottom = 0;
};
Crop ReadCrop(pugi::xml_node picture);
bool SetCrop(pugi::xml_node picture, const Crop& c);
// Cropping by dragging the frame's edges: the crop that keeps the picture where it is when its
// frame goes from `before` to `after` (same rotation), given the crop before.
Crop CropForFrame(const Crop& c, const draw::Rect& before, const draw::Rect& after);

// ---- Tables (a p:graphicFrame holding a:tbl). The frame's size follows the grid.
pugi::xml_node TableOf(pugi::xml_node frame);
int TableRows(pugi::xml_node frame);
int TableColumns(pugi::xml_node frame);
// A new row before row `at` (0 to rows), like the row next to it, with empty cells; merged cells
// spanning the place grow. A new column the same way.
bool InsertTableRow(pugi::xml_node frame, int at);
bool InsertTableColumn(pugi::xml_node frame, int at);
// The row or column goes; merged cells over it shrink, and a merge starting in it starts in the
// next one. False for the last row or column (delete the table instead).
bool DeleteTableRow(pugi::xml_node frame, int row);
bool DeleteTableColumn(pugi::xml_node frame, int column);
void SetTableStyle(pugi::xml_node frame, std::string_view styleId);
std::string TableStyle(pugi::xml_node frame);
// tblPr's options: firstRow, lastRow, firstCol, lastCol, bandRow, bandCol.
bool TableOption(pugi::xml_node frame, const char* option);
void SetTableOption(pugi::xml_node frame, const char* option, bool on);

// The element as text (no declaration, no formatting), for the clipboard and tests.
std::string ToXml(pugi::xml_node node);

}  // namespace fw::slide::objects
