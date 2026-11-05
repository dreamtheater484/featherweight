# Friction review

The friction review (plan §11, "Zero friction") writes each core flow down step by step, in Word and in Featherweight, so that a flow never gains a step without a reason in the plan. D3 is done when it passes for writing a letter, formatting a CV and fixing a table in a received document (plan §6.4).

`tests/friction-checks.ps1` runs the flows that can be scripted through Featherweight's `--script` mode, off-screen. It checks what the saved file says and counts the steps against this review's budget (see [Scripted checks](#scripted-checks)).

## How steps are counted

- **C**: a click (or a drag). **K**: a key or a chord (Ctrl+S is one). **D**: a decision the flow asks for (a name, a format, a place).
- Typing the content itself, including Enter between paragraphs, isn't counted: it's the same in both.
- **Word** means Word for Microsoft 365 on Windows (version 2410 and later) with its default ribbon, in a window at least 1366 pixels wide.
- **Featherweight** means claude/d2-integration 5957ba4, with D3's operations added as they land.
- When a step has a mouse way and a keyboard way, the table gives the one most people use, and the other in brackets.

## Summary

| Flow | Word | Featherweight today | Featherweight once the findings are fixed | Status |
|---|---|---|---|---|
| Write a letter | 6 C + 3 K, 3 D | 5 C + 4 K, 2 D (F1: Esc) | 5 C + 3 K, 2 D | Passes after F1. The letter flow is scripted. |
| Format a CV (without tab stops and margins) | 2 C + 9 K | 3 C + 14 K | 3 C + 9 K (the one extra click is F4, accepted) | Passes after F1–F3. Tab stops and margins come with D3 (the ruler, page setup). The CV flow is scripted. |
| Fix a table in a received document | 9 C + 2 K + 2 drags | Waits for table editing | Target: no more than Word | D3 tables (claude/d3-tables) |

## Where Featherweight needs more than Word

| # | Flow, step | Word | Featherweight | Owner | Status |
|---|---|---|---|---|---|
| F1 | Letter and CV: choosing a style in the style box, then typing | Typing goes on in the page | Focus stays in the box: typing goes nowhere until a click in the page or Esc. **+1 every time** | widgets (the toolbar's dropdown) | Reported 2026-11-05 |
| F2 | CV: Enter after a heading or the title | The next paragraph is Normal (the style's `w:next`) | It stays Heading 1 / Title. **+1 K per heading** (Ctrl+Shift+N) | widgets (Enter in the editor) | Reported 2026-11-05 |
| F3 | CV: Enter on an empty list item | Ends the list: the item becomes plain text | Stays a list item, and so does what follows. **+1 per list** | widgets, with d2-format (lists) | Reported 2026-11-05 |
| F4 | Letter and CV: a style without a shortcut (No Spacing, Title, Subtitle, Quote) | 1 C in the Home tab's gallery | 2 C: open the style box, choose | Accepted: one toolbar row, no ribbon (plan §5.2). Normal and Heading 1–3 have Word's keys in both. | By design |
| F5 | Letter: the date, in a narrow window | The ribbon shrinks, Insert stays | Below about 1150 pixels wide, the Insert box drops out of the toolbar: then it's command search (Alt+Q, "date", Enter) | d2-format (toolbar priorities) | Note: worth a look, not a finding |

Also found while scripting: **torture finding 7** happens in the app. Centering a line with Ctrl+E and pressing Enter lets AutoCorrect's capital fix garble the line ("second" saved as "SecondS"). Reported to d1-measure as part of finding 7.

## Where Featherweight needs less

- **The date** in a letter: Insert box, then a format (2 C), against Word's Insert tab, Date & Time, a format and OK (4 C).
- **Saving a new file**: Featherweight suggests the first words as the name, in Documents. Word also asks where to save (OneDrive or this PC): one decision fewer.
- **A received document** opens ready to edit. Word opens attachments and downloads in Protected View, with an "Enable Editing" click first.

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

A .docx received by mail, with a four-column table. Fix a typo in a cell, add a row at the end and one in the middle, delete a row, merge two header cells, widen a column, and save.

| # | Step | Word | Featherweight (target, as table editing lands) |
|---|---|---|---|
| 1 | Open the attachment | Double-click: 1 C, then Enable Editing in Protected View: 1 C | Double-click: 1 C. It opens ready to edit. |
| 2 | Fix a typo in a cell | Click in the cell, type | The same |
| 3 | A row at the end | Click in the last cell, Tab: 1 C + 1 K | Same as Word: Tab in the last cell |
| 4 | A row in the middle | The ⊕ at the row's left edge: 1 C (or right-click, Insert, Insert Rows Below: 3 C) | At most 3 C |
| 5 | Delete a row | Right-click, Delete, Delete Rows: 3 C | At most 3 C |
| 6 | Merge two header cells | Select them (drag), right-click, Merge Cells: 1 drag + 2 C | At most the same |
| 7 | Widen a column | Drag its border: 1 drag | The same |
| 8 | Save | Ctrl+S: 1 K | Ctrl+S: 1 K. The original is kept for 30 days as a backup, with no step. |

This flow is added to the scripted checks when claude/d3-tables lands.

## Scripted checks

```
powershell -ExecutionPolicy Bypass -File tests\friction-checks.ps1 [-Workarounds]
```

- **How each flow runs.** `Featherweight.exe --snapshot --size 1100x700 --script @<flow>.script`: a new document, the flow's clicks, keys and typing, then `save:` (standing for Ctrl+S and Enter in the Save dialog).
- **What's checked afterwards.**
  - `fwtool dump --model` must show each paragraph with the style, list label and text the flow wrote.
  - `fwtool check` must pass.
  - The step count must stay within the budget: Word's count plus the extras accepted here (F4).
- **Images and saved files** go to `build\friction`.
- **The scripts are the flows as they should go.** While F1–F3 are open, both flows fail on what the saved file says.
- **`-Workarounds`** adds the steps the findings need today. The content then checks out, and the counts show the cost: the letter takes 9 (budget 9), the CV 17 (budget 12).
- **Click coordinates** (the style box and its list) are for that window size. A toolbar change that moves them makes the check fail, and it's fixed here.

The checks open Featherweight's own window off-screen, and nothing else. They aren't in CI, like `visual-checks.ps1`.
