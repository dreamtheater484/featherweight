#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "slide/view/Deck.h"

// A new, blank presentation (P2, p2-slides): what File > New > Presentation opens and the build-
// from-scratch script starts from. It's the deck PowerPoint 2021 makes: 16:9, the Office theme,
// the Office master with its eleven layouts (Title Slide … Vertical Title and Text), and one empty
// Title Slide, its theme fwdraw's (draw::Theme::DefaultXml). Shipped as the bytes the generator
// (NewDeckSource.cpp, in the tests only) makes.
namespace fw::slide::view {

// The .pptx's bytes.
std::vector<uint8_t> BlankPresentation();
// The generator of those bytes (NewDeckSource.cpp: tests only).
std::vector<uint8_t> GenerateBlankPresentation();

// A deck read from bytes (a new presentation, a recovered one). Null with a plain message in
// `error` when they aren't a presentation.
std::unique_ptr<Deck> OpenDeckFromBytes(std::vector<uint8_t> bytes, std::wstring& error);

}  // namespace fw::slide::view
