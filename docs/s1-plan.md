# S1: the spreadsheet viewer

The work plan for S1 (IMPLEMENTATION_PLAN.md §7, the S1 row of the milestones table). Each session reads this first, then §7 and §4.3 of the plan.

S1 opens `.xlsx`, `.xlsm`, `.xltx`, `.xltm`, `.csv` and `.tsv` files read-only: cells with their formats, formulas recalculated where Excel would, charts, pictures, notes, find, copy and print. Editing is S2.

S1 starts before 2.0's fidelity targets are met (decision 23). Nothing in S1 depends on them.

## Exit criteria

- **Display:** ≥ 99 % of non-empty cells show the same text as Excel's PDF export, and ≥ 90 % of printed pages pass the pixel threshold (SSIM ≥ 0.95 at 100 dpi, plan §10).
- **CSV:** every corpus CSV opens without any value changing.
- **Formulas:** of the corpus formulas that use only implemented, non-volatile functions, ≥ 99.5 % reproduce Excel's cached values, and every mismatch is documented.
- **Never damage a file:** open and save with no edits keeps untouched parts byte for byte; re-serialize-all equals the original after canonicalization, with each exception documented; `fwtool check` and the Open XML SDK validator stay clean.
- **Budgets (plan §11), on the development PC:**
  - a 1-million-cell `.xlsx` shows its first screen in < 1.5 s;
  - a 100 MB CSV shows its first screen in < 1 s and is fully loaded in < 5 s;
  - a typical workbook uses < 150 MB;
  - scrolling keeps every frame within the refresh interval, and nothing runs when idle;
  - the exe grows by ≤ 1.5 MB in all (5.67 MB now; < 11 MB at 3.0);
  - opening a PDF or a document is no slower than before S1.
