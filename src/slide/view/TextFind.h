#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "slide/view/TextDoc.h"
#include "slide/view/TextModel.h"

// Find and replace in a deck (P2, p2-text; plan §8.2): the text of every slide's shapes, text boxes,
// placeholders and table cells, then its speaker notes, slide by slide, as PowerPoint searches.
// Matches stay within a paragraph. Replacing goes through the edit layer: one match is one undo
// step ("Replace"), Replace All one step for the whole deck ("Replace All").
namespace fw::slide::view {

struct TextMatch {
  TextTarget target;
  TextPos from, to;
};

// Every match, in order: slide by slide, each slide's shapes in z-order (cells row by row), then
// its notes. `wholeWords`: no letter or digit just before or after.
std::vector<TextMatch> FindAll(TextModel& model, std::wstring_view query, bool matchCase, bool wholeWords);

// The text body of a target as an editable working copy (for replacing, and for the tab).
std::unique_ptr<TextDoc> OpenText(TextModel& model, const TextTarget& target);

// Replaces one match (its text takes the look of the match's first character). False when the
// text there isn't the match any more, or the edit layer refused.
bool ReplaceMatch(TextModel& model, const TextMatch& match, std::wstring_view query, bool matchCase, std::wstring_view with);
// Replaces them all as one undo step; how many it replaced.
int ReplaceAll(TextModel& model, const std::vector<TextMatch>& matches, std::wstring_view with);

}  // namespace fw::slide::view
