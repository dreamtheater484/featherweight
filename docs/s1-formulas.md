# S1 formulas: the real-world corpus

The gate (docs/s1-plan.md, "Formulas"): of the corpus formulas that use only implemented,
non-volatile functions, at least 99.5 % reproduce Excel's cached values, and every mismatch is
documented. This page documents them; `docs/s1-formulas-report.md` lists every one, cell by cell.

## How it's measured

s1-corpus's 200 real-world workbooks (`D:\Documents\Codex\FeatherweightCorpus\xlsx`: budgets,
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
its formulas match. Five did (the report's "Read in another region"): Apache POI's three format
test files (en-US), a UK government form (en-GB) and a German university's timesheet (de-DE).
Calculated in en-NL alone, 99.58 % match.

## Result (2026-11-17)

113,944 formulas: **106,344 compared, 106,100 match (99.77 %).** Not compared: 7,600 volatile or
depending on one; none kept as cached (every function the corpus uses is implemented); none
without a cached value. The engine's own Excel files (`tests/office/excel`, calculated on this
PC): 696 of 696.

## The 244 mismatches

File | Saved by | Mismatches | Why | Whose
---|---|---|---|---
poi-numberformattests-xlsx-number-formats.xlsx | Mac Excel | 200 of 325 | Fraction codes with literal text between the digit placeholders (`#\:#=/=#`, `#_#/#`, `?\:? ?0#/000`): Excel splits the digits between the whole number, numerator and denominator differently (23.75 as `\|#\:#=/=#\|` is `\|23:3=/=4\|` in Excel, `\|:2=/=4\|` here). Apache POI's torture tests for its own formatter. | number formats (s1-numfmt)
tudarmstadt-stundentool-wise-26-27-stundenzettel-hilfskrafte.xlsx | Excel 16 (German) | 27 of 3,284 | `TEXT(B29,"TT.MM.JJ")`: German Excel types T for day and J for year; the number formats know only the Dutch letters so far, so `TT` stays literal ("bis TT.10.26", Excel "bis 04.10.26"). | number formats (s1-numfmt)
poi-elapsedformattests-xlsx-elapsed-time-formats-h.xlsx | Mac Excel 2008 | 11 of 234 | Elapsed-time codes: `[h]` or `[s]` a second time in one section shows the hour or second of the day here, the elapsed total in Excel (6); `m` right after `ss` is a month here, minutes in Excel (5). | number formats (s1-numfmt)
libreoffice-named-ranges-global-xlsx-workbook-level-defined.xlsx | LibreOffice 3.7 | 6 of 14 | Not Excel's values: LibreOffice saved it. Five cells it wrote as `=A4` hold its array formula's values (2..6; `=A4` is A4's value, 1), and `=Global5` names `global6`, which LibreOffice cached as #NAME? while Excel matches names case-insensitively (5, as here). | none: the file's values aren't Excel's

Without the two POI test files, which test number-format corner cases rather than real-world use,
33 of 105,785 compared formulas differ (99.97 %).
