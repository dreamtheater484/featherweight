#pragma once

#include <memory>
#include <string>

#include "slide/view/Deck.h"

// The tab's Deck over p1-model's presentation (fwslide): slides, sizes, hidden slides, the shapes'
// resolved text, notes and links. Slides are drawn through p1-render's renderer once it's in the
// integration branch; until then as a white page with each shape's text in its place.
namespace fw::slide::view {

// Null with a plain-language message in `error` when the file can't be read as a presentation.
std::unique_ptr<Deck> OpenDeck(const std::wstring& path, std::wstring& error);

}  // namespace fw::slide::view
