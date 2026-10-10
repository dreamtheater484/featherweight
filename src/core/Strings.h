#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fw {

std::wstring Utf8ToWide(std::string_view s);
std::string WideToUtf8(std::wstring_view s);

std::wstring ToLower(std::wstring_view s);
std::wstring Trim(std::wstring_view s);
std::string Trim(std::string_view s);
std::vector<std::wstring> Split(std::wstring_view s, wchar_t sep);
bool EqualsNoCase(std::wstring_view a, std::wstring_view b);
bool EndsWithNoCase(std::wstring_view s, std::wstring_view suffix);

// Path helpers that accept both '\' and '/'.
std::wstring FileNameOf(std::wstring_view path);
std::wstring DirOf(std::wstring_view path);

uint64_t Fnv1a64(std::wstring_view s);
std::wstring Hex64(uint64_t v);

// "125%" style label for a zoom factor (1.0 == 100%).
std::wstring FormatPercent(double factor);

// Friendly relative time, e.g. "Just now", "5 minutes ago", "Yesterday".
std::wstring RelativeTime(int64_t secondsAgo);

// Parses a decimal integer; returns false on anything else.
bool ParseInt(std::wstring_view s, int& out);

}  // namespace fw
