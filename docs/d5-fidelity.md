# D5 fidelity: measurements

The fidelity piece of D5 (`docs/d5-plan.md`). Measured with `fwtool compare` against Word's renders in the private corpus (108 Word documents with a reference PDF, made by Word on Roy's PC).

## Handover (6 Nov 2026, paused for the weekly usage limit)

### Where it stands

Measured on `claude/d5-int` 79b6f6c with 52ae90d:
- **Targets:** 143 of 152 page counts (94.1 %); 110,431 of 114,076 lines (96.8 %).
- **Copies:** 196 of 258 (76.0 %); 93.5 % of lines.

The 97 % page target needs 148.

Measure with `fwtool compare --json F <FW_CORPUS>\reference`. Since 52ae90d, a missing or unreadable reference PDF fails only its own document. Before, it failed every document after it.

### The 9 misses, the next causes worst first

1. **worldbank-sustainable** (Word 38, ours 37). Owner: d1-measure. Fixed by the held group below, which can't land yet.
2. **govuk revised-methodology** (109 / 119). Owner: d1-model.
   - 130 paragraph marks are tracked deletions. Word's final view joins each such paragraph to the next.
   - We drop only the empty ones.
3. **un-financial report** (348 / 353). Owner: d1-text.
   - Its lines are 98.4 % right. The extra pages build up from line breaks: about one more line every few pages (for example, page 9's "The Secretary-General is responsible…").
   - The held split-row rule gains it 44 lines.
4. **gojp-mhlw-form b1** (23 / 18) and **b4** (20 / 13). Owner: d1-text.
   - The document grid.
   - Their reference PDFs were missing from about 14:00 on 6 Nov, apparently being rendered again.
5. **who-lqsi Arabic front page** (1 / 2). Owner: d1-text.
6. **acuk-example-cv** (1 / 2). Owner: d1-measure.
   - Our lines are about 0.012 pt taller each, on every kind of line, and the last line misses page 1 by 0.08 pt.
   - Rounding metrics to device units didn't help. No rule found yet.
7. **EPA brownfields QAPP** (171 / 160). Owner: d1-measure.
   - The reference is odd: Word's own count is 160.
   - Rows are 0.5 pt taller in Word (see the border group).
   - Worksheet 15.2 then puts one row more on page 61 with us, and the three-column list of abbreviations after it runs onto an extra page.
8. **bund-abnahmeprotokoll** (5 / 4): a reference-locale exception. It stays a miss (see below).

### Held rule groups

Each group is right where it was checked, but doesn't gain pages yet. The code is small: `docs/d5-held-rules.patch` applies to d5-int 79b6f6c. It puts each rule behind an environment switch, and also has the rejected `FW_SECTDROP` (below). Measure a group with its switches set, then land it without them.

- **A. A split row needs room for each cell's space after** (`FW_CUTAFTER`, in Flow.cpp's row cut).
  - A cell's line counts as fitting only with its paragraph's space after, when it's that paragraph's last line.
  - Gains 71 lines (un-financial +44, worldbank-sustainable +27). The short-form contract's "12. Charges" row then moves on whole, as in Word.
  - Costs worldbank-guidance its page match. We wrap "Europe, etc.).  All supporting documents must be included in the bid." before "bid.", where Word keeps it on one line. That line is d1-text's.
    - The cause: it's the last line of a justified paragraph, in Times New Roman 12, ending "bid. " with a trailing space. We measure it at 331.6 pt (two spaces after "etc.).") against 324.6 pt of room. Word fits it, so Word squeezes its spaces about 15 %.
    - Both of `Squeezes`' limits allow that: a quarter of the spaces' width, and half the stretch.
    - But `Squeeze` (LineLayout.cpp) leaves out a paragraph's last line ("text must follow").
    - Letting a last line squeeze when it ends in a trailing space fits the line. The breaker then ends it as a wrap and adds an empty line, so the line's end needs handling too.
  - **Land A once that line is fixed.**
