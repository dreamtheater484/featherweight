# Friction review

The friction review (plan §11, "Zero friction") writes each core flow down step by step, in Word and in Featherweight, so that a flow never gains a step without a reason in the plan. D3 is done when it passes for writing a letter, formatting a CV and fixing a table in a received document (plan §6.4). D5 adds the rest of the Word flows, D3's to D5's (flows 4 to 15).

S4 adds the workbook flows (plan §7, the S4 row; s2-plan's "total a column" and "fix a value in a received workbook"): see [Workbooks (S4)](#workbooks-s4).

P3 adds the presentation flows (docs/p3-plan.md: presenting, transitions and animations, comments, passwords, pictures, templates, .ppt and .odp): see [Presentations (P3)](#presentations-p3).

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

## Presentations (P3)

This is the presentation part of the friction review (docs/p3-plan.md, exit criteria: "presenting with the presenter view, adding transitions and animations, commenting, protecting, exporting pictures and opening a `.ppt` take no more steps than in PowerPoint 2021"). It uses the Word part's rules ("How steps are counted").

- **C** is a click (or a drag), **K** is a key or a chord, and **D** is a decision (a name, a place, a format, a password, a monitor). Typing the content isn't counted. A scroll in a list counts as a step.
- **PowerPoint** means PowerPoint 2021 on Windows with its default ribbon and modern (threaded) comments, in a window at least 1366 px wide. Its steps come from its standard UI and weren't measured. The ones to confirm are marked *(check in PowerPoint)*. They're on Roy's checklist pass (docs/TEST_CHECKLIST.md, P3 part).
- **Featherweight** was read from the code on each piece's branch, 2026-11-24. The app wasn't run. Every step cites the code:
  - presenting: `claude/p3-present` 85be16e2;
  - transitions and animations: `claude/p3-anim` 3d38ee17, then 1b83de6e (counted by p3-anim, FeatherweightNotes\p3\p3-anim-friction.md);
  - comments and passwords: `claude/p3-review` 9f08b056;
  - pictures and templates, and .odp: `claude/p3-files` 799f5bc5;
  - .ppt: `claude/p3-ppt` da559019.
  Paths are under `src/`. The pieces weren't merged together yet, so where two of them change the same code the counts take each as it is on its own branch.
- When a step has a mouse way and a keyboard way, the table gives the one most people use, and the other in brackets.

### Summary

| Flow | PowerPoint 2021 | Featherweight | Notes |
|---|---|---|---|
| 1. Present with the presenter view on a second monitor | 7 K, 1 D (mouse: 8 C) | 7 K, 1 D, **plus 3 C, 1 D** to choose the monitor (mouse: 9 C + 1 scroll, plus the monitor) | **+3 C, 1 D** (P1): the show goes to the monitor the app is on, and the audience sees the presenter view. By mouse, start, black and end are a click more each (P2 to P4). |
| 1b. Present from the current slide | 1 K (mouse: 1 C) | 1 K (mouse: 2 C) | The keyboard ties. There's no status-bar button (P5). |
| 2. Transitions: one with a direction and a duration, on every slide, previewed | 9 | 9 | Passes, step for step (p3-anim's count: a click or a key each). |
| 2b. Animations: an effect, a second one, its Start, duration, delay, order, removed | 16 | 16 | Passes. Only the common effects are offered (P7). Sequence has no All at Once (P6). |
| 3. Commenting a deck: add, reply, edit, resolve, the pane, next, delete one and all | 18 C + 4 K | 18 C + 4 K | Ties. Editing is a click more (P9). A comment can't be tied to a shape or text (P10), a reply can't be edited or deleted alone (P11), and the box is one line of 256 characters (P13). |
| 4. Protecting: open a protected deck, save it after an edit, protect one, remove the password | 11 C + 6 K, 2 D | 6 C + 6 K, 2 D | Passes, 5 C fewer. A wrong password is 1 K, against PowerPoint's 2 C + 1 K. |
| 5a. The current slide as PNG | 6 C + 1 K, 1 D | 3 C + 1 K, 1 D | Passes. |
| 5b. Every slide as PNG | 7 C + 1 K, 1 D | 3 C + 1 K, 1 D | Passes. |
| 5c. A deck from a .potx, from Explorer, then saved | 1 C + 2 K, 2 D | 1 C + 2 K, 2 D | Ties. From inside the app it's 6 C against 4 C until the release flag is set (P14). |
| 6a. A .ppt from mail: open, edit, save | 2 C + 1 K | 1 C + 2 K | Ties at 3: no Enable Editing, but saving is a key more (P15, by design). A password-protected .ppt is refused (P16). |
| 6b. An .odp from mail: open, edit, save | 3 C + 1 K | 1 C + 2 K | Passes. |

Presenting is the one flow over PowerPoint's count, by P1. The rest pass or tie.

### Where Featherweight needs more than PowerPoint, or can't do it

| # | Flow, step | PowerPoint 2021 | Featherweight | Owner | File:line | Status |
|---|---|---|---|---|---|---|
| P1 | 1, before the show: which monitor gets it | Automatic: the slides go to the projector, the presenter view stays with the app *(check in PowerPoint)*. Wrong anyway: Swap Presenter View and Slide Show, 2 C, during the show. | Automatic is the monitor the app window is on (`monitor = -1`, "the owner's monitor"), and the presenter view goes to the other. With the app on the laptop, the projector shows the presenter view with the notes. The way round it is Slide Show, Monitor ▾, the projector, before F5: **+3 C, 1 D**, in each tab, not kept. The presenter view has no swap. Mid-show it's Esc, those 3 C, F5. | p3-present | slide/show/SlideShow.h:27, SlideShow.cpp:344-348 and 423-430; slide/view/PresentationView.h:285 | Open (sent to p3-present 2026-11-24) |
| P2 | 1: black the screen by mouse | The presenter view's black-screen button: 1 C | No button. Right-click the show window on the projector, Black screen: 2 C. **+1 C**. B ties. | p3-present | slide/show/PresenterView.cpp:194-202; SlideShow.cpp:1397-1420 | Open |
| P3 | 1: end by mouse | End Slide Show in the presenter view: 1 C | No End button, and the presenter view has no title bar. Right-click the show window, End show: 2 C. **+1 C**. Esc ties. | p3-present | slide/show/PresenterView.cpp:100; SlideShow.cpp:1406 | Open |
| P4 | 1: start by mouse | Start From Beginning in the quick access toolbar: 1 C *(check in PowerPoint)* | Slide Show, From Beginning: 2 C. Quick access has Save, Undo and Redo. **+1 C**. F5 ties. | p3-present, with R1 (quick access) | slide/view/PresentationRibbon.cpp:368-370; AppRibbon.cpp:106-119 | Open |
| P5 | 1b: from the current slide by mouse | The status bar's Slide Show button: 1 C | Right-click the slide, Present from this slide (or Slide Show, From Current Slide): 2 C. **+1 C**. Shift+F5 ties. | p3-present | slide/view/PresentationCommands.cpp:346 and 352-364 | Open |
| P6 | 2b: change a text effect between by paragraph and as one object | Effect Options, Sequence: 2 C | At 3d38ee17 there was no Sequence section after the effect was added: changing it meant removing the effect and adding it again. Now Effect Options has As One Object and By Paragraph: 2 C. PowerPoint's third choice, All at Once, isn't offered. | p3-anim | p3-anim-friction.md | Fixed on `claude/p3-anim` 1b83de6e, except All at Once |
| P7 | 2b: an effect outside the common ones | More Entrance Effects… (and Emphasis, Exit, Motion Paths): 2 C and a pick | The gallery has 7 entrances, 2 emphasis and 7 exits. There are no More Effects dialogs. Effects from PowerPoint outside the list are kept, play in the show and can be reordered, retimed, replaced or removed. Their own options can't be changed. | p3-anim | p3-anim-friction.md | Recorded by p3-anim |
| P8 | 2b: reorder by dragging in the Animation Pane | A drag: 1 C | At 3d38ee17 there was no drag, only Move Earlier and Move Later or the pane's arrows (1 C each, so one place ties). p3-anim has added the drag. | p3-anim | p3-anim-friction.md | Fixed by p3-anim in the commit after 1b83de6e |
| P9 | 3: edit your comment | The pencil on it, then Ctrl+Enter: 1 C + 1 K *(check in PowerPoint)* | Click its card, Edit, then Enter: 2 C + 1 K. **+1 C**. | p3-review | slide/view/PresentationComments.cpp:450-458 and 622-633 | Open (sent to p3-review) |
| P10 | 3: a comment on a shape or on text | Select it, New Comment: the comment is tied to it and moves with it | **Can't.** New comments are tied to the slide only (`pc:sldMkLst`). With a shape selected the marker is only placed at its top right corner, and it stays put when the shape moves. | p3-review | slide/edit/CommentOps.cpp:539-547; slide/view/PresentationComments.cpp:342-348 | Open |
| P11 | 3: edit or delete one reply | Its … menu, Edit or Delete comment: 2 C *(check in PowerPoint)* | **Can't.** A card is a whole thread: Edit changes its first comment and Delete removes the thread with its replies. The edit layer can do it. | p3-review | slide/view/PresentationComments.cpp:450-479 and 625; slide/edit/CommentOps.cpp:644-688 | Open |
| P12 | 3: show or hide the pane from another tab | The status bar's Comments button: 1 C *(check in PowerPoint)* | Review, Show Comments: 2 C. **+1 C**. Ctrl+Alt+M opens it, but to a new comment. | p3-review | slide/view/PresentationCommands.cpp:361-373; PresentationRibbon.cpp:402 | Open |
| P13 | 3: a comment of more than one line, or more than 256 characters | Enter starts a new paragraph | **Can't.** The box is one line: Enter posts and pasted line breaks become spaces. It holds 256 characters, so a longer comment is cut while it's typed, and a longer one from PowerPoint is cut at 256 when it's edited and saved. | p3-review | ui/Controls.cpp:888; ui/Widgets.cpp:191-196 and 319-341 | Open: the multi-line box. Since `claude/p3-review` 0e6ca275 a comment over 256 characters or with line breaks isn't put in the box to edit (the pane says to reply), so saving can't cut it |
| P14 | 5c: a .potx from inside the app | File, New, Personal, the template: 4 C *(check in PowerPoint)* | File > New doesn't list .potx while the presentation editor isn't marked shipped, and More templates… opens on a filter that hides it: 6 C. **+2 C**. With the flag set for 4.0 (decision 28) it's 3 C. | p3-files, with the 4.0 release flag | slide/view/UserTemplates.cpp:60-61; core/FileKinds.cpp:23-24; App.cpp:1866-1878 | Ends with the release flag |
| P15 | 6a: saving a .ppt | Ctrl+S keeps the .ppt (Compatibility Mode): 1 K | Ctrl+S, then Enter in Save As with `<name>.pptx` beside it: 2 K. **+1 K**. The flow still ties: there's no Enable Editing. | | slide/view/PresentationView.cpp:116-124 (p3-ppt) | By design (p3-plan: a .ppt opens with Save As .pptx, as .doc and .xls do; F17, W10) |
| P16 | 6a, outside the flow: a password-protected .ppt | The password, Enter | Refused: "… it is protected with a password, which Featherweight can't open yet." p3-ppt's 186828df decrypts RC4 in the importer, but the tab doesn't ask for the password yet. | p3-ppt | slide/ppt/Ppt.cpp:87-88; slide/view/ModelDeck.cpp:364 | Open |

These don't change the counts but are worth a look:

- **Enter leaves a posted comment in the box** (p3-review). Post clears it with `SetText`, which a ComboBox ignores while it has the keyboard (ui/Controls.cpp:896-897; slide/view/PresentationComments.cpp:438). The box then says "Reply", so a second Enter posts the same text again as a reply. The Post button clears it. Fixed in `claude/p3-review` 0e6ca275 (the box clears while it has the keyboard; the password boxes too).
- **Protect, edit, undo leaves the deck unprotected** (p3-review). Encrypt with Password marks the tab unsaved with `Edited(0)` (slide/view/PresentationProtect.cpp:133-134). An undo back to the opened state calls `Changed(false)`, which marks it clean (slide/view/PresentationFile.cpp:111-114, FileSession.cpp:69-83). Ctrl+S then saves nothing: the file stays unprotected while File > Info says a password is required. Remove password is the same the other way. Fixed in `claude/p3-review` 0e6ca275: a password set or removed keeps the tab unsaved until a save writes it, whatever undo does.
- **Black screen in the presenter view** shows the current slide black too (slide/show/PresenterView.cpp:218-224), so the presenter can't see the slide they'll bring back. PowerPoint keeps it visible there *(check in PowerPoint)*.
- **Back by mouse** in the presenter view is a wheel turn over the slides (PresenterView.cpp:369-371). PowerPoint has ◀.
- **Show Comments with nothing picked** opens a new-comment box but doesn't reset where its marker goes: it lands at (0, 0), or where the last New Comment put one (slide/view/PresentationComments.cpp:360-364).
- **Anyone's comment can be edited, resolved or deleted** (PresentationComments.cpp:450-479). PowerPoint's modern comments let you edit only your own *(check in PowerPoint)*.
- **For the merges:**
  - p3-ppt and p3-files each add their own format to `core/FileKinds.cpp:12-13` and change `PresentationView::Load`.
  - p3-review's `TakeProtected` took any compound file and refused one that isn't an encrypted package (slide/view/PresentationProtect.cpp:24-31, 43, 57-60), so a .ppt might not have opened. Since 0e6ca275 it takes only an encrypted package, and other compound files go on to the importers.
  - The .ppt, .odp and protected-deck flows are to be counted again on p3-int once all three are in.

### Where Featherweight needs less

- **Jump to a slide by mouse in the show**: the strip of slides is always in the presenter view, 1 C, against See All Slides and the slide (2 C).
- **Pictures**: File, Export, Current slide (or All slides) as PNG…, Enter: 3 C + 1 K. PowerPoint needs Change File Type, PNG, Save As, then Just This One or All Slides (and an OK for all slides): 6 or 7 C + 1 K. The width is a box in the Save dialog (1920 px at first). PowerPoint's needs a registry value *(check in PowerPoint)*.
- **Protecting**: File opens on Info, and Encrypt with Password is one entry with both boxes in one panel: 2 C + 3 K against 5 C + 2 K. Removing the password has its own button: 3 C + 1 K against 5 C + 2 K.
- **A wrong password**: the card stays with the text selected: 1 K, against OK and opening the file again (2 C + 1 K).
- **Deleting every comment in the deck**: 2 C with no question (an undo brings them back), against 3 C.
- **A .ppt or .odp from mail** opens ready to edit, with no Protected View. Saving an .odp is Enter in Save As against PowerPoint's keep-the-format question.

### Not checked

- The flows were read from the code, not played. Two findings were checked against the code a second time: P1 (`ShowOptions::monitor` -1 and `MonitorFromWindow(owner)` in SlideShow.cpp) and the two comment and password bugs above.
- PowerPoint's own steps marked *(check in PowerPoint)*: the Automatic monitor rule, quick access's buttons, the message after exporting all slides, the PNG size, File opening on Info or Home, where Edit, Reply and Resolve sit on a modern comment card, whether deleting every comment asks first, the status bar's Comments button, comments on text, and the .odp keep-the-format question.
- Featherweight: whether F5 and Shift+F5 reach the show while the keyboard is in the ribbon or the notes; whether a shape whose text is being edited is still selected after New Comment; how the one-line box shows a multi-paragraph comment from PowerPoint.
- Saved .pptx from .odp: 14 of 14 corpus decks open in PowerPoint without repair (`readback\odp-saved-open-check.json`, 2026-11-24, p3-files 54aec9ac). From .ppt: not checked yet.
- None of these flows is scripted. `tests/friction-checks.ps1` has the Word flows only; comments (`comment:` script ops) and transitions (`transition:`, `anim:`) could be scripted the same way.

Owners: P1 to P5 p3-present (P4 with R1, whose quick access it is); P6 to P8 p3-anim; P9 to P13 p3-review; P14 ends with the 4.0 release flag; P15 is by design; P16 p3-ppt. A finding is marked fixed when its fix is in `claude/p3-int`.

### The flows step by step

#### 1. Presenting with the presenter view

An open deck with notes and a second monitor (the projector) as an extended display. The app window is on the laptop's screen. Start from the beginning, go forward and back, jump to slide N, read the notes and the next slide, black the screen and bring it back, then end.

| Step | PowerPoint 2021 | Featherweight | file:line (p3-present 85be16e2) |
|---|---|---|---|
| 0. Which monitor gets the show | 0. "Automatic" puts the show on the projector and the presenter view on the laptop *(check in PowerPoint)*. If it's wrong: Display Settings, Swap Presenter View and Slide Show: 2 C, during the show. | "Automatic" is the monitor the app window is on (`monitor = -1`: "the owner's monitor"). With the app on the laptop, the show fills the laptop and the presenter view goes to the projector. To fix it: Slide Show, Monitor ▾, the projector: **3 C, 1 D**, before F5. It's per tab and not saved. The presenter view has no swap. If you notice during the show: Esc, those 3 C, then F5 again. | SlideShow.h:27; SlideShow.cpp:344-348 and 423-430; PresentationCommands.cpp:632; PresentationRibbon.cpp:375-387; PresentationView.h:285 |
| 1. Start from the beginning, with the presenter view | F5: 1 K. The presenter view comes on by itself with two monitors. (Mouse: Start From Beginning in quick access: 1 C *(check in PowerPoint)*. Or Slide Show, From Beginning: 2 C.) | F5: 1 K. Use Presenter View is on by default, so the presenter view opens full screen on the other monitor. (Mouse: Slide Show, From Beginning: 2 C. Quick access has only Save, Undo and Redo.) | PresentationCommands.cpp:116, 244, 622-647 (637); PresentationView.h:286; PresentationRibbon.cpp:368-370 and 388; PresenterView.cpp:95-111; AppRibbon.cpp:106-119 |
| 2. Forward | → (or Space, N, PgDn): 1 K. (Click the slide or ▶: 1 C.) | → (or Space, N, PgDn, Enter): 1 K, in either window. The presenter view passes its keys to the show. (Click the current or next slide in the presenter view: 1 C.) | SlideShow.cpp:1379 and 1110-1146; PresenterView.cpp:333-337 and 355-356; SlideShow.cpp:271 |
| 3. Back | ← (or Backspace, P, PgUp): 1 K. (◀: 1 C.) | ← (or Backspace, P, PgUp): 1 K. (Mouse: no back button. Wheel up over the slides: 1 scroll.) | SlideShow.cpp:1380 and 1150-1167; PresenterView.cpp:369-371 |
| 4. Jump to slide N | Type N, Enter: 1 K, 1 D. (See All Slides, then the slide: 2 C.) | Type N, Enter: 1 K, 1 D. (The strip of slides is always shown: click the slide, 1 C. Add a scroll when it's out of view: about 11 thumbnails fit at 1920×1080.) | SlideShow.cpp:1364-1376; PresenterView.cpp:155-159, 262-282 and 350-354 (click), 367-368 (strip scroll) |
| 5. See the notes and the next slide | 0: both are in the presenter view | 0: the notes (wheel to scroll, A− and A+ to zoom) and the next slide. When the slide has clicks left, it shows "Next animation" instead. | PresenterView.cpp:213-259; SlideShow.cpp:1342-1355 |
| 6. Black the screen | B (or .): 1 K. (The presenter view's black-screen button: 1 C.) | B (or .): 1 K. (Mouse: no button in the presenter view. Right-click the show window on the projector, then Black screen: 2 C.) | SlideShow.cpp:1389 and 1330-1334; SlideShow.cpp:1397-1420; PresenterView.cpp:194-202 (the only buttons: Pause, Reset, A−, A+) |
| 7. Bring the slide back | B again, or any key: 1 K *(check in PowerPoint: whether any key or only B)*. (The button again: 1 C.) | B again, or any key but W, comma and Esc: 1 K. It doesn't also advance. (A click on the slide: 1 C.) | SlideShow.cpp:1331 and 1360-1362; PresenterView.cpp:355-356 |
| 8. End | Esc: 1 K. (End Slide Show at the top of the presenter view: 1 C.) | Esc (or −): 1 K, in either window. Alt+F4 in the presenter view ends it too. (Mouse: no End button. The presenter view is a captionless popup. Right-click the show window, then End show: 2 C. Or click past the last slide.) | SlideShow.cpp:1391; PresenterView.cpp:100, 335, 375; SlideShow.cpp:1406 and 1417; PresenterView.cpp:230 |
| | **Total** | **7 K, 1 D** (mouse: 8 C) | **7 K, 1 D + 3 C, 1 D** for the monitor (mouse: 9 C + 1 scroll, + the monitor) |

#### 1b. Start from the current slide

| Step | PowerPoint 2021 | Featherweight | file:line (p3-present 85be16e2) |
|---|---|---|---|
| 1. From the current slide | Shift+F5: 1 K. (The Slide Show button in the status bar: 1 C. Or Slide Show, From Current Slide: 2 C.) | Shift+F5: 1 K, with the presenter view as in step 1. (Mouse: right-click the slide, then Present from this slide: 2 C. Or Slide Show, From Current Slide: 2 C. The status bar has no show button.) | PresentationCommands.cpp:116, 245 and 630; PresentationCommands.cpp:346; PresentationRibbon.cpp:371; PresentationCommands.cpp:352-364 (status bar) |
| | **Total** | **1 K** (mouse: 1 C) | **1 K** (mouse: 2 C) |

The show ends on the slide it was on, and the tab goes to that slide, as in PowerPoint (PresentationCommands.cpp:639-643).

Alt+F5 in Featherweight puts the show and the presenter view side by side, each in a window, on one monitor, from the current slide (PresentationCommands.cpp:117 and 246; SlideShow.cpp:364-375; PresenterView.cpp:101-107). That's for rehearsing. It's not in the counts.

#### 2. Transitions and animations

Counted by p3-anim on `claude/p3-anim` 3d38ee17 and brought up to 1b83de6e, from a slide open in the editor with the Home tab showing. A step is a click or a key; a pick in an open gallery or menu is one step.

| Task | PowerPoint 2021 | Featherweight (claude/p3-anim, from 1b83de6e) |
|---|---|---|
| Give a slide a transition | Transitions tab, pick in the gallery: 2 | same: 2 (Alt, K opens the tab too) |
| Change its direction | Effect Options, pick: 2 | same: 2 |
| Set its duration | Duration box, type, Enter: 3 | same: 3 (spin box) |
| Advance after a time | After check box, type in its box: 2-3 | same: 2-3 |
| Same transition on every slide | Apply To All: 1 | same: 1 |
| Preview it | Preview: 1 | same: 1 (plays on the slide in the editor, never full screen) |
| Animate a shape | select it, Animations tab, pick in the gallery: 3 | same: 3 |
| A second effect on the same shape | Add Animation, pick: 2 | same: 2 |
| With Previous / After Previous | Start box, pick: 2 | same: 2 |
| Duration / delay | box, type, Enter: 3 each | same: 3 each |
| Reorder | Move Earlier / Later: 1 each, the pane's arrows, or drag in the pane | same (dragging in the pane since the commit after 1b83de6e) |
| Open the Animation Pane | Animation Pane: 1 | same: 1 |
| Pick an effect in the pane and change it | click it, pick in the gallery: 2 | same: 2 (the gallery changes the picked effect only) |
| Remove an effect | pane: pick, Delete: 2; or gallery None | same: 2 |
| Text a paragraph at a time | Effect Options > By Paragraph: 2 (default on text placeholders) | same: 2 (Effect Options > As One Object / By Paragraph, 1b83de6e); by paragraph by default on body/content placeholders with more than one paragraph |

#### 3. Commenting a deck

A received deck with a colleague's thread on slide 2. The ribbon shows Home. The Review tab is counted once, in step 1; after that it's open (as the S4 model's "the Data tab is open"). The Comments pane is open after step 1. Each step starts with no thread picked.

| Step | PowerPoint 2021 | Featherweight | file:line |
|---|---|---|---|
| 1. A comment on the slide | Review, New Comment, type, Ctrl+Enter (or Post): 2 C + 1 K [Ctrl+Alt+M, type, Ctrl+Enter: 2 K] | Review, New Comment, type, Enter: 2 C + 1 K [Ctrl+Alt+M, type, Enter: 2 K]. The pane opens and its box has the keyboard. The marker goes at the top left, stacked under earlier ones. | slide/view/PresentationRibbon.cpp:393, 398; slide/view/PresentationCommands.cpp:110, 242-246; slide/view/PresentationComments.cpp:285, 340-369 (focus 359, 584-587), 79-82, 423-440 |
| 2. A comment on a shape | Click the shape, New Comment, type, Ctrl+Enter: 2 C + 1 K. The comment is tied to the shape. | Click the shape, New Comment, type, Enter: 2 C + 1 K. The marker is only placed at the shape's top right corner. The comment is tied to the slide, so it doesn't follow the shape (P10). | slide/view/PresentationComments.cpp:342-348; slide/edit/CommentOps.cpp:539-547 (only `pc:sldMkLst`) |
| 2b. A comment on selected text | Select the text, New Comment, type, Ctrl+Enter: 1 drag + 1 C + 1 K, tied to the text *(check in PowerPoint)* | Can't. It goes by the shape, as in step 2 (P10). | slide/edit/CommentOps.cpp:539-547 |
| 3. Reply to the colleague's comment | The thread's Reply box, type, Ctrl+Enter: 1 C + 1 K *(check in PowerPoint)* | Click the thread's card, type, Enter: 1 C + 1 K. The box at the top of the pane becomes "Reply" and takes the keyboard. Clicking the marker on the slide does the same. | slide/view/PresentationComments.cpp:622-633, 79-82, 442-448, 548-551; markers 175-188, 360-363 |
| 4. Edit your comment | Edit (the pencil) on it, change it, Ctrl+Enter: 1 C + 1 K *(check in PowerPoint)* | Click its card, Edit, change it, Enter: 2 C + 1 K. **+1 C** (P9). Right after posting it's still picked: 1 C + 1 K. | slide/view/PresentationComments.cpp:87-88, 450-458, 427-429, 433-435 |
| 5. Resolve a thread | … (More thread actions), Resolve thread: 2 C *(check in PowerPoint)* | Click its card, Resolve: 2 C. A legacy comment's Resolve is greyed out (PowerPoint's legacy comments have no Resolve either). | slide/view/PresentationComments.cpp:89-90, 460-467, 554-555; slide/edit/CommentOps.cpp:711-723 |
| 6. Hide, then show the pane | Review > Show Comments, twice: 2 C (the pane's X hides it too). From another tab: the status bar's Comments button, or Comments at the top right: 1 C each *(check in PowerPoint)* | Review > Show Comments, twice: 2 C. The pane's close button, or Esc while the pane has the keyboard, hides it. From another tab: Review first, +1 C each way (P12). | slide/view/PresentationRibbon.cpp:402; slide/view/PresentationComments.cpp:286-289; ui/SidePanel.cpp:17-21; AppShell.cpp:330-331; slide/view/PresentationCommands.cpp:361-373 (no Comments button in the status bar) |
| 7. Go to the next comment | Review > Next: 1 C | Review > Next: 1 C. It goes to the next slide's thread, picks it and opens the pane. It wraps around. | slide/view/PresentationRibbon.cpp:401; slide/view/PresentationComments.cpp:371-409 |
| 8. Delete one thread | …, Delete thread: 2 C *(check in PowerPoint)* (or pick it, Review > Delete: 2 C) | Click its card, Delete: 2 C (or the card, Review > Delete). It takes the replies with it. | slide/view/PresentationComments.cpp:91-92, 469-479, 259-262; slide/view/PresentationRibbon.cpp:399 |
| 8b. Delete one reply | …, Delete comment on the reply: 2 C *(check in PowerPoint)* | Can't. The pane only picks threads, so Delete takes the whole thread (P11). | slide/view/PresentationComments.cpp:625, 469-479 (the API can: slide/edit/CommentOps.cpp:644-688) |
| 9. Delete all on this slide | Delete ▾, Delete All Comments and Ink on This Slide: 2 C | Delete ▾, Delete all comments on this slide: 2 C | slide/view/PresentationRibbon.cpp:399; slide/view/PresentationComments.cpp:263-266, 296 |
| 10. Delete all in the deck | Delete ▾, Delete All Comments and Ink in This Presentation…, Yes: 3 C *(check in PowerPoint: the question)* | Delete ▾, Delete all comments in this presentation: 2 C. No question; one undo brings them back. | slide/view/PresentationComments.cpp:267-270, 297; slide/edit/CommentOps.cpp:690-709 |
| **Total** (steps 1-10) | **18 C + 4 K** | **18 C + 4 K** | |

Where legacy comments differ (PowerPoint 2013-2019, or a received deck with old comments): in PowerPoint you type a new comment or reply and click away (no Post). Edit is a click in the text. Delete is the comment's X. There's no Resolve. In Featherweight, new comments are always modern. A received legacy comment can be replied to, edited and deleted, and stays legacy. Resolve is greyed out for it (slide/view/PresentationComments.cpp:554; slide/edit/CommentOps.cpp:718). How PowerPoint 2021 with modern comments treats a deck's legacy comments, I couldn't settle *(check in PowerPoint)*.

#### 4. Protecting a deck

A deck to send with a password, then a received protected deck.

| Step | PowerPoint 2021 | Featherweight | file:line |
|---|---|---|---|
| 1. Open a protected .pptx | Double-click, type the password, Enter: 1 C + 1 K, 1 D | Double-click. The tab shows the password card, and its box has the keyboard. Type the password, Enter: 1 C + 1 K, 1 D. | App.cpp:1821-1835, 1990; slide/view/PresentationView.cpp:87-96; slide/view/PresentationProtect.cpp:35-51, 53-82, 89-109; PasswordCard.cpp:12-21 |
| 2. A wrong password | An error, OK, open it again, type, Enter: 2 C + 1 K more *(check in PowerPoint)* | The card says "That password isn't right. Try again." The box shakes and its text is selected. Type it again, Enter: 1 K more. An empty box gets the same message. A damaged file or an unsupported encryption keeps the card, with the reason. | PasswordCard.cpp:84-88; slide/view/PresentationProtect.cpp:66-79, 91-108 |
| 3. Save after an edit | Ctrl+S, with the same password: 1 K | Ctrl+S: 1 K. It asks nothing. It's written encrypted with the key from the password. The backup of the original is the encrypted file, and the autosave is encrypted too. | slide/view/PresentationView.cpp:111-115; FileSession.cpp:110, 115-123, 124-135; slide/view/PresentationFile.cpp:116-124 |
| 4. Protect a deck with a password | File, Info, Protect Presentation, Encrypt with Password, type, Enter, type again, Enter, Save: 5 C + 2 K, 1 D (4 C if File opens on Info *(check in PowerPoint)*) [Save As route: F12, Tools, General Options, type, OK, type again, OK, Save: 1 K + 5 C, 1 D *(check in PowerPoint)*] | File (it opens on Info), Protect Presentation: Encrypt with Password, type, Tab, type again, Enter, Ctrl+S: 2 C + 3 K, 1 D. The File page closes and a side panel opens with the Password box ready. Enter in the first box says "The passwords you typed don't match", so Tab comes first. There's no password in Save As; it isn't needed. | AppRibbon.cpp:195, 295-303; ui/FilePage.cpp:246-249; slide/view/PresentationRibbon.cpp:480-490; slide/view/PresentationProtect.cpp:122-128, 139-158, 191-195, 201-217, 130-137; ui/Controls.cpp:237-240, 1093-1096; App.cpp:2065 |
| 5. Remove the password | File, Info, Protect Presentation, Encrypt with Password, Delete (the dots), Enter, Save: 5 C + 2 K *(check in PowerPoint: whether the dots start selected)* | File, Protect Presentation: Encrypt with Password, Remove password, Ctrl+S: 3 C + 1 K | slide/view/PresentationProtect.cpp:149-150, 178, 219-224 |
| **Total** (steps 1, 3, 4, 5) | **11 C + 6 K, 2 D** (9 C + 6 K if File opens on Info) | **6 C + 6 K, 2 D** | |

The two typed passwords don't match: Featherweight's panel says "The passwords you typed don't match." (slide/view/PresentationProtect.cpp:205-206). PowerPoint shows an error to close first *(check in PowerPoint)*.

#### 5. Exporting pictures, and a deck from a template

#### 5a. The current slide as PNG

| Step | PowerPoint 2021 | Featherweight | file:line (p3-files 799f5bc5) |
|---|---|---|---|
| 1. File | 1 C | 1 C. The File page opens on Info. | ui/FilePage.h:41 |
| 2. Export | 1 C | 1 C | AppRibbon.cpp:321-324 |
| 3. The format | Change File Type, PNG Portable Network Graphics, Save As: 3 C | Current slide as PNG…: 1 C. It's the second entry, after Export PDF…. | PresentationRibbon.cpp:469-478; PictureExport.cpp:90-97 |
| 4. Name and place | The deck's name is suggested *(check in PowerPoint: which folder)*. Enter: 1 K, 1 D | `Slide<N>.png` in the deck's folder is suggested. A Width box (1280, 1920, 2560, 3840 pixels; 1920 at first) sits in the dialog. Enter: 1 K, 1 D | PictureExport.cpp:100-124 (105-113 name and folder, 114-123 width); PictureExport.cpp:26; PresentationView.h:215 |
| 5. Which slides | Just This One: 1 C | 0. A banner says "Exported slide N to …". | PictureExport.cpp:63-72 and 133-137 |
| | **Total** | **6 C + 1 K, 1 D** | **3 C + 1 K, 1 D** |

(PowerPoint by keyboard: F12, Save as type ▾, a scroll, PNG, Enter, Just This One: 2 K + 3 C + 1 scroll.)

#### 5b. Every slide as PNG

| Step | PowerPoint 2021 | Featherweight | file:line (p3-files 799f5bc5) |
|---|---|---|---|
| 1-2. File, Export | 2 C | 2 C | as in 2a |
| 3. The format | Change File Type, PNG, Save As: 3 C | All slides as PNG…: 1 C | PresentationRibbon.cpp:470-471; PictureExport.cpp:92-95 |
| 4. Name and place | The deck's name. Enter: 1 K, 1 D | The deck's name in its folder, with the Width box. Enter: 1 K, 1 D | PictureExport.cpp:105-113 |
| 5. Which slides | All Slides: 1 C | 0 | |
| 6. The result | A message says each slide was saved as a file in a folder, OK: 1 C *(check in PowerPoint)* | 0. A folder named as the file holds Slide1.png, Slide2.png and so on, as PowerPoint names them. A banner says "Exported N slides to the folder …". | PictureExport.cpp:74-87 and 133-137 |
| | **Total** | **7 C + 1 K, 1 D** | **3 C + 1 K, 1 D** |

A different width is the Width box: +2 C in Featherweight. It's remembered for the tab, not between sessions (PresentationView.h:215). PowerPoint exports at a fixed size, and changing it needs a registry value *(check in PowerPoint: 1280 px wide for 16:9 by default?)*.

#### 5c. A deck from a .potx template

| Step | PowerPoint 2021 | Featherweight | file:line (p3-files 799f5bc5) |
|---|---|---|---|
| 1. A new deck from the template | Double-click the .potx: 1 C ("Presentation1") | Double-click: 1 C. A new "Presentation1" opens; the template isn't touched. | App.cpp:1787-1791; PresentationView.cpp:95-97 and 120-121; App.h:356 |
| 1b. From inside the app | File, New, Personal, the template: 4 C *(check in PowerPoint)* | Not listed: File > New lists .potx only once the presentation editor is marked shipped, and it isn't. File, New, More templates… opens the Open dialog on the personal templates folder, but its Templates filter leaves out .potx. Then file type ▾, PowerPoint presentations, double-click the template: **6 C**. (Or Ctrl+O, browse, double-click: 1 K + 1 C, plus browsing.) | UserTemplates.cpp:60-61; core/FileKinds.cpp:23-24; AppRibbon.cpp:245-261; App.cpp:1460-1462, 1866-1878 |
| 2. Save | Ctrl+S, the name and the place, Enter: 2 K, 2 D *(check in PowerPoint: whether 2021 shows File > Save As first, +1 C for Browse)* | Ctrl+S opens Save As with `Documents\Presentation1.pptx`. The name, the place, Enter: 2 K, 2 D | App.cpp:2062-2075; slide/view/PresentationFile.cpp:41-51 |
| | **Total (from Explorer)** | **1 C + 2 K, 2 D** | **1 C + 2 K, 2 D** |

#### 6. Opening a .ppt or an .odp and saving it

#### 6a. A .ppt received by mail: double-click, edit, Ctrl+S

| Step | PowerPoint 2021 | Featherweight | file:line |
|---|---|---|---|
| 1. Open the attachment | Double-click, then Enable Editing in Protected View: 2 C | Double-click: 1 C. The .ppt is converted to a .pptx in memory and opens ready to edit. A banner says "… is a PowerPoint 97–2003 presentation. Saving makes a new `<name>`.pptx and leaves it as it is." It asks nothing. | p3-ppt da559019: core/FileKinds.cpp:12; App.cpp:1787-1791; slide/view/ModelDeck.cpp:356-369; slide/view/PresentationView.cpp:86-99 and 116-124 |
| 2. Edit | Typed | Typed. The converted deck can be edited: CanEdit only refuses a damaged main part. | p3-ppt: slide/edit/Edit.cpp:763-770 |
| 3. Save | Ctrl+S keeps the .ppt (Compatibility Mode): 1 K. (The Compatibility Checker asks first only when newer features would be lost: +1 C.) | Ctrl+S opens Save As, because the tab has no path: `<name>.pptx` beside the .ppt is suggested, as a .pptx. Enter: 2 K. Later Ctrl+S saves that .pptx without asking. | p3-ppt: slide/view/PresentationView.cpp:119 and 123; App.cpp:2062-2075; slide/view/PresentationFile.cpp:41-43; slide/view/PresentationSave.cpp:89-93 and 123-128 |
| | **Total** | **2 C + 1 K** | **1 C + 2 K** |

What each app asks: PowerPoint asks for Enable Editing. On save it asks nothing, unless the Compatibility Checker shows. Featherweight asks nothing on open. On the first save it shows Save As with the new `.pptx` name filled in.

For a mail attachment, "beside the .ppt" is the mail program's temporary folder. PowerPoint's Ctrl+S also writes there, into the .ppt itself. Featherweight's Save As at least shows the folder. That isn't counted.

A .pps starts as a show, and a .pot opens as a new presentation, as in PowerPoint (p3-ppt: slide/view/PresentationView.cpp:114-115 and 131-132).

#### 6b. An .odp: double-click, edit, Ctrl+S

| Step | PowerPoint 2021 | Featherweight | file:line (p3-files 799f5bc5) |
|---|---|---|---|
| 1. Open | Double-click: 1 C. From mail, Enable Editing: +1 C | Double-click: 1 C. It's converted and opens ready to edit. A banner says "… is an OpenDocument presentation. Saving makes a new `<name>`.pptx and leaves it as it is." | core/FileKinds.cpp:12; slide/view/PresentationView.cpp:86-108; slide/view/ModelDeck.cpp:355-373 |
| 2. Edit | Typed | Typed | |
| 3. Save | Ctrl+S, then keep the format in PowerPoint's question: 1 K + 1 C *(check in PowerPoint)* | Ctrl+S, then Enter in Save As, with `<name>.pptx` beside the .odp suggested: 2 K | slide/view/PresentationSave.cpp:48-53; slide/view/PresentationFile.cpp:43; App.cpp:2062-2075 |
| | **Total** | **3 C + 1 K** (from mail) | **1 C + 2 K** |

An .otp opens as a new presentation (slide/view/PresentationView.cpp:120-121).
