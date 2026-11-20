// Tables in slides (a:tbl): the grid, merged cells, and the table style applied per cell.
#include "slide/Internal.h"

namespace fw::slide {

const Table* Presentation::GetTable(const Shape& shape, const SlideBase& shownOn) {
  (void)shape;
  (void)shownOn;
  return nullptr;  // next step
}

}  // namespace fw::slide
