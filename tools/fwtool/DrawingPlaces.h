#pragma once

// A torture rule (D5): drawing objects where Word doesn't allow them. Word refuses a file with a
// shape or a floating drawing in a footnote, an endnote, a comment or a text box's text ("You
// can't put drawing objects into a text box, callout, comment, footnote, or endnote"); an inline
// picture or chart there is fine. Kept out of Torture.cpp, so the unit tests can drive it.
#include <map>
#include <string>
#include <string_view>

#include "pkg/Package.h"

namespace fwtool::torture {

// How many such drawings one XML part `part` has, by place: "a shape or floating drawing in a
// footnote (/word/footnotes.xml)" -> 1. A shape is a wps, wpg or wpc drawing, or VML with a
// text box; a floating drawing is a wp:anchor, or VML positioned absolutely. Inside
// mc:AlternateContent only the branch a reader takes counts, so a shape and its VML copy are one.
std::map<std::string, int> MisplacedDrawings(std::string_view part, std::string_view xml);
// The same, over every WordprocessingML part of a package.
std::map<std::string, int> MisplacedDrawings(const fw::pkg::Package& pkg);

}  // namespace fwtool::torture
