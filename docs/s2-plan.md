# S2: editing spreadsheets

The work plan for S2 (IMPLEMENTATION_PLAN.md §7, the S2 row of the milestones table). Each session reads this first, then §7, §4.3 and §11 of the plan, and `docs/s1-plan.md` for what S1 built.

S2 makes the S1 viewer an editor: typing values and formulas, formatting, inserting and deleting rows, columns and sheets, sort and filter, and saving `.xlsx` and CSV without damaging anything. S3 (charts, tables, conditional formatting and validation created, about 350 functions, protection, page setup, notes written, Export PDF) follows straight after; release 3.0 comes with S4.

Roy (2026-11-17): S2 starts as soon as S1 is merged, S3 straight after it, and he does the manual checks only after S3. So S2's gate is automated end to end; the checklist part for S2 is written now and passed by Roy after S3.

## Exit criteria

- **Build from scratch:** a household budget and a small invoice can be built from a blank workbook, scripted through the same commands the keyboard and mouse use (not by writing the model directly), saved, and opened in Excel without a prompt, with the same values, formulas and formats Excel shows.
- **Never damage a file** (the torture gate): every corpus workbook (`tests/office/sheet`, `tests/office/excel`, the generated files and the 205 real-world workbooks), after scripted random edits (typing values and formulas, formatting, inserting and deleting rows, columns, cells and sheets, renaming and moving sheets, sort, filter, paste) and a save: no new Open XML SDK validator errors, `fwtool check` passes, and Excel opens it **without a repair prompt**. Every finding is fixed or documented.
- **Untouched means untouched:** open, edit one cell, save: every part the edit didn't touch is copied byte for byte; the edited sheet differs only where it must (plus the calculation chain dropped, §7.3).
- **CSV** (§7.1): saving an existing CSV keeps its encoding, BOM, delimiter, quoting and line endings, and writes untouched rows exactly as read; the "CSV keeps only the values of the first sheet" banner when the workbook holds more; new CSV files are UTF-8 with a BOM and the regional list separator.
- **Formulas typed in the region** (decision 17): English function names, Windows' decimal and list separators (`=SUM(A1;2,5)` here), dates in the regional order; stored invariant; Excel opens them with the same results.
- **Budgets (plan §11), on the development PC:**
  - keystroke to screen while typing in a cell: < 16 ms typical, < 50 ms worst;
  - recalculation after one edit, 100,000-formula workbook: < 100 ms typical;
  - inserting a row in a 1-million-cell workbook: < 100 ms to screen; undo as fast;
  - saving the 1-million-cell workbook runs on a worker thread (copy-on-write snapshot); the UI keeps responding;
  - S1's budgets still met; nothing runs when idle;
  - the exe grows by ≤ 1.5 MB in S2 (7.04 MB after S1; < 11 MB at 3.0, with S3 and S4 still to come).
- **Fuzzed:** `fuzz_xlsx` extended with edits and saving, `fuzz_formula` with formula entry in both locales, `fuzz_csv` with saving; 60 s each, no findings.
- **Checklist:** the S2 part of `docs/TEST_CHECKLIST.md`, written in S2; Roy passes it after S3.

## Reference PC

As S1: Excel 2021 on this PC, Windows' regional format en-NL (decimal comma, `dd/MM/yyyy`, `;` list separator in formulas). Excel automation always in a new, separate instance (`DispatchEx`), alerts, events, macros and link updates off; end only the Excel you started.

## Pieces

Integration branch: `claude/s2-int`, made from `main` after the S1 merge. Each session works on its own branch from it, pushes, and says in its turn-end report what's ready to merge. **The coordinator integrates:** merges into `claude/s2-int`, builds, runs ctest, pushes. At the end, one PR into `main`, which the coordinator merges (Roy, 2026-11-17).

The S1 sessions carry on, each with the piece closest to what it built.

| Piece | Session | Weight | Size budget |
|---|---|---|---|
| Edit layer, reference registry, saving, recovery | s2-model (was s1-model) | 25 % | 450 KB |
| Editing in the grid: entry, formula entry, fill, clipboard, rows, columns and sheets | s2-grid (was s1-grid) | 25 % | 450 KB |
| Formula entry and incremental recalculation | s2-calc (was s1-calc) | 12 % | 200 KB |
| Format Cells, format painter, sort and filter, CSV saving | s2-format (was s1-numfmt) | 18 % | 350 KB |
| Torture gate, Excel open check, build-from-scratch scripts, budgets | s2-corpus (was s1-corpus) | 15 % | 0 (tools only) |
| Final gate, checklist | coordinator | 5 % | 0 |

The size budgets add up to 1.45 MB; each piece measures with the build-map method s1-grid used for the S1 ledger entry.

### Order

