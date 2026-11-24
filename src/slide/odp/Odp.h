#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "pkg/Limits.h"
#include "pkg/Status.h"

// OpenDocument presentations (.odp, and .otp templates) as PowerPoint presentations (P3, p3-files):
// the slides with their backgrounds and hidden flags, each master page as a slide layout with its
// shapes, and every shape with what it draws (text frames, custom shapes with their geometry,
// pictures, tables, connectors and lines, groups) and its text (paragraphs, lists and bullets,
// runs with their fonts, sizes, colors and links, slide numbers and dates), written as .pptx bytes
// that the .pptx reader opens (as D5 does for .odt and S4 for .ods). Charts, notes, transitions and
// animations aren't read yet: the slide opens without them. The source is never written to: a
// presentation opened from one is saved with Save As, as a .pptx.
namespace fw::slide::odp {

pkg::Status OdpToPptx(std::span<const uint8_t> odp, std::vector<uint8_t>& pptx,
                      const pkg::Limits& limits = pkg::Limits::Default());

}  // namespace fw::slide::odp
