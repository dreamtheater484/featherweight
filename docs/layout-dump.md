# Layout dump

The layout dump is what Featherweight's Word layout puts on each page, written so that it can be measured against Word (plan §10, milestone D1). D1's pagination writes one per document with `fw::WriteLayoutDump` (`src/core/LayoutDump.h`, part of `fwcore`, so the layout library doesn't depend on `fwtool`). `fwtool compare` reads it, and `fwtool lines` writes the same format from Word's own PDF, which is how the measurement tests itself.

## Format, version 1

UTF-8 JSON. Each line of the document is one line of the file, so dumps read and diff well:

```json
{
  "format": "featherweight-layout",
  "version": 1,
  "source": "layout/report.docx",
  "producer": "Featherweight 1.1.0",
  "pages": [
    {"width": 612, "height": 792, "lines": [
      {"story": "header", "x": 72.02, "baseline": 46.44, "width": 133.15, "size": 11.04, "text": "Featherweight layout report"},
      {"story": "body", "x": 72.02, "baseline": 105.26, "width": 274.26, "size": 15.96, "text": "Chapter 1. But column light footer simple"},
      {"story": "footer", "x": 280.61, "baseline": 753.72, "width": 51.14, "size": 11.04, "text": "Page 1 of 8"}
    ]}
  ]
}
```

| Field | Meaning |
|---|---|
| `format` | Always `featherweight-layout`. |
| `version` | `1`. See [Versions](#versions). |
| `source` | The document, named as the reference manifest names it (`layout/report.docx`, relative to the corpus). |
| `producer` | What wrote the dump, for people reading it. |
| `pages` | Every page, in order, blank pages included. |
| `pages[].width`, `height` | The page size in points. |
| `pages[].lines` | The lines drawn on the page. |

Each line:

| Field | Meaning |
|---|---|
| `story` | Where the text comes from: `body`, `header`, `footer`, `footnote`, `endnote`, `cell` (a table cell), `textbox` (a text box, or a shape with text) or `lineNumber` (a line number in the margin, w:lnNumType). |
| `x` | The left edge of the first glyph (its pen position), in points from the page's left edge. |
| `baseline` | The baseline, in points from the page's top edge. |
| `width` | From `x` to the end of the last glyph's advance, trailing spaces left out, in points. |
| `size` | The largest font size on the line, in points. |
| `text` | What is drawn, in reading order. |

Numbers have at most two decimals. Points are 1/72 inch.

## What the layout writes

- **One line object per line drawn**, with the text as drawn: list numbers and bullets, note reference marks, field results (page numbers), and the hyphen added where a word is hyphenated at the end of the line. A tab is written as `\t` or a space; tab leaders may be left out. Hidden text isn't drawn, so it isn't written. Lines without text (an empty paragraph) may be left out.
- **The story:** when stories nest, the outer one wins in this order: `header` and `footer`, then `footnote` and `endnote`, then `textbox`, then `cell`, then `body`. So a table in a footnote is `footnote`, and a text box in a header is `header`. Text in a frame is `body`.
- **The order of lines:** stories can come in any order on a page, but within a story, write lines in reading order: top to bottom in a column, columns left to right, table cells row by row with each cell's lines together, and each text box's lines together. A table's repeated header row is written on every page it's drawn on.
- **Positions** are only shown, not scored (see below). They help find why a line breaks differently: a baseline that drifts says the line heights or spacing differ, a different `width` says the text is measured differently.

## How it's compared

`fwtool compare` (`tools/fwtool/Fidelity.cpp`) measures two things per document, with Word's PDF as the reference:

- **Page count:** do they match?
- **Line breaks:** for each of Word's lines, does Featherweight's line end at the same word?
  - Words are compared by their letters and digits only, in lowercase, with ligatures spelled out. Punctuation, quotes, bullets, dashes and tab leaders don't count, so how a PDF maps those glyphs doesn't matter. Ideographs and kana are words of their own.
  - A word hyphenated at a line end is joined again, and the line ends inside it. Such a line matches only when Featherweight hyphenates the same word at the same place.
  - Lines are grouped in five areas: the main text (body and table cells), notes, headers, footers and text boxes. Each area's words are aligned on their own, in reading order: common words at the start and end first, then words that occur once on each side as anchors, then a longest common subsequence between anchors. Words left over on both sides are aligned again across areas, so a text box that one side lists elsewhere still counts. Because each line is judged by its own end, one difference doesn't shift the comparisons after it.
  - Word's lines without words (a lone bullet, a line of leaders) aren't counted.

The totals over a corpus are the share of documents whose page count matches, and the share of all of Word's lines that break where Word breaks them (D1's targets are 90 % and 95 %, plan §6.4).

## Word's side

`fwtool lines` reads Word's PDF export (`tests/office/reference/`, made with tags by `scripts/reference-renders.ps1`) with PDFium:

- Characters come in the order Word drew them. A line is a run of characters in the same container, on the same baseline (give or take half the font size, so superscripts and note marks stay on their line), that doesn't jump back to the left (except in right-to-left text). The baseline is that of the line's largest text.
- Turned text (vertical text in text boxes and table cells) is read in its own direction: a character's angle, in quarter turns, turns its position back first, so its line runs down or up the page instead of falling apart into a line per glyph. Such a line is placed (`x`, `baseline`) where its first character is, and its header or footer half comes from there too.
- Containers and stories come from the PDF's tag tree. Word tags table cells `TD` and `TH`, footnotes `Footnote`, endnotes `Endnote` and text boxes `Textbox` (role-mapped to `Note` and `Sect`); a line never crosses one of them.
- Headers and footers are untagged pagination artifacts. Word doesn't say which is which, so an artifact line in the top half of the page is a header, and in the bottom half a footer.
- A gap of at least 0.18 em between characters is a space: Word often draws no space glyph at a tab or after a note mark. PDFium's hyphen at a line end (U+0002) is written as `-`.

## Versions

`version` goes up only when a change would make an older reader misread a dump: a field changes its meaning, or a new field is required. Readers refuse a dump with a higher version than they know. New optional fields don't change the version, and readers ignore fields they don't know.
