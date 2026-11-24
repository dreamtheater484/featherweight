# P3: advanced presentations, and release 4.0

The work plan for P3 (IMPLEMENTATION_PLAN.md §8, the P3 row of the milestones table, and §8.2's "Advanced" list). Each session reads this first, then §8, §4.3, §4.7, §10 and §11 of the plan, `docs/p1-plan.md` and `docs/p2-plan.md` for what P1 and P2 built, and `docs/r1-plan.md` for the ribbon.

P3 finishes the presentation editor: presenter view, adding and changing transitions and animations, video and audio, comments, password-protected presentations, templates, exporting slides as pictures, and opening `.ppt` and `.odp`. With P2 and R1 it makes release **4.0**.

Coordinator, 2026-11-24: P3 starts now on `claude/r1-int` f08c8a10 (all of P1, P2 and R1), while P1, P2 and R1 wait only for Roy's checks and the quiet-PC timings. Roy (2026-11-22): continue with the P milestones, no questions unless really unavoidable. Later fixes to P1, P2 and R1 are merged into p3-int as they land. P3 adds its commands straight to the ribbon (decision 27).

## Exit criteria

- **Presenter view:** with two monitors, the show on one and the presenter view on the other: the current and next slide (or next animation), the notes (scrollable, zoomable), a timer with pause and reset, the clock, and a strip of slides to jump to. The show's keys work in both windows. With one monitor, Alt+F5 opens the presenter view in a window, as PowerPoint does. With the presenter view running, a 50-slide deck still presents at the display's refresh rate with no dropped frames during transitions (P1's frame bench, presenter view on).
- **Transitions and animations:** the Transitions tab (the common transitions of §8.2 in a gallery, Effect Options, Duration, advance on click or after a time, Apply To All, Preview) and the Animations tab (the common animations in a gallery, Effect Options, Add Animation, the Animation Pane with order, On Click, With Previous and After Previous, Duration, Delay, move earlier or later, Preview). Saved as PowerPoint writes them; a deck saved with them opens in PowerPoint without a repair prompt, and PowerPoint reads back the same effects, triggers, durations and delays (its object model: `SlideShowTransition`, `TimeLine.MainSequence`). Effects Featherweight can't edit stay as they were, also on slides whose other effects were edited.
- **Video and audio:** Insert > Video and Audio from a file (embedded as PowerPoint does, with a poster frame); they play in the show through Media Foundation into the shared Direct3D 11 device (§8.3), on click or automatically as stored, any format Windows can play. A 1080p H.264 video plays in the show without dropped frames on the typical PC. Linked media plays when the file is there; nothing is ever fetched from the internet (§4.7). Media Foundation loads only when media plays: launch time and memory after launch don't change.
- **Comments:** show, add, reply, edit, delete and resolve, in a comments pane with markers on the slide, as the documents' comments work; the Review tab's comment group; Ctrl+Alt+M. Both of PowerPoint's formats are read (modern comments with `ppt/authors.xml`, and legacy `commentAuthors.xml` comments) and kept; new comments are written in the format PowerPoint 2021 on the reference PC shows (p3-review decides by an open check and records it). A deck with comments added opens in PowerPoint without repair and shows them, threads included.
- **Password-protected presentations:** opening an encrypted `.pptx` (agile and standard encryption) through `src/pkg/OfficeCrypto` and the password card, as documents and workbooks do; saving keeps the password; File > Info > Protect Presentation > Encrypt with Password sets and removes it. PowerPoint opens Featherweight's encrypted file with the password. An encrypted `.ppt` (RC4 CryptoAPI) opens too (p3-ppt).
- **Templates:** `.potx` and `.potm` open as a new presentation; Save As writes `.potx`; File > New and the start screen offer the personal templates folder (`src/UserTemplates`), as for documents and workbooks.
- **Picture export:** File > Export > the current slide or all slides as PNG, at a chosen width (1920 px by default), drawn as the view draws them (SSIM ≥ 0.99 against the app's own PDF of the deck at the same size).
- **`.ppt` import:** `.ppt`, `.pps` and `.pot` (PowerPoint 97–2003) open read-only, with Save As `.pptx`, as `.doc` and `.xls` do. On the `.ppt` corpus, ≥ 85 % of slides at SSIM ≥ 0.95 against PowerPoint's PDF of the same `.ppt`. The saved `.pptx` opens in PowerPoint without repair. `fuzz_ppt` (60 s in CI) finds nothing.
- **`.odp` import:** `.odp` opens read-only, with Save As `.pptx`, as `.ods` and `.odt` do. On the `.odp` corpus, ≥ 80 % of slides at SSIM ≥ 0.95 against PowerPoint's PDF of the same `.odp`. The saved `.pptx` opens in PowerPoint without repair. `fuzz_odp` (60 s in CI) finds nothing.
- **Never damage a file:** P2's torture gate gains P3's operations (transitions, animations, comments, media, encryption round trip); every corpus deck after random edits and a save: no new validator errors, passes `fwtool check`, opens in PowerPoint without repair. Saving without edits stays byte-identical.
- **Friction review:** presenting with the presenter view, adding transitions and animations, commenting, protecting, exporting pictures and opening a `.ppt` take no more steps than in PowerPoint 2021; findings fixed or recorded in `docs/friction-review.md` (P3 part).
- **Lightweight (§11):** the exe grows by ≤ 1.0 MB in P3 (about 12.35 MB after R1; < 15 MB at 4.0). Launch time and memory after launch no worse than R1's. No CPU use when idle, also with a video on a slide in the editor. Each size step has a ledger line with its reason.
- **Checklist:** a P3 part in `docs/TEST_CHECKLIST.md`, which Roy passes.
- **Release 4.0:** after Roy has checked P2, R1 and P3 (P1 lands first, as 3.1).

## Reference PC

As P1 and P2 (`docs/p1-plan.md` and `docs/p2-plan.md`, "Reference PC"):
- PowerPoint 2021 on this PC, single instance. Check for POWERPNT.EXE first, never automate or close Roy's, quit only the instance your automation started, AutomationSecurity = 3 (macros forced off), alerts off.
- PowerPoint runs only in Roy's active session (at the console or over Remote Desktop), never in a disconnected or locked one, and only under the shared `D:\Documents\Codex\FeatherweightNotes\quiet-pc.lock`.
- **The `.ppt` and `.odp` corpora are generated, not downloaded:** PowerPoint saves the 152 screened decks in `D:\Documents\Codex\FeatherweightCorpus\pptx` as `.ppt` and as `.odp`, and makes the PDF references of those files, into `FeatherweightCorpus\ppt` and `FeatherweightCorpus\odp` (p3-corpus). Hand-made edge cases (RC4-encrypted `.ppt`, `.pps`, `.pot`, media, comments) are made the same way. No downloads without Roy's yes.
- Only one monitor may be attached: the presenter view's two-monitor layout is then tested through the show's monitor choice and a windowed presenter view; no virtual display drivers (that would change the system).
- Quiet-PC timings and the frame bench run once, at the end, through the coordinator, under the lock: one job on 4.0's final head (no separate P2, R1 and P3 runs), 3 runs per scenario instead of 5, no side-by-side runs of older exes whose numbers are already in the ledger. About 10–15 minutes in all.

## Pieces

Integration branch: `claude/p3-int`, made from `claude/r1-int` f08c8a10. Each session works on its own branch from p3-int, pushes, and says in its turn-end report what's ready to merge. **The coordinator integrates:** merges into `claude/p3-int`, builds Release, runs ctest, pushes, and checks CI. Sessions merge only `origin/claude/p3-int`.

| Piece | Session | Weight | Size budget | Command ids |
|---|---|---|---|---|
| Presenting: the presenter view (second monitor and windowed), video and audio (insert, poster frame, playback in the show through Media Foundation, the Video Format and Playback contextual tabs), the Slide Show tab's new items | p3-present | 20 % | 220 KB | 3800–3829 |
| Transitions and animations: edit operations for `p:transition` and `p:timing`, the Transitions and Animations tabs, the Animation Pane, Preview in the editor | p3-anim | 18 % | 180 KB | 3830–3879 |
| Comments (both formats, pane, markers, the Review tab's comment group) and password-protected presentations (open, save, set and remove) | p3-review | 12 % | 120 KB | 3880–3909 |
| Templates (`.potx` in and out, personal templates), picture export (PNG), and `.odp` import with Save As `.pptx` | p3-files | 15 % | 160 KB | 3910–3949 |
| `.ppt`, `.pps` and `.pot` import (records to the slide model, RC4 decryption), Save As `.pptx`, `fuzz_ppt` | p3-ppt | 15 % | 220 KB | 3950–3959 |
| The generated `.ppt` and `.odp` corpora and their references; P3 operations in the torture gate; the PowerPoint read-back check for transitions, animations and comments; `fuzz_odp`; budgets; the friction review; the P3 checklist part | p3-corpus | 15 % | 0 (tools only) | — |
| Final gate, release 4.0 | coordinator | 5 % | 0 | — |

The size budgets add up to 900 KB, with 100 KB in reserve. Each piece measures with the linker-map method (`$env:LINK='/MAP'` from PowerShell, then p1-show's `mapsplit.py`) and records a ledger line with its reason. Ids 3960–3999 are in reserve; in `src/Commands.h` each piece adds its ids in its own block, under a comment naming the piece.

### Order

1. **Edit operations first, pushed as soon as they compile:** p3-anim's transition and timing operations, p3-present's media insert, and p3-review's comment operations, each in its own file in `src/slide/edit/` (`TimingOps.cpp`, `MediaOps.cpp`, `CommentOps.cpp`) through p2-model's `Editor`, with a short API note in `FeatherweightNotes\p3\`. p3-corpus adds each to the torture gate as it lands.
2. Everything else in parallel.
3. p3-corpus generates the `.ppt` and `.odp` corpora early (they need Roy's active session); until then p3-ppt and p3-files work from hand-made files and the format specifications ([MS-PPT], ODF 1.2/1.3).

### Reuse (share, don't copy)

- **The edit layer and writer:** `src/slide/edit` (p2-model): operations, transactions, undo, the round-trip store, the reference registry (`src/pkg/References.h`; media and comment parts are registered there).
- **The slide show:** `src/slide/show` (p1-show): `SlideShow`, `Timing` (the animation interpreter P1 plays), the monitor choice, the frame bench and `quiet-bench.ps1`.
- **The ribbon:** `src/slide/view/PresentationRibbon.cpp` (r1-slide left Transitions and Animations for P3); the coverage baseline `tests/ribbon/slide.txt` is regenerated by whoever adds items, and the coordinator regenerates it after each merge round.
- **Comments:** the documents' comments pane and threads (`src/doc`, `src/word`) and the workbooks' (S3, `src/sheet`).
- **Encryption:** `src/pkg/OfficeCrypto` and `src/PasswordCard` (documents and workbooks); RC4 CryptoAPI as `src/sheet/xls/Crypt.cpp` and `src/doc/msdoc` do it.
- **Binary formats:** `src/pkg/CompoundFile`; the `.doc` importer (`src/doc/msdoc`) and the `.xls` importer (`src/sheet/xls`, including its drawing records: Office Art, which `.ppt` shares).
- **ODF:** `src/pkg/Odf` and the `.ods` and `.odt` importers (`src/sheet/ods`, `src/doc/odf`).
- **Templates:** `src/UserTemplates` (s4-ods) and P2's new-deck code (`src/slide/view/NewDeck*`).
- **Pictures:** P1's `SlideRender` and the PDF export's page drawing; WIC for PNG.

## Source layout

- `src/slide/edit/`: P3's operations, one file per piece.
- `src/slide/show/`: the presenter view and media playback (`PresenterView*`, `Media*`).
- `src/slide/view/`: the tabs, panes and dialogs, in new files per piece; shared files only for hooking in.
- `src/slide/ppt/` (new, in `fwslide`): the `.ppt` importer. `src/slide/odp/` (new): the `.odp` importer.
- Unit tests: `tests/unit/test_slide_<piece>.cpp`; fuzzers `tests/fuzz/fuzz_ppt.cpp`, `tests/fuzz/fuzz_odp.cpp`.

## Rules

As P1, P2 and R1 (their "Rules"), plus:

- **Through the edit API only.** No code outside `src/slide/edit` changes the model.
- **Never damage a file** is the first gate, not the last: the torture run is part of every piece's done.
- **Importers are read-only** and never crash: every input is untrusted (§4.7); limits as `src/pkg/Limits`; a fuzzer from the first push.
- **Lightweight:** a ledger line with a reason for every size step; no new third-party libraries without the coordinator's OK; Media Foundation and WIC are Windows components, loaded only when used.
- **Quiet PC:** while the coordinator runs quiet-PC jobs it sends "hold": finish the current edit without building, benchmarking or starting PowerPoint, push, and wait for "go".
- **Timings tolerate a PC in use (Roy, 2026-11-24):** mouse movement, clicks and typing in other programs never throw a run away. A timing job waits about a minute for no builds and CPU < 20 % and keeps the display on itself; only a build, a leftover Featherweight or a failed measurement spoils a run. Use medians over several runs instead of asking for an untouched PC. Benches that take the screen ignore input except Esc and a focus change.
- **Roy's Featherweight and PowerPoint:** never close an instance you didn't start; benchmarks and UI checks use their own instances.
- **Usage:** run to 100 % of the windows (Roy, 2026-11-23). Push after every working step (a WIP commit is fine) and keep a "Resume here" block at the top of your state note.
- **Notes:** `D:\Documents\Codex\FeatherweightNotes\p3\<piece>-state.md`, current at every turn end.
- **Commits:** small, on your branch, no Co-Authored-By trailer. Push and say what's ready to merge.
- **Questions:** Roy asked not to be asked unless it's unavoidable. Decide within this plan and record the decision below; ask the coordinator, not Roy.

## Decisions during P3

- **Coordinator, 2026-11-24: P3 starts before P1, P2 and R1 land.** They wait only for Roy's checks and the quiet-PC timings, and nothing in P3 changes what Roy checks there. P3 branches from r1-int so its commands go straight into the ribbon.
- **Coordinator, 2026-11-24: generated corpora for `.ppt` and `.odp`.** PowerPoint 2021's own `.ppt` and `.odp` writers make them from the screened `.pptx` corpus, so no download is needed and PowerPoint's PDF of each file is a fair reference. Older `.ppt` files from PowerPoint 97–2003 are rarer in this set; if the generated set proves too narrow, p3-corpus says so and the coordinator decides.
- **Coordinator, 2026-11-24: P3's pieces go to the sessions that built the closest code** (new sessions couldn't be started from Remote Control; S3 and S4 did the same): p1-show becomes p3-present (after it closes P1's frame bench), p2-model p3-anim, p2-text p3-review, s4-ods p3-files, s4-xls p3-ppt, p2-corpus p3-corpus. Each works on a new branch `claude/p3-<piece>` cut from `claude/p3-int`.
- **Coordinator, 2026-11-24: the size ledger after merges.** Both-sides merges had kept ledger lines twice and dropped others. `tests/budgets/ledger.json` now keeps each entry once, with the milestone lines from 23 Nov on in lineage order (S4, P1, P2, R1, then P3), so the last line is the branch's own; the coordinator's merge rounds resolve ledger conflicts that way (`FeatherweightNotes\coordinator\fix_ledger.py`). Pieces only append their own lines.