1. **First, and early (everyone needs it): s2-model's edit API** — operations, transactions, undo — pushed as soon as it compiles, with a note in the shared notes folder (`s2-model-edit-api.md`). Until then the others build on S1's model read-only (dialogs, parsing, the torture script's skeleton).
2. Everything else in parallel, each piece through the edit API only (§4.3 rule 4: no code changes the model directly).
3. The torture gate runs from the first save onward, not at the end.

## Edit layer, reference registry, saving and recovery (s2-model)

Reuse what D2 built for Word wherever it fits: `src/doc/edit` (operations with inverses, transactions, typing coalescing), safe saving, the backup, the owner file, change detection and the recovery journal (§4.3 rules 3, 4, 6, §13). Share code rather than copy it; if something Word-specific is in the way, generalize it in place with Word's tests still passing.

- **Operations** with inverses on the copy-on-write row blocks (§4.3 rules 4, 5): set cell (value, formula, style), set range styles, row and column properties (height, width, hidden, outline level), merge and unmerge, insert and delete rows, columns and cells (shift right or down), sheet add, rename, move, delete, color, hide, freeze panes, defined names. Transactions; undo and redo cost about as much as the edit.
- **The reference registry** (§4.3 rule 2, §7.3 "Structural edits"): inserting or deleting rows and columns, and renaming, moving or deleting sheets, update every place a reference lives: formulas (cells, shared, array, dynamic), defined names, print areas and titles, merged cells, hyperlinks, tables and autofilters, conditional formats and data validations including the x14 extension-list ones, sparklines, chart series and their caches, pivot sources, and note anchors in VML. `#REF!` where Excel puts it. An unknown extension that could hold references blocks the edit, with a banner. Each kind gets a test that compares with what Excel does to the same file (Excel probe scripts, as S1's).
- **Derived data:** the calculation chain is dropped when any formula changes; a formula with an unsupported function is written back with its value and `fullCalcOnLoad`; chart caches follow their cells; dynamic-array formulas carry `cm` and `metadata.xml`; `_xlfn.` and `_xlws.` prefixes as Excel writes them.
- **The writer for edited workbooks:** untouched parts copied raw; the edited sheet and the parts that must follow (sharedStrings, styles, workbook, calcChain, metadata) written so Excel opens them without a prompt. Strings and styles deduplicated; unused ones kept (Excel's own behavior) unless that breaks a rule.
- **Saving:** `.xlsx`, `.xlsm` (macros kept byte for byte, never run), `.xltx`/`.xltm` (opened as a new workbook; Save As). Safe saving as for Word; the snapshot compare (Save becomes Save As when the file changed on disk); serialization on a worker thread from a copy-on-write snapshot (§4.3 rule 5).
- **Crash recovery** from the operation journal, encrypted with DPAPI, versioned (§4.3 rule 6).
- **`fwtool`:** `torture` learns workbooks (with s2-corpus); `check` verifies the registry's kinds independently of the model.
- **Fuzzed:** `fuzz_xlsx` with random operations, undo, redo and save.

## Editing in the grid (s2-grid)

All through the edit API. Behavior as Excel's, keystroke for keystroke, unless the plan says otherwise.

- **Entering data:** type to replace, F2 to edit in the cell, the formula bar (now editable), Enter, Tab, Shift+Enter, Shift+Tab and the arrows as in Excel (Enter after Tab returns to the starting column), Esc cancels, Ctrl+Enter fills the selection, Alt+Enter a line break, AutoComplete from the column, Ctrl+; and Ctrl+Shift+; (date and time), Delete clears contents.
- **Formula entry** (§7.2): clicking or arrow keys insert a reference while typing a formula (point mode), referenced ranges get colored borders that can be dragged, function names autocomplete with argument tips (the list and the tips from s2-calc), F4 cycles relative and absolute.
- **The fill handle:** drag to copy or extend series of numbers, dates, weekdays, months and text with a number; double-click fills down to the end of the neighboring data; Ctrl+D and Ctrl+R.
- **Clipboard:** cut, copy and paste within and between workbooks (formulas adjust; cut moves and keeps references), paste special (values, formats, formulas, transpose), and pasting from Excel (its HTML and text formats), Word and the web. The marching-ants copy border; Esc clears it.
- **Rows, columns and cells:** insert and delete (Ctrl++, Ctrl+−, the context menu), heights and widths by dragging and double-click to fit, hide and unhide, merge and unmerge, group and ungroup (Alt+Shift+→/←), freeze panes.
- **Sheets:** add, rename (double-click the tab), move (drag the tab), delete, color, hide and unhide, from the tabs' context menu.
- **Find and replace, go to** (Ctrl+H, Ctrl+G, F5), undo and redo with their lists.
- **AutoSum** (Alt+=) with the range s2-calc proposes.
- **The tab's file behavior as for documents:** the unsaved dot, Save, Save As, close prompts, recovery offers, change detection, reopening.
- **The friction review** (§11): "total a column" and "fix a value in a received workbook" written down with their steps; never more than Excel's.
- **Budgets:** keystroke to screen in a cell; inserting a row in the million-cell workbook; repaint only what changed.

## Formula entry and incremental recalculation (s2-calc)

- **Formula entry in the region** (decision 17): parse what's typed with English names and Windows' separators into the invariant form, and show stored formulas back in the region's form in the formula bar and while editing. Errors as Excel reports them (the "There's a problem with this formula" cases become a message, never a crash).
- **Incremental recalculation after edits:** only what the edit dirtied, time-sliced with "Calculating…" in the status bar when it's long; circular references detected and shown in a banner; iterative calculation when the file asks; manual mode respected (F9 calculates). Budget: < 100 ms typical after one edit in a 100,000-formula workbook.
- **What the grid needs from the engine:** the function list with argument names and short descriptions (English, small), the reference spans in a formula being typed (for the colored borders and point mode), F4's next form of a reference, AutoSum's proposed range (as Excel picks it), and the formula's shift when copied, filled or moved (with s2-model's registry for structural edits).
- **Functions:** S1 has 165+, more than S2's 150. Add whatever the real-world corpus and the build-from-scratch workbooks use that's missing; the rest of the 350 is S3.
- **Fuzzed:** `fuzz_formula` with entry in en-US and en-NL.

## Format Cells, format painter, sort and filter, CSV saving (s2-format)

- **Format Cells** (Ctrl+1): number (general, number, currency, accounting, date, time, percent, fraction, scientific, text, custom, with the codes shown and typed in the region's form: `LocalToInvariant`), alignment (horizontal, vertical, wrap, indent, orientation), font, border, fill. The toolbar's quick formats (bold, italic, underline, font, size, colors, borders, alignment, number format, increase and decrease decimals, merge and center). Cell styles (the built-in gallery) and the format painter.
- **Typing values** as Excel converts them (unlike CSV opening): numbers, percentages, currency, dates and times in the region's order, fractions, leading apostrophe for text; the cell gets the format Excel would give it.
- **Sort** by one or more columns (with the header guess Excel makes), **filter** (Ctrl+Shift+L: value lists, text and number filters, written as `autoFilter` Excel reads back), **remove duplicates**.
- **CSV saving** (§7.1, from what S1's reader recorded): existing files keep encoding, BOM, delimiter, quoting and line endings, untouched rows written exactly as read; the banner when the workbook holds more than CSV can store (Save as .xlsx / Keep CSV); new CSV files UTF-8 with a BOM and the regional list separator.
- **Fuzzed:** `fuzz_csv` with saving (a file read and saved untouched is byte-identical).

## Torture gate, Excel open check, build from scratch, budgets (s2-corpus)

- **Excel open check:** a script like `word-open-check` for workbooks: opens each saved file in a separate Excel instance and detects the repair prompt reliably (find a method that doesn't depend on alerts being shown, for example Excel's repair log or `CorruptLoad` behavior, and prove it on a deliberately broken file). Reports per file: opens cleanly / repaired (with Excel's repair log) / refused.
- **`fwtool torture` for workbooks** (with s2-model): scripted random edits through the edit API (typing values and formulas in both locales, formatting, insert and delete rows, columns, cells and sheets, rename and move sheets, merge, sort, filter, paste), undo and redo of random spans, then save; `fwtool check`, the validator, the Excel open check and a reopen in Featherweight. Seeds recorded, so every finding replays.
- **Build from scratch:** the household budget and the small invoice as scripts through the grid's commands (not the model), saved, then compared with Excel: same values, formulas and formats shown (cell texts through `Range.Text`).
- **Budgets:** add the S2 timings to `fwtool bench`/bench-app: keystroke in a cell, recalculation after one edit (100,000 formulas), insert a row in the million-cell workbook, save of the million-cell workbook (UI responsiveness during it).
- **Corpus:** the 205 real-world workbooks and 37 CSV files from S1; the 7 refused files stay out. More only with Roy's yes (ask in the coordinator's session, which relays).

## Source layout

- `src/sheet/edit/` (new, in `fwsheet`): operations, transactions, the registry. `src/sheet/xlsx/` gains the writer; `src/sheet/csv/` the CSV writer.
- `src/sheet/view/` (`fwsheetview`): editing UI, dialogs (Format Cells, sort, filter, find and replace, go to).
- Unit tests: `tests/unit/test_sheet_<piece>.cpp`. The shared rules from S1 (one `add_subdirectory` line per folder; the coordinator resolves one-line conflicts).

## Rules

As S1 (`docs/s1-plan.md`, "Rules"), plus:

- **Through the edit API only.** No code outside `src/sheet/edit` changes the model.
- **Never damage a file** is the first gate, not the last: the torture run is part of every piece's done.
- **Lightweight:** each piece measures its size and records it in the ledger with a reason; no new third-party libraries without the coordinator's OK.
- **Roy's Featherweight:** never close an instance you didn't start; benchmarks and UI checks use their own instances.
- **Usage:** check `get_usage` before each new step; at 98 % weekly usage stop at a clean pushed commit with a short note.
- **Commits:** small, on your branch, clear messages, no Co-Authored-By trailer. Push, and say what's ready to merge in your turn-end report.
