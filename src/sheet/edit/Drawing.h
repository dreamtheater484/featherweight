#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// A sheet's drawing part (xl/drawings/drawingN.xml, S3, s3-charts): its objects (charts, pictures,
// shapes) as Excel lists them, each a top-level anchor. Edits rewrite one object's bytes and keep
// the rest of the part as it was. The edit layer's Editor ops put the result into the package.
namespace fw::sheet::edit::drawing {

// A top-level element of the drawing: an anchor, or an mc:AlternateContent holding anchors.
struct Object {
  size_t begin = 0, end = 0;  // its bytes in the part
  uint32_t id = 0;            // its first cNvPr's id (the shape id); 0 when it has none
  bool alternate = false;     // mc:AlternateContent
};
// The drawing's objects in order (back to front); empty when it can't be read.
std::vector<Object> Objects(std::string_view xml);
const Object* Find(const std::vector<Object>& objects, uint32_t id);

// A shape id the drawing doesn't use: one past the highest cNvPr id.
uint32_t NextShapeId(std::string_view xml);

// Where an object goes, as the drawing writes it. EMU (12700 a point); x and y from the sheet's
// top left.
struct Marker {
  uint32_t col = 0, row = 0;
  int64_t colOff = 0, rowOff = 0;
};
struct Place {
  Marker from, to;
  int64_t x = 0, y = 0, cx = 0, cy = 0;
};
// `anchor` (one object's bytes) placed at `p`: the corners of a twoCellAnchor, the corner and
// size of a oneCellAnchor, the position and size of an absoluteAnchor, and the shape's own
// position and size (its xfrm). Empty when it isn't an anchor.
std::string Placed(std::string_view anchor, const Place& p);

// The relationship ids an object's bytes use (r:id, r:embed, r:link, r:pict and the like).
std::vector<std::string> RelIds(std::string_view object);

}  // namespace fw::sheet::edit::drawing
