#pragma once

#include <array>
#include <cstdint>

#include "draw/Painter.h"

// Where an object sits on a slide, and what dragging its handles does to it (P2, p2-objects; plan
// §8.2 "Objects"). Headless and in slide points, so the tab, tests and the torture script share it.
//
// A frame is the object's box before rotation, turned clockwise about its centre, then flipped
// within the box: DrawingML's a:xfrm in the slide's coordinates. Handles are numbered clockwise
// from the top left, as documents and workbooks number theirs; lines have only their two ends.
namespace fw::slide::objects {

struct Frame {
  draw::Rect rect;          // before rotation
  float rotation = 0;       // degrees clockwise, [0, 360)
  bool flipH = false, flipV = false;

  draw::Point Centre() const { return {(rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2}; }
  // The corners as shown: top left, top right, bottom right, bottom left of the unturned box.
  std::array<draw::Point, 4> Corners() const;
  // The axis-aligned box around the turned frame: what guides, align and distribute use, as
  // PowerPoint does.
  draw::Rect Bounds() const;
  // The object's own coordinates (0, 0)-(width, height), flips included, to the slide's.
  draw::Matrix Transform() const;
  // A line's ends as shown: from the box's top left to its bottom right, flips and turn applied.
  std::array<draw::Point, 2> LineEnds() const;
  // A line through two points (rotation 0, flips saying which way it runs).
  static Frame FromLine(draw::Point from, draw::Point to);
};

enum class Handle : int8_t {
  None = -1,
  TopLeft, Top, TopRight, Right, BottomRight, Bottom, BottomLeft, Left,  // 0-7, clockwise
  Rotate,   // the round handle above the top edge
  Start, End,  // a line's ends
  Body,     // inside the object: moves it
};
inline bool IsCorner(Handle h) { return h >= Handle::TopLeft && h <= Handle::Left && (static_cast<int>(h) % 2) == 0; }
inline bool IsSizing(Handle h) { return h >= Handle::TopLeft && h <= Handle::Left; }

// What a frame offers: lines have their two ends and no turn handle; tables and charts can't be
// flipped; groups and pictures with locked aspect keep it at the corners.
struct HandleSet {
  bool line = false;
  bool rotate = true;
  bool lockAspect = false;
};

// Where a handle is drawn, slide points. The turn handle sits `rotateOffset` points above the top
// edge's middle (along the turned frame).
draw::Point HandlePoint(const Frame& f, Handle h, float rotateOffset);

// The handle under `p`, within `tolerance` points; Body when `p` is inside the frame (within
// `tolerance` of a line), None otherwise. Handles win over the body, the turn handle over others.
Handle HitHandle(const Frame& f, const HandleSet& set, draw::Point p, float tolerance, float rotateOffset);

// Whether `p` is on the frame (inside the turned box, or within `tolerance` of a line's segment).
bool HitFrame(const Frame& f, bool line, draw::Point p, float tolerance);

struct ResizeOptions {
  bool keepAspect = false;  // Shift, or a locked aspect: corners keep the proportions
  bool fromCentre = false;  // Ctrl: the opposite side moves too
  float minSize = 0;        // points; lines may be 0 wide
};
// The frame after dragging handle `h` from `from` to `to` (slide points). Dragging past the
// opposite side flips the object, as PowerPoint does.
Frame Resize(const Frame& start, Handle h, draw::Point from, draw::Point to, const ResizeOptions& o);

// A line after dragging one end to `to`; `snap45` (Shift) keeps it at a multiple of 45 degrees.
Frame MoveLineEnd(const Frame& start, Handle end, draw::Point to, bool snap45);

// The frame turned by the drag of the turn handle from `from` to `to` about its centre; `snap15`
// (Shift) rounds the result to 15 degrees.
Frame Rotate(const Frame& start, draw::Point from, draw::Point to, bool snap15);

// Degrees in [0, 360).
float NormalizeDegrees(float degrees);

// Rectangle helpers.
draw::Rect Union(const draw::Rect& a, const draw::Rect& b);
draw::Rect Normalized(draw::Point a, draw::Point b);
bool Contains(const draw::Rect& outer, const draw::Rect& inner);
bool Intersects(const draw::Rect& a, const draw::Rect& b);
draw::Rect Offset(const draw::Rect& r, float dx, float dy);

}  // namespace fw::slide::objects
