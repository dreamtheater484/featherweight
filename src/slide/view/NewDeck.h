#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "slide/view/Deck.h"

// A new, blank presentation (P2, p2-slides): what File > New > Presentation opens and the build-
// from-scratch script starts from. It's the deck PowerPoint 2021 makes: 16:9, the Office theme,
// the Office master with its eleven layouts (Title Slide … Vertical Title and Text), and one empty
// Title Slide. Generated, not shipped: the theme is fwdraw's (draw::Theme::DefaultXml).
namespace fw::slide::view {

// The .pptx's bytes.
std::vector<uint8_t> BlankPresentation();

// A deck read from bytes (a new presentation, a recovered one). Null with a plain message in
// `error` when they aren't a presentation.
std::unique_ptr<Deck> OpenDeckFromBytes(std::vector<uint8_t> bytes, std::wstring& error);

}  // namespace fw::slide::view