- **B. Borders as room in table rows** (`FW_TBLS`, in Table.cpp).
  - Every resolved horizontal border takes room: the stronger of a cell's top border and the bottom border of the cell above, and the last row's bottom border. This means `c.props.top` / `bottom`, which merge the table, its style and `tcBorders`; today only `tcBorders` counts.
  - Rows then sit exactly where Word's do (short-form contract, worldbank-sustainable, EPA's acronym table).
  - It needs A, or the contract loses 76 lines and its page match.
  - With A, it costs EPA 90 lines (cause 7).
- **C. A break-only paragraph after a table must end within the text area** (`FW_BRKFIT`, in Flow.cpp's fit check).
  - Today a line with nothing but a page break may overflow the page. With C, right after a table, it may run into the footnote area but not past the text area's bottom.
  - Matches Word in worldbank-sustainable, wipo-form-mm2, hamburg and govuk-draft-investigations. Doesn't match in gojp-b3, whose lines snap to the document grid.
  - Without the text-area condition, it also loses wipo-form-mm2.
- **A + B + C on d5-int:** worldbank-sustainable matches (+1); worldbank-guidance (−1) and gojp-b3 (−1) are lost; EPA −90 lines. Net −1 page.
  - With the "bid." line fixed, net 0.
  - Then, if C leaves out pages with a document grid (an untested guess), +1.

### Tested and rejected

- **Dropping the space before at the top of every page after a section break:** changes only EPA, −30 lines. Word keeps the space at some section starts in that document and drops it at others.
- **Sizing lines that hold only an inline drawing by the paragraph mark:** doesn't change the public `drawings-dml.docx` (Word 3 pages, ours 2). There the inline drawings themselves are too small (about 340 pt against Word's 670), which is d1-draw's.

## Progress

| Step | Page counts match | Lines break where Word breaks them |
|---|---:|---:|
| Start: `claude/d5-int` 2c7f571 | 98 of 108 (90.7 %) | 106,609 of 111,064 (96.0 %) |
| Hyphenation as Word on this PC hyphenates | 98 (90.7 %) | 107,167 (96.5 %) |
| `numId 0` drops the style's list indents; no automatic spacing between items of one list | 98 (90.7 %) | 107,218 (96.5 %) |
| A page break keeps its paragraph mark in mode 14 too | 97 (89.8 %) | 107,216 (96.5 %) |
| Contextual spacing collapses before dropping the space after; space after must fit above footnotes | 98 (90.7 %) | 107,226 (96.5 %) |
| Merged with `claude/d5-int` e38b850 (equations, drawings in the dump, `.doc` import) | 98 (90.7 %) | 107,255 (96.6 %) |
| A paragraph that only holds a section break gets no list number (tudresden 35 → 33 pages, as in Word; QAPP 64 → 63, Word 62) | 99 (91.7 %) | 107,259 (96.6 %) |
| In mode 15, a list level that shows no number has no tab after it; a page break before doesn't keep the space before at the page top (short-form contract 77 → 76 pages, as in Word) | 100 (92.6 %) | 107,415 (96.7 %) |
| With `doNotUseHTMLParagraphAutoSpacing`, no automatic spacing; a line with only a column break needn't fit its column (region-3 QAPP 63 → 62 pages, as in Word) | 101 (93.5 %) | 107,477 (96.8 %) |
| A split table row goes on at each cell's next line, without the space between (worldbank-guidance 75 → 74 pages, as in Word) | 102 (94.4 %) | 107,477 (96.8 %) |
| Target | 105 (97 %) | 108,843 (98 %) |

From 766982d on, the targets (below) are the measure. With the last step, on `claude/d5-fidelity` alone: **142 of 152 (93.4 %), 110,073 of 114,076 lines (96.5 %)**; copies 170 of 258 (65.9 %), 210,767 of 239,892 lines (87.9 %). On `claude/d5-int` 79b6f6c, with the other sessions' work: 143 of 152, 110,431 lines (see the handover).

