#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fw {

// The layout dump (docs/layout-dump.md): the pages of a laid-out document and the lines on them.
// D1's pagination writes one per document; `fwtool compare` measures it against Word's own PDF
// (plan §10), and `fwtool lines` writes the same format from that PDF.

// Where a line's text comes from.
enum class LayoutStory { Body, Header, Footer, Footnote, Endnote, Cell, TextBox };

struct LayoutLine {
  LayoutStory story = LayoutStory::Body;
  double x = 0;         // the left edge of the first glyph, in points from the page's left edge
  double baseline = 0;  // in points from the page's top edge
  double width = 0;     // from x to the right edge of the last glyph, in points
  double size = 0;      // the largest font size on the line, in points
  std::wstring text;    // what is drawn, in reading order: list labels, note marks, field results
                        // and a hyphen added at the break included
};

struct LayoutPage {
  double width = 0;   // points
  double height = 0;
  std::vector<LayoutLine> lines;  // in reading order within each story
};

struct LayoutDump {
  static constexpr int kVersion = 1;
  std::string source;    // the document as the reference manifest names it, "layout/report.docx"
  std::string producer;  // what wrote the dump, "Featherweight 1.1.0"
  std::vector<LayoutPage> pages;
};

std::string_view StoryName(LayoutStory story);  // "body", "header", ...
std::optional<LayoutStory> StoryFromName(std::string_view name);

// UTF-8 JSON, one line of text per line of the document, so dumps read and diff well.
std::string WriteLayoutDump(const LayoutDump& dump);
// Refuses a dump from a newer major version, or one that isn't a layout dump; `error` says why.
std::optional<LayoutDump> ReadLayoutDump(std::string_view json, std::string* error = nullptr);

}  // namespace fw
