#pragma once

#include <memory>
#include <string>

#include "slide/view/Deck.h"

// A stand-in for p1-model's reader and p1-render's renderer while they're being written: it reads
// the slide list, slide size, hidden slides, the shapes' text and boxes, notes and hyperlinks
// straight from the package, and draws each slide as a white page with its text. Replaced by the
// adapter over the real model and renderer as they land (p1-plan "Order and dependencies").
namespace fw::slide::view {

// Null with a message in `error` when the file isn't a presentation that can be read.
std::unique_ptr<Deck> OpenPlaceholderDeck(const std::wstring& path, std::wstring& error);

}  // namespace fw::slide::view
