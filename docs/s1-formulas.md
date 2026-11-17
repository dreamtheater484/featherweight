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

205 workbooks, 117,231 formulas: **109,629 compared, 109,622 match (99.99 %).** Not compared:
7,602 volatile or depending on one; none kept as cached (every function the corpus uses is
implemented, INFO since dab4a5d); none without a cached value. The engine's own Excel cases
(`tests/unit/sheet_calc_cases.inc`, calculated on this PC): 616 of 616.

Earlier in the day this was 99.77 % with 244 mismatches: Apache POI's number-format tests (211:
fractions with text around the bar, elapsed-time codes) and German TEXT() letters (27). s1-numfmt
fixed those in its number formats (06f1305, be72038), as Windows Excel 2021 shows them.

## The 7 mismatches

File | Saved by | Mismatches | Why
---|---|---|---
libreoffice-named-ranges-global-xlsx-workbook-level-defined.xlsx | LibreOffice 3.7 | 6 of 14 | Not Excel's values: LibreOffice saved the file. Five cells it wrote as `=A4` hold its array formula's values (2..6; `=A4` is A4's value, 1), and `=Global5` names `global6`, which LibreOffice cached as #NAME? while Excel matches names case-insensitively (5, as here).
poi-conditionalformattingsamples-xlsx-conditional.xlsx | Excel 16 | 1 of 352 | `SUBTOTAL(9,Table69[Sales])` over a table with an icon filter: SUBTOTAL leaves out rows a filter hides, whatever its function number. The engine does (a0d4767); the model doesn't yet say which hidden rows a filter hid (asked of s1-model).

## Against this PC's Excel

s1-corpus's cells files (`<corpus>\reference\xlsx\...\*.cells.json`) hold, for every formula,
Excel's value as cached, after this PC's Excel opened the file, and after a full recalculation
(`CalculateFull`), in the region it recorded (en-NL):

    fwtool formulas --excel <corpus>\reference\xlsx --report <file.md> <corpus>\xlsx

This compares what the app does after opening (`WorkbookCalc::OpenStep`, as at idle) with Excel's
values once opened, and a full recalculation with Excel's. 65,137 formulas listed (205 workbooks):

- **Recalculated: 64,702 of 64,710 match (99.99 %).** The 8 others: 5 elapsed-time codes after a
  seconds code (below), 2 `_xlfn._LONGTEXT` (Excel 2021 doesn't know it and gives #NAME?; the
  Excel that saved the file did, and the engine gives the text), and the filtered SUBTOTAL above.
- **After opening: 64,364 of 64,710 (99.47 %).** All 346 others are formulas Excel recalculated
  on opening and the app didn't yet, for the rules below (6 workbooks: POI's three format tests,
  the two US time sheets, LibreOffice's named ranges).

**What Excel recalculates on opening** (Excel 2021, 16.0 build 20326, probed with edited copies of
corpus files): besides volatile functions, what depends on them, formulas without a cached value
and `fullCalcOnLoad`, *everything*, unless the file's `calcId` is at least 191028 (191027 and
none: everything; 191028, 191029, 191030: not) and its calc features
(`<xcalcf:calcFeatures>` in workbook.xml) include all of RD, Single, FV and CNMTM (LET_WF isn't
needed, an unknown extra is fine). A sheet's own `<sheetCalcPr fullCalcOnLoad="1"/>`: that
sheet's formulas. In manual mode, nothing, even with an old calcId or fullCalcOnLoad. The Mac
`ArchID`, `fileVersion`, `date1904` and a missing calcChain make no difference. 114 of the 205
workbooks are recalculated in full by this rule, mostly small ones (13,745 of 118,540 formulas).
The engine takes a sheet's fullCalcOnLoad (CalcSettings::fullCalcSheets, a0d4767); both rules wait
on the model keeping sheetCalcPr and calcFeatures (asked of s1-model).

**Elapsed codes after a seconds code** (s1-numfmt's): Windows Excel 2021 shows an elapsed code
that follows an `s` code in the same section as the s value with its fraction, made positive, plus
0.5, read as days, in the elapsed unit, rounded down: `s" secs and "[m]" mins"` for 3.14159 is
"53 secs and 77581 mins" ((53.376 + 0.5) × 1440), for 0 "0 secs and 720 mins"; `s:m" @ hour "[hh]`
for 3.14159 "53:23 @ hour 1293". The file's Mac Excel and Featherweight give the elapsed time
(4523 mins, 0 mins, 75).

**SUBTOTAL and filters** (probed): rows hidden inside an autoFilter range that has criteria
(a `filterColumn`) are filtered, and every SUBTOTAL leaves them out (9 as well as 109); with no
criteria they count as hidden by hand. AGGREGATE doesn't tell them apart: options 0 and 4 take
them, 5 leaves them out.