## What the targets count

The 97 % and 98 % targets count each document once:
- **Counted:** the original 108 `.docx` documents, plus the new distinct documents d1-corpus adds (the public chart, equation, right-to-left and East Asian files).
- **Copies:** format copies of a counted document are measured against Word's render of each copy, but reported apart and kept out of the targets.
  - These are the `.dotx`, `.doc`, `.odt` and password-protected copies.
  - The manifest marks each one with `"copyOf"`, the source of the document it copies. For an older manifest without it, compare uses the folder and name: a document under `dotx/`, `doc/`, `odt/` or `protected/` with the same category and name as one under `docx/`.
- **How compare opens them:** `.doc` and `.odt` copies go through the importers, and protected copies are decrypted with the manifest's test `"password"`, as the app opens them.
- **How compare says it:** `fwtool compare` prints the targets' line first and the copies' line after it. Its JSON has `"copy": true` on each copy and the copies' totals under `"copies"`. Its report puts the copies in a table of their own.

On 766982d, with d1-corpus's manifest of 411 entries:

| Set | Page counts match | Lines |
|---|---:|---:|
| **Targets**: 152 distinct documents | 139 (91.4 %) | 109,822 of 114,078 (96.3 %) |
| The original 108 | 99 | 107,259 of 111,064 (96.6 %) |
| The 44 new ones (charts, equations, right-to-left, East Asian) | 40 | 2,563 of 3,014 (85.0 %) |
| **Copies**: 258, one `.odt` has no render | 168 (65.1 %) | 210,648 of 239,892 (87.8 %) |
| `.doc` | 83 of 108 | 101,465 of 111,331 (91.1 %) |
| `.odt` | 53 of 107 | 68,595 of 86,034 (79.7 %) |
| `.dotx` | 25 of 31 | 35,946 of 37,530 (95.8 %) |
| Password-protected | 7 of 12 | 4,642 of 4,997 (92.9 %) |

The 4 new documents whose page count differs:
- gojp-mhlw-form-b1, b2 and b3: Word 23, 28 and 12 pages, ours 17, 21 and 8 (Japanese, the document grid);
- who-lqsi-controlled-document-front-page-arabic: Word 1 page, ours 2.

D1 ended at 98/108 and 95.9 %, not the 99/108 and 95.2 % in the plan. Those numbers were measured before D1's last layout commit (399a105, spaces squeezed only in mode 15). That commit gained 776 lines and lost eu-bijlage's page match, which had been a coincidence (1,070 of its 1,385 lines matched).

The page-break step makes eu-bijlage match. It also costs two page matches that were coincidences:
- eu-anexo: its own cause, from page 6 on;
- bund-abnahmeprotokoll: see the exceptions below.

## Reference-locale exceptions

These documents differ only because the reference Word is English. We show what Word in the document's own language shows. They still count as mismatches in the totals.

- **bund-abnahmeprotokoll** (5 pages in Word, 4 here). Its headers hold `STYLEREF "Überschrift 1;Überschrift-1"`.
  - English Word doesn't know the German style name, so it prints "Error! Use the Home tab to apply …" over three lines, and page 4's body moves down 26 pt.
  - German Word, and we, show the heading.

## Fonts

131 font names appear in the corpus (`w:rFonts` and theme fonts). Checked with `fwtool wrap`, which shows what fwtext finds.

- **Present (50):** Calibri, Times New Roman, Arial, Wingdings, Courier New, Symbol, Cambria, Tahoma, Calibri Light, Segoe UI, and the rest of the Windows and Office fonts the corpus uses (Liberation Sans and Serif, Cambria Math, MS Gothic, Verdana, SimSun, Century Gothic, Georgia, Garamond, Consolas, Noto Sans, and so on).
- **Office cloud fonts:** Aptos and Aptos Display (8 documents each), Open Sans (4), MS Mincho (4), Yu Mincho, Source Code Pro.
  - Word downloads these into `%LOCALAPPDATA%\Microsoft\FontCache\4\CloudFonts`.
  - DirectWrite's system collection doesn't see them, so we substitute Segoe UI or Times New Roman.
  - Adding that folder to our collection gained only 105 lines and lost one page match (edu-uw-letterhead, 1 page becomes 2). It's parked: the gain is small, and the fonts are licensed for Office.
