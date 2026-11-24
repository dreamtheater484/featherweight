#pragma once

#include <pugixml.hpp>

#include <cstdint>
#include <string>
#include <string_view>

// The shapes of an OpenDocument presentation as DrawingML (P3, p3-files): a custom shape's enhanced
// geometry (its equations evaluated, its path drawn), polygons, and draw:transform.
namespace fw::slide::odp {

// An a:custGeom for draw:enhanced-geometry on a shape `w` by `h` EMU: the equations evaluated with
// the shape's modifiers, the path in its viewBox's units, arcs as Bézier curves, and the first text
// area as the text rectangle. Empty when there's no path to draw.
std::string CustomGeometry(pugi::xml_node geometry, int64_t w, int64_t h);
// An a:custGeom for svg:points (a polygon, or a polyline when `closed` is false) in `viewBox`.
std::string PolygonGeometry(std::string_view points, std::string_view viewBox, bool closed);

// draw:transform ("rotate (0.5) translate (1in 2in)", applied in the order written): a point at
// (x, y) EMU goes to (a·x + b·y + e, c·x + d·y + f).
struct Transform {
  double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
  bool Read(std::string_view text);  // false when it isn't one
  // The rotation it makes, clockwise in degrees (DrawingML's sense), in [0, 360).
  double Degrees() const;
};

}  // namespace fw::slide::odp
