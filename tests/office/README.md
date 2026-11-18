# Office corpus (public)

Files for the never-damage tests (plan §10), made by `make_test_office.py`:

- `generated/`: documents, workbooks and decks made with python-docx, openpyxl and python-pptx; and `drawings-dml.docx` and `drawings-vml.docx`, pictures and shapes written by hand in DrawingML and VML for fwdraw (plan §4.5) by `make_drawing_tests.py`. `fwtool render` draws their drawings to PNG to compare with the reference renders by eye.
- `edge/`: hand-made files that must round-trip:
  - Markup Compatibility (`mc:AlternateContent`, `mc:Ignorable`)
  - `extLst` extensions and repeated unknown attributes (`x14ac:dyDescent`)
  - unknown namespaces and custom XML with every XML corner: CRLF line ends, comments, processing instructions, CDATA, character references
  - a UTF-16 part, and part names with a space and a non-ASCII letter
  - unusual zip layouts: stored entries, data descriptors, zip64 headers, comments and extra fields
- `layout/`: Word documents for the layout measurement (D1), made by `make_layout_docs.py`. Each stresses what decides where lines and pages break: automatic hyphenation, footnotes and endnotes, tabs with leaders, ligatures, two columns, a floating text box, a table that splits across pages, and a ten-page report with different line spacing.
- `malicious/`: hand-made files that must be refused with a plain message:
  - zip bombs: by compression ratio, by declared size, by a lying header, and by overlapping entries
  - a nesting bomb, and a DTD with an entity-expansion bomb
  - missing content types, zip encryption, duplicate part names
  - broken XML, a file that isn't a zip, and a truncated file