- **Missing on this PC, so Word substitutes them too (81 names, mostly 1–2 documents each):**
  - names from PDF converters: TimesNewRomanPSMT, Arial-BoldMT, AdvOT…;
  - brand fonts: BundesSans and BundesSerif, Marianne, Uni Sans, Encode Sans, Frutiger, Proxima Nova;
  - Linux and Mac fonts: Droid Sans Fallback, Linux Libertine and Biolinum, Lucida Grande, Helvetica, which we map to Arial as Word does.

## Compatibility modes and `w:compat` flags

Lines that don't match, after the hyphenation step:

| Mode | Documents | Lines that match | Lines missing |
|---|---:|---:|---:|
| 15 | 95 | 96.5 % | 3,757 |
| 14 | 10 | 97.2 % | 126 |
| 11 | 2 | 93.0 % | 14 |
| none | 1 | 100 % | 0 |

Modes 11, 12 and 14 cost 140 lines in all, so the mode work goes to the flags that cost the most:

| Flag | Documents | Lines missing in them |
|---|---:|---:|
| `ulTrailSpace` | 12 | 1,381 (mostly the EPA brownfields QAPP) |
| `useFELayout` | 30 | 949 (probably incidental: Word sets it in many documents) |
| `suppressTopSpacing`, `suppressBottomSpacing` | 2 | 507 (the UN financial report) |
| `doNotUseHTMLParagraphAutoSpacing` | 4 | 86 |

## Findings

- **Hyphenation.** Word hyphenates a language only with Office's proofing tools for it. The Office on this PC has English, Spanish and French, and leaves German alone: 1 hyphenated line in hamburg where we had 59, and 24 in bund-bsi (all at real hyphens) where we had 202.
  - fwtext now checks Office's PROOF folder (`Hyphenator::WordHyphenates`). Without Office, every language we have patterns for is hyphenated, as before.
  - Word also keeps at least three letters before a Spanish hyphen. With that, un-convencion has no hyphenated lines, as in Word.
  - The hyphenation zone in mode 15 is still ignored, as d1-text measured. Using it explained the Spanish lines less well (+28 against +53).
- **Lists.**
  - A paragraph that leaves its style's list with `w:numId 0` loses the style's indents too (+40 lines).
  - Between two items of one list, `beforeAutospacing` and `afterAutospacing` give no space, as between HTML `<li>`s. This took revised-methodology from 127 pages to 119 (Word: 109).
