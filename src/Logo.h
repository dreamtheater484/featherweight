#pragma once

#include "Gfx.h"

namespace logo {

// Draws the Featherweight mark (rounded tile with a feather) into `size` x `size` pixels at (x, y).
void Draw(gfx::Canvas& c, float x, float y, float size);
// Draws just the feather glyph in a single color (for small chrome icons).
void DrawFeather(gfx::Canvas& c, float cx, float cy, float length, D2D1_COLOR_F color);

}  // namespace logo
