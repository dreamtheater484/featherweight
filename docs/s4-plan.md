# S4: breadth, and release 3.0

The work plan for S4 (IMPLEMENTATION_PLAN.md §7, the S4 row of the milestones table). Each session reads this first, then §7, §4.3, §10 and §11 of the plan, and `docs/s3-plan.md` for how S1–S3 were built.

S4 completes the spreadsheet app for release 3.0: `.xls` and `.ods` files, password-protected workbooks, templates, pivot tables (refresh, and simple ones created), multi-threaded recalculation and very large files, sparklines, a full corpus pass and a friction review.

Roy (2026-11-19, decision 25): S4 starts in parallel with S3's last steps (its gate and PR), on top of S3's final code; the manual checks for S1–S4 come after S4. So S4's gate is automated end to end, like S2's and S3's.

## Exit criteria

- **Files:**
  - `.xls` (Excel 97–2003, BIFF8) opens with cell text matching Excel's at S1's thresholds (≥ 99 % of non-empty cells, ≥ 90 % of printed pages at SSIM ≥ 0.95) on a corpus made by having Excel save the real-world `.xlsx` corpus as `.xls`; Save As `.xlsx` from it opens in Excel without a repair.
  - `.ods` opens the same way, on a corpus made by having Excel save the corpus as `.ods`.
  - Password-protected (encrypted) `.xlsx` and `.xls` open after the password, through the same password card documents use; saving keeps the encryption.
  - Templates: `.xltx`/`.xltm` open as a new workbook (S1 does), and File → New offers the user's own templates folder as Word does (D5).
