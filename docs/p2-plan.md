# P2: editing presentations

The work plan for P2 (IMPLEMENTATION_PLAN.md §8, the P2 row of the milestones table). Each session reads this first, then §8, §4.3, §10 and §11 of the plan, `docs/p1-plan.md` for what P1 built, and `docs/s2-plan.md` for how the spreadsheet viewer became an editor.

P2 makes the P1 viewer an editor: slides added, duplicated, deleted, moved and hidden; text typed and formatted, with autofit, find and replace, spelling and hyperlinks; pictures, shapes, text boxes, tables and charts inserted, moved, resized, rotated and arranged; themes, backgrounds, slide size, slide numbers, footer and date; notes; copying slides between decks; and saving `.pptx` without damaging anything, with crash recovery. P3 (presenter view, editing transitions and animations, video, comments, `.ppt`/`.odp`, encryption) and release 4.0 come after.

Roy (2026-11-22): continue with the P milestones, no questions unless really unavoidable. P2 starts now on P1's code (`claude/p1-int` 4aacca6) while P1's last measurement (the real-world corpus against PowerPoint) finishes, as S4 started alongside S3's gate. P1's fixes are merged into p2-int as they land.

## Exit criteria

- **Build from scratch:** a 10-slide deck can be built from a blank presentation with Featherweight alone (title slide, bullets, a picture, a table, a chart, shapes, a theme, notes, slide numbers) by a scripted run, and it opens in PowerPoint without a repair prompt and looks as Featherweight shows it (SSIM ≥ 0.95 per slide against PowerPoint's PDF of the saved file).
- **Never damage a file (the gate):** every corpus deck (the 18 public decks, the generated ones and the 152 real-world decks), after scripted random edits (including deleting, duplicating and moving slides, changing layouts, typing, moving and deleting shapes) and a save:
  - no new validator errors,
  - passes `fwtool check`,
  - opens in PowerPoint without a repair prompt,
  - the parts nothing edited are kept as they were (round-trip store).
  - Saving without edits writes a file PowerPoint opens unchanged, slide for slide (SSIM ≥ 0.99 against the original's PDF).
- **Editing budgets (§11):** a keystroke in a text box to screen < 16 ms typical, < 50 ms worst; a slide operation (new, duplicate, delete, move) on a 50-slide deck < 50 ms to screen; saving a typical deck runs on a worker thread; undo of any operation < 50 ms.
- **Lightweight:** the exe grows by ≤ 1.4 MB in P2 (about 11.0 MB after P1; < 15 MB at 4.0). Memory with a typical deck < 150 MB. Nothing runs when idle. Opening is no slower than at P1.
- **Fuzzed:** `fuzz_pptx` gains edits (as `fuzz_xlsx` did in S2): random operations, save, reread, check; 60 s in CI, no findings.
- **Checklist:** a P2 part in `docs/TEST_CHECKLIST.md`, which Roy passes with P1's.
- **Release:** P1 and P2 are checked together; the release follows Roy's checks (3.1 or a beta, decided then).

## Reference PC

As P1 (`docs/p1-plan.md`, "Reference PC"):
- PowerPoint 2021 on this PC, single instance. Check for POWERPNT.EXE first, never automate or close Roy's, quit only the instance your automation started, AutomationSecurity = 3 (macros forced off), alerts off.
- References and open-checks in PowerPoint run while Roy is signed in, at the console or in an active Remote Desktop session if p1-corpus's test (2026-11-22) shows Remote Desktop renders match the console's; never in a disconnected session.
- The corpus is the screened one in `D:\Documents\Codex\FeatherweightCorpus\pptx` (152 decks, Defender-scanned, allow-listed parts). No new downloads without Roy's yes; any new download meets the safety bar of P1's (Defender on with fresh signatures, the EICAR proof, a scan per file before and after the screen, an allow-list of parts).

## Pieces

Integration branch: `claude/p2-int`, made from `claude/p1-int` 4aacca6. Each session works on its own branch from p2-int, pushes, and says in its turn-end report what's ready to merge. **The coordinator integrates:** merges into `claude/p2-int`, builds, runs ctest, pushes. Sessions merge only `origin/claude/p2-int`.

| Piece | Session | Weight | Size budget | Command ids |
|---|---|---|---|---|
| Edit layer for `fwslide`: operations, transactions, undo, the reference registry for slides; the `.pptx` writer (round-trip store, nothing untouched rewritten); crash recovery; the slide-operation and copy-slides API | p2-model | 25 % | 300 KB | 3400–3419 |
| Slides in the UI: new slide with a layout (Ctrl+M), duplicate (Ctrl+D), delete, move by dragging thumbnails, hide, change layout, slide size; design (built-in themes, theme colors, backgrounds); slide numbers, footer and date; notes editing; copying slides between decks (source design or destination's) | p2-slides | 15 % | 250 KB | 3420–3459 |
| Text: editing in placeholders, text boxes, shapes and table cells with what `fwtext` offers (formatting, bullets, numbering), autofit computed after an edit, find and replace, spelling as you type, hyperlinks (insert, edit; they work in the show) | p2-text | 20 % | 300 KB | 3460–3499 |
| Objects: insert pictures, shapes, text boxes, tables and charts; move, resize and rotate with smart guides and snapping; align, distribute, group (Ctrl+G, Ctrl+Shift+G), bring forward and send back; fill, outline and shadow; crop pictures | p2-objects | 22 % | 450 KB | 3500–3549 |
| Torture gate (scripted random edits, validator, `fwtool check`, PowerPoint open check), save-without-edits check, build-from-scratch script, `fuzz_pptx` edits, editing budgets, the P2 checklist part | p2-corpus | 13 % | 0 (tools only) | — |
| Final gate | coordinator | 5 % | 0 | — |

The size budgets add up to 1.3 MB, with 100 KB in reserve; each piece measures with the linker-map method (p1-show's `mapsplit.py`) and records a ledger line with its reason.

### Order

1. **First, and early (everyone needs it): p2-model's edit API**, pushed as soon as it compiles: operations, transactions, undo, selection-independent; the slide operations; a note in `FeatherweightNotes\p2\p2-model-edit-api.md`. Until then the others build their UI and tools against P1's read-only model and the draft header.
2. Everything else in parallel, each piece through the edit API only (§4.3 rule 4: no code outside `src/slide/edit` changes the model).
3. The torture gate runs from the first save onward, not at the end: p2-corpus starts with the harness and the save-without-edits check.

### Reuse (share, don't copy)

- **The reference registry** in `src/pkg/References.h` (`DocFormat::PowerPoint`): slide lists, sections in the extension list, custom shows, links between slides, layouts, masters, notes, media.
- **Edit layers already built:** `src/doc/edit` (D2–D4: text operations, typing coalescing, undo, recovery journal) and `src/sheet/edit` (S2: transactions, round-trip store, worker-thread save). Text editing in shapes uses `src/doc`'s DrawingML text code where it fits, as P1's reader does.
- **Objects:** D4's shapes and text boxes in documents and S3's pictures, shapes and charts in workbooks (`src/draw`, `src/sheet/objects`, `src/chart`): handles, smart guides, rotation, crop, the fill/outline/shadow panes.
- **Spelling:** `src/doc/proof`. **Find and replace:** the documents' and workbooks' bars.
- **Rendering:** P1's `src/slide/render` (`SlideRender`), thumbnails at idle; edits invalidate only the slides and thumbnails they touch.
- **Saving and recovery:** the documents' and workbooks' backup-on-first-save, recovery folder (§13), DPAPI-encrypted journals, the banner when another program changes the file.

## Source layout

- `src/slide/edit/` (new, in `fwslide`): operations, transactions, the registry, the writer (`src/slide/write/` if it grows), recovery.
- `src/slide/view/` (P1's tab): the editing UI, panes and dialogs, split by piece into new files; shared files only for hooking in.
- Unit tests: `tests/unit/test_slide_<piece>.cpp`. One `add_subdirectory` or list line per piece; the coordinator resolves one-line conflicts by keeping both.

## Rules

As P1 and S2 (`docs/p1-plan.md` and `docs/s2-plan.md`, "Rules"), plus:

- **Through the edit API only.** No code outside `src/slide/edit` changes the model.
- **Never damage a file** is the first gate, not the last: the torture run is part of every piece's done.
- **Lightweight:** a ledger line with a reason for every size step; no new third-party libraries without the coordinator's OK.
- **Roy's Featherweight and PowerPoint:** never close an instance you didn't start; benchmarks and UI checks use their own instances.
- **Usage:** check `get_usage` before each step. Stop gracefully at **98 % of the 5-hour window** and at **98 % weekly**: no new step, commit and push (a WIP commit is fine), state note exact, end the turn. The coordinator resumes everyone after the reset.
- **Notes:** `D:\Documents\Codex\FeatherweightNotes\p2\<piece>-state.md`, current at every turn end.
- **Commits:** small, on your branch, no Co-Authored-By trailer. Push and say what's ready to merge.
- **Questions:** Roy asked not to be asked unless it's unavoidable. Decide within this plan and record the decision below; ask the coordinator, not Roy.

## Decisions during P2

**p2-objects (2026-11-22):**
- Objects' headless code is its own library, `fwslideobjects` (`src/slide/objects`: frames and handles, smart guides, align/distribute/order, selection over a slide's object tree, the PresentationML of objects, chart parts with their embedded workbook), shared by the tab, fwtool and the torture script. The tab's part is `ObjectTool` (`src/slide/view/ObjectTool.*`), hooked into PresentationView with one-line calls.
- Every object edit goes through the deck's editor: `Deck::Editor()` (ModelDeck makes it on first use and owns it). Format changes (fill, outline, weight, dash, shadow) read the shape with `ShapeXml`, change only that part with `fwslideobjects`, and write it back with `SetShapeXml`, one step for the selection; moves, resizes and turns are one `SetTransforms` step.
- Dragging draws two bitmaps made at the drag's start (the slide without the dragged objects, and them alone) and moves, scales or turns the second each frame; the edit is made on release. Larger than 4096 px (zoomed far in): outlines only.
- Snapping: smart guides first (slide edges and centre, other objects' edges and centres at the same level, equal spacing), else a 6-point grid; Alt turns both off. Arrow keys nudge 6 points, Ctrl+arrow 1 point.
- Inserted objects follow PowerPoint 2021: shapes with the theme's style, lines as connectors, pictures at their size (scaled to fit, centred), tables 640 points wide with 29.2-point rows in Medium Style 2 - Accent 1, charts 640 x 360 with PowerPoint's starting data and a minimal embedded workbook (Sheet1). Editing chart data replaces the embedded workbook with a minimal one (its Excel formatting isn't kept).
- Table rows and columns added copy their neighbour's cell properties and first run's look, clear the text, and grow merges they land in; a16 row and column ids get new values.
- The clipboard has a private format (the objects' XML and the parts their relationships name, charts with their workbook); pasting outside Featherweight's objects takes pictures (files, PNG, bitmaps) and text (a new text box).
- Without a cell chosen (text editing tells it), table row and column commands work at the table's end.
