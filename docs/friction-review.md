# Friction review

The friction review (plan §11, "Zero friction") writes each core flow down step by step, in Word and in Featherweight, so that a flow never gains a step without a reason in the plan. D3 is done when it passes for writing a letter, formatting a CV and fixing a table in a received document (plan §6.4). D5 adds the rest of the Word flows, D3's to D5's (flows 4 to 15).

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
