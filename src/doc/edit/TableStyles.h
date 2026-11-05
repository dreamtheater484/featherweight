#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "doc/edit/Editor.h"

namespace fw::doc::edit {

// Table styles (plan §6.2 "Tables"): the document's own, and a few of Word's basics that it may
// not have yet ("Table Grid", "Plain Table 1", "Grid Table 1 Light", "Grid Table 4 - Accent 1").

// The names a table style box offers: the document's table styles (not the hidden default one),
// then the basics it doesn't have, by name.
std::vector<std::string> TableStyleNames(const Document& doc);

// The style of the table around `at`, by name: the document's, or a basic added to its styles part
// with Word's definition. One undo step. Refused for a name that's neither.
Result ApplyTableStyle(Editor& editor, Pos at, std::string_view name);

}  // namespace fw::doc::edit