- **Pivot tables** (decision 18): every pivot in the corpus refreshes to the values Excel shows after its own refresh; simple ones (rows, columns, filters; sum, count, average) are created from a range, saved, and Excel opens and refreshes them without a repair to the same values. Features beyond those stay kept, not editable (§7.2).
- **Scale:** multi-threaded recalculation, at least 3× faster than single-threaded on 4 cores for the 100,000-formula workbook's full recalculation, with identical results; very large files (the 1-million-cell workbook, a 100 MB CSV, a 50 MB `.xlsx`) open and edit within §11's budgets.
- **Sparklines** shown as Excel shows them (line, column, win/loss; markers, axis, colors).
- **Never damage a file:** S3's torture gate over the whole corpus (the 246 workbooks plus the `.xls` and `.ods` sets), 0 failures, 0 new validator errors, 0 Excel repairs; fuzzers for the new readers (`fuzz_xls`, `fuzz_ods`, the pivot cache) 60 s clean, and `fuzz_xlsx` with pivots.
- **Friction review** (§11) done for spreadsheets; findings fixed or recorded.
- **Budgets** (§11) met on the typical consumer PC; the exe < 11 MB (3.0's budget); `scripts/budgets.ps1` passes.
- **Checklist:** the S4 part of `docs/TEST_CHECKLIST.md`; Roy passes S1–S4 after the gate. Then release 3.0.

## Reference PC

As S1–S3: Excel 2021, en-NL. Excel automation in a new, separate instance (`DispatchEx`), alerts, events, macros and link updates off; end only the Excel you started. The `.xls` and `.ods` corpora are made by Excel from the existing corpus (SaveAs), so no downloads are needed; more files only with Roy's yes.

## Pieces

Integration branch: `claude/s4-int`, made from `claude/s3-int` 9bda891 (S3's final code). S3's PR fixes, if CI asks for any, are merged into s4-int as they land. Each session works on its own branch from s4-int, pushes, and says in its turn-end report what's ready to merge; the coordinator merges, builds, runs ctest and pushes. Sessions merge only `origin/claude/s4-int`.

| Piece | Session | Weight | Size budget |
|---|---|---|---|
| `.xls` (BIFF8) reader into the workbook model | s4-xls | 25 % | 400 KB |
| `.ods` reader, encrypted workbooks, templates | s4-ods | 15 % | 200 KB |
| Pivot tables: refresh, create simple ones, write as Excel | s4-pivot | 22 % | 300 KB |
| Multi-threaded recalculation, very large files, sparklines | s4-scale | 18 % | 150 KB |
| `.xls`/`.ods` corpora, torture over all, friction review, budgets, checklist | s4-corpus | 15 % | 0 (tools only) |
| Final gate, release 3.0 | coordinator | 5 % | 0 |

The size budgets add up to 1.05 MB (S3 ended about 1.28 MB over S2's 7.91 MB; 3.0's ceiling is 11 MB).

### Reuse (share, don't copy)

- `src/pkg/CompoundFile` (D5: the compound file reader for `.doc`) for `.xls`; `src/pkg/OfficeCrypto` (D5: decryption) for encrypted workbooks; `src/PasswordCard` for the password prompt; `src/doc/odf` (D5: `.odt`) for the ODF package and styles.
- The workbook model, edit layer and writer from S1–S3: readers fill the same model, so editing, saving (as `.xlsx`) and the grid need nothing new.

### Shared files

- Command ids: s4-xls 3000–3019, s4-ods 3020–3039, s4-pivot 3040–3079, s4-scale 3080–3099.
- Journal ops: s4-pivot from 110, s4-scale from 130 (others from 140).
- Each piece puts its code in new files and touches shared ones only to hook in.

## Rules

As S3 (`docs/s3-plan.md`, "Rules"): through the edit API only; never damage a file; nothing active; lightweight (ledger entry with a reason); Excel automation rules; Roy's Featherweight never closed; Playwright's own headless Chromium only.

- **Usage:** check `get_usage` before each step. Stop gracefully at **98 % of the 5-hour window** (Roy, 2026-11-19; it was 92 %) and at **98 % weekly**: no new step, commit and push (a WIP commit is fine), state note exact, end the turn. The coordinator resumes everyone after the reset.
- **Notes:** `D:\Documents\Codex\FeatherweightNotes\s4\<piece>-state.md`, current at every turn end.
- **Commits:** small, on your branch, no Co-Authored-By trailer. Push and say what's ready to merge.

## Decisions during S4

- **2026-11-19, the friction review** (`docs/friction-review.md`, "Workbooks (S4)", by s4-corpus) has these owners:
  - W1 (the toolbar at 1366 px), W2 (command search lists S2–S4's workbook commands), W4 (the filter value box gets the keyboard, Enter applies) and W6 (table style previews): the S3 tables session, on its own branch from s4-int, with command ids 3100–3119.
  - W5 (AutoSum over a selection, as Excel does) and W7 (charts, tables and rules allowed in a workbook opened from `.csv`; saving to `.csv` asks once, keep as `.xlsx` or save the data only): the S3 formulas session, with command ids 3120–3129.
  - W3 (Page setup reachable from the sheet): the S3 page setup session, with its header carriage-return fix.
  - W13 (File → New: a blank workbook and the templates folder): s4-ods.
  - W8 (Filter by mouse, one click more than Excel) is accepted as it is. W9 and W10 are by design.
- **2026-11-19, the spreadsheet editor ships in 3.0:** S4 turns its "shipped" flag on, so File → New offers workbooks (W13).
- **2026-11-19:** the 5-hour usage stop is 98 % (Roy).
- **2026-11-19, .ods page setup:** the .ods reader also brings each sheet's page setup across (paper, orientation, margins, scaling, print ranges and titles, headers and footers, breaks), because the printed-pages target needs it. It may go about 20 KB over s4-ods's 200 KB budget, with the reason in the ledger.
- **2026-11-19, the printed-pages criterion:** S1's 96.2 % (230 of 239 pages) was measured on the 17 public workbooks in `tests/office/excel`, not on the real-world corpus; real-world pages were first measured in S4 (`scripts/sheet-pages.ps1`). So S4's pages criterion is: the public set stays at ≥ 90 % (S1's measure), and the `.xls` and `.ods` copies pass at least the rate of their own `.xlsx` originals on the same workbooks. The real-world `.xlsx` rate is recorded as a baseline for later milestones to raise.
- **2026-11-19, the reference renders:** 932 of 991 rendered. 39 `.xls` and 17 `.ods` failed while Roy's session was on Remote Desktop (Excel lays out on the redirected printer) and are rerun when he's next signed in at the console. The criteria are measured on the references there are (165 `.xls`, 152 `.ods` for cells; 162 and 147 for pages), which doesn't wait for the rerun.