- `reference/`: Office's own PDF of every file in `generated/`, `edge/` and `layout/`, and of D5's `doc/`, `odt/`, `dotx/`, `protected/` and `script/` (Word can't reopen its own `odt/forms.odt` and `forms-protected.odt`, so those two have none; a format copy's entry names its original in `copyOf`, a protected file's its test `password`), plus `manifest.json`: the Office build, page count, the fonts Office drew with, and each source file's hash. These are the reference renders (plan §10) that `fwtool compare` measures Featherweight against. `scripts/reference-renders.ps1` makes them on the Office reference PC; run it again when a file changes or Office changes noticeably. With `-Folders tests\office\layout` it renders one folder and keeps the other folders' entries.
- `torture/`: Word documents for the editing torture test (D2), made by `make_torture_docs.py` with the standard library only. These are the structures an editor must keep intact that the other folders lack: a table of contents as Word writes it (hyperlinked entries, each with a PAGEREF field), Word's empty `_GoBack` bookmark, fields inside content controls, and a simple field (`toc-fields.docx`); and a table as years of edits leave one, with a header row that starts a grid column in and body cells that span two columns with shared properties (`merged-table.docx`); and two sections whose header and footer hold a PAGE field, links, a content control, a bookmark, `mc:AlternateContent` and an element Word doesn't know, the second section showing them through Link to Previous, in a transitional and a strict copy (`headers-fields.docx`, `headers-fields-strict.docx`); and a plain four-column table as one arrives by mail, with a header row and a typo, for the friction review's "fix a table" flow too (`received-table.docx`); and a form with every kind of content control and legacy form field the editor fills in (rich and plain text, one locked, check box, drop-down list, combo box, date, a block control, controls in a table; FORMTEXT, FORMCHECKBOX, FORMDROPDOWN), open and with forms protection enforced (`forms.docx`, `forms-protected.docx`). There's also a contract under review: two reviewers' insertions, deletions, a deleted paragraph mark and formatting changes, with three comments, one a reply and one resolved (`review-contract.docx`). It comes as written by the D4 review work's tests, not from `make_torture_docs.py`. There are no reference renders for them.
- `doc/`, `odt/`, `dotx/`, `protected/`, `script/`: D5's test files, all written by Word (Microsoft 365) on the Office reference PC by `scripts/make-test-files.ps1`.
  - `doc/` and `odt/`: Word 97-2003 and OpenDocument copies of `generated/` and `torture/`, for the `.doc` and `.odt` importers.
  - `dotx/`: templates.
  - `protected/`: password-protected `.docx` (agile encryption) and `.doc` (RC4 CryptoAPI), with the password `featherweight`. One file isn't from this Word: `ecma376-standard.docx` has ECMA-376 standard encryption (EncryptionInfo 3.2, AES-128, as Office 2007 wrote it), which Word 2016 and later no longer write. Its password is `Password1234_`, and `ecma376-standard-plain.docx` is the same document decrypted. Both are msoffcrypto-tool's test files (`tests/inputs/ecma376standard_password.docx` and `tests/outputs/ecma376standard_password_plain.docx` at bdb71ea), under the MIT License, Copyright (c) 2015 nolze.
    - `report-featherweight.docx` is the one not from Word: `layout/report.docx` as Featherweight saves a protected document (`fwtool encrypt`: agile encryption, AES-256, SHA-512), same password. The ctest `office_protected` opens it and Word's protected files with `fwtool decrypt` and encrypts them again; the visual checks show the password card on it.
  - `script/`: documents Word wrote from scratch:
    - charts of each type fwchart draws, and a radar chart for one it shows from its fallback (`charts.docx`; this Word stores each chart's data as `oleObject.bin` rather than an `.xlsx`, which the SDK validator reports, and the charts' cached values are all there);
    - equations of every OMML construct, display and inline (`equations.docx`);
    - Arabic and Hebrew paragraphs, mixed runs, and a right-to-left list and table (`rtl.docx`);
    - Japanese on a document grid, Chinese and Korean, and a vertical section (`eastasia.docx`), from the text in `make_test_files.json`.

  The same script with `-Private` makes the same kinds of copies of the private corpus, beside its `docx` folder.
- `broken/`: hand-made files that open and save, but that `fwtool check` must reject, each for one consistency rule (plan §4.3, rule 2):
  - a relationship id that doesn't resolve, a duplicate paragraph id, a duplicate slide id
  - a bookmark that starts but doesn't end, a field that begins but doesn't end
  - a comment reference without its comment, a formula that names a missing sheet
  - a stale calculation chain (it names a cell without a formula)

- `sheet/`, `excel/`, `csv/`: S1's spreadsheet files (below, "Spreadsheets").

`ctest` runs `fwtool` over all of them:

```bash
fwtool roundtrip tests/office/generated tests/office/edge tests/office/layout
fwtool roundtrip --reserialize-all tests/office/generated tests/office/edge tests/office/layout
fwtool check tests/office/generated tests/office/edge tests/office/layout
fwtool check --expect-reject tests/office/broken
fwtool roundtrip --expect-reject tests/office/malicious
```

`fwtool check` applies the reference registry (`src/pkg/References.h`, rules in `ReferenceRules.cpp`): every reference resolves, ids are unique, pairs are complete, and fields stay where Word needs them. A field may not begin in one hyperlink, custom XML element or simple field and separate or end outside it, nor separate or end inside a content control it didn't begin in: Word refuses or repairs such a file, and the Open XML SDK validator doesn't notice (`Check_FieldsStayInTheirContainers`). Tables likewise: Word refuses a row without cells and repairs a cell without a paragraph or one that ends with a nested table (`Check_WordTablesWordCanOpen`). So with content controls that hold one paragraph (date, drop-down, combo box, plain text, picture): Word says a file has unreadable content when one holds a second paragraph or a table, around a paragraph, a cell or a row (`Check_OneParagraphControls`). It also checks that the derived data it can see (the calculation chain, chart caches and embedded workbooks, VML notes) matches its sources. What Office tolerates, such as a missing style, is a warning. It also lists the extensions it doesn't know, near which structural edits will be blocked.

### Word documents through the model (D1)

With `--model`, Word documents are read into the document model, which writes back every part it owns. The saved copy must pass `fwtool check`. `--out DIR` keeps the copies, for the validator:

```bash
fwtool roundtrip --model --out <copies> <corpus> tests/office/generated tests/office/edge tests/office/layout
fwtool roundtrip --model --reserialize-all <corpus> tests/office/generated tests/office/edge tests/office/layout
dotnet build tools/validate/Validate.csproj -c Release -o build/validate
build/validate/fwvalidate --saved <copies> <corpus> tests/office/generated tests/office/edge tests/office/layout
```

`fwvalidate` (a test tool, never shipped) runs the Open XML SDK validator on each copy and its original. Any error the copy has that the original doesn't fails the run (plan §3). Errors are compared by id, part and description, so namespace prefixes may differ.

**Re-serialize all: the exceptions.** In this mode every XML part is rewritten: parts the model owns come from the model, and the others go through the DOM. Each rewritten part must equal the original after canonicalization (plan §10). These parts are copied byte for byte instead:
- **Parts that aren't XML:** pictures (PNG, JPEG, TIFF, EMF, WMF), obfuscated fonts (`.odttf`), embedded objects and workbooks (`.bin`, `.xlsx`), printer settings, thumbnails. There is no XML to rewrite.
- **Legacy VML drawing parts** (`application/vnd.openxmlformats-officedocument.vmlDrawing`, such as a sheet's comment shapes): their content type isn't XML, and Office writes them as loose HTML-style markup that isn't always well-formed. In Word documents, VML sits inside `document.xml` (`w:pict`, `w:object`), so it is rewritten with the part.
- **Folder entries in the zip**, such as `ppt/`, which aren't parts.

Measured on 2026-11-05: all 108 private corpus documents and the 27 files in `generated`, `edge` and `layout`. Results: round trip 135/135, `fwtool check` on the copies 135/135 (no new warnings), validator 0 new errors (401 in the originals), re-serialize all 135/135.

### Workbooks through the model (S1)

With `--model`, workbooks are read into the workbook model (`src/sheet`), every sheet, and the model writes back the parts it owns: each worksheet (sheetData from the model's cells, rows, formulas and values) and the shared strings. `--workbooks` takes only `.xlsx`, `.xlsm`, `.xltx` and `.xltm` files from the folders. `check --model` also reads each workbook into the model and fails when a cell names a format or a shared string the workbook doesn't have, or shares a formula that has no master. `ctest` runs these on every folder of `tests/office` but `malicious`, `protected` and `reference` (`broken` only through the round trip):

```bash
fwtool roundtrip --model --workbooks tests/office/<folder>...
fwtool roundtrip --model --reserialize-all --workbooks tests/office/<folder>...
fwtool check --model --workbooks tests/office/<folder>...
fwtool roundtrip --model --expect-reject --workbooks tests/office/malicious
fwtool dump --model <workbook>
```

What the model keeps as written, so that what it writes equals the original after canonicalization:
- **Around sheetData:** the rest of the worksheet part (sheet properties, views, columns, merges, conditional formats, hyperlinks, page setup, drawings, extensions) is kept as written and put back as it was. The model reads what it shows from it (views, columns, merges, hyperlinks, page setup, headers and footers, breaks); S2's editing writes those from the model.
- **Inside sheetData**, besides what the model reads: unknown attributes of rows and cells, interned (`x14ac:dyDescent` is stored once, not once per row); a value whose text the model would write otherwise (`1E-3`, `01`, `true`, an error Excel doesn't know, a `t="d"` date); a cell's content when it isn't `f`, `v` and `is` in that order (rich inline strings, comments, unknown elements); comments, unknown elements and text between rows and cells; and rows and cells out of order, which the model can't store and keeps as written where they were.
- **Shared strings:** `<si><t>text</t></si>` (with `xml:space="preserve"` or not) is stored as text; any other string (rich runs, phonetic text, attributes) keeps its XML, and the model reads its text and runs from it.
- **The workbook part and the styles** are read into the model and, in re-serialize-all mode, go through the DOM like the other parts.

**Re-serialize all: the exceptions.** None so far beyond the ones above for Word documents (parts that aren't XML, VML drawings, folder entries).

Measured on 2026-11-16: the 9 workbooks in `generated`, `edge` and `broken`, 15 workbooks Excel wrote (chart data embedded in the private corpus's Word documents), and a generated 1-million-cell workbook: round trip, re-serialize all and `check --model` all pass.

## Editing torture test (D2)

D2 is done when every corpus file, after scripted random edits and a save, has no new validator errors, passes `fwtool check`, and opens in Word without a repair prompt (plan §6.4). Three tools check that:

- **`fwtool torture`** applies rounds of seeded random edits to each Word document through the editor: typing (line breaks too, and with D3 page and column breaks and optional and non-breaking hyphens, as characters), deletes and replacements across fields, bookmarks, comments, content controls, tables and tracked changes, paragraph splits and joins, run, paragraph and paragraph-mark formatting, the Word tab's formatting commands as it runs them (lists: bullets, numbering, multilevel, restart, continue, indent; styles: apply from the gallery, modify, update to match; toggles, font size, clear formatting, fonts, spacing, borders, the Paragraph panel's fields one at a time; with D3 the ruler's tab stops), the format painter, copy and paste (a range or a block of cells, through the clipboard's HTML or RTF or as it is, pasted in body text or a cell, now and then over a selection), groups of edits undone as one step (as Paste and Replace All make), undo and redo, and saves in the middle (Ctrl+S, often followed by an undo back past it and another save), each checked like the last one. With D3's operations built in, they're edits too: tables (insert, text to table and back, rows and columns, merge and split, widths and borders, row heights, AutoFit, shading, header rows), pictures (insert, each format from `tests/fuzz/seeds/picture` too, often followed by save, delete, save, undo, save; resize, rotate, crop, wrap, move, alt text, reset), fields, links and bookmarks, headers, footers and page numbers, page setup, columns, section, page and column breaks, hyphenation, and table styles. With D4's, also: content controls and legacy form fields filled in as their kind is (text typed over, placeholders back, boxes ticked, items and dates picked), new controls, and Tab to the next fill-in (more often in a protected form, where most other edits are refused); comments (new, reply, edited, resolved, deleted); footnotes and endnotes (new, deleted); Track Changes on and off (a fifth of the rounds start with it on) and changes accepted or rejected in a range or all at once; shapes and text boxes (insert each kind, resize, turn, move, wrap, fill, outline, add text); text and picture watermarks, and none; and tables of contents (inserted at a paragraph's start or splitting it; their pages updated, or the whole table rebuilt from the headings there are now). After every save the edited document is also saved as RTF and as text (Save As): the RTF must be ASCII, one group, every group closed, and the UTF-8 and UTF-16 text files must say the same. Bookmarks and link changes aim at fields, links and content controls that are there. A refused edit must leave the document as it was (paragraphs, sections, parts, modified, undo, changed parts; and every paragraph on odd seeds). The read-back comparison includes each table's shape and each picture's size and wrap, and every saved table must be whole (rows with cells, cells ending with a paragraph, a grid as wide as its widest row, vertical merges that continue a cell above), and a saved file keeps no picture nothing shows (an image relationship its part doesn't use, a media part nothing points to): no more of these problems than the original has. On odd seeds, every command that changed a paragraph is undone and redone at once, and must put back exactly what it changed and do it again (a header or footer part a command made may stay, unreferenced, as D3's headers make them outside undo). A formatting change TreeEdit puts back (in a text box) counts as refused. `FW_TORTURE_SKIP=command,paint` (or `save`, `format`, `d3`, `d4`, `paste`; `headers` leaves out D3's header and footer changes, `pagenumber` its page numbers) types instead of those edits, to look past an open finding. Then it saves as the app's Save does (`docx::Serialize`: the changed parts, the document properties, the reopen check). The saved file must open, pass `fwtool check` with no errors the original didn't have, and read back into the model as the edited document. A failure is shrunk to the fewest edits that still fail it and kept in `<out>/repro/` as a text file of edits, with the file they make. `--replay` applies one again:
  ```bash
  fwtool torture --seed 1 --edits 40 --rounds 3 --out build/torture tests/office/generated
  ```
  ```bash
  fwtool torture --replay build/torture/repro/<name>-seed<seed>.txt
  ```
  The replay prints the parts the save wrote and writes `<name>-replay.docx` next to the reproducer. When the edited model and the saved file read back differ, it also writes both texts in full (`-replay-edited.txt`, `-replay-readback.txt`). A reproducer from another checkout finds its document by the tail of the recorded path, so `tests/office/...` files replay anywhere.

  On rounds with an odd seed, every edit is also checked for locality: paragraphs outside the ones it's applied to must read the same before and after (those after it moved by the paragraphs it added or removed). That is the `spill` failure. Even seeds skip it, because reading every paragraph first fills the model's caches, and a stale cache is only caught when they start cold.
  With D5 it takes `.doc`, `.odt` and `.ott` files too, and opens every file as the app does:
  - **Imports and templates:** a `.doc` or an `.odt` is imported into a `.docx`, and a `.dotx` or `.dotm` becomes a new document made from it. The validator and Word compare the saved copy with the `.docx` the app opened (`<out>/opened`).
  - **Protected files:** a password-protected `.docx` or `.doc` is decrypted with its test password from `protected/`. Saving encrypts it again with the same password, and the result must decrypt to what was saved. `scripts/torture.ps1` has Word open that copy with its password (`<out>/saved-protected/<password>`).
  - **Charts:** their parts must keep their bytes.
  - **Equations:** they must stay as they were, compared as XML. When their part is rewritten, the writer declares the namespace again on each child.
  - **Edits:** they aim at charts and equations too, and typing includes Arabic, Hebrew, Chinese, Japanese and Korean text.

- **`fwvalidate`** (above) with `--compare` or `--pairs`: the same comparison for explicit pairs, since the torture test's saved copies aren't named like their originals. `--json` writes the new errors with where they are.
- **`scripts/word-open-check.ps1`** opens each file in Word, hidden and in safe mode, read-only, with macros off, and reports whether Word shows a repair prompt or an error. Word's alerts stay on so that a repair prompt is shown, read and cancelled, never answered. Nothing is ever saved. It needs the Office reference PC. A dialog about that PC's Office rather than the file, such as a missing proofing tool ("Word hyphenation feature is not available"), gives the status `notice`, which passes. When Word's automation fails (Word doesn't start or answer), the file is tried again; after three failures it is `unchecked`, which says nothing about the file and asks for another run. A bare "Command failed" with no dialog is tried again too (a Word that wasn't ready gave it once), but if all three tries get it, it stays the file's error. Before opening anything, the check makes sure COM attached to the Word it started. It never ends a Word it didn't start, and it runs one at a time on the PC, sharing a mutex with `reference-renders.ps1`. With `-Review` it also reads what Word sees in the opened file (its paragraphs, tracked changes and comments, with replies and resolved state) into the JSON.
- **`scripts/excel-open-check.ps1`** does the same for workbooks (S2). Each file gets its own new Excel, hidden, read-only, with macros, events and link updates off; nothing is saved, and only the Excel it started is ever ended.
  - **Alerts are off**, and the repair prompt is found without them. A workbook Excel would offer to repair fails to open normally ("Open method of Workbooks class failed"); the check then opens it with `CorruptLoad` repair, then extract.
  - **Statuses:** `ok` opened normally, with no repair log and no "[Repaired]" caption. `repaired` opened only by repairing; the lines of Excel's repair log (`error*.xml` in TEMP) are reported, such as "Removed Records: Cell information from /xl/worksheets/sheet1.xml part". `refused` means no way of opening worked. `unchecked` means Excel's automation failed three times.
  - **Proof:** `scripts/excel-open-check-proof.ps1` makes broken copies of `excel/sheets.xlsx` (a shared string past the end, a missing style, duplicate sheet names, a truncated sheet, no workbook part, not a ZIP), checks them and compares each status with the expected one.
  - **A known repair:** Excel repairs one corpus workbook as downloaded, LibreOffice's frozen-panes test ("Repaired Records: View"). So the torture gate has to compare each saved copy with its original's status.
- **`scripts/sheet-torture.ps1`** is S2's workbook gate. It runs `fwtool torture --workbooks` (seeded edits through the edit API, see `fwtool` help), then the validator, then `excel-open-check.ps1` on the saved copies.
  - **Originals:** a copy Excel repairs or refuses exactly as it does the original is "original too", which passes.
  - **Undo and redo:** after a round's edits, undoing them all must give the workbook as opened, and redoing them the edited one.
  - **Saves:** a save is checked only when the writer can write everything that changed; otherwise the summary says what it couldn't write. `--ops` limits the edits, for example to what today's writer saves.
  - **Defaults:** without `-Paths` it takes `sheet`, `excel`, `generated`, `edge` and, with `-Corpus`, the private corpus's `xlsx`. `-Ci` runs without Excel.
- **`scripts/build-from-scratch.ps1`** is S2's "build from scratch" check. `build/budget.steps` and `build/invoice.steps` are a household budget and a small invoice, typed step by step in the en-NL region.
  - **Excel's copy:** Excel plays the steps (`FormulaLocal` converts what's typed as typing does), saves, reopens its copy and dumps what it shows. The dump has, per cell, the formula, value, `Range.Text`, number format, font, fill, alignment and borders; per sheet, the column widths, merges and frozen panes.
  - **Featherweight's copy:** with `-Featherweight build\Release\Featherweight.exe`, Featherweight plays the same steps through the grid's commands, as the keyboard does them. The script translates each step into a `--script @file` for a `--snapshot` run of its own (hidden, with its own settings and clipboard): Go to, typing, Enter, AutoSum, Ctrl+D and Ctrl+R, the fill handle, the format keys and choices, Column width, Freeze panes, Rename sheet and Save As. Its copies land in `<out>\featherweight`, each with a picture of the end and what the app printed.
  - **Comparing:** Featherweight's copies (or a folder of them, with `-Saved <folder>`) are dumped the same way and must match, and must open without a repair.
  - **Number formats:** they go through the COM call with en-US. Without it, PowerShell's call carries the user's locale, and `Range.NumberFormat` takes and gives the region's codes ("dd/mm/jjjj", "Standaard"). The S1 cells files' `-CellFormats` field has those codes; nothing reads it.
  - **AutoSum:** Excel's own AutoSum leaves a hidden Excel editing the cell, so Excel types the formula the step names, and Featherweight's AutoSum must propose it.
- **`scripts/contract-review-check.ps1`** is D4's contract review check. It reviews `torture/review-contract.docx` in Featherweight's editor through `fwtool torture --replay`, with the operations the Review pane runs:
  - accept a replacement, reject an inserted clause, accept a changed number, and reject a deleted paragraph mark
  - reply to a comment and resolve its thread
  - save as the app does

  Then `fwvalidate` compares the saved file with the original, and `word-open-check.ps1 -Review` opens it in Word. Word must open it without a prompt and show what the review left: each decided paragraph's text, only the formatting changes still tracked, and the reply under the comment it answers, with that thread resolved. It needs the Office reference PC.

Each saved copy in `<out>/saved` comes with its edits (`<name>.edits.txt`), so a copy that the validator or Word complains about can be shrunk too. That's slower, since each try replays the edits and asks the judge again, and with Word a try takes about 10 seconds:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\torture-minimize.ps1 build\torture\saved\<name>.edits.txt -Judge Word
```

`scripts/torture.ps1` runs all three over the corpus (`tests/office` and, when `FW_CORPUS` is set, the private corpus) and writes `summary.md`. When Word complains about a saved copy, it also opens the original: if Word complains about that too, the editor isn't to blame, and the summary says so. It also replays the kept reproducers in `tests/torture` (each names its file relative to `FW_CORPUS` or the checkout, and is left out when that file isn't there), and their saved copies go to the validator and Word with the rest. CI runs `scripts/torture.ps1 -Ci`: the public corpus, no Word.

```powershell
powershell -ExecutionPolicy Bypass -File scripts\torture.ps1
```

## Layout fidelity (D1)

D1 is done when, across the corpus, the page count matches Word's for at least 90 % of documents and at least 95 % of lines break where Word breaks them (plan §6.4). `fwtool compare` measures both against the reference renders. Featherweight's side is a layout dump per document, which D1's pagination writes. `docs/layout-dump.md` describes the format and exactly how lines are compared.

Until the layout exists, the measurement checks itself on Word's own lines. `ctest` runs these:

```bash
fwtool compare --self-test tests/office/reference
```

```bash
fwtool lines tests/office/reference --out build/fidelity/word
```

```bash
fwtool compare tests/office/reference --dumps build/fidelity/word --min-pages 100 --min-lines 100
```

The self-test reads every Word reference PDF and checks three things: Word's lines match themselves through the dump format (100 %), moving one line break costs exactly that line, and adding a page changes only the page count. The other two write Word's lines as dumps, then measure them through the files and the thresholds.

Once the layout writes dumps, or `LayOutDocument` in `tools/fwtool/LayoutHook.cpp` calls it:

- **The corpus:** the totals, the documents that differ most, and JSON and Markdown reports. With `--min-pages 90 --min-lines 95` it's a CI gate. Without `--dumps`, it lays out each document through the hook.
  ```bash
  fwtool compare tests/office/reference --dumps DIR --json fidelity.json --report fidelity.md
  ```
- **One document**, which is what to look at while tuning layout. It shows where the line breaks first diverge from Word's, side by side with the baselines, then every line that differs (`--all` shows more than 20). The second argument can be the document itself once the hook lays it out.
  ```bash
  fwtool compare tests/office/reference/layout/report.docx.pdf DIR/layout/report.docx.layout.json
  ```
- **Word's lines**, to see what the measurement sees in a PDF: each line's story, position, size and text.
  ```bash
  fwtool lines tests/office/reference/layout/footnotes.docx.pdf
  ```

The reference renders use the Office reference PC's fonts. The plan also wants every metric measured with only the fonts that ship with Windows. That's a mode of the layout (which fonts it may use), not of the reference renders, so it comes with the layout.

## The private corpus

Real documents say much more about layout than generated ones, but they can't be published. They stay in a folder outside the repository, named by the environment variable `FW_CORPUS`. Nothing from it is ever committed: not the documents, not their reference renders, and not the reports, which quote lines of their text.

**What to collect:** Word documents (`.docx`, a few `.docm` and `.dotx`) that you and people around you actually wrote or received.

- Weight it toward what consumers deal with: letters, CVs, reports and theses, school assignments, forms, invoices, contracts, minutes, newsletters and manuals.
- Mix their origins (Word for Windows and Mac, Word for the web, exports from Google Docs and LibreOffice), paper sizes (A4 and Letter) and languages.
- Make sure these all occur: footnotes, long tables, columns, text boxes, headers and footers with page numbers, lists, pictures with text wrapping, and hyphenation.
- Most should be 1 to 30 pages, and a few over 100 pages, for the opening budgets.
- Leave out password-protected files.

**How many:** 100 to 200 documents for D1's first measurement, which also sets the targets for Windows-only fonts. The plan aims for at least 1,000 per format by each viewer release. The documents go in subfolders by kind (`docx\letters`, `docx\reports`): the reports name documents by their path, so problems by kind stand out.

**The layout**, which `scripts\fetch-corpus.ps1` builds from `tests\office\real-world.json`:

```
<FW_CORPUS>\
  docx\<category>\<id>.docx                 the documents that passed the safety screen
  reference\docx\<category>\<id>.docx.pdf   Word's PDF of each
  reference\manifest.json                   as tests\office\reference\manifest.json; sources are
                                            relative to FW_CORPUS, PDFs to reference\
  quarantine\                               downloads being screened; never rendered or measured
```

The published part of the corpus is listed in `tests/office/real-world.json`: blank templates and published documents from governments, universities, international organizations and publishers, each with its source, terms of use, hash, language, producer, Word's compatibility mode and tags for the structures it contains, so tests can pick files by tag. Rejected candidates stay in its `rejected` list with their reasons. `scripts/fetch-corpus.ps1` rebuilds it exactly. It downloads each file over HTTPS from the allowed hosts only, checks its hash, and screens it before anything opens it: `.docx` only, no macros, ActiveX, OLE objects, attached templates, external links other than plain hyperlinks, or risky fields, then `fwtool check` and Microsoft Defender.

```powershell
powershell -ExecutionPolicy Bypass -File scriptsetch-corpus.ps1 -Out $env:FW_CORPUS
```

**How to run it**, in PowerShell on the Office reference PC, with Word installed:

1. Collect the documents, for example in `D:\Documents\Codex\FeatherweightCorpus`, outside the repository, and name the folder in `FW_CORPUS`. Terminals opened afterwards see it. Documents of your own go in `docx\<category>\` too.
   ```powershell
   setx FW_CORPUS D:\Documents\Codex\FeatherweightCorpus
   ```
2. Make the reference renders. Word exports every document in `docx\` (and `xlsx\`, `pptx\`) to `reference\`, with a manifest; each document takes a few seconds. Run it again after adding documents: unchanged ones keep their PDF, and `-Force` renders them all again. `-Out` puts the renders elsewhere, also outside the repository. Nothing in the corpus is changed or deleted.
   ```powershell
   powershell -ExecutionPolicy Bypass -File scripts\reference-renders.ps1 -Corpus $env:FW_CORPUS
   ```
3. Check that the measurement reads your documents' PDFs correctly. Every document must pass. `fwtool` takes the corpus folder or its `reference` folder. A note that Word's page count differs from its PDF isn't a failure. Word counts before it exports, sometimes still the count the file was saved with, and the PDF is what's measured against.
   ```powershell
   build\tools\fwtool\Release\fwtool.exe compare --self-test $env:FW_CORPUS
   ```
4. Once the layout exists, measure the corpus, with the reports next to it:
   ```powershell
   build\tools\fwtool\Release\fwtool.exe compare $env:FW_CORPUS --json "$env:FW_CORPUS\fidelity.json" --report "$env:FW_CORPUS\fidelity.md"
   ```
   To look at one document:
   ```powershell
   build\tools\fwtool\Release\fwtool.exe compare "$env:FW_CORPUS\reference\docx\letters\offer.docx.pdf" "$env:FW_CORPUS\docx\letters\offer.docx"
   ```

## Spreadsheets (S1)

S1's public files (`docs/s1-plan.md`, "Corpus and measuring"), at least one per feature:

- `sheet/`: workbooks made by `make_sheet_tests.py` with openpyxl (no cached formula values, as openpyxl and many other producers write them):
  - `numfmt.xlsx`: the number-format sampler. Every built-in format id 0–58 written as Excel writes them (an id with no code, so the reader must know it, and take 14, 22 and the currency ones from the locale), about 150 custom codes (sections, conditions, colors, locale and currency tags, fractions, elapsed times, `_` and `*`, literals, scientific, scaling) over six values, the same codes in narrow columns ("####"), and General over 40 values in columns of ten widths.
  - `dates-1900.xlsx`, `dates-1904.xlsx`: the same serials (0, 60 = 29 February 1900, negatives, 9999-12-31 and beyond, fractions) in both date systems, and date functions.
  - `merged-frozen.xlsx`, `rows-columns.xlsx` (widths, heights, hidden, outline groups, default sizes), `text.xlsx` (alignment, wrap, rotation, overflow, fonts, rich text, scripts), `borders-fills.xlsx` (every border and pattern, gradients, theme colors with tints, indexed colors), `sheets.xlsx` (hidden and very hidden sheets, tab colors, right to left, zoom, gridlines, links, page layout view).
  - `functions.xlsx`: one sheet per function on s1-calc's first list (about 64), each with its cases over the same data (numbers, text, mixed values, dates, an error, lookup tables). `formulas.xlsx`: operators and precedence, references (other sheets, 3D, names, mixed), errors, array formulas, structured references; Excel adds shared formulas and dynamic arrays to its copy.
  - `conditional-formats.xlsx`, `tables.xlsx`, `pictures-shapes.xlsx`, `notes.xlsx` (notes, and threaded comments with a reply and a resolved one, written as Excel 365 writes them), `notes-at-end.xlsx` (the same, both sheets printing their notes and comments at the end of the sheet), `charts.xlsx` (column, bar, line, pie, doughnut, area, scatter, combo, radar, and a chart sheet), `print-settings.xlsx` (print area and titles, fit to width and to a page, scale, margins, centering, headers and footers with first and even pages, gridlines and headings, page breaks and order).
- `excel/`: the same workbooks as Excel saves them, made by `scripts/make-sheet-files.ps1` on the Office reference PC. Excel recalculates every formula (so each has its cached value) and adds what openpyxl can't write: shared formulas, spilling dynamic arrays (and a blocked one, which Excel stores as `#VALUE!` with rich-value metadata), shapes and a group, sparklines. The script takes the PC's traces out of the copies (the folder they were saved in, the author, the threaded comments' person).
- `csv/`: CSV and TSV files as `make_sheet_tests.py` writes them, byte for byte (`.gitattributes` keeps git off them): UTF-8 with and without a byte-order mark, UTF-16 LE and BE, Windows-1252; comma, semicolon (with a `sep=` line too), tab and pipe; quoting with line breaks inside; CRLF, LF, CR and mixed line ends; leading zeros, 16+ digit numbers, date-like codes, formula-like text; ragged, empty, malformed, very wide (16,384 and 16,500 fields) and very long (a 40,000-character field) files. `csv/expected.json` gives each file's encoding, byte-order mark, delimiter and line ends, and the exact text of every field: opening a CSV must keep every one of them (plan §7.1).

**Reference renders** (`scripts/reference-renders.ps1 -Folders tests\office\sheet,tests\office\excel`): Excel exports each workbook to PDF with its print settings, and writes `<file>.cells.json` beside it. Excel runs as for Word: a new instance of its own, hidden, macros forced off, links never updated, read-only, and only that Excel is ended. Each workbook is opened twice: first with calculation manual, for the formulas' cached values, then as anyone opens it (Excel recalculates what it would), for the PDF and the cells; then Excel recalculates everything. A cells file holds, per sheet:

- the used range, hidden rows and columns, column widths and the standard width;
- every non-empty cell as `[ref, text, value, formatLocal]`: the text Excel shows (`Range.Text`, "####" included), the raw value (numbers to 17 digits, errors as `{"e": "#N/A"}`) and, in the public files, the number format as Excel's object model reports it here. That is in local form on the reference PC (`#.##0,00`, `dd/mm/jjjj` for format 14, `Standaard` for General), so it shows how Excel reads the code in this locale; the file's own code is in its `styles.xml`;
- every formula cell as `[ref, formula, cached, opened, recalculated]`: the formula (`Formula2`), the value cached in the file (empty text when the file has none, as openpyxl's files), the value after opening, and Excel's own full recalculation.

**The printer.** Excel lays out for Windows' default printer as it is when Excel starts: page breaks, and also the column widths and row heights it reports and saves (the standard width is 8.43 with the reference printer, 8.09 with Microsoft Print to PDF). Setting Excel's printer afterwards doesn't lay out again. So `make-sheet-files.ps1` and `reference-renders.ps1` stop unless the default printer is the reference printer the manifest records (`excel.printer`, or `-ExcelPrinter`). The display counts too: the reference PC is used through Remote Desktop, and while that session is disconnected Excel lays out differently again (standard width 8.29 with the reference printer). So both scripts first check a blank sheet's standard width against the manifest's `excel.standardWidth` (8.43: the references were made with the session connected) and render nothing when it differs. Render with someone signed in: either the Remote Desktop session connected, or signed in at the PC itself. The 17 Nov renders, made with Roy signed in at the console, match the 16 Nov ones apart from dates.

Excel's PDF export on this PC stops drawing a page at a data bar, an icon set or a sparkline: the rest of that page, or all of it, is empty. Printing through Microsoft Print to PDF draws them: `-PrintPdf conditional-formats.xlsx,charts.xlsx` writes `<file>.print.pdf` beside the export (the manifest's `printPdf` and `printPages`), laid out for that printer, so its page breaks can differ slightly. But with "Let Windows manage my default printer" on, printing makes Microsoft Print to PDF the default until the next print elsewhere, and the next run stops; so only with that setting off.

`fwtool compare --cells` measures S1's cell text against `text`: each workbook is read into the model and each visible cell formatted by the number-format engine in the locale Excel recorded (the 99 % target counts visible cells). Differences that only the column width decides ("####", and General shortened to fit) are counted apart until the grid measures widths; a `*` fill matches when Excel's text is ours with the fill repeated there; formulas without a cached value, and those Excel recalculated on opening (volatile ones), aren't compared. `--json FILE` writes the counts and the differences per workbook, `--min-cells C` fails below C %.

```bash
fwtool compare --cells tests/office/reference
```

`fwtool formulas` (with s1-calc) will measure the engine's recalculation against `cached`.

**The locale.** Excel formats with the Windows regional format, and the reference PC's is English (Netherlands), `en-NL`: decimal comma, `.` for thousands, dates `dd/MM/yyyy`, 24-hour times, list separator `;` in Excel. Every cells file records what Excel used (`locale`: separators, date order, Windows' date and time patterns, currency), and so does the manifest (`excel`), with the printer the PDFs were laid out for. The comparison formats with that recorded locale, not the PC's.

**What Excel taught us** (things the reader and the engines must match):
- With alerts off, Excel refuses to open a file it would have to repair; the scripts treat that as a broken file instead of saving a repaired copy.
- A formula with the number `1E308` (above Excel's largest literal, 9.99999999999999E+307) makes Excel refuse the whole file.
- Excel 2021's object model can't add threaded comments, but Excel 2021 keeps them when it saves.
- On this PC `Range.NumberFormat` gives codes in local form, not in the file's (invariant) form.
- Excel's calculation chain lists every cell of a multi-cell array formula, though only the first has an `f` element (`fwtool check` now allows that).
- A chart sheet's chart anchored at 0 x 0 (as openpyxl writes it) shows and prints as an empty page; `make_sheet_tests.py` gives it the size Excel writes.
- Excel's PDF export leaves out data bars, icon sets and sparklines, and the rest of the page after them (above); printing draws them.
- Excel's column widths, row heights and page breaks follow the default printer Excel started with (above); a copy Excel saved for Microsoft Print to PDF has `defaultRowHeight="14.5"` instead of 15.

**Opening budgets** (plan §11): `make_budget_files.py` writes the two big files to `build/budgets` (or a folder it's given), never into git: `million-cells.xlsx` (50,000 rows by 20 columns, with a formula column and its cached values, 6.8 MB) and `100mb.csv` (about 1.5 million rows, with leading zeros, 18-digit codes and quoted fields). Both are deterministic.

To regenerate (the venv as below; the second and third steps need Excel on the Office reference PC):

```bash
.venv/Scripts/python tests/office/make_sheet_tests.py
```

```powershell
powershell -ExecutionPolicy Bypass -File scripts\make-sheet-files.ps1
```

```powershell
powershell -ExecutionPolicy Bypass -File scripts\reference-renders.ps1 -Folders tests\office\sheet,tests\office\excel -PrintPdf conditional-formats.xlsx,charts.xlsx
```

## Opening budgets

Plan §11 sets the budgets on the typical consumer PC:

- A typical 20-page `.docx` shows its first page within 400 ms.
- A 300-page one shows its first page within 700 ms, and is fully paginated within 3 s.

`fwtool bench` times opening each Word document from the open request. Each time is the median of several runs after a warm-up:

```bash
fwtool bench --runs 5 --json bench.json tests/office/layout
```

The stages are those of `fw::OpenTimings` (`src/core/OpenTimings.h`):

1. the file read into memory
2. the package opened
3. the model built
4. the first page laid out
5. the first page shown (the app only)
6. every page laid out

Whoever opens a document calls `Start()` when it's asked for, and each engine calls `Mark()` as it passes its stage. That way the app and `fwtool bench` time the same things. Until the layout exists, `bench` times the read and the package, and parses Word's XML parts once (`xml`) as a stand-in for the model. The layout hook (`tools/fwtool/LayoutHook.cpp`) adds the model, the first page and pagination.

How `bench` applies the budgets:

- **First page:** within 400 ms up to 20 pages, and 700 ms beyond.
- **Fully paginated:** within 3 s up to 300 pages, and 10 ms a page beyond. Documents up to 20 pages have no separate pagination budget.
- **Page count:** until the layout counts pages, `bench` uses the count Word last saved (`docProps/app.xml`).

`--budgets` fails when a budget is missed. The numbers that count come from the typical consumer PC. The development PC is several times faster, so there `bench` only shows the trend.

`bench` times the engines inside `fwtool`. The whole app, from the launch to the first page on screen, is timed by `fwtool bench-app` and gated by `scripts\budgets.ps1` (`tests/budgets/README.md`). Once the app opens `.docx`, its fixed files join the PDFs there.
## Regenerating

Use a venv, never the global Python. From the repository root:

```bash
python -m venv .venv
```

```bash
.venv/Scripts/python -m pip install -r tests/office/requirements.txt
```

```bash
.venv/Scripts/python tests/office/make_test_office.py
```

`make_drawing_tests.py` and `layout/`'s own script, `make_layout_docs.py`, need only the
standard library:

```bash
python tests/office/make_drawing_tests.py
python tests/office/make_layout_docs.py
```

The output is deterministic: document dates and zip timestamps are fixed, so regenerating changes nothing unless the script or a pinned library changes. `.gitattributes` keeps git from touching line ends in these files.