- **An empty list number.** A level with `w:numFmt none` and an empty `w:lvlText` (the short-form contract's defined terms) gets no tab after it in mode 15, so the term starts at the cell's edge. Word's render of the `.doc` copy (mode 11) still has the tab, so the rule is for mode 15 only.
- **Page tops after `w:pageBreakBefore`.** Word drops the space before there, as after a page break character. Checked on the 14 target documents that use it: 14 page tops come closer to Word's (tudresden, both bund-bsi documents, the short-form contract), 2 move away (bund-bsi page 5).
- **A split table row** goes on, on the next page, at the top of each cell's next line. The space after above the cut and the space before below it stay behind, as at the top of a page (worldbank-guidance matches).
- **Open: borders as room in table rows.** Word's rows are taller by the border width, whether the border comes from the table, its style or the cell.
  - Counting all of them puts the rows of the short-form contract, worldbank-sustainable and EPA's acronym table exactly where Word's are.
  - It still loses 154 target lines and the contract's page match (EPA −90, the contract −76). Other differences were hiding behind the shorter rows:
    - the contract's split rows still part from Word's;
    - Word ends a split row about 1.3 pt higher above footnotes than we do (worldbank-sustainable page 6, where widow control then moves two lines).
  - Re-measured on `claude/d5-int` 79b6f6c (targets 143/152, 110,431 lines). The experiments are kept as a patch, not committed.
    - **A split row needs room for the space after of each cell's last paragraph on the page.** This fixes the contract's split rows (the whole "12. Charges" row moves on, as in Word) and gains 71 lines (un-financial +44, worldbank-sustainable +27). It costs worldbank-guidance its page: there we wrap "Europe, etc.).  All supporting documents must be included in the bid." before "bid.", where Word keeps it on one line (a justified line with a double space, for d1-text). With that line fixed, this rule should land.
    - **With the borders counted as well**, worldbank-sustainable's rows match Word's. Only EPA still loses (−90 lines): worksheet 15.2's table puts one row more on page 61 than Word, and the three-column list of abbreviations after it runs onto an extra page.
    - **worldbank-sustainable's last miss** is the paragraph after a table that holds only a page break. Word gives it a page of its own (page 13 is empty), where our rule lets a break-only line overflow. Requiring it to fit after a table makes worldbank-sustainable match. It loses wipo-form-mm2 and gojp-b3, where Word keeps such a paragraph on the full page, so the rule that separates the cases is still open.
    - Narrower, and closer: a break-only paragraph right after a table must end within the text area, though it may run into the footnote area. This matches Word in worldbank-sustainable, wipo-form-mm2, hamburg and govuk-draft-investigations, but not in gojp-b3 (document grid). Together with the borders and the split rule, the group gets worldbank-sustainable right and loses worldbank-guidance (the "bid." line) and gojp-b3, a net −1 for now (patch `experiments3.patch` in the session scratchpad).
- **Space before at the top of a page after a section break.** Dropping it always changes only EPA, and costs 30 lines (158 pages; the PDF has 171). Word drops it on EPA's "LIST OF ACRONYMS" page but needs it kept elsewhere in the same document.
- **Open: deleted paragraph marks.** In Word's final view, a paragraph whose mark is a tracked deletion joins the next paragraph. We only drop such paragraphs when they're empty. revised-methodology has 130 of them; doing this properly means laying two model paragraphs out as one.
- **Open: EPA brownfields QAPP (1,150 lines missing).** Its reference PDF has 171 pages, while Word itself counts 160. Two findings there are held back until they help the totals:
  - Table borders: Word's rows are taller by the border width (0.5 pt rows in the acronym table: 12.56 pt against our 12.07 pt). Counting table and style borders makes pages 6–9 match exactly. On its own it costs 98 lines, because the page before is one row short.
  - Space before after a section break: Word drops it at the top of some pages after a section break and keeps it on others with the same structure.
- **The region-3 QAPP's extra page** came from its attachment page (55), a continuous two-column section with a column break in it. Two causes, both needed to fix it:
  - With `w:doNotUseHTMLParagraphAutoSpacing`, Word ignores `w:afterAutospacing` and `w:beforeAutospacing` and uses the spacing set: 5 pt after, not 14, so column one ends 9 pt higher.
  - A line with nothing but a column break stays at the foot of its column even when the column is full, as a page break's line does. We had moved it to column two, so the text after the break started a new page.
  - Its rows on page 14 are still 0.5 pt shorter than Word's (see table borders below).
- **Open: worldbank-sustainable (38 pages, ours 37).** A table row splits differently across pages 11–12, so the page-break paragraph after the table fits on our page 12 but not on Word's.
- **Rounding of line heights.** Rounding font metrics to device units (96 to 1440 dpi) changes ±20 lines and doesn't help. Word's PDF positions sit on a 0.12 pt grid. acuk-example-cv's last two lines miss page 1 because of a drift of 0.7 pt that is still unexplained.
