#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "doc/edit/Editor.h"

// Formatted text for the clipboard (milestone D2): what copying takes out of a document, what
// pasting puts into one, and the small converters between it and RTF and HTML, both ways. Only
// what a reader sees is kept: bold, italic, underline, strikethrough, raised and lowered text,
// size, color and font; alignment, indents, spacing and headings. Copying within Featherweight
// also keeps style names and, in the same document, lists.
namespace fw::doc::edit {

struct ClipRun {
  std::u16string text;  // \t for a tab, \v for a line break; never a paragraph break
  bool bold = false, italic = false, underline = false, strike = false;
  VertAlign vertAlign = VertAlign::Baseline;
  int32_t halfPoints = 0;          // the size; 0 when not said (pasting keeps the size there)
  std::optional<uint32_t> color;   // 0xRRGGBB; none for automatic
  std::string font;                // UTF-8; empty when not said
  std::string charStyle;           // the w:rStyle id (Featherweight's own copies)
  bool SameLook(const ClipRun& o) const {
    return bold == o.bold && italic == o.italic && underline == o.underline && strike == o.strike &&
           vertAlign == o.vertAlign && halfPoints == o.halfPoints && color == o.color && font == o.font &&
           charStyle == o.charStyle;
  }
};

enum class ClipAlign : uint8_t { Left, Center, Right, Justify };

struct ClipPara {
  std::vector<ClipRun> runs;
  ClipAlign align = ClipAlign::Left;
  int32_t heading = 0;      // 1-9 for a heading (its outline level + 1), 0 for body text
  std::u16string label;     // the list label shown ("•", "2."), when the paragraph starts here
  // In twips; nullopt when not said.
  std::optional<int32_t> indent, firstLine, spaceBefore, spaceAfter;
  // Featherweight's own copies: the paragraph style and the list (used in the same document).
  std::string style, numId;
  int32_t ilvl = 0;
};

// Paragraphs with a paragraph break between each two: text that ends with a paragraph mark has an
// empty last paragraph. A paragraph's own formatting comes with its mark, so pasting applies it to
// every paragraph but the last.
struct Clip {
  std::vector<ClipPara> paras;
  uint64_t source = 0;  // the document it came from (Featherweight's own copies), 0 when unknown
  bool Empty() const;
  // As plain text: list labels and a tab before the text, CR LF between paragraphs and for line
  // breaks.
  std::u16string PlainText() const;
};

// The text in [from, to) of one story, as shown: deleted text, field codes and hidden text left
// out. `source` names the document, for pasting lists back into it.
Clip CopyClip(const Document& doc, Pos from, Pos to, uint64_t source = 0);

// Replaces [from, to) with the clip, as one undo step ("Paste"). `target` names the document as
// CopyClip's `source` does. Formatting the text there already has is left alone; the rest is set
// as direct formatting. Nothing changes when the editor refuses.
Result PasteClip(Editor& editor, Pos from, Pos to, const Clip& clip, uint64_t target = 0);

// The converters. RTF is 7-bit ASCII ("Rich Text Format" on the clipboard); HTML is the
// clipboard's "HTML Format" (CF_HTML: a header with offsets, then UTF-8). Featherweight's own
// copies carry their source, styles and lists in data-fw-* attributes.
std::string ToRtf(const Clip& clip);
std::string ToHtml(const Clip& clip);
std::optional<Clip> FromRtf(std::string_view rtf);
// Takes CF_HTML (only the fragment is read) or plain HTML.
std::optional<Clip> FromHtml(std::string_view html);
// True when the HTML came from Featherweight.
bool IsOwnHtml(std::string_view html);

}  // namespace fw::doc::edit
