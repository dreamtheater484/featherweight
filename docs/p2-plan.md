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

- **p2-corpus, 2026-11-22: PowerPoint in Remote Desktop.** p1-corpus's probe (the 18 public decks in Roy's active Remote Desktop session against the console's references: SSIM min 0.99992) settles it: PowerPoint's open checks and PDFs run in an active session, at the console or over Remote Desktop, never in a disconnected or locked one, and never while a PowerPoint of anyone else's is running. `scripts\powerpoint-open-check.ps1` stops (exit 3 or 4) and leaves the rest "unchecked" otherwise; queued runs wait for those conditions and the shared `FeatherweightNotes\quiet-pc.lock`.
- **p2-corpus: repair detection in PowerPoint** as Excel's check does it, without relying on the prompt: alerts off, a normal read-only open, then Open2007 with OpenAndRepair ("repaired" if only that opens it, "refused" if neither), a silent repair seen as other slides than the presentation part lists, and any dialog read and cancelled ("error"). `scripts\powerpoint-open-check-proof.ps1` proves it on broken decks.
- **p2-corpus: save without edits** is measured against PDFs PowerPoint makes in the same run from the unchanged copy and from its original (same PowerPoint, same day: date fields agree), not against the stored corpus references; every slide at SSIM ≥ 0.99 (`fwtool pdf-compare --min-ssim 0.99`). fwtool's own check is stricter: the save writes no part, and the parts and their order are the original's.
- **p2-corpus: one adapter for the edit API.** The torture test and the editing budgets both go through `DeckTarget` (`tools\fwtool\DeckTorture.h`): new, duplicate, delete, move, hide, layout, typing, notes, move, resize, rotate, delete, group, ungroup, order, format, undo, redo, save. It is written against p2-model's header once it lands; until then every edit is refused (and `budgets.ps1` skips the deck editing budgets with a note).
- **p2-corpus: the public decks in the gate** are the 27 in `tests\office\presentations`, `generated`, `edge` and `show` (the broken and malicious ones are for `fwtool check` and the reader, not for editing); then the 152 screened real-world decks.
- **p2-corpus: build from scratch** plays `tests\office\build\deck.steps` through the app's script ops (`scripts\build-deck.ps1`); the presentation tab's ops it needs are listed, with their owners, in `FeatherweightNotes\p2\p2-corpus-build-ops.md` (names proposed; the pieces may rename them).
- **p2-corpus: section lists.** `fwtool check` reports a slide in two sections, a slide in none (when the deck has sections) and two sections with one id as warnings until PowerPoint's verdict on each is recorded by the proof; then the ones PowerPoint repairs become errors. A section naming a missing slide is already an error.

**p2-model (2026-11-22):**
- Edits are part-level copy-on-write: published part DOMs never change; an edit clones the parts it touches and publishes them; undo steps keep the versions from before and swap them back. The package stays the file as opened (never changed); edits live in a part store over it (parts, relationship sets, content types), so a save writes a snapshot on a worker thread while editing goes on.
- After an edit the model reads again only what changed (the slides touched; everything after a master, layout, theme, size or notes-master change). Replaced objects live one edit longer, so pointers stay valid until the change listeners have run.
- The recovery journal records part states, not operations: replaying needs no knowledge of the operations, so a later exe replays an older journal, and each write holds only the parts changed since the last one.
- Deleting slides or shapes only removes references; the writer leaves out parts that were reachable when the file was opened and aren't now (undo needs nothing back; unused pictures and charts leave the file). Parts the file had unreachable stay.
- Deleting a slide others link to: the link's relationship goes and its r:id is emptied, as PowerPoint leaves it. Deleting shapes removes their animation effects (emptied time-node lists collapse up to p:timing), builds and connector ends.
- mc:AlternateContent (torture findings): the mc:Fallback is never dropped. A shape in an mc:Choice is edited where it is; the block keeps its wrapper (math needs it), its other Choices and its Fallback. What applies to the Fallback's shape too is applied there as well (the shape with the same cNvPr id): transform, fill, outline, effects and geometry between plain shapes (sp, pic, cxnSp), name, alt text and hidden, z order and deletes inside a group in the block; z order and deletes of the whole block move or remove it with every branch. Everything else (text, crop, links) leaves the Fallback as it is (for math it's usually a picture of the equation). Shapes can't be moved into or out of a group inside the block (the Fallback would lack them or have them twice). When the Fallback was chosen (no Choice understood), its content replaces the block, so the edit is what every app shows.
- Duplicating a slide copies its charts, SmartArt, embeddings, tags and notes, shares pictures, media and its layout, leaves comments out and renews p14:creationId. Notes on a deck without a notes master: one is made as PowerPoint's default, on a copy of the slide master's theme.
- The Editor lives in the tab's ModelDeck (p2-objects' Deck::Editor()); PresentationFile (FileSession) does the tab's saving, backup and recovery.

**p2-objects (2026-11-22):**
- Objects' headless code is its own library, `fwslideobjects` (`src/slide/objects`: frames and handles, smart guides, align/distribute/order, selection over a slide's object tree, the PresentationML of objects, chart parts with their embedded workbook), shared by the tab, fwtool and the torture script. The tab's part is `ObjectTool` (`src/slide/view/ObjectTool.*`), hooked into PresentationView with one-line calls.
- Every object edit goes through the deck's editor: `Deck::Editor()` (ModelDeck makes it on first use and owns it). Format changes (fill, outline, weight, dash, shadow) read the shape with `ShapeXml`, change only that part with `fwslideobjects`, and write it back with `SetShapeXml`, one step for the selection; moves, resizes and turns are one `SetTransforms` step.
- Dragging draws two bitmaps made at the drag's start (the slide without the dragged objects, and them alone) and moves, scales or turns the second each frame; the edit is made on release. Larger than 4096 px (zoomed far in): outlines only.
- Snapping: smart guides first (slide edges and centre, other objects' edges and centres at the same level, equal spacing), else a 6-point grid; Alt turns both off. Arrow keys nudge 6 points, Ctrl+arrow 1 point.
- Inserted objects follow PowerPoint 2021: shapes with the theme's style, lines as connectors, pictures at their size (scaled to fit, centred), tables 640 points wide with 29.2-point rows in Medium Style 2 - Accent 1, charts 640 x 360 with PowerPoint's starting data and a minimal embedded workbook (Sheet1). Editing chart data replaces the embedded workbook with a minimal one (its Excel formatting isn't kept).
- Table rows and columns added copy their neighbour's cell properties and first run's look, clear the text, and grow merges they land in; a16 row and column ids get new values.
- The clipboard has a private format (the objects' XML and the parts their relationships name, charts with their workbook); pasting outside Featherweight's objects takes pictures (files, PNG, bitmaps) and text (a new text box).
- Without a cell chosen (text editing tells it), table row and column commands work at the table's end.
