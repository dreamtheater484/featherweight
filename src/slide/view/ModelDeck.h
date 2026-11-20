#pragma once

#include <memory>
#include <string>

#include "slide/view/Deck.h"

// The tab's Deck over p1-model's presentation (fwslide) and p1-render's renderer: slides drawn as
// PowerPoint draws them (prepared once per slide and kept for the slides near the one shown),
// notes pages from the deck's own notes pages, the shapes' resolved text with each paragraph's
// place for find, and the shapes' and runs' links.
namespace fw::slide::view {

// Null with a plain-language message in `error` when the file can't be read as a presentation.
std::unique_ptr<Deck> OpenDeck(const std::wstring& path, std::wstring& error);

}  // namespace fw::slide::view
