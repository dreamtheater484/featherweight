# S3: charts, tables and more

The work plan for S3 (IMPLEMENTATION_PLAN.md §7, the S3 row of the milestones table). Each session reads this first, then §7, §4.3 and §11 of the plan, and `docs/s1-plan.md` and `docs/s2-plan.md` for what S1 and S2 built.

S3 finishes the spreadsheet editor: charts, tables, conditional formatting and data validation created and edited; about 350 functions with dynamic arrays and names; protection; page setup; notes and comments written; hyperlinks, pictures and shapes. S4 (`.xls` and `.ods`, pivot tables, password-protected files, sparklines, multi-threaded recalculation, release 3.0) follows.

Roy (2026-11-17, decision 24): S3 runs straight after S2, and he does the manual checks for S1, S2 and S3 together after S3. So S3's gate is automated end to end, like S2's.

## Exit criteria

- **The feature list works:** everything under "basic" in §7.2, and everything under "advanced" except S4's items (pivot tables, sparklines, password-protected workbooks). Each item is in the checklist's S3 part with its steps, and each has an automated test or script that drives it through the grid's commands.
- **Never damage a file** (S2's torture gate, extended): every corpus workbook, after scripted random edits that now also create, change and delete charts, tables, conditional formats, validations, notes, comments, hyperlinks, pictures, shapes, names, protection and page setup, then a save: no new validator errors, `fwtool check` passes, Excel opens it without a repair prompt (`scripts/excel-open-check.ps1`). Every finding fixed or documented.
- **Created as Excel shows it:** a fixture set built in Featherweight through the grid's commands (every §7.2 chart type, table styles with a total row, every conditional format kind, every validation kind, notes and threaded comments, hyperlinks, a picture and shapes, page setups with print titles and scaling) opens in Excel without a repair prompt; Excel reads back the same objects through automation (chart type and series, table range and style, rule types and ranges, validation lists, comment text, link targets, page setup values); and Excel's PDF export of each matches Featherweight's Export PDF at S1's threshold (SSIM ≥ 0.95 per page).
- **Formulas:** about 350 functions, each with Excel 2021 cases (done early on `claude/s3-calc`); dynamic arrays spill as you type, with `#SPILL!` where Excel puts it, and are written so Excel keeps them dynamic (`cm` and `metadata.xml`); names defined and managed in the Name Manager.
- **Budgets (plan §11), on the development PC:**
  - S1's and S2's budgets still met; nothing runs when idle;
  - a chart redraws within the keystroke budget (< 16 ms typical) after an edit to its data;
  - creating a chart, table or rule from a selection: < 100 ms to screen;
  - the exe grows by ≤ 1.35 MB in S3 (7.91 MB after S2; < 11 MB at 3.0, S4 still to come).
- **Fuzzed:** `fuzz_xlsx` with S3's operations, undo, redo and save; `fuzz_formula` with S3's functions (done); 60 s each, no findings.
- **Checklist:** the S3 part of `docs/TEST_CHECKLIST.md`, written in S3; Roy passes S1, S2 and S3 after the gate.

## Reference PC

As S1 and S2: Excel 2021 on this PC, Windows' regional format en-NL. Excel automation always in a new, separate instance (`DispatchEx`), alerts, events, macros and link updates off; end only the Excel you started. The Epson ET-4750 is Windows' default printer; never change printer or system settings.

## Pieces

Integration branch: `claude/s3-int`, made from `claude/s2-int` (S2's merged state) plus `claude/s3-calc`. Each session works on its own branch from it, pushes, and says in its turn-end report what's ready to merge. **The coordinator integrates:** merges into `claude/s3-int`, builds, runs ctest, pushes; at the end one PR into `main`, which the coordinator merges. Sessions merge only `origin/claude/s3-int`, never each other's branches.

The S2 sessions carry on (renamed), each with the piece closest to what it built; their contexts compact as they fill, and the state notes and the git history carry the work over.

| Piece | Session | Weight | Size budget |
|---|---|---|---|
| Charts created and edited; pictures and shapes inserted | s3-charts (was s2-model) | 24 % | 400 KB |
| Tables, conditional formatting, data validation | s3-data (was s2-format) | 22 % | 350 KB |
| Notes and comments written, hyperlinks, protection | s3-review (was s2-grid) | 14 % | 200 KB |
| Page setup, print preview, Export PDF | s3-calc, after its own piece | 12 % | 150 KB |
| Functions, dynamic arrays while editing, names | s3-calc (was s2-calc) | 10 % (about 7 % done) | 250 KB (170.5 KB used) |
| Torture gate extended, "created as Excel shows it", budgets, checklist | s3-corpus (was s2-corpus) | 13 % | 0 (tools only) |
| Final gate | coordinator | 5 % | 0 |

The size budgets add up to 1.35 MB; each piece measures with the linker-map method and records it in `tests/budgets/ledger.json` with a reason.

### Order

1. **First (others need it): adding new parts.** Charts, pictures, notes, comments, tables and hyperlinks all add parts and relationships to a saved workbook (a drawing, a chart, `comments`, VML, a table, the relationship and the content type). If S2's writer has no single helper for that, s3-charts writes one in `src/sheet/edit` (an operation with an inverse that adds or removes a part with its relationship and content type) and pushes it as soon as it compiles, with a note `s3-add-part.md` in the notes folder. Until then the others build their dialogs and model operations.
2. Everything else in parallel, each piece through the edit API only (§4.3 rule 4).
3. The torture run learns each new operation as it lands, not at the end.

### Shared files

- **Command ids** (`src/Commands.h`): each piece uses its own block: s3-charts 2800–2849, s3-data 2850–2899, s3-review 2900–2939, s3-print 2940–2969, s3-calc 2970–2999.
- **The grid's UI:** each piece puts its handlers and dialogs in its own new files (`WorkbookCharts.cpp`, `WorkbookData.cpp`, `WorkbookReview.cpp`, `WorkbookPageSetup.cpp`, `WorkbookNames.cpp`); shared files (`WorkbookView.h`, `WorkbookCommands.cpp`, `WorkbookInput.cpp`, the toolbar and menus) get only the lines that hook them in. One-line conflicts there are the coordinator's to resolve.
- **The reference registry** (S2) already updates chart series, tables, conditional formats, validations, hyperlinks and note anchors on structural edits; a piece that adds a new kind of reference adds it to the registry with a test.

## Charts, pictures and shapes (s3-charts)

`fwchart` draws every §7.2 chart type since S1; S3 creates and edits them and writes them.

- **Insert a chart** from a selection (Insert → Chart, and Alt+F1 for the default chart): column, bar, line, pie, doughnut, area, scatter and combo, with series and categories guessed from the selection as Excel guesses them (header row and column, rows or columns by shape).
- **Edit a chart:** change its type, title, axis titles, legend (position, on or off), data labels, colors (series and point fills, Excel's color palettes), the data range (Select Data: add, remove and reorder series), move and resize by dragging, delete, cut, copy and paste; move to a chart sheet and back.
- **Write it** as Excel does: `xl/charts/chartN.xml` with the series formulas and their caches (the caches follow the cells), `xl/drawings/drawingN.xml` with the anchor, the relationships and content types; Excel's default style and colors, so the chart looks the same in Excel. Charts in files Featherweight didn't write keep everything it doesn't edit byte for byte.
- **Pictures:** Insert → Pictures (PNG, JPEG, GIF, BMP; SVG with its PNG fallback where Windows can make one, otherwise not offered), from a file and pasted from the clipboard; move, resize (keeping the aspect ratio with Shift), delete, alt text. Pictures are stored once even when pasted twice.
- **Shapes:** Insert → Shapes: rectangle, rounded rectangle, oval, line, arrow, text box; fill, outline and text; move, resize, delete.
- **Objects and cells:** move and size with cells as Excel does (the anchor kinds), and objects follow inserted and deleted rows and columns.
- **Budgets:** chart redraw after a data edit within the keystroke budget; creating a chart < 100 ms to screen.

## Tables, conditional formatting, data validation (s3-data)

- **Tables:** Insert → Table and Ctrl+T (with "My table has headers"), the table styles gallery (light, medium, dark, as Excel's built-in ones), banded rows and columns, first and last column, the total row (with its function per column, written as `SUBTOTAL` as Excel does), the header row's filter buttons, resizing (the corner handle, and growing when you type just below or right of it), renaming, Convert to Range. Structured references typed and shown (`Table1[Amount]`, `[@Amount]`), calculated columns that fill down. Written as `xl/tables/tableN.xml` with the sheet's `tableParts`.
- **Conditional formatting:** highlight cells rules (greater than, less than, between, equal to, text that contains, a date occurring, duplicate values), top and bottom rules, data bars, color scales and icon sets with Excel's presets; New Rule (including "use a formula", typed in the region's form); Manage Rules (edit, delete, reorder, stop if true, applies to); Clear Rules. Written as Excel writes them, including the x14 extension-list parts for data bars and icon sets.
- **Data validation:** Data → Data Validation: any value, whole number, decimal, list (typed or a range, with the in-cell dropdown), date, time, text length, custom formula; the input message; the error alert (stop, warning, information) enforced when typing and pasting as Excel does; Circle Invalid Data. Written as Excel writes them, including the x14 lists that point to other sheets.
- **Fuzzed:** the rules' formulas and lists through `fuzz_formula`.

## Notes, comments, hyperlinks, protection (s3-review)

- **Notes** (Excel's old comments): New Note (Shift+F2), edit, delete, show and hide one or all, the red corner, resizing the box. Written as Excel writes them: `xl/commentsN.xml` and the VML drawing with its shape per note, so Excel shows them in the same place and size.
- **Threaded comments:** New Comment, reply, edit, delete, resolve; written as Excel 365 writes them (`threadedComments`, `persons`) with the note fallback Excel 2021 reads. The author's name comes from Windows' user name, not from anything the user didn't type.
- **Hyperlinks:** Insert → Link (Ctrl+K): a web address, a place in this workbook (cell or name), a file, an e-mail address; edit, remove; the `HYPERLINK` function. Clicking follows a link as Excel does; links to a web address or file go through the same confirmation the Word editor uses. Nothing is opened without a click.
- **Protection:** Protect Sheet (with its permissions list and an optional password, hashed as Excel hashes it: SHA-512 with salt and spin count), Unprotect Sheet (asking for the password), Protect Workbook structure; locked and hidden cells (Format Cells → Protection) respected: typing, pasting, deleting, inserting, formatting and the other edits are refused on a protected sheet as Excel refuses them, with Excel's message; hidden formulas don't show in the formula bar. Protected files from Excel are respected the same way.

## Page setup, print preview, Export PDF (s3-print)

S1 prints and exports as Excel prints; S3 lets the user change how.

- **Page Setup:** orientation, paper size, scaling (adjust to %, fit to N pages wide by M tall, and the "fit sheet on one page", "fit all columns on one page" shortcuts in print preview), margins (with center horizontally and vertically), headers and footers (Excel's presets and custom sections with the codes for page, pages, date, time, file and sheet), print area (set, add to, clear), print titles (rows and columns to repeat), gridlines, headings, page order, first page number.
- **Page breaks:** insert and remove manual page breaks; Page Break Preview with draggable breaks.
- **Print preview** applies changes at once, with the settings beside the preview as in Excel's backstage; printing a selection, the active sheets or the whole workbook.
- **Export PDF** follows the same page setup (it already shares S1's print path; check it after every change above).
- Written as Excel writes them (`pageSetup`, `pageMargins`, `headerFooter`, `printOptions`, `rowBreaks`, `colBreaks`, the `_xlnm.Print_Area` and `_xlnm.Print_Titles` names).

## Functions, dynamic arrays while editing, names (s3-calc)

S3's formula engine work is mostly done on `claude/s3-calc` (merged into `claude/s3-int` at the start): 357 functions with Excel 2021 cases, and the engine side of defined names (`NameManager.h`). What's left:

- **The Name Manager** (Ctrl+F3): the list (name, value, refers to in the region's form, scope, comment), New, Edit, Delete, filter; Define Name from the selection; Create from Selection (Ctrl+Shift+F3); the Name Box defines a name when you type one; names in AutoComplete while typing a formula; Paste Names (F3).
- **Dynamic arrays while editing:** a typed formula that returns an array spills, with the spill border, `#SPILL!` when it's blocked (and the reason as Excel gives it), the spill range referenced as `A1#`, and editing a cell in a spill range refused as Excel does; written with `cm` and `metadata.xml` (check S2's writer does it for newly typed formulas), and implicit intersection (`@`) shown where Excel shows it.
- **Functions:** whatever the real-world corpus or the S3 fixtures use that's still missing; the distributions and `CONVERT` were left out on purpose (rare in the corpus) and stay out unless the corpus says otherwise.
- **Recalculation:** charts, conditional formats and validations that depend on formulas update after an edit within the S2 budgets.

## Torture gate, "created as Excel shows it", budgets, checklist (s3-corpus)

- **`fwtool torture`** learns each S3 operation as it lands (create, edit and delete charts, pictures, shapes, tables, rules, validations, notes, comments, hyperlinks, names, protection, page setup), with seeds recorded so every finding replays. The full run over the corpus (the same 246 workbooks as S2's gate) plus the Excel open check.
- **The fixture set** (exit criterion "created as Excel shows it"): scripts that build each fixture through the grid's commands, save it, have Excel read back the objects, and compare Excel's PDF export with Featherweight's (SSIM ≥ 0.95, S1's tool). Findings go to the piece that owns them.
- **Budgets:** add S3's timings to `fwtool bench` (chart redraw after a data edit; creating a chart, table and rule from a selection); check S1's and S2's budgets still hold; size per piece from the ledger.
- **The checklist's S3 part** in `docs/TEST_CHECKLIST.md`: every §7.2 item S3 adds, with steps Roy can follow; written as pieces land and checked against the app.
- **Corpus:** the 205 real-world workbooks and 37 CSV files from S1; the 7 refused files stay out. More only with Roy's yes (ask in the coordinator's session, which relays).

## Notes folder

Hand-over notes between sessions live in `D:\Documents\Codex\FeatherweightNotes\s3\` (outside the repository, so every worktree sees the same folder): one `<piece>-state.md` per session with what's pushed, what's next and what it waits on, kept current at every turn end, so a fresh session can take over a piece if a context fills up.

## Rules

As S1 and S2 (`docs/s1-plan.md` and `docs/s2-plan.md`, "Rules"), in short:

- **Through the edit API only.** No code outside `src/sheet/edit` changes the model. Every new edit is an operation with an inverse, so undo and redo work for it.
- **Never damage a file:** the torture run is part of every piece's done. Parts and attributes Featherweight doesn't edit are kept byte for byte.
- **Nothing active:** macros never run; links open only on a click, with the confirmation; external data is never refreshed.
- **Lightweight:** each piece measures its size and records it in the ledger with a reason; no new third-party libraries without the coordinator's OK.
- **Excel automation:** a new, separate instance (`DispatchEx`), alerts, events, link updates and macros off; end only the Excel you started, and always end it. Roy may be using Excel at the same time.
- **Roy's Featherweight:** never close an instance you didn't start; benchmarks and UI checks use their own instances.
- **Browsers:** Playwright's own Chromium, headless only. Never the installed Chrome or Edge (Roy's global CLAUDE.md).
- **Usage:** check `get_usage` before each new step; at 98 % weekly usage stop at a clean pushed commit with a short note in your state file. The coordinator resumes everyone after the weekly reset.
- **Commits:** small, on your branch, clear messages, no Co-Authored-By trailer. Push, and say what's ready to merge in your turn-end report.
