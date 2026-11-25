// A new, blank presentation (P2, p2-slides): the .pptx as NewDeckSource.cpp generates it, shipped
// as its bytes (21 KB of data instead of 50 KB of generator code). The test
// SlideNew_BlankMatchesSource checks they agree; FW_BLANK_INC=<path> fw_tests writes them again.

#include "slide/view/NewDeck.h"

#include <iterator>

namespace fw::slide::view {

namespace {

#include "slide/view/NewDeckData.inc"

}  // namespace

std::vector<uint8_t> BlankPresentation() { return {std::begin(kBlankDeck), std::end(kBlankDeck)}; }

}  // namespace fw::slide::view
