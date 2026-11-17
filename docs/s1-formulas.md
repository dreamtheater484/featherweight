# S1 formulas: the real-world corpus

The gate (docs/s1-plan.md, "Formulas"): of the corpus formulas that use only implemented,
non-volatile functions, at least 99.5 % reproduce Excel's cached values, and every mismatch is
documented. This page documents them; `docs/s1-formulas-report.md` lists every one, cell by cell.

## How it's measured

s1-corpus's 205 real-world workbooks (`D:\Documents\Codex\FeatherweightCorpus\xlsx`: budgets,
business, calculators, grades, invoices, schedules, statistics and feature files):

    fwtool formulas --locale en-NL --regions en-US,en-GB,de-DE --report docs/s1-formulas-report.md <corpus>\xlsx

Each workbook is read, every formula recalculated from scratch, and each result compared with the
value the file cached: numbers to 15 significant digits, text, booleans and errors exactly. Not
compared: formulas that Excel itself recalculates on opening (volatile functions such as TODAY,
NOW, OFFSET, INDIRECT and everything depending on them, whose cached values are stale by design),
and formulas without a cached value.

**Regions.** TEXT()'s format codes and text read as numbers follow the region of the Excel that
calculated the file (`TT.MM.JJ` is a date in German Excel, `£0.00` a number in British Excel), and
a file doesn't record that region. Workbooks are calculated in this PC's region (en-NL); one with
mismatches is also calculated in en-US, en-GB and de-DE, and counts in the region where most of
its formulas match. Seven did (the report's "Read in another region"): Apache POI's three format
test files and two US time sheets (en-US), a UK government form (en-GB) and a German university's
timesheet (de-DE).

## Result (2026-11-17)

205 workbooks, 117,231 formulas: **109,629 compared, 109,613 match (99.99 %).** Not compared:
7,602 volatile or depending on one; none kept as cached (every function the corpus uses is
implemented, INFO since dab4a5d); none without a cached value. The engine's own Excel cases
(`tests/unit/sheet_calc_cases.inc`, calculated on this PC): 615 of 615 compared.

Earlier in the day this was 99.77 % with 244 mismatches: Apache POI's number-format tests (211:
fractions with text around the bar, elapsed-time codes) and German TEXT() letters (27). s1-numfmt
fixed those in its number formats (06f1305, be72038), as Windows Excel 2021 shows them.

## The 16 mismatches

File | Saved by | Mismatches | Why
---|---|---|---
libreoffice-named-ranges-global-xlsx-workbook-level-defined.xlsx | LibreOffice 3.7 | 6 of 14 | Not Excel's values: LibreOffice saved the file. Five cells it wrote as `=A4` hold its array formula's values (2..6; `=A4` is A4's value, 1), and `=Global5` names `global6`, which LibreOffice cached as #NAME? while Excel matches names case-insensitively (5, as here).
poi-elapsedformattests-xlsx-elapsed-time-formats-h.xlsx | Mac Excel 2008 | 10 of 234 | Elapsed codes after another date code (`s" secs and "[m]" mins"`, `s:m" @ hour "[hh]`): Mac Excel 2008 cached the elapsed time (4523 mins, hour 75); Featherweight shows what Windows Excel 2021 does (77581 mins, hour 1293, below), as this PC's Excel does once it opens the file.

POI's ConditionalFormattingSamples was a seventeenth until SUBTOTAL left out rows a filter hides
(below).

## Against this PC's Excel

s1-corpus's cells files (`<corpus>\reference\xlsx\...\*.cells.json`) hold, for every formula,
Excel's value as cached, after this PC's Excel opened the file, and after a full recalculation
(`CalculateFull`), in the region it recorded (en-NL):

    fwtool formulas --excel <corpus>\reference\xlsx --report <file.md> <corpus>\xlsx

This compares what the app does after opening (`WorkbookCalc::OpenStep`, as at idle) with Excel's
values once opened, and a full recalculation with Excel's. 65,137 formulas listed (205 workbooks):

- **After opening: all 64,710 match.** Before the opening rules below were in, 64,364 (99.47 %):
  Excel had recalculated 341 more (POI's three format tests, the two US time sheets,
  LibreOffice's named ranges); before s1-numfmt's bd96de9, 5 elapsed codes differed.
- **Recalculated: 64,708 of 64,710 (99.99 %).** The 2 others are `_xlfn._LONGTEXT`: Excel 2021
  doesn't know it and gives #NAME?; the Excel that saved the file did, and the engine gives the
  text.

**What Excel recalculates on opening** (Excel 2021, 16.0 build 20326, probed with edited copies of
corpus files): besides volatile functions, what depends on them, formulas without a cached value
and `fullCalcOnLoad`, *everything*, unless the file's `calcId` is at least 191028 (191027 and
none: everything; 191028, 191029, 191030: not) and its calc features
(`<xcalcf:calcFeatures>` in workbook.xml) include all of RD, Single, FV and CNMTM (LET_WF isn't
needed, an unknown extra is fine). A sheet's own `<sheetCalcPr fullCalcOnLoad="1"/>`: that
sheet's formulas. In manual mode, nothing, even with an old calcId or fullCalcOnLoad. The Mac
`ArchID`, `fileVersion`, `date1904` and a missing calcChain make no difference. 114 of the 205
workbooks are recalculated in full by this rule, mostly small ones (13,745 of 118,540 formulas).
`WorkbookCalc::Settings()` applies it, with s1-model's sheetCalcPr and calcFeatures (3001dc3).

**Elapsed codes after another date code** (s1-numfmt's bd96de9, 255 of 256 values it measured):
Windows Excel 2021 shows an elapsed code that follows another date code in the same section as
that code's field before rounding, plus 0.5, modulo its range, read as days, in the elapsed unit,
rounded down: `s" secs and "[m]" mins"` for 3.14159 is "53 secs and 77581 mins"
((53.376 + 0.5) × 1440), for 0 "0 secs and 720 mins"; `s:m" @ hour "[hh]` for 3.14159 "53:23 @
hour 1293". Mac Excel 2008, which saved POI's file, shows the elapsed time (4523 mins, 0 mins,
75); Featherweight follows Windows Excel.

**SUBTOTAL and filters** (probed): rows hidden inside an autoFilter range that has criteria
(a `filterColumn`) are filtered, and every SUBTOTAL leaves them out (9 as well as 109); with no
criteria they count as hidden by hand. AGGREGATE doesn't tell them apart: options 0 and 4 take
them, 5 leaves them out. `WorkbookCalc::RowFiltered`, with s1-model's Table::filtered and
Sheet::Filter() (f0c456a).
