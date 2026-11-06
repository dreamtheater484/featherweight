# D5 fidelity: measurements

The fidelity piece of D5 (`docs/d5-plan.md`). Measured with `fwtool compare` against Word's renders in the private corpus (108 Word documents with a reference PDF, made by Word on Roy's PC).

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

## What the targets count

The 97 % and 98 % targets count each document once:
- **Counted:** the original 108 `.docx` documents, plus the new distinct documents d1-corpus adds (the public chart, equation, right-to-left and East Asian files).
- **Copies:** format copies of a counted document are measured against Word's render of each copy, but reported apart and kept out of the targets. These are the `.dotx`, `.doc`, `.odt` and password-protected copies, which sit under `dotx/`, `doc/`, `odt/` and `protected/` with the same category and name as the `.docx` under `docx/`.
- **How compare says it:** `fwtool compare` prints the targets' line first and the copies' line after it. Its JSON has `"copy": true` on each copy and the copies' totals under `"copies"`. Its report puts the copies in a table of their own.

Copies on e38b850:

| Copies | Page counts match | Lines |
|---|---:|---:|
| `.doc`, opened through the `.doc` import | 82 of 108 | 101,461 of 111,331 (91.1 %) |
| `.dotx` | 25 of 31 | 35,946 of 37,530 (95.8 %) |
| Password-protected | 0 of 12 | not measured: the corpus doesn't record their passwords |
| All copies | 107 of 151 (70.9 %) | 137,407 of 153,858 (89.3 %) |

D1 ended at 98/108 and 95.9 %, not the 99/108 and 95.2 % in the plan. Those numbers were measured before D1's last layout commit (399a105, spaces squeezed only in mode 15). That commit gained 776 lines and lost eu-bijlage's page match, which had been a coincidence (1,070 of its 1,385 lines matched).

The page-break step makes eu-bijlage match. It also costs two page matches that were coincidences:
- eu-anexo: its own cause, from page 6 on;
- bund-abnahmeprotokoll: see the exceptions below.

## Reference-locale exceptions

These documents differ only because the reference Word is English. We show what Word in the document's own language shows. They still count as mismatches in the totals.

- **bund-abnahmeprotokoll** (5 pages in Word, 4 here). Its headers hold `STYLEREF "Überschrift 1;Überschrift-1"`.
  - English Word doesn't know the German style name, so it prints "Error! Use the Home tab to apply …" over three lines, and page 4's body moves down 26 pt.
  - German Word, and we, show the heading.
| Target | 105 (97 %) | 108,843 (98 %) |

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
- **Open: deleted paragraph marks.** In Word's final view, a paragraph whose mark is a tracked deletion joins the next paragraph. We only drop such paragraphs when they're empty. revised-methodology has 130 of them; doing this properly means laying two model paragraphs out as one.
- **Open: EPA brownfields QAPP (1,150 lines missing).** Its reference PDF has 171 pages, while Word itself counts 160. Two findings there are held back until they help the totals:
  - Table borders: Word's rows are taller by the border width (0.5 pt rows in the acronym table: 12.56 pt against our 12.07 pt). Counting table and style borders makes pages 6–9 match exactly. On its own it costs 98 lines, because the page before is one row short.
  - Space before after a section break: Word drops it at the top of some pages after a section break and keeps it on others with the same structure.
- **Open: usgov region-3 QAPP (62 pages, ours 63).**
  - On page 14, Word's schedule table rows are about 0.5 pt taller each, the border width; the drift is 5.6 pt by the end of the table.
  - That pushes Word's A6 heading to page 15. We fit 55 lines on that page, Word 43.
  - Counting table and style borders as room (re-measured on 766982d) changes nothing here and costs 78 target lines, mostly in EPA. So the extra page comes from later in the document; still to find.
- **Open: worldbank-sustainable (38 pages, ours 37).** A table row splits differently across pages 11–12, so the page-break paragraph after the table fits on our page 12 but not on Word's.
- **Rounding of line heights.** Rounding font metrics to device units (96 to 1440 dpi) changes ±20 lines and doesn't help. Word's PDF positions sit on a 0.12 pt grid. acuk-example-cv's last two lines miss page 1 because of a drift of 0.7 pt that is still unexplained.
