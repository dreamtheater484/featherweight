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
Calculated in en-NL alone, 99.77 % match.

## Result (2026-11-17)

113,944 formulas: **106,344 compared, 106,338 match (99.99 %).** Not compared: 7,600 volatile or
depending on one; none kept as cached (every function the corpus uses is implemented); none
without a cached value. The engine's own Excel files (`tests/office/excel`, calculated on this
PC): 696 of 696.

Earlier in the day this was 99.77 % with 244 mismatches: Apache POI's number-format tests (211:
fractions with text around the bar, elapsed-time codes) and German TEXT() letters (27). s1-numfmt
fixed those in its number formats (06f1305, be72038), as Windows Excel 2021 shows them. One case
where Windows Excel 2021 and the file's Mac Excel 2008 differ: a larger unit after a smaller one
in a section with an elapsed code (`s:m" @ hour "[hh]` for 3.14159 is "53:23 @ hour 1293" in
Windows Excel 2021, "53:23 @ hour 75" in the file and here).

## The 6 mismatches

File | Saved by | Mismatches | Why
---|---|---|---
libreoffice-named-ranges-global-xlsx-workbook-level-defined.xlsx | LibreOffice 3.7 | 6 of 14 | Not Excel's values: LibreOffice saved the file. Five cells it wrote as `=A4` hold its array formula's values (2..6; `=A4` is A4's value, 1), and `=Global5` names `global6`, which LibreOffice cached as #NAME? while Excel matches names case-insensitively (5, as here).
