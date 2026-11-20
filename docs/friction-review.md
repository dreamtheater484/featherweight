# Friction review

The friction review (plan §11, "Zero friction") writes each core flow down step by step, in Word and in Featherweight, so that a flow never gains a step without a reason in the plan. D3 is done when it passes for writing a letter, formatting a CV and fixing a table in a received document (plan §6.4). D5 adds the rest of the Word flows, D3's to D5's (flows 4 to 15).

S4 adds the workbook flows (plan §7, the S4 row; s2-plan's "total a column" and "fix a value in a received workbook"): see [Workbooks (S4)](#workbooks-s4).

`tests/friction-checks.ps1` runs the flows that can be scripted through Featherweight's `--script` mode, off-screen. It checks what the saved file says and counts the steps against this review's budget (see [Scripted checks](#scripted-checks)).

## How steps are counted

- **C**: a click (or a drag). **K**: a key or a chord (Ctrl+S is one). **D**: a decision the flow asks for (a name, a format, a place).
- Typing the content itself, including Enter between paragraphs, isn't counted: it's the same in both.
- **Word** means Word for Microsoft 365 on Windows (version 2410 and later) with its default ribbon, in a window at least 1366 pixels wide.
- **Featherweight** means claude/d2-integration 5957ba4, with D3's operations added as they land, for flows 1 to 3. Flows 4 to 15 were counted on claude/d5-friction (2026-11-06), from d5-int with the D5 pieces.
- **A scroll** in a list counts as a step. A list shows what fits in a 700-pixel window.
- When a step has a mouse way and a keyboard way, the table gives the one most people use, and the other in brackets.

## Summary

| Flow | Word | Featherweight today | Featherweight once the findings are fixed | Status |
|---|---|---|---|---|
| Write a letter | 6 C + 3 K, 3 D | 5 C + 3 K, 2 D | 5 C + 3 K, 2 D | Passes (claude/d3-int, 2026-11-05: 5 C + 3 K, 8 of 9). The letter flow is scripted. |
| Format a CV (without tab stops and margins) | 2 C + 9 K | 3 C + 9 K | 3 C + 9 K (the one extra click is F4, accepted) | Passes (claude/d3-int, 2026-11-05: 3 C + 9 K, 12 of 12). Tab stops and margins come with D3 (the ruler, page setup). The CV flow is scripted. |
| Fix a table in a received document | 10 C + 2 K + 2 drags | 11 C + 2 K + 2 drags | 11 C + 2 K + 2 drags (the two extra clicks are F6, accepted) | Passes (claude/d3-int, 2026-11-05: 11 C + 2 K + 2 drags, 15 of 16). The table flow is scripted. |
| Pictures | 6 C + 1 drag | 5 C + 1 drag | | Passes |
| Headers and page numbers | 5 C + 1 K | 4 C + 1 K | | Passes |
| Page setup | 7 C | 6 C | | Passes (8 C before F14) |
| Links and bookmarks | 6 K + 2 C | 6 K + 2 C | | Passes (10 before F9 and F10) |
| Review a contract | 7 C + 2 K | 7 C + 2 K | | Passes (11 before F11) |
| Table of contents and notes | 7 C + 1 K | 4 C + 1 K | | Passes (a scroll more before F12) |
| Fill in a form | 5 C + 1 K | 5 C + 1 K | | Passes |
| Text boxes, shapes, watermark | 9 C + 1 drag | 8 C (+1 scroll) | | Passes. The watermark is one click more (F15, accepted) |
| Export PDF | 4 C | 2 C + 1 K | | Passes |
| A letter from a template | 3 C + 2 K | 2 C + 2 K | | Passes (a drag more before F16) |
| Opening other formats | 1 C, Ctrl+S | 1 C, Ctrl+S + Enter | | Saving a `.doc` is one key more (F17, by design) |
| A password-protected document | 1 C + 1 K | 1 C + 1 K | | Passes (`claude/d5-crypt`) |

## Where Featherweight needs more than Word

| # | Flow, step | Word | Featherweight | Owner | Status |
|---|---|---|---|---|---|
| F1 | Letter and CV: choosing a style in the style box, then typing | Typing goes on in the page | Focus stays in the box: typing goes nowhere until a click in the page or Esc. **+1 every time** | widgets (the toolbar's dropdown) | Fixed (claude/d3-tables 75c11ed): after a choice from a toolbar list, typing goes to the page |
| F2 | CV: Enter after a heading or the title | The next paragraph is Normal (the style's `w:next`) | It stays Heading 1 / Title. **+1 K per heading** (Ctrl+Shift+N) | widgets (Enter in the editor) | Fixed (claude/d3-tables 75c11ed): Enter at a paragraph's end takes the style's w:next |
| F3 | CV: Enter on an empty list item | Ends the list: the item becomes plain text | Stays a list item, and so does what follows. **+1 per list** | widgets, with d2-format (lists) | Fixed (claude/d3-tables f31d826): Enter on an empty item ends the list (one undo step) |
| F4 | Letter and CV: a style without a shortcut (No Spacing, Title, Subtitle, Quote) | 1 C in the Home tab's gallery | 2 C: open the style box, choose | Accepted: one toolbar row, no ribbon (plan §5.2). Normal and Heading 1–3 have Word's keys in both. | By design |
| F6 | Table: a row in the middle | 1 C on the ⊕ at the row's edge | 3 C: right-click, Insert, Rows Below (as Word's own menu) | Accepted: the review's target for this step was at most 3 C. A handle is worth a look once layout gives cell boxes. | Accepted |
| F7 | Table: widening a column | 1 drag of its border | Dragging a border selects text. Today: right-click, Properties, the Column width box, a number, Enter: **3 C + 1 K instead of 1 drag** | widgets (d3-tables), waiting on d1-model's `Layout::Cells(page)` | Fixed (claude/d3-tables 7abe49e): column borders and row bottoms drag on the page |
| F5 | Letter: the date, in a narrow window | The ribbon shrinks, Insert stays | Below about 1150 pixels wide, the Insert box drops out of the toolbar: then it's command search (Alt+Q, "date", Enter) | d2-format (toolbar priorities) | Fixed with F13 (claude/d5-friction): Insert now goes after change case, the indents, ¶, the navigation pane, the table grid, Layout, the format painter, Review and Highlight |
| F8 | Letter and CV: Ctrl+Shift+N for the Normal style | Normal style | A new document: the app's Ctrl+N took Ctrl+Shift+N too (5998996). The letter flow failed on d5-int. | widgets | Fixed (claude/d5-friction 3367a2d) |
| F9 | Links: Ctrl+K, then the address | Ctrl+K, type, Enter | The panel didn't take the keyboard, and Enter didn't insert: **1 K + 2 C instead of 2 K** | widgets | Fixed (claude/d5-friction 33827ce) |
| F10 | Bookmarks: Ctrl+Shift+F5, then the name | Type, Enter | The same as F9: **+2 steps** | widgets | Fixed (claude/d5-friction 33827ce) |
| F11 | Review: replying to a comment | Its Reply box, type, Ctrl+Enter | Its card, the box, type, the Reply button: **+1 C** | widgets | Fixed (claude/d5-friction 33827ce): the card puts the keyboard in the box for a reply, and Enter posts |
| F12 | Insert ▾: footnote, endnote, table of contents, watermark, content controls | One tab each (References, Design) | Past the list's 12 rows: **+1 scroll** | widgets | Fixed (claude/d5-friction 80ff7f0): notes, the TOC, text box and watermark come right after Picture. The list shows up to 24 rows. Shapes, breaks and content controls come last. |
| F13 | Insert ▾ in a narrower window | Insert's tab stays | Insert was the first box to drop out (F5) | widgets | Fixed (claude/d5-friction 80ff7f0) |
| F14 | Page setup: paper size | Layout, Size, A4: 3 C | Layout ▾, Page setup…, Paper, A4: **4 C** | widgets | Fixed (claude/d5-friction 80ff7f0): Layout ▾ lists the sizes |
| F15 | Watermark | Design, Watermark, a preset: 3 C | Insert ▾, Watermark…, Kind: Text: **4 C** | | Accepted: one panel for text and picture watermarks, with the presets in its text box. It's rare. |
| F16 | Templates: replacing a placeholder | Click it (a content control), type | The placeholders were plain text in brackets: a drag or a triple-click to select one | widgets | Fixed (claude/d5-friction): each placeholder is a content control showing its placeholder, in Word's grey Placeholder Text. One click selects it, and typing replaces it. |
| F17 | Saving a `.doc` | Ctrl+S keeps the `.doc` | Ctrl+S opens Save As with `<name>.docx` suggested: **+1 K** | | By design: imported formats are never overwritten (D5 plan) |
| F18 | Opening an `.odt` | No notice. Word asks about the format on the first save | A `.doc` showed a banner that saving makes a new `.docx`; an `.odt` didn't | widgets | Fixed (claude/d5-friction): the same banner for an `.odt` |

Also found while scripting: **torture finding 7** happens in the app. Centering a line with Ctrl+E and pressing Enter lets AutoCorrect's capital fix garble the line ("second" saved as "SecondS"). Reported to d1-measure as part of finding 7.

## Where Featherweight needs less

- **The date** in a letter: Insert box, then a format (2 C), against Word's Insert tab, Date & Time, a format and OK (4 C).
- **Saving a new file**: Featherweight suggests the first words as the name, in Documents. Word also asks where to save (OneDrive or this PC): one decision fewer.
- **A received document** opens ready to edit. Word opens attachments and downloads in Protected View, with an "Enable Editing" click first.
- **Updating a table of contents**: right-click, Update entire table (2 C), against Word's four clicks and a dialog.
- **Export PDF**: ⋯, Export PDF…, Enter, against Word's File, Export, Create PDF/XPS, Publish.
- **Page numbers**: Insert ▾, Page numbers…, Insert (3 C), against Word's four.
- **Content controls** are in Insert ▾. Word needs the Developer tab turned on first.
- **A template** from the start screen: one click, against Word's two (and Word's featured templates change).

## 1. Write a letter

A block letter: the sender's address, the date, the recipient's address, the salutation, three paragraphs, the closing and the name. Then it's saved and printed.

| # | Step | Word | Featherweight |
|---|---|---|---|
| 1 | A new, blank document | Start screen, Blank document: 1 C (Ctrl+N: 1 K) | Start screen, New document: 1 C (Ctrl+N: 1 K) |
| 2 | Address lines without space between them | Home, No Spacing in the gallery: 1 C | Style box, No Spacing: 2 C (F4). Today also +1 (F1). (Shift+Enter at each line's end, in both: no style needed.) |
| 3 | The sender's address | typed | typed |
| 4 | The date | Insert, Date & Time, a format, OK: 4 C, 1 D | Insert box, a format: 2 C, 1 D |
| 5 | The recipient's address | typed | typed |
| 6 | Back to normal paragraphs for the body | Ctrl+Shift+N: 1 K (or Home, Normal: 1 C) | Ctrl+Shift+N: 1 K (or the style box: 2 C) |
| 7 | Salutation, body, closing, name | typed | typed |
| 8 | A spelling error | Right-click, the suggestion: 2 C | Right-click, the suggestion: 2 C |
| 9 | Save | Ctrl+S, then name and place, Enter: 2 K, 2 D | Ctrl+S, then the name (suggested), Enter: 2 K, 1 D |
| 10 | Print | Ctrl+P, Print: 1 K + 1 C | Ctrl+P, Print: 1 K + 1 C |
| | **Scripted part (1, 2, 4, 6, 9)** | **6 C + 3 K = 9** | **5 C + 3 K = 8** (today 9, F1) |

## 2. Format a CV

One page: the name as the title, a contact line centered under it, and four sections (Profile, Experience, Education, Skills) as Heading 1. Each job is a line with its title in bold and its dates against the right margin, followed by bullets. The margins are narrow. Then it's saved.

| # | Step | Word | Featherweight |
|---|---|---|---|
| 1 | A new, blank document | 1 C | 1 C |
| 2 | The name as Title | Home, Title in the gallery: 1 C | Style box, Title: 2 C (F4). Today +1 (F1). |
| 3 | Enter: the next paragraph is normal text | 0 | Today +1 K (F2) |
| 4 | The contact line, centered | Ctrl+E: 1 K | Ctrl+E: 1 K |
| 5 | A section heading, then Enter | Ctrl+Alt+1: 1 K (or the gallery: 1 C) | Ctrl+Alt+1: 1 K. Today +1 K after it (F2). |
| 6 | The job title in bold | Ctrl+B twice: 2 K | Ctrl+B twice: 2 K |
| 7 | The dates against the right margin | Show the ruler (View, Ruler: 2 C, once). Pick a right tab in the tab selector (2 C). Click the ruler at the margin (1 C). Then Tab: 1 K | Tab: 1 K. Setting the tab stop waits for the ruler (D3, claude/d3-page); target no more than Word's 5 C. |
| 8 | Achievements as bullets | Ctrl+Shift+L: 1 K (or the Bullets button: 1 C; or "* ") | The same |
| 9 | End the list | Enter on the empty item: typed | Today +1 (F3) |
| 10 | Narrow margins | Layout, Margins, Narrow: 3 C | Waits for page setup (D3, claude/d3-page); target no more than 3 C |
| 11 | Save | Ctrl+S, name and place, Enter: 2 K, 2 D | Ctrl+S, the name, Enter: 2 K, 1 D |
| | **Scripted part (1–6, 7 without the tab stop, 8, 9, 11), two headings** | **2 C + 9 K = 11** | **3 C + 9 K = 12**, F4 accepted (today 17: F1, F2 ×3, F3) |

Export PDF comes in D4 and isn't part of D3's review.

## 3. Fix a table in a received document

A .docx received by mail, with a four-column table (`tests/office/torture/received-table.docx`, made by `make_torture_docs.py`). Fix a typo in a cell, add a row at the end and one in the middle, delete a row, merge two header cells, widen a column, and save.

| # | Step | Word | Featherweight |
|---|---|---|---|
| 1 | Open the attachment | Double-click: 1 C, then Enable Editing in Protected View: 1 C | Double-click: 1 C. It opens ready to edit. |
| 2 | Fix a typo in a cell ("Chiar") | Double-click the word, type: 1 C | The same |
| 3 | A row at the end | Click in the last cell, Tab: 1 C + 1 K | The same |
| 4 | A row in the middle | The ⊕ at the row's left edge: 1 C (or right-click, Insert, Insert Rows Below: 3 C) | Right-click, Insert, Rows Below: 3 C (F6, accepted) |
| 5 | Delete a row | Right-click, Delete, Delete Rows: 3 C | Right-click, Delete, Rows: 3 C |
| 6 | Merge two header cells | Select them (drag), right-click, Merge Cells: 1 drag + 2 C | The same |
| 7 | Widen a column | Drag its border: 1 drag | Today: right-click, Properties, Column width, type, Enter: 3 C + 1 K (F7). Once border drags land: 1 drag. |
| 8 | Save | Ctrl+S: 1 K | Ctrl+S: 1 K. The original is kept for 30 days as a backup, with no step. |

Word: 10 C + 2 K + 2 drags (14). Featherweight today: 14 C + 3 K + 1 drag (18); with F7 fixed, 11 C + 2 K + 2 drags (15), within the budget of 16 (Word's 14 and F6's two clicks).

## 4. Pictures (D3)

A picture from a file, made smaller, with the text wrapping around it.

| # | Step | Word | Featherweight |
|---|---|---|---|
| 1 | Insert a picture from a file | Insert, Pictures, This Device, double-click the file: 4 C | Insert ▾, Picture…, double-click the file: 3 C. Dragging it in from Explorer takes 1 drag in both. |
| 2 | Smaller, keeping its shape | Drag a corner (it's selected after inserting): 1 drag | The same |
| 3 | Text wraps around it | The Layout Options button, Square: 2 C | Wrap text ▾ (on the toolbar while it's selected), Square: 2 C |
| | **Total** | **6 C + 1 drag** | **5 C + 1 drag** |

## 5. Headers and page numbers (D3)

A line of text in the header, and page numbers centered at the bottom.

| # | Step | Word | Featherweight |
|---|---|---|---|
| 1 | Edit the header | Double-click the top margin: 1 C | The same |
| 2 | Back to the text | Esc: 1 K (or double-click the body) | The same |
| 3 | Page numbers at the bottom, centered | Insert, Page Number, Bottom of Page, Plain Number 2: 4 C | Insert ▾, Page numbers… (Bottom and Center are the defaults), Insert page number: 3 C |
| | **Total** | **5 C + 1 K** | **4 C + 1 K** |

## 6. Page setup (D3)

A4 paper, narrow margins, landscape.

| # | Step | Word | Featherweight |
|---|---|---|---|
| 1 | A4 | Layout, Size, A4: 3 C | Layout ▾, Size: A4: 2 C (it was 4 C through Page setup: F14) |
| 2 | Narrow margins | Margins, Narrow (the Layout tab is open): 2 C | Layout ▾, Margins: Narrow: 2 C |
| 3 | Landscape | Orientation, Landscape: 2 C | Layout ▾, Landscape: 2 C |
| | **Total** | **7 C** | **6 C** |

## 7. Links and bookmarks (D3)

A link on selected text to a web page, a bookmark on a heading, and a link to that heading.

| # | Step | Word | Featherweight |
|---|---|---|---|
| 1 | A link on the selection | Ctrl+K, type the address, Enter: 2 K, 1 D | The same (it was 1 K + 2 C: F9) |
| 2 | A bookmark | Ctrl+Shift+F5, type the name, Enter: 2 K, 1 D | The same (it was 1 K + 2 C: F10) |
| 3 | A link to a heading | Ctrl+K, Place in This Document, the heading, Enter: 2 K + 2 C | Ctrl+K, open the address list, the heading, Enter: 2 K + 2 C |
| | **Total** | **6 K + 2 C** | **6 K + 2 C** |

## 8. Review a contract (D4)

A received contract with tracked changes and comments (`tests/office/torture/review-contract.docx`). Accept one change, reject the next, reply to one comment, resolve another, and save.

| # | Step | Word | Featherweight |
|---|---|---|---|
| 1 | The review tools | The Review tab: 1 C | The 💬 Review pane: 1 C |
| 2 | The first change | Next: 1 C | Next: 1 C |
| 3 | Accept it and go to the next | Accept (and Move to Next): 1 C | Accept: 1 C (it moves on) |
| 4 | Reject that one | Reject (and Move to Next): 1 C | Reject: 1 C |
| 5 | Reply to a comment | Its Reply box: 1 C, type, Ctrl+Enter: 1 K | Its card: 1 C (the box gets the keyboard), type, Enter: 1 K. It was 3 C: F11 |
| 6 | Resolve another | Its ⋯ menu, Resolve thread: 2 C | Its card, Resolve: 2 C |
| 7 | Save | Ctrl+S: 1 K | Ctrl+S: 1 K |
| | **Total** | **7 C + 2 K** | **7 C + 2 K** |

## 9. Table of contents and notes (D4)

A table of contents at the start, a footnote, and the table updated after a new heading.

| # | Step | Word | Featherweight |
|---|---|---|---|
| 1 | The table of contents | References, Table of Contents, Automatic Table 1: 3 C | Insert ▾, Table of contents: 2 C (it needed a scroll: F12) |
| 2 | A footnote | Ctrl+Alt+F: 1 K | Ctrl+Alt+F: 1 K |
| 3 | Update the table | Click in it, Update Table, Update entire table, OK: 4 C | Right-click in it, Update entire table: 2 C |
| | **Total** | **7 C + 1 K** | **4 C + 1 K** |

## 10. Fill in a form (D4)

A form with content controls: a text box, a check box, a drop-down list and a date.

| # | Step | Word | Featherweight |
|---|---|---|---|
| 1 | The text | Click the placeholder (it's selected), type: 1 C | The same |
| 2 | The check box | 1 C | 1 C |
| 3 | An item from the list | Open it, the item: 2 C | The same (or Alt+Down) |
| 4 | The date | Click the control, type the date: 1 C (or the calendar: 3 C) | Click it, type the date: 1 C. Its menu has Today, Yesterday and Tomorrow, but no calendar (worth a look, not a finding). |
| 5 | Save | Ctrl+S: 1 K | Ctrl+S: 1 K |
| | **Total** | **5 C + 1 K** | **5 C + 1 K** |

Inserting content controls is much shorter. Word needs the Developer tab first (File, Options, Customize Ribbon, Developer, OK), then Developer and the control. Featherweight has them in Insert ▾ (2 C, plus a scroll in a 700-pixel window).

## 11. Text boxes, shapes and watermarks (D4)

| # | Step | Word | Featherweight |
|---|---|---|---|
| 1 | A text box | Insert, Text Box, Simple Text Box: 3 C | Insert ▾, Text box: 2 C |
| 2 | A rectangle | Insert, Shapes, Rectangle, then drag to draw it: 3 C + 1 drag | Insert ▾, Rectangle: 2 C, at a default size (plus a scroll in a 700-pixel window) |
| 3 | A "DRAFT" watermark | Design, Watermark, DRAFT 1: 3 C | Insert ▾, Watermark…, Kind: Text (DRAFT is the default): 4 C (F15, accepted) |
| | **Total** | **9 C + 1 drag** | **8 C** |

## 12. Export PDF (D4)

| # | Step | Word | Featherweight |
|---|---|---|---|
| 1 | A PDF beside the document | File, Export, Create PDF/XPS, Publish (the name is suggested): 4 C | ⋯, Export PDF…, Enter (the name is suggested): 2 C + 1 K |

## 13. A letter from a template (D5)

The letter template, the sender's name filled in, saved.

| # | Step | Word | Featherweight |
|---|---|---|---|
| 1 | The template, from the start screen | The template, Create: 2 C, when it's among the featured ones (otherwise More templates and a search) | Templates: Letter: 1 C |
| 1b | The template, from an open document | File, New, the template, Create: 4 C | +, New from template, Letter: 3 C |
| 2 | Replace a placeholder | Click it (it's a content control), type: 1 C | The same (it took a drag: F16) |
| 3 | Save | Ctrl+S, name and place, Enter: 2 K, 2 D | Ctrl+S, the name, Enter: 2 K, 1 D |
| | **Total (from the start screen, one placeholder)** | **3 C + 2 K** | **2 C + 2 K** |

## 14. Opening other formats (D5)

| # | Step | Word | Featherweight |
|---|---|---|---|
| 1 | Open a `.doc` or an `.odt` | Double-click: 1 C (and Enable Editing when it was downloaded: 1 C) | Double-click: 1 C. A banner says that saving makes a new `.docx` and leaves the file as it is (F18). |
| 2 | Save a `.doc` | Ctrl+S keeps it a `.doc` (Compatibility Mode): 1 K | Ctrl+S, then Enter in Save As, where `<name>.docx` beside it is suggested: 2 K (F17, by design) |
| 3 | Save an `.odt` | Ctrl+S, then "Use Word format" in the format question: 1 K + 1 C | Ctrl+S, then Enter: 2 K |
| 4 | Open a `.dotx` | Double-click: a new document from it: 1 C | The same |

## 15. A password-protected document (D5)

| # | Step | Word | Featherweight (`claude/d5-crypt`) |
|---|---|---|---|
| 1 | Open it | Double-click, type the password, Enter: 1 C + 1 K, 1 D | Double-click, type the password in the tab, Enter: 1 C + 1 K, 1 D |
| 2 | A wrong password | An error, OK, then open it again: +2 C | The box shakes and says so; type it again |
| 3 | Save | Ctrl+S, with the same password: 1 K | The same |

## Scripted checks

```
powershell -ExecutionPolicy Bypass -File tests\friction-checks.ps1 [-Workarounds]
```

- **How each flow runs.** `Featherweight.exe [<file>] --snapshot --size 1100x700 --script @<flow>.script`: a new document (or a copy of the received one), the flow's clicks, keys, drags and typing, then `save:` (standing for Ctrl+S, and Enter in the Save dialog for a new file). The right-click menu is Windows' own, which a script can't click: a right-click is a click there and its items are the command they run (`cmd:<id>`), counted as the clicks they take.
- **What's checked afterwards.**
  - `fwtool dump --model` must show each paragraph with the style, list label and text the flow wrote.
  - `fwtool check` must pass, and the table flow's saved XML must have the wider column, the merged header cells and no Lamp row.
  - The step count must stay within the budget: Word's count plus the extras accepted here (F4, F6).
- **Images and saved files** go to `build\friction`.
- **The scripts are the flows as they should go.** All three pass on claude/d3-int (F1–F3 and F7 fixed). A finding that comes back makes its flow fail on what the saved file says.
- **`-Workarounds`** adds the steps the findings need today. The content then checks out, and the counts show the cost: the letter takes 9 (budget 9), the CV 17 (budget 12), the table 18 (budget 16).
- **Click coordinates** (the style box and its list, the table's cells and the Table panel's Column width box) are for that window size. A toolbar change that moves them makes the check fail, and it's fixed here.

The checks open Featherweight's own window off-screen, and nothing else. They aren't in CI, like `visual-checks.ps1`.

## Workbooks (S4)

This is the spreadsheet part of the friction review (plan §11, "Zero friction"; s2-plan: "total a column" and "fix a value in a received workbook", never more steps than Excel's; S4 plan: the friction review for spreadsheets). It uses the Word part's rules (`docs/friction-review.md`, "How steps are counted").

- **C** is a click (or a drag), **K** is a key or a chord, and **D** is a decision (a name, a place, a format). Typing the content isn't counted, and neither is the Enter that ends a cell entry. A scroll in a list counts as a step.
- **Excel** means Excel for Microsoft 365 on Windows, with the default ribbon, in a window at least 1366 px wide. Excel's steps come from its standard UI and weren't measured here. Two of them are marked *(check in Excel)*.
- **Featherweight** means `claude/s4-corpus` 18c7831 (worktree `s1-corpus-aa316c`), read from the code. The app wasn't run. Each step cites the code. For flows 10 and 11 the S4 branches were also read: `claude/s4-int` 3d550f5 (the .ods reader merged) and `claude/s4-ods-e4ae8e` cc6e522 (password-protected workbooks, not yet on s4-int).
- **The workbook toolbar** is counted with every item shown. That needs a window at least 1557 px wide at 100 % scaling (the sum of the item widths, `Toolbar.cpp:206-226`). At 1366 px some items drop out (W1): a snapshot of the workbook tab 1366 pixels wide (`Featherweight.exe --snapshot --size 1366x400`) shows no Merge, Styles or Data box, and one 1600 pixels wide shows all three.
- When a step has a mouse way and a keyboard way, the table gives the one most people use, and the other in brackets.

### Summary

| Flow | Excel | Featherweight | Notes |
|---|---|---|---|
| 1. Open and read a workbook | 7 C | 6 C | Passes. Freezing the top row is one click fewer. |
| 2. Find a value | 3 K | 2 K | Passes. The first match is selected as you type, and there's no dialog to close. |
| 3. Total a column | 1 C + 2 K | 1 C + 2 K | Passes (s2-plan flow). There's no AutoSum button. Selecting the numbers and then AutoSum doesn't work: W5. |
| 4. Fix a value in a received workbook and save | 3 C + 1 K | 2 C + 1 K | Passes (s2-plan flow). There's no Protected View, and saving an .xlsx asks nothing. A .csv that now holds more than values asks once (W9, by design). |
| 5. Sort and filter a list | 11 C + 1 K | 14 C | **+2**: the number filter (W4) and Filter on by mouse (W8, accepted?). At 1366 px, Data ▾ isn't on the toolbar (W1). |
| 6. Make a chart of a range | 1 C + 1 K (mouse: 4 C) | 1 C + 1 K (mouse: 3 C) | Passes. In a workbook opened from a .csv, it's refused until saved as .xlsx (W7). |
| 7. Make a table | 1 C + 2 K | 2 C + 1 K | Passes with the default style. A chosen style takes 2 clicks more by mouse (W6). |
| 8. Conditional formatting: values above X | 1 drag + 3 C + 1 K | 1 drag + 4 C | Passes (5 = 5). |
| 9. Print, fit to one page wide; Export PDF | Print 1 K + 3 C; PDF 4 C | Print 2 K + 4 C; PDF 2 C + 1 K | Print is **+2** (W3). Export PDF passes. |
| 10. Open an .xls or .ods and save it | 1 C, then 1 K | .xls: 1 C, then 2 K | Saving an .xls is one key more (W10, by design, as F17). .ods isn't in this build (W12); it's on s4-int with the same steps as .xls. |
| 11. A password-protected workbook | 1 C + 1 K, 1 D | Refused in this build | W11. cc6e522 (s4-ods) gives Excel's count. To be counted once it lands. |
| 12. A workbook from a template (.xltx) | 1 C, then 2 K, 2 D | 1 C, then 2 K, 1 D | Passes from Explorer. There's no template list inside the app (W13). |

### Where Featherweight needs more than Excel

| # | Flow, step | Excel | Featherweight | Area | File:line | Status |
|---|---|---|---|---|---|---|
| W1 | 5 and 7, in a 1366 px window (100 % scaling) | The Data tab is always there | Data ▾ (priority 2) drops out of the toolbar below about 1463 px, after Styles (below 1557 px). Merge drops below 1389 px. Sort, Custom sort, Remove duplicates, Data validation and Table… are then gone from the toolbar. Sort can only be reached through the filter's panel: Ctrl+Shift+L, the column's button, Sort A to Z (the filter stays on). Data validation and Remove duplicates can't be reached at all. At 150 % on a 1920 px screen (1280 DIPs), the format painter drops too. These widths are computed from the code; the 1366-pixel snapshot shows it. | s2-format (toolbar priorities) | WorkbookFormat.cpp:1395-1424, Toolbar.cpp:206-226 | Fixed (fb2cf25, s4-int ce2cffa): items that don't fit a narrow window go into an overflow menu |
| W2 | Any flow whose toolbar box has dropped out (the Word review's fallback, F5) | Alt+Q finds every command | Alt+Q finds the grid, print, chart and picture commands. It doesn't find Sort, Filter, Table, Conditional formatting, Data validation, Format Cells or the number formats: ids 2700-2799 and 2850-2879 aren't in `Commands()`. | s2-grid (WorkbookCommands), with s2-format and s3-data | WorkbookCommands.cpp:310-324, AppShell.cpp:129-141 | Fixed (fb2cf25, s4-int ce2cffa): command search finds the data and format commands |
| W3 | 9: print with all columns on one page | Ctrl+P, Scaling ▾, Fit All Columns on One Page, Print: 1 K + 3 C | Ctrl+F2, Page setup, Scaling ▾, Fit all columns on one page, Ctrl+P, Print in Windows' dialog: 2 K + 4 C. **+2**. Page setup is on the toolbar only in print preview. Outside it, Page setup is on the sheet tab's menu or Alt+Q; the ⋯ menu's Page setup… is only for documents. | s3-print | WorkbookCommands.cpp:412-417, WorkbookPageSetup.cpp:243-244 and 884-892, WorkbookPrint.cpp:175-193, App.cpp:1552-1560 | Fixed (f3304c5, s4-int ce2cffa): page setup from the sheet, on the toolbar's print group |
| W4 | 5: a number filter (greater than X) | Number Filters, Greater Than…, type (the box has the keyboard), Enter: 3 C + 1 K | Condition ▾, "is greater than", the Value box, type, Apply: 4 C. **+1**: picking a condition doesn't move the keyboard to the box, and Enter there doesn't apply. | s2-format (filter panel) | WorkbookFilter.h:72-78 and 92-93, Controls.cpp:1068-1080 | Fixed (fb2cf25, s4-int ce2cffa) |
| W5 | 3: AutoSum with the numbers selected | Select them, Alt+=: the total goes under them, with no Enter: 1 drag + 1 K | AutoSum works from the active cell only. With the numbers selected, it starts `=SUM(…)` in the selection's first number, and Enter would replace that number. You have to click the cell below first: 1 C + 2 K. **+1**, and a trap. | s2-calc (AutoSum) | WorkbookFunctions.cpp:193-230 (198-199, 214) | Fixed (4930c5b, s4-int ce2cffa): with the numbers selected, the totals go in at once, as Excel's |
| W6 | 7: a table in a chosen style, by mouse | Home, Format as Table, a style (with previews), OK: 3 C | Data ▾, Table…, Style ▾, the style, Create table: 5 C, plus a scroll. **+2**. The style list has 61 names and no previews. | s3-data | WorkbookData.cpp:887-894 and 920-1009 | Fixed (fb2cf25, s4-int ce2cffa) |
| W7 | 6, 7 and 8 in a workbook opened from a .csv | They work (lost when saved as CSV; Excel's bar warns) | Refused, each with its own message. A chart: "Save the workbook as an Excel workbook first, then insert the chart." A table: "Tables need an Excel workbook: save this one as .xlsx first." A rule: "Only a worksheet of an Excel workbook keeps these rules." The menus also show them disabled. Ctrl+Shift+S and Enter come first: **+2 K, 1 D**. | s3-charts, s3-data (edit layer) | Edit.cpp:4452-4454, 5217-5218 and 5266-5267 (src/sheet/edit), WorkbookCharts.cpp:288, WorkbookData.cpp:793, WorkbookShapes.cpp:448, Sheet.h:358 ("empty for CSV") | Fixed (f522e27, s4-int ce2cffa): a workbook from CSV takes charts, tables, rules, pictures, shapes and notes; saving it as CSV asks first |
| W8 | 5: Filter on, by mouse | Filter, with the Data tab open after sorting: 1 C | Data ▾, Filter: 2 C. **+1**. Ctrl+Shift+L is 1 K in both. | s2-format | WorkbookFormat.cpp:1415-1421 | Accepted (the coordinator, 2026-11-19): one toolbar row (plan §5.2), as F4 |
| W9 | 4: saving a .csv that now holds more than values (a formula, a width, formatting, another sheet) | Ctrl+S saves. Excel 365 shows a "Possible data loss" bar and doesn't ask *(check in Excel)* | A banner asks: Save as .xlsx or Keep CSV. **+1 C**, once per tab. | | App.cpp:1910-1921 and 1335-1341, WorkbookCommands.cpp:1031-1040, CsvWrite.cpp:143-185 | By design (s2-plan, WorkbookView.h:122-123) |
| W10 | 10: saving an .xls (and an .ods on s4-int) | Ctrl+S keeps the format (Compatibility Mode): 1 K | Ctrl+S, then Enter in Save As, with `<name>.xlsx` beside it suggested: 2 K. **+1 K** | | WorkbookView.cpp:136-173, App.cpp:1887-1897 | By design (plan §7.1: the .xls is never written), as F17 |
| W11 | 11: a password-protected .xlsx or .xls | Password, Enter | Refused with "…it is protected with a password, which workbooks don't support yet" (.xlsx), or "…it is protected with a password" (.xls) | s4-ods | WorkbookView.cpp:139-150, Xls.cpp:585-590 | Done on `claude/s4-ods-e4ae8e` cc6e522, not merged |
| W12 | 10: opening an .ods | Double-click: 1 C | Not offered: .ods is on the "later" list, so it's not in the Open dialog's filter and not registered with Explorer. Opened anyway, it fails as "…isn't an Excel workbook". | s4-ods | FileKinds.cpp:11-12, App.cpp:1698-1712, FileAssoc.cpp:66, WorkbookView.cpp:155-160 | Done on `claude/s4-int` 3d550f5 |
| W13 | 12: a template from inside the app | File, New, Personal, the template: 4 C | No list. New from template has only Word's Letter, CV and Report. New workbook isn't offered either while the workbook editor isn't marked as shipped. Ctrl+O and double-clicking the .xltx works (1 K + 1 C, plus browsing). | s4-ods (S4 plan line 15: "File → New offers the user's own templates folder"), plus the release flag | App.cpp:1497-1503 and 1510-1523, FileKinds.cpp:22-23, EmptyState.cpp:100-103 | Fixed (093f9a1 the editor ships; ca89ea7 File > New offers the user's templates; s4-int ce2cffa) |

These don't change the counts but are worth a look:

- **AutoSum has no toolbar button.** By mouse it's Alt+Q, "autosum", Enter (WorkbookFormat.cpp:1267-1459 has no AutoSum item; WorkbookCommands.cpp:314 puts it in command search).
- **The Create table and Conditional formatting panels don't take the keyboard** (AppShell.cpp:170-178). Ctrl+T then Enter doesn't create the table: it's a click on Create table. The rule's Value box needs a click. The counts tie with Excel, but the hand goes to the mouse.
- **The Conditional formatting panel keeps the last rule kind** (WorkbookData.cpp:244). The first time it's "Greater than". After another kind, it's 2 clicks to choose again.
- **Find searches the shown text** (WorkbookCommands.cpp:728). Excel's default looks in formulas: a value shown as "1,234.50" is found by "1,234.50" here and by "1234.5" in Excel.

### Where Featherweight needs less

- **A received workbook** opens ready to edit. Excel opens attachments and downloads in Protected View, with Enable Editing first.
- **Find** selects the first match as you type, and Esc goes back to the cell: 2 K against Excel's 3.
- **Freeze the top row**: right-click A2, Freeze panes (2 C), against View, Freeze Panes, Freeze Top Row (3 C).
- **A chart by mouse**: Chart ▾, then a type (2 C), against Insert, the type's gallery, a subtype (3 C).
- **Export PDF**: ⋯, Export PDF…, Enter (2 C + 1 K), against File, Export, Create PDF/XPS, Publish (4 C).
- **Saving a workbook made from a template**: `Budget1.xlsx` beside the template is suggested, so there's one decision (the name) against Excel's two.

### 1. Open and read a workbook

A received .xlsx with three sheets and a long list, whose header row is to stay in view.

| # | Step | Excel | Featherweight |
|---|---|---|---|
| 1 | Open it | Double-click: 1 C (reading works in Protected View) | Double-click: 1 C (App.cpp:1635-1639 and 1649-1692). The file's active sheet, zoom and selection show (WorkbookView.cpp:189-213). |
| 2 | The next sheet | Click its tab: 1 C (Ctrl+PgDn: 1 K) | The same (WorkbookInput.cpp:341-357. Ctrl+PgDn: WorkbookCommands.cpp:175-176, and App.cpp:1143-1146 gives it to the workbook rather than the app's tabs) |
| 3 | A sheet whose tab doesn't show | Right-click the tab arrows, double-click it in Activate: 2 C | Right-click the tabs, then the sheet in the list: 2 C (WorkbookSheetOps.cpp:293-295) |
| 4 | The file's frozen panes | As saved: 0 | As saved: 0 (WorkbookView.cpp:267, 340-345) |
| 5 | Freeze the top row | View, Freeze Panes, Freeze Top Row: 3 C | Right-click A2 (that selects it), Freeze panes: 2 C (WorkbookInput.cpp:439-442, WorkbookSheetOps.cpp:343 and 456-483). Alt+Q, "freeze top row", Enter: 2 K (WorkbookCommands.cpp:314, WorkbookSheetOps.cpp:115). |
| | **Total** | **7 C** | **6 C** |

### 2. Find a value

| # | Step | Excel | Featherweight |
|---|---|---|---|
| 1 | Find | Ctrl+F: 1 K | Ctrl+F: 1 K (App.cpp:1284 and 2043-2059. The active cell's text is filled in and selected, so typing replaces it.) |
| 2 | The value | Typed | Typed. The first match from the active cell is selected as you type (App.cpp:157-160, WorkbookCommands.cpp:701-750). |
| 3 | The first match | Enter (Find Next): 1 K | 0 |
| 4 | Back in the sheet, to change it | Esc closes the dialog: 1 K | Esc: 1 K. The match stays selected (App.cpp:163 and 2061-2068). |
| | **Total** | **3 K** | **2 K** |

The next match is Enter or F3 in Featherweight (App.cpp:162 and 1292, WorkbookCommands.cpp:761-784). Both search the active sheet only (WorkbookCommands.cpp:712).

### 3. Total a column

| # | Step | Excel | Featherweight |
|---|---|---|---|
| 1 | The cell under the numbers | Click: 1 C | Click: 1 C |
| 2 | AutoSum | Alt+=: 1 K (or Home, AutoSum: 1 C) | Alt+=: 1 K (WorkbookCommands.cpp:216, App.cpp:660-663, WorkbookSheetOps.cpp:219). `=SUM(B2:B9)` is proposed as Excel picks it, with the range in point mode (WorkbookFunctions.cpp:193-230). There's no toolbar button: otherwise it's Alt+Q, "autosum", Enter. |
| 3 | Accept | Enter: 1 K | Enter: 1 K |
| | **Total** | **1 C + 2 K** | **1 C + 2 K** |

Two related cases:

- **Reading a total without a formula** is the same in both: select the column, and the status bar shows Average, Count and Sum (WorkbookCommands.cpp:519-535).
- **Selecting the numbers and then AutoSum** (1 drag + 1 K in Excel) doesn't work in Featherweight: W5.

### 4. Fix a value in a received workbook and save

| # | Step | Excel | Featherweight |
|---|---|---|---|
| 1 | Open the attachment | Double-click, then Enable Editing in Protected View: 2 C | Double-click: 1 C. It opens ready to edit. |
| 2 | Go to the cell | Click it: 1 C (or Find: flow 2) | The same |
| 3 | The new value | Typed | Typed |
| 4 | Save | Ctrl+S: 1 K | Ctrl+S: 1 K (App.cpp:1277 and 1884-1959). An .xlsx asks nothing: only macros saved into .xlsx, or more than values into CSV, ask (WorkbookCommands.cpp:1031-1040). What's being typed is entered first (WorkbookCommands.cpp:926-935). The file is written in the background, "Saved" shows, and the original is kept as a backup with no step (WorkbookFile.h:12-16). |
| | **Total** | **3 C + 1 K** | **2 C + 1 K** |
| 4b | Save, when the file is a .csv | Ctrl+S: 1 K. Excel 365 saves as CSV and shows a "Possible data loss" bar *(check in Excel)* | Ctrl+S: 1 K while the sheet holds only values. If it now holds more (a formula, a column width, formatting, another sheet; CsvWrite.cpp:143-185), a banner asks "CSV keeps only the values of the first sheet." with Save as .xlsx and Keep CSV: +1 C, once per tab (App.cpp:1910-1921 and 1335-1341). W9, by design. |

### 5. Sort and filter a list

A list with a header row. Sort it by one column, turn the filter on, show one value of a column, then rows above a number in another.

| # | Step | Excel | Featherweight |
|---|---|---|---|
| 1 | Sort by a column | Click a cell in it, Data, Sort A to Z: 3 C | Click a cell in it, Data ▾, Sort A to Z: 3 C (WorkbookFormat.cpp:1407-1424, WorkbookFilter.cpp:229-241 and 337-343: the region around the cell, Excel's header guess) |
| 2 | Filter on | Filter (the Data tab is open): 1 C (Ctrl+Shift+L: 1 K) | Data ▾, Filter: 2 C (Ctrl+Shift+L: 1 K) (WorkbookCommands.cpp:211, WorkbookFilter.cpp:82-116). W8. |
| 3 | Only one value | The column's button, (Select All), the value, OK: 4 C | The column's button, (Select all), the value, Apply: 4 C (WorkbookInput.cpp:315-316, WorkbookFilter.h:197-214 and 254-288) |
| 4 | Values greater than X | The button, Number Filters, Greater Than…, type, Enter: 3 C + 1 K | The button, Condition ▾, "is greater than", the Value box, type, Apply: 5 C (WorkbookFilter.h:23-41, 72-78 and 92-93). W4. |
| | **Total** | **11 C + 1 K** | **14 C** (12 C + 1 K with Ctrl+Shift+L) |

In a 1366 px window, Data ▾ isn't on the toolbar (W1). Sorting then goes through the filter's panel: Ctrl+Shift+L, the column's button, Sort A to Z (WorkbookFilter.h:85-91).

### 6. Make a chart of a range

| # | Step | Excel | Featherweight |
|---|---|---|---|
| 1 | The data | Click in it: 1 C (or drag over it) | The same. One cell takes the region around it, as Excel does (WorkbookCharts.cpp:361-363). |
| 2 | A column chart | Alt+F1: 1 K (or Insert, Column or Bar Chart, Clustered Column: 3 C) | Alt+F1: 1 K (WorkbookCommands.cpp:218) (or Chart ▾, Column: 2 C; WorkbookCharts.cpp:388-402). The chart is placed in the middle of the window and selected (WorkbookCharts.cpp:364-381). |
| | **Total** | **1 C + 1 K** (mouse: 4 C) | **1 C + 1 K** (mouse: 3 C) |

Another type works the same way: Insert, the gallery, a subtype (3 C) against Chart ▾, then the type (2 C). In a workbook opened from a .csv, Featherweight refuses the chart until the workbook is saved as .xlsx (W7).

### 7. Make a table (Ctrl+T)

| # | Step | Excel | Featherweight |
|---|---|---|---|
| 1 | The data | Click in it: 1 C | The same |
| 2 | Make it a table | Ctrl+T, then Enter in Create Table (the range and the headers are guessed): 2 K | Ctrl+T (WorkbookCommands.cpp:196-197), then Create table in the panel (the range and the headers are guessed, style Medium 2 as Excel's): 1 K + 1 C (WorkbookData.cpp:870-871, 932-941, 991-1007 and 1108-1113). The panel doesn't take the keyboard, so Enter isn't OK. |
| | **Total** | **1 C + 2 K** | **2 C + 1 K** |
| 2b | By mouse, in a chosen style | Home, Format as Table, a style, OK: 3 C | Data ▾, Table…, Style ▾, the style (61 names: a scroll), Create table: 5 C + 1 scroll. W6. |

### 8. Conditional formatting: highlight values above X

| # | Step | Excel | Featherweight |
|---|---|---|---|
| 1 | The cells | Drag over them: 1 drag | The same |
| 2 | The rule | Home, Conditional Formatting, Highlight Cells Rules, Greater Than…: 3 C | Conditional ▾, New rule…: 2 C (WorkbookFormat.cpp:1425-1438, WorkbookData.cpp:784-790). The panel opens with "Greater than", "Light Red Fill with Dark Red Text" and the selection filled in (WorkbookData.cpp:193-195, 240-247 and 349-354; Rules.h:56-58). |
| 3 | The value | Type it (the box has the keyboard), Enter: 1 K | The Value box: 1 C, type, Add rule: 1 C (WorkbookData.cpp:213-214) |
| | **Total** | **1 drag + 3 C + 1 K** | **1 drag + 4 C** |

### 9. Print a sheet, and Export PDF

The sheet printed with all its columns on one page wide, then a PDF of it.

| # | Step | Excel | Featherweight |
|---|---|---|---|
| 1 | See it as it prints | Ctrl+P (the preview is beside the settings): 1 K | Ctrl+F2 (or the toolbar's preview button): 1 K (WorkbookCommands.cpp:180 and 412-416, WorkbookPrint.cpp:124-145) |
| 2 | All columns on one page | Scaling ▾, Fit All Columns on One Page: 2 C | Page setup (on the toolbar in preview), Scaling ▾, Fit all columns on one page: 3 C (WorkbookCommands.cpp:417, WorkbookPageSetup.cpp:243-244, 869 and 884-892) |
| 3 | Print | Print: 1 C | Ctrl+P, then Print in Windows' dialog: 1 K + 1 C (App.cpp:1278 and 1990-2011, WorkbookPrint.cpp:175-227) |
| | **Total** | **1 K + 3 C** | **2 K + 4 C** (W3) |
| 4 | A PDF beside the workbook | File, Export, Create PDF/XPS, Publish (the name is suggested): 4 C | ⋯, Export PDF…, Enter (`<name>.pdf` is suggested): 2 C + 1 K (App.cpp:1541 and 2013-2041, WorkbookPrint.cpp:290-313). It exports the pages print preview shows: the active sheet, with the scaling set. |

Printing as it is takes the same in both: Ctrl+P, Print (1 K + 1 C).

### 10. Opening other formats (.xls, .ods)

| # | Step | Excel | Featherweight |
|---|---|---|---|
| 1 | Open an .xls | Double-click: 1 C (Compatibility Mode; Enable Editing when it was downloaded: +1 C) | Double-click: 1 C. A banner says "… is an Excel 97–2003 workbook. Saving makes a new `<name>`.xlsx and leaves it as it is." (WorkbookView.cpp:136-173) |
| 2 | Save it | Ctrl+S keeps the .xls: 1 K (the Compatibility Checker asks first when newer features would be lost: +1 C) | Ctrl+S, then Enter in Save As, with `<name>.xlsx` beside it suggested: 2 K (WorkbookView.cpp:162-165, App.cpp:1887-1897, WorkbookFile.cpp:74-76). W10, by design. |
| 3 | Open an .ods | Double-click: 1 C | Not in this build (W12). On `claude/s4-int` 3d550f5: double-click: 1 C, with the same banner ("… is an OpenDocument spreadsheet…"), and the tab is a copy. |
| 4 | Save an .ods | Ctrl+S, then keep the format in Excel's question: 1 K + 1 C *(check in Excel)* | s4-int: Ctrl+S, Enter: 2 K. To be counted once s4-int is the build reviewed. |

### 11. A password-protected workbook

| # | Step | Excel | Featherweight |
|---|---|---|---|
| 1 | Open it | Double-click, type the password, Enter: 1 C + 1 K, 1 D | This build refuses it with a banner. An encrypted .xlsx: "…it is protected with a password, which workbooks don't support yet" (WorkbookView.cpp:139-145). An .xls with its own password: "…it is protected with a password" (WorkbookView.cpp:146-150, Xls.cpp:585-590). On `claude/s4-ods-e4ae8e` cc6e522: the password card, as for documents: double-click, the password in the tab, Enter: 1 C + 1 K, 1 D. |
| 2 | A wrong password | An error, OK, then open it again: +2 C | cc6e522: the card stays |
| 3 | Save | Ctrl+S, with the same password: 1 K | cc6e522: an .xlsx is saved encrypted with its password: 1 K. An .xls becomes a new .xlsx encrypted with its password: Ctrl+S, Enter: 2 K. |

These are to be counted once cc6e522 lands.

### 12. A workbook from a template (.xltx)

| # | Step | Excel | Featherweight |
|---|---|---|---|
| 1 | A new workbook from the template | Double-click the .xltx: 1 C ("Budget1") | Double-click: 1 C ("Budget1"; the template isn't touched) (App.cpp:1635-1639, WorkbookFile.cpp:37-48, WorkbookView.cpp:175-178) |
| 1b | From inside the app | File, New, Personal, the template: 4 C (from the start screen: the template, Create: 2 C) | Not offered (W13). Ctrl+O, double-click the .xltx: 1 K + 1 C, plus browsing (App.cpp:1694-1737) |
| 2 | Save | Ctrl+S, the name and the place, Enter: 2 K, 2 D | Ctrl+S, the name (`Budget1.xlsx` beside the template is suggested, or Documents if that folder can't be written), Enter: 2 K, 1 D (App.cpp:1884-1897 and 1767-1774, WorkbookFile.cpp:41-48) |
| | **Total (from Explorer)** | **1 C + 2 K, 2 D** | **1 C + 2 K, 1 D** |

### Not checked

- The flows were read from the code, not played. Checked against the app or the code since: W1 on a 1366-pixel snapshot (above); W2 (`WorkbookView::Commands()` lists no id from CmdFormatCells 2700 on, nor CmdCondFormat 2850 on); W5 (`WorkbookView::AutoSum` proposes from the active cell only and starts editing it).
- Double-clicking a workbook in Explorer opens Featherweight only when it's the default app for that type. Featherweight registers itself under "Open with" (FileAssoc.cpp:66).
- None of these flows is scripted yet. `tests/friction-checks.ps1` has only the Word flows. Flows 3, 4 and 8 could be scripted the same way through `--script`.

Owners (the coordinator, 2026-11-19): W1, W2, W4 and W6 s3-data; W5 and W7 s3-calc; W3 the S3 print session; W13 s4-ods. W8 is accepted; W9 and W10 are by design; W11 and W12 are s4-ods's (done on its branch and on s4-int). Each finding is marked fixed when its fix is in `claude/s4-int`: W1 to W7 and W13 are, at ce2cffa (2026-11-19); the counts above are the review's, before the fixes, and are counted again when the workbook flows are scripted (tests\workbook-friction-checks.ps1).