- **Fuzzed:** `fuzz_xlsx`, `fuzz_csv`, `fuzz_numfmt` and `fuzz_formula` (the engine's parser), 60 s in CI, no findings.
- **Checklist:** a spreadsheet part in `docs/TEST_CHECKLIST.md`, passed by Roy.
- **Release:** Roy decides. 2.0 isn't out yet, so S1 may ship as a beta, as D5 did with 0.95.

## Reference PC

Excel 2021 (16.0, ProPlus 2021 Retail) on this PC, Windows locale en-US: short dates `M/d/yyyy`, decimal point `.`, list separator `,`. Excel's PDF exports use these, so the number-format engine is measured in en-US. Functions newer than Excel 2021 show their cached values in the reference too.

## Pieces

Integration branch: `claude/s1-int`, made from `d0/host` 2026-11-16. Each session works on its own branch from it, pushes, and tells the coordinator in its turn-end report. **The coordinator integrates:** merges finished branches into `claude/s1-int`, builds, runs ctest and pushes. At the end, one PR into `main` (Roy merges).

The weights are for progress reports.

| Piece | Session | Phase | Weight | Size budget |
|---|---|---|---|---|
| Corpus, reference renders, `fwtool compare` for workbooks | s1-corpus | 1 | 15 % | 0 (tools only) |
| Workbook model and `.xlsx` reader | s1-model | 1 | 20 % | 300 KB |
| Number formats, then CSV | s1-numfmt | 1 | 12 % | 150 KB |
| Formula engine | s1-calc | 1 | 20 % | 450 KB |
| Grid view, find, copy, print | s1-grid | 2 | 18 % | 300 KB |
| Conditional formats, tables, pictures, notes, charts completed | (who's free) | 3 | 10 % | 250 KB |
| Final gate, checklist | coordinator | 3 | 5 % | 0 |

- **Phase 1** starts at once.
- **Phase 2** starts when s1-model has pushed the model's public API (`src/sheet/model/Workbook.h` and friends) and its reader reads cells.
- **Phase 3** goes to sessions as they finish.

### Source layout

Plan §4.8: `src/sheet/` with `model/`, `xlsx/`, `csv/`, `calc/`, `numfmt/` and `view/`.

- `fwsheet` (static library): model, xlsx, csv, calc and numfmt. Headless: no window, no PDFium, no `fwui`.
- `fwsheetview`: the view, on `fwui`, linked into the exe only.
- `src/sheet/CMakeLists.txt` holds `add_library(fwsheet STATIC)` and one `add_subdirectory` per folder; each folder's own `CMakeLists.txt` adds its files with `target_sources`. Two pieces adding a line there conflict by one line, and the coordinator resolves it.
- Unit tests: `tests/unit/test_sheet_<piece>.cpp`.

### Corpus and measuring (s1-corpus)

There are no workbooks in the private corpus yet (`D:\Documents\Codex\FeatherweightCorpus` has 158 Word documents), and 11 generated `.xlsx` files in `tests/office/`.

- **Public test files first** (`tests/office/`, generated with openpyxl, then opened and saved once by Excel where Excel's own output matters). At least one file per S1 feature:
  - a number-format sampler: every built-in format and the custom code features (sections, conditions, colors, locales, fractions, elapsed times, `_` and `*`, "####");
  - the 1900 and 1904 date systems;
  - merged cells, frozen panes, hidden and outline rows and columns, column widths and row heights, wrapped and rotated text, borders and fills, theme colors;
  - formulas: one sheet per implemented function, shared and array formulas, names, other-sheet references, errors;
  - conditional formats, tables, pictures, shapes, notes, threaded comments, every §7.2 chart type, a chart sheet;
  - print settings: print area, titles, fit to page, orientation, headers and footers;
  - CSV: UTF-8 with and without BOM, UTF-16, ANSI; comma, semicolon, tab and pipe; quoted fields with line breaks; leading zeros, 16+ digit numbers, date-like codes (`1-2`, `MAR1`), and very long rows.
- **Real-world workbooks:** extend `scripts/fetch-corpus.ps1` (and `tests/office/real-world.json`) to `.xlsx` and `.csv`.
  - At least 150 workbooks and 30 CSV files to start, weighted toward what consumers receive: budgets, invoices, schedules, timesheets, grade lists, price lists, forms and government statistics tables. The plan's 1,000 per format comes later.
  - Reputable public sources only (government open-data portals, statistics offices, universities, Apache POI's and LibreOffice's test files where the license allows).
  - **The safety screen**, as for Word, plus for workbooks: reject macros (`vbaProject.bin`, macro-enabled content types), external links (`externalLink` parts), data connections and query tables, Power Query (`DataMashup`), OLE objects, ActiveX, embeddings, and formulas with DDE (`|`), `WEBSERVICE`, `FILTERXML`, `RTD`, `CALL` or `REGISTER`. Defender scans the quarantine folder.
  - **Ask Roy in the session before the first download**, listing the sources, the number of files and the total size.
  - Layout: `xlsx\<category>\<id>.xlsx`, `csv\<category>\<id>.csv`; reference PDFs in `reference\xlsx\…` and the manifest.
- **Reference renders:** `scripts/reference-renders.ps1` gets an Excel mode. Each file is exported to PDF with `ExportAsFixedFormat`, its print settings as saved. Also record per sheet: the displayed text of every non-empty cell (`Range.Text`, which includes "####"), the used range, and each formula's cached value and Excel's own recalculated value.
- **`fwtool compare` for workbooks**, once s1-model and s1-numfmt have landed:
  - cell text: our displayed text per cell against Excel's `Range.Text`, as the 99 % target counts it;
  - pixels: our printed pages against Excel's PDF (needs s1-grid's printing; phase 3);
  - `fwtool formulas`: recalculate every formula and compare with Excel's cached values (with s1-calc).
- **Then:** the never-damage checks for workbooks (round trip, re-serialize-all, check, validator) on the whole corpus, and the opening budgets (a generated 1-million-cell workbook and a 100 MB CSV).

### Workbook model and `.xlsx` reader (s1-model)

- **First, and early (others need it):** the model's public API, pushed as soon as it compiles. Then message the coordinator.
- **Model** (plan §7.3): sheets store cells sparsely in copy-on-write blocks of rows, about 16–32 bytes per cell with a value. Shared strings with rich runs; deduplicated formats; repeated unknown attributes interned (§4.3 rule 1); shared formulas kept shared; cached values typed (number, string, bool, error).
- **Read** with `fwpkg`'s streaming reader for sheet data and shared strings, and the DOM for small parts. The active sheet first; the others at idle (§4.3 rule 8).
- **Covered:** workbook (sheets and their state, defined names, `date1904`, `calcPr`, the active tab), styles (number formats, fonts, fills, borders, cell XFs, cell styles, DXFs, indexed and theme colors through `fwdraw`), sheet views (frozen and split panes, selection, zoom, gridlines, right-to-left), columns and rows (widths, heights, hidden, outline levels, collapsed), merged cells, hyperlinks, page setup and print options, header and footer text, and the parts that phase 3 reads (conditional formats, tables, drawings, comments) kept intact.
- **The round-trip store and a writer** for the round-trip tests (saving is S2): untouched parts copied raw; re-serialize-all equals the original after canonicalization.
- **`fwtool`:** `roundtrip`, `check` and `dump` for workbooks.
- **Fuzzed:** `fuzz_xlsx`.
- **Phase 3:** the drawing anchors (pictures, shapes, charts, chart sheets) and notes.

### Number formats, then CSV (s1-numfmt)

- **Number formats** (`src/sheet/numfmt/`): one engine for all of Excel's format codes. It needs nothing from the other pieces.
  - The built-in formats by id, and the locale-dependent ones (14, 22 and the like) from the locale.
  - Sections and conditions, colors (`[Red]`, `[Color10]`), locale and currency tags (`[$€-x]`, `[$-409]`), digit placeholders, thousands separators and scaling, percent, scientific, fractions, dates and times (`[h]`, `AM/PM`, fractional seconds), `@`, `_x` and `*x`, literals and escapes.
  - General as Excel shows it, which depends on the column width (up to 11 characters, then rounding or scientific).
  - "####" when a value doesn't fit: the engine takes a width-fit callback, so it stays headless.
  - The 1900 (with 1900 as a leap year) and 1904 date systems; 15 significant digits.
  - The output: text, color, and the `_` and `*` spans the view needs for padding and fill.
  - A locale interface: Windows' settings in the app, fixed locales in tests.
  - **Value parsing** for CSV now and for typing in S2: numbers, percentages, currency, dates and times in the locale's order.
  - Fuzzed: `fuzz_numfmt`.
- **CSV and TSV** (`src/sheet/csv/`), when the model API is in (plan §7.1):
  - encoding (BOM, UTF-8 validity, UTF-16, the ANSI code page) and delimiter detection;
  - Excel's quoting rules, line breaks inside quotes, any line ending;
  - **a value is kept as text whenever converting it would lose information.** A value becomes a number or date only if formatting it back with the format it was given yields exactly the original text. Leading zeros, 16+ digits and date-like codes stay text;
  - the source's encoding, BOM, delimiter, quoting and line endings recorded for S2's saving;
  - read once into memory, parsed in parallel chunks; the first screen first;
  - fuzzed: `fuzz_csv`.
- **Phase 3:** conditional formats (they share the condition and color logic).

### Formula engine (s1-calc)

`src/sheet/calc/`, plan §7.3 "Formula engine". It reads cells through an interface it defines, so it starts before the model exists; it binds to the model once s1-model's API lands.

- **Parser:** A1 and R1C1 references, sheets and 3D references, names, structured references, arrays, errors, the range, union and intersection operators, `_xlfn.` and `_xlws.` prefixes. References to other workbooks are kept but not resolved.
- **Evaluation:** formulas compile to compact bytecode. Ranges are evaluated lazily, without copying cells. Excel's behavior where it matters: 15 significant digits, the date systems, error propagation, implicit intersection, comparisons between text and numbers.
- **Dependency graph:** ranges tracked as ranges (`SUM(A:A)` is one dependency); dirty propagation and topological recalculation; volatile functions; circular references detected; iterative calculation when the file asks.
- **After opening** (plan §7.3 "Cached values"): show the cached values; then recalculate in the background, time-sliced on the UI thread, only what Excel would: volatile functions and their dependents, formulas without a cached value, everything with `fullCalcOnLoad`. Manual mode is respected. A formula with an unsupported function keeps its cached value, marked possibly out of date.
- **The first ~50 functions,** chosen by counting the corpus once it exists. Until then start with the obvious ones: SUM, IF, AVERAGE, COUNT, COUNTA, COUNTIF(S), SUMIF(S), AVERAGEIF(S), MIN, MAX, ROUND, ROUNDUP, ROUNDDOWN, INT, ABS, MOD, VLOOKUP, HLOOKUP, INDEX, MATCH, XLOOKUP, IFERROR, IFNA, AND, OR, NOT, ISBLANK, ISNUMBER, ISERROR, CONCATENATE, CONCAT, TEXTJOIN, LEFT, RIGHT, MID, LEN, TRIM, UPPER, LOWER, PROPER, SUBSTITUTE, FIND, SEARCH, VALUE, TEXT (with s1-numfmt's engine), DATE, YEAR, MONTH, DAY, TODAY, NOW, EDATE, EOMONTH, WEEKDAY, NETWORKDAYS, SUMPRODUCT, ROW, COLUMN, OFFSET, INDIRECT.
- **`fwtool formulas`:** recalculates every corpus formula and compares with the cached values (with s1-corpus).
- **Fuzzed:** `fuzz_formula` extended to the engine's parser and evaluator.

### Grid view (s1-grid, phase 2)

`src/sheet/view/`, a `TabContent` (plan §4.2, §5.3), drawn through `Painter`.

- Only visible cells are drawn, with cached text layouts (`fwtext`). Text overflows into empty neighbors. Column widths follow Excel's rule (the widest digit of the default font), so they match.
- Headers, gridlines, merged cells, frozen panes, hidden rows and columns, outline groups (expand and collapse), zoom, right-to-left sheets, sheet tabs (hidden sheets left out), the formula bar (read-only in S1).
- Selection, keyboard navigation (Ctrl+arrows, Ctrl+PgUp/PgDn), copy (text, TSV, HTML), find, and the status bar's sum, average and count.
- Workbook kinds turned on in `FileKinds`, the start screen and recent files; `--snapshot` for workbooks.
- **Print and print preview** through `PagedView`: page breaks, print area and titles, scaling and fit to page, orientation, margins, headers and footers, gridlines. This is what the pixel target measures.
- The opening and scrolling budgets above.

## Rules

- **Never damage a file:** the round trip, re-serialize-all, `fwtool check` and the validator stay clean on every corpus workbook. Parts S1 doesn't understand are kept byte for byte.
- **Nothing active:** macros never run, external links and connections are never refreshed, `WEBSERVICE` and the like are never called. Their cached values are shown.
- **Lightweight:** engines do nothing until a workbook is opened. Each piece measures its size with `scripts/budgets.ps1` and records it in the ledger with a reason. No new third-party libraries without the coordinator's OK.
- **Excel automation** (reference renders): always a new, separate Excel instance (`DispatchEx`), with alerts, events, link updates and macros off; end only the Excel you started, and always end it. Roy may be using Excel at the same time.
- **Downloads:** ask Roy first (see the corpus piece). Each download goes into its own quarantine folder and is screened before anything opens it.
- **Browsers:** Playwright's own Chromium, headless only. Never the installed Chrome or Edge (Roy's global CLAUDE.md).
- **Usage:** check `get_usage` before each new step. At 98 % weekly usage, stop at a clean pushed commit with a short note.
- **Commits:** small, on your branch, with clear messages; no Co-Authored-By trailer. Push your branch, and say in your turn-end report what is ready to merge.
