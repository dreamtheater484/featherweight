#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "draw/Painter.h"

// Arrange (P2, p2-objects; plan §8.2): align and distribute selected objects, and their order
// (bring forward, send backward, to front, to back), as PowerPoint does them. Pure: boxes in,
// offsets or an order out; the tab turns them into edits.
namespace fw::slide::objects {

enum class AlignTo : uint8_t { Left, Centre, Right, Top, Middle, Bottom };

// Offsets (dx, dy) moving each box (axis-aligned bounds, slide points) into line. With `toSlide`
// (PowerPoint's choice when one object is selected) they line up with the slide's edge or centre,
// else with the selection's outermost box.
std::vector<draw::Point> Align(const std::vector<draw::Rect>& boxes, AlignTo how, bool toSlide, draw::Size slide);

// Offsets spreading the boxes evenly across (horizontal) or down: equal gaps between them, the
// first and last staying where they are; with `toSlide` (one or two objects, or by choice) the
// first goes to the slide's edge and the last to the other edge.
std::vector<draw::Point> Distribute(const std::vector<draw::Rect>& boxes, bool horizontal, bool toSlide, draw::Size slide);

enum class Order : uint8_t { Forward, Backward, ToFront, ToBack };

// The siblings' new order after moving the selected ones (`selected[i]` for sibling i, bottom to
// top): a permutation of 0..n-1 listing the old indexes bottom to top. Selected objects keep
// their order among themselves; forward and backward pass one unselected neighbour.
std::vector<size_t> Reorder(const std::vector<bool>& selected, Order how);

}  // namespace fw::slide::objects
