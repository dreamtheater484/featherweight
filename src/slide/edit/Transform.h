#pragma once

#include <pugixml.hpp>

#include <cstdint>
#include <vector>

// A shape's place as the file holds it (a:xfrm, or a graphic frame's p:xfrm), read and written, and
// the math of grouping and ungrouping: the one copy, shared by the edit layer's shape operations and
// p2-objects' ObjectXml (whose Xfrm is this Box).
namespace fw::slide::edit::xfrm {

struct Box {
  int64_t x = 0, y = 0, cx = 0, cy = 0;  // EMU, in the parent's coordinates
  int32_t rot = 0;                       // 60000ths of a degree
  bool flipH = false, flipV = false;
  int64_t chX = 0, chY = 0, chCx = 0, chCy = 0;  // a group's child space
  bool child = false;                            // chOff and chExt present
};

// spPr's or grpSpPr's a:xfrm, or a graphicFrame's p:xfrm; with `create` made where it goes (spPr or
// grpSpPr too), with the prefixes in scope. Null when there's none (or the element has no place for one).
pugi::xml_node XfrmOf(pugi::xml_node object, bool create);
// False when the object has no xfrm. rot is taken modulo a full turn, a negative size reads as 0,
// and child space counts only with both chOff and chExt.
bool Read(pugi::xml_node object, Box& out);
// off and ext (chOff and chExt too when b.child; left as they are otherwise), rot and the flips (a
// zero or false one removed, as PowerPoint writes them); the xfrm made when missing.
void Write(pugi::xml_node object, const Box& b);
// The box around `children` (rotated ones by their turned corners); its child space is that box.
Box GroupBox(const std::vector<Box>& children);
// A child of group `group` in the group's parent space: scaled by ext/chExt, the group's flips
// mirroring it about the centre (a single flip turns the child's rotation the other way), the
// group's rotation added about its centre, rotation kept in [0, 21600000), flips combined.
Box Ungrouped(const Box& group, const Box& child);

}  // namespace fw::slide::edit::xfrm
