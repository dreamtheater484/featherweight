# Office corpus (public)

Files for the never-damage tests (plan §10), made by `make_test_office.py`:

- `generated/`: documents, workbooks and decks made with python-docx, openpyxl and python-pptx.
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
- `reference/`: Office's own PDF of every file in `generated/`, `edge/` and `layout/`, plus `manifest.json`: the Office build, page count, the fonts Office drew with, and each source file's hash. These are the reference renders (plan §10) that `fwtool compare` measures Featherweight against. `scripts/reference-renders.ps1` makes them on the Office reference PC; run it again when a file changes or Office changes noticeably. With `-Folders tests\office\layout` it renders one folder and keeps the other folders' entries.
- `broken/`: hand-made files that open and save, but that `fwtool check` must reject, each for one consistency rule (plan §4.3, rule 2):
  - a relationship id that doesn't resolve, a duplicate paragraph id, a duplicate slide id
  - a bookmark that starts but doesn't end, a field that begins but doesn't end
  - a comment reference without its comment, a formula that names a missing sheet
  - a stale calculation chain (it names a cell without a formula)

`ctest` runs `fwtool` over all of them:

```bash
fwtool roundtrip tests/office/generated tests/office/edge tests/office/layout
fwtool roundtrip --reserialize-all tests/office/generated tests/office/edge tests/office/layout
fwtool check tests/office/generated tests/office/edge tests/office/layout
fwtool check --expect-reject tests/office/broken
fwtool roundtrip --expect-reject tests/office/malicious
```

`fwtool check` applies the reference registry (`src/pkg/References.h`, rules in `ReferenceRules.cpp`): every reference resolves, ids are unique, pairs are complete, and the derived data it can see (the calculation chain, chart caches and embedded workbooks, VML notes) matches its sources. What Office tolerates, such as a missing style, is a warning. It also lists the extensions it doesn't know, near which structural edits will be blocked.

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

**How to run it**, in PowerShell on the Office reference PC, with Word installed:

1. Collect the documents, for example in `D:\Documents\Codex\FeatherweightCorpus`, outside the repository, and name the folder in `FW_CORPUS`. Terminals opened afterwards see it. Documents of your own go in `docx\<category>\` too.
   ```powershell
   setx FW_CORPUS D:\Documents\Codex\FeatherweightCorpus
   ```
2. Make the reference renders. Word exports every document in `docx\` (and `xlsx\`, `pptx\`) to `reference\`, with a manifest; each document takes a few seconds. Run it again after adding documents: unchanged ones keep their PDF, and `-Force` renders them all again. `-Out` puts the renders elsewhere, also outside the repository. Nothing in the corpus is changed or deleted.
   ```powershell
   powershell -ExecutionPolicy Bypass -File scripts\reference-renders.ps1 -Corpus $env:FW_CORPUS
   ```
3. Check that the measurement reads your documents' PDFs correctly. Every document must pass. `fwtool` takes the corpus folder or its `reference` folder.
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

`layout/` has its own script, which needs only the standard library:

```bash
python tests/office/make_layout_docs.py
```

The output is deterministic: document dates and zip timestamps are fixed, so regenerating changes nothing unless the script or a pinned library changes. `.gitattributes` keeps git from touching line ends in these files.
