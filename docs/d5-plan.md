# D5: breadth, and release 2.0

The work plan for D5 (IMPLEMENTATION_PLAN.md §6, the D5 row of the milestones table). Each session reads this first.

## Exit criteria

- With Office's fonts (Word is installed on this PC):
  - page counts match Word for ≥ 97 % of documents;
  - ≥ 98 % of lines break where Word breaks them.
- The targets for Windows-only fonts set in D1 still hold.
- All budgets are met (§11): exe < 7 MB (4.97 MB now), typing < 16 ms typical and < 50 ms worst in 300 pages, memory after launch < 40 MB, nothing runs while idle.
- The manual checklist passes (a D5 part is added to `docs/TEST_CHECKLIST.md`).
- Release 2.0 is published, with Roy's OK.

## Pieces

Branches start from `origin/claude/d5-int`. **fwpkg** integrates: it merges finished branches into `claude/d5-int`, then builds, runs ctest and pushes. The weights below are for progress reports.

| Piece | Session | Branch | Weight | Size budget |
|---|---|---|---|---|
| CFB reader, password-protected files, integration | fwpkg | `claude/d5-crypt` | 10 % | 80 KB |
| `.doc` import | d1-model | `claude/d5-doc` | 15 % | 400 KB |
| `.odt` import and templates | widgets | `claude/d5-odt` | 10 % | 300 KB with the templates |
| Charts (`fwchart`) | d1-draw | `claude/d5-charts` | 12 % | 250 KB |
| Equations, then right-to-left and East Asian text | d1-text | `claude/d5-script` | 18 % | 300 KB |
| Fidelity and compatibility modes | d1-measure | `claude/d5-fidelity` | 20 % | 100 KB |
| Test files, huge documents, corpus pass, friction review, torture | d1-corpus | `claude/d5-quality` | 10 % | 0 |
| Final gate, checklist and release | coordinator | `d0/host` | 5 % | 0 |

### CFB and password-protected files (fwpkg)

- **First, and early (others need it):** a [MS-CFB] compound file reader in `fwpkg`, hardened and fuzzed (`fuzz_cfb`). Push it to `claude/d5-int` as soon as it works.
  - API: `fw::pkg::CompoundFile::Open(std::span<const uint8_t>) -> std::unique_ptr<CompoundFile>`, then `Stream(std::string_view path) -> std::optional<std::vector<uint8_t>>` and `Streams()`.
  - Also a writer, for saving encrypted files.
- **[MS-OFFCRYPTO]:** agile and standard encryption of `.docx`, through CNG (BCrypt), with no crypto library of our own.
  - Open asks for the password (the same prompt as PDF passwords).
  - Save keeps the same password.
  - Backups and recovery copies stay encrypted.
  - Recovery journals use DPAPI (plan §4.3).
- **Encrypted `.doc` files** (RC4 CryptoAPI) open when d1-model's reader can use the decrypted streams. Otherwise they get a clear message.

### `.doc` import (d1-model)

- **The [MS-DOC] reader, into the `fwdoc` model:** FIB, the piece table (CLX), CHPX, PAPX and SEPX, styles (STSH), lists (LST and LFO), tables, fields, headers and footers, notes, comments, bookmarks and links, and inline pictures plus OfficeArt floats as far as practical.
- **Saving:** Save goes to Save As `.docx`. A banner explains it once (plan §5).
- **Fuzzed:** `fuzz_doc`.
- **Fidelity:** compared with Word on the `.doc` copies of the corpus that d1-corpus makes.

### `.odt` import and templates (widgets)

- **The ODF text reader:** styles and automatic styles, paragraphs and spans, lists, tables, frames and pictures, headers and footers, notes, bookmarks and links, sections and page layout. Same Save As rule as `.doc`.
- **Fuzzed:** `fuzz_odt`.
- **Templates:**
  - a letter, a CV and a report, small and embedded in the exe;
  - "New from template" on the start screen and in the + menu;
  - opening a `.dotx` or `.dotm` makes a new untitled document from it.

### Charts (d1-draw)

- **`fwchart`:** charts shown read-only from `c:chartSpace`'s cached values.
  - Types: column, bar, line, pie, area and scatter, with titles, axes, gridlines, legends and data labels as far as practical.
  - Other types show the stored fallback picture, or a placeholder.
- **Everywhere:** on screen, printed and in Export PDF.
- **Kept on save:** the chart part, unchanged.
- **Fuzzed:** `fuzz_chart`.

### Equations, right-to-left and East Asian text (d1-text)

- **Equations: OMML shown read-only.**
  - `m:oMath` and `m:oMathPara`: fractions, scripts, radicals, n-ary operators, delimiters, matrices, `eqArr`, accents, bars, functions, boxes, limits.
  - Cambria Math through DirectWrite, using the font's MATH table where it helps.
  - On screen, printed and in Export PDF; kept unchanged on save.
  - Fuzzed through `fuzz_layout`.
- **Then right-to-left and complex scripts:**
  - bidi paragraphs and runs, and Arabic, Hebrew and Indic shaping through DirectWrite;
  - the caret and selection in right-to-left text.
- **Then East Asian line breaking:** kinsoku, compression and the grid as far as Word applies them.

### Fidelity and compatibility modes (d1-measure)

- **First:** re-measure the private corpus. Write down which fonts are present and missing.
- **Compatibility:** implement the `compatibilityMode` (11, 12, 14) and `w:compat` flags that matter, chosen by measuring the corpus (plan §6.3).
- **Then:** fix the biggest causes of page and line mismatches, until page counts match for ≥ 97 % of documents and lines for ≥ 98 %.
- **Coordination:** each layout fix goes on `claude/d5-fidelity`, and d1-text and d1-draw are told about anything that touches their code.

### Quality (d1-corpus)

- **First, test files for the other pieces**, from Word on this PC, made from the existing corpus:
  - `.doc` and `.odt` copies;
  - password-protected `.docx` and `.doc`;
  - `.dotx` templates;
  - documents with charts and equations;
  - reference renders of each.
- **Public documents** with charts, equations, right-to-left and East Asian text come only from reputable sources (for example the Open XML SDK tests, LibreOffice's test files, public government forms).
- **Then:**
  - huge documents: 1,000+ pages, large tables, many pictures (opening, typing, scrolling, memory);
  - a full corpus pass: open, save, check, and the Word open check, including the new formats;
  - a friction review of every flow (`docs/friction-review.md`, `tests/friction-checks.ps1`);
  - the torture test extended to charts, equations and right-to-left text.

## Rules

- **Never damage a file:**
  - `fwtool check` and the Open XML SDK validator stay clean;
  - the torture test stays clean;
  - parts we don't edit (charts, equations, `.doc`-origin data) are kept byte for byte where possible.
- **Imported formats** (`.doc`, `.odt`) are never overwritten. Save goes to Save As `.docx`.
- **Lightweight:** each piece measures its size with `scripts/budgets.ps1` and records it in the ledger with a reason. No new third-party libraries without the coordinator's OK.
- **Word automation:**
  - always a new, separate Word instance (DispatchEx), in safe mode;
  - end only the Word you started, and always end it;
  - Roy may be using Word at the same time.
- **Browsers:** Playwright's own Chromium, headless only. Never the installed Chrome or Edge (Roy's global CLAUDE.md).
- **Usage:** the weekly limit is close. Work economically: short fuzz runs (60 s) unless hunting something, and short reports.
- **Commits:** small, on your branch, with clear messages; no Co-Authored-By trailer. Push your branch; when a piece (or a usable part of it) is ready, message fwpkg to merge it, and the coordinator with one line.
