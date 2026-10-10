#pragma once

#include <optional>
#include <string_view>
#include <vector>

namespace fw {

// Parses page ranges such as "1-3, 7, 10-" (1-based, as people type them) into
// 0-based page indices in the order given. "10-" means page 10 to the end.
// Returns nullopt for invalid input or pages outside 1..pageCount.
std::optional<std::vector<int>> ParsePageRanges(std::wstring_view text, int pageCount);

}  // namespace fw
