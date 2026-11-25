# CodeQL triage

CodeQL's code scanning (`.github/workflows/codeql.yml`) on the shipped exe, and what was decided for each alert
of high or critical security severity. The 4.1 gate requires none open (`docs/f1-plan.md`, "External review").
Alerts that are false positives or accepted are dismissed on GitHub with the reason below; real ones are fixed.

## First run (claude/f1-trust 98f91109, 2026-11-25): 17 alerts, all "high"

| Alert | Rule | Where | Decision |
|---|---|---|---|
| 16 | cpp/incorrect-string-type-conversion | src/CrashReport.cpp (the crash note read as UTF-16) | **Fixed** (f1-trust): the bytes are copied into the wide string, not cast |
| 1 | cpp/integer-multiplication-cast-to-long | src/doc/edit/Editor.cpp:5300 | **Real, to fix** (f1-word's area): `widths[k] * room` is an `int` product, so a document with huge column widths overflows it before the division. The fix multiplies in 64 bits (`int64_t{widths[k]} * room / now`); results are the same for every width that didn't overflow |
| 17 | cpp/incorrect-string-type-conversion | src/sheet/calc/Value.cpp:629 | **False positive**: `LCMapStringEx` with `LCMAP_SORTKEY` writes a byte array into its `LPWSTR` parameter, as its documentation says; the cast is the API's |
| 15 | cpp/weak-cryptographic-algorithm | src/pkg/OfficeCrypto.cpp:454 | **Accepted (won't fix)**: RC4 only decrypts old `.doc`, `.xls` and `.ppt` files that were encrypted with it, to open them. Nothing is encrypted with RC4: protected files are saved with AES-256 and SHA-512 |
| 14 | cpp/comparison-with-wider-type | src/draw/Pdf.cpp:1511 | **False positive**: the vector has `numGlyphs` entries, a 16-bit count, so the 16-bit counter reaches its size |
| 12, 13 | cpp/comparison-with-wider-type | src/doc/model/Schema.cpp:386, 453 | **False positive**: the schema's type table is built from the program's own tables, far below `TypeId`'s range |
| 2–10 | cpp/integer-multiplication-cast-to-long | src/draw/Pdf.cpp:731, 734, 1536–1539 | **False positive**: products of `float`s (dash lengths times pen width, font units times 1000/unitsPerEm) converted to `double` for writing; they can't come near `float`'s range (3.4e38) |
| 11 | cpp/integer-multiplication-cast-to-long | src/sheet/view/PrintLayout.cpp:21 | **False positive**: a row's print position in points times a scale; far below `float`'s range |
