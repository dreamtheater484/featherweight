#pragma once

#include <cstdint>
#include <vector>

#include "draw/Painter.h"

// Smart guides and snapping (P2, p2-objects; plan §8.2): while objects move or resize, their box
// snaps to the slide's edges and centre, to other objects' edges and centres, and to equal spacing
// between neighbours, and the guides that matched are shown. Boxes are axis-aligned (a turned
// object's bounds), in slide points. Alt turns snapping off; the grid is used only where no guide
// matches.
namespace fw::slide::objects {

struct Guide {
  enum class Kind : uint8_t {
    Vertical,    // a line x = at, from y = from to y = to
    Horizontal,  // a line y = at, from x = from to x = to
    GapX,        // equal spacing: a horizontal gap from x = from to x = to, drawn at y = at
    GapY,        // a vertical gap from y = from to y = to, drawn at x = at
  };
  Kind kind = Kind::Vertical;
  float at = 0, from = 0, to = 0;
};

struct SnapSettings {
  float tolerance = 4;    // points: how near counts (the view passes a few pixels' worth)
  float grid = 0;         // points; 0: no grid
  bool slide = true;      // the slide's edges and centre
  bool objects = true;    // other objects' edges and centres
  bool spacing = true;    // equal spacing between neighbours
};

struct Snap {
  float dx = 0, dy = 0;   // what to add to the box's offset
  std::vector<Guide> guides;  // what matched, after snapping
};

// Snapping a box being moved: `moving` is where the drag puts it now. `others` are the boxes of the
// objects that don't move, at the same level (the slide, or the group being edited).
Snap SnapMove(const draw::Rect& moving, const std::vector<draw::Rect>& others, draw::Size slide, const SnapSettings& s);

// Snapping the edges a resize drags: `left`/`right`/`top`/`bottom` say which edges move (an
// unturned object's handle). dx moves the dragged vertical edge, dy the horizontal one.
struct Edges {
  bool left = false, right = false, top = false, bottom = false;
};
Snap SnapResize(const draw::Rect& box, Edges edges, const std::vector<draw::Rect>& others, draw::Size slide,
                const SnapSettings& s);

}  // namespace fw::slide::objects
