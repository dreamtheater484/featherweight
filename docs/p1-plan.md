# P1: the presentation viewer and slide show

The work plan for P1 (IMPLEMENTATION_PLAN.md §8, the P1 row of the milestones table). Each session reads this first, then §8, §4.3–§4.6, §10 and §11 of the plan, and `docs/s1-plan.md` for how the spreadsheet viewer was built. P1 is release **3.1**.

P1 opens `.pptx`, `.pptm`, `.potx`, `.potm` and `.ppsx` files read-only in a presentation tab. It shows slides with their inheritance, thumbnails and notes, and plays the slide show with the common transitions, animations and hyperlinks. It prints and exports PDF. Editing is P2.

Roy (2026-11-20): P1 starts now, while S4's PR finishes CI and before his manual checks of S1–S4. It builds on S4's final code (`claude/s4-int` ac6d0ff), and nothing in P1 waits on those checks.

## Exit criteria

- **Display:**
  - ≥ 90 % of corpus slides pass the pixel threshold against PowerPoint's PDF exports (SSIM ≥ 0.95 at 100 dpi, plan §10).
  - The 17-file public set rule from S4 applies here too. A public set of generated and permissively licensed decks in `tests/office/` is measured the same way and reported separately.
- **Slide show:**
  - On the typical consumer PC, a 50-slide deck presents at the display's refresh rate with no dropped frames during transitions.
  - The common transitions and animations play as §8.2 lists them; others play as a fade or appear.
  - Hyperlinks work during the show.
- **Speed and size (plan §11):**
  - A 50-slide `.pptx` reaches its first slide in < 500 ms.
  - A typical deck uses < 150 MB.
  - Nothing runs when idle.
  - The exe grows by ≤ 1.0 MB in P1 (10.05 MB after S4; < 15 MB at 4.0).
  - Opening a PDF, a document or a workbook is no slower than before P1.
- **Never damage a file:** P1 doesn't save decks. Opening never changes the file. `fwtool check` and the validator learn `.pptx`, and every corpus deck passes them, so P2 starts from a clean baseline.
- **Fuzzed:** `fuzz_pptx` (the reader and inheritance) and the animation timing parser, 60 s in CI, no findings.
- **Checklist:** a presentation part in `docs/TEST_CHECKLIST.md`, which Roy passes with S1–S4's.
- **Release:** 3.1, after Roy's checks.

## Reference PC

- PowerPoint 2021 (Office16) is on this PC, Windows' regional format is en-NL, as for S1–S4.
- **PowerPoint runs as a single instance.** Unlike Excel, `DispatchEx` attaches to a PowerPoint that is already running.
  - Before automating, check whether POWERPNT.EXE is running.
  - If Roy has PowerPoint open, don't automate it. Wait, and never close his window or his decks.
  - Only quit a PowerPoint that your own automation started, and set alerts off.
- **Render PowerPoint references only from an active session: the console or Remote Desktop** (decision of 2026-11-22 below), never a disconnected one. If the session is disconnected (`qwinsta`), queue the renders and say so. Excel keeps its own check: it lays out on the redirected printer over Remote Desktop (S4: 56 renders failed).
- Corpus downloads need Roy's yes. Ask in the session before the first download, listing the sources, the number of files and the total size, as in S1. Each download goes into its own quarantine folder and is screened before anything opens it.

## Pieces

Integration branch: `claude/p1-int`, made from `claude/s4-int` ac6d0ff (S4's final code). S4's PR fixes, if CI asks for more, are merged into p1-int as they land. Each session works on its own branch from p1-int, pushes, and says in its turn-end report what's ready to merge. The coordinator merges, builds, runs ctest and pushes. Sessions merge only `origin/claude/p1-int`.

| Piece | Session | Weight | Size budget |
|---|---|---|---|
| Presentation model and `.pptx` reader: slides, layouts, masters, themes, inheritance resolved and cached; `.ppsx`/`.potx`/`.pptm`; Embedded OpenType fonts | p1-model | 22 % | 250 KB |
| Slide rendering: shapes, text with autofit, pictures, tables, charts, groups, SmartArt's stored drawing, backgrounds; thumbnails at idle | p1-render | 25 % | 300 KB |
| The presentation tab: thumbnails pane, slide view, notes pane, zoom, find, copy; print (slides, handouts, notes) and Export PDF | p1-view | 15 % | 150 KB |
| Slide show: full screen, next slide pre-rendered, transitions, the animation interpreter, hyperlinks, input; `.ppsx` straight into the show | p1-show | 20 % | 250 KB |
| Corpus, PowerPoint reference renders, `fwtool compare` for slides, `fuzz_pptx`, the validator for `.pptx`, budgets, checklist | p1-corpus | 13 % | 0 (tools only) |
| Final gate, release 3.1 | coordinator | 5 % | 0 |

The size budgets add up to 950 KB.

### Order and dependencies

1. p1-model goes first. Its model API (`src/slide/`, a new `fwslide` library) is what the others build on.
   - Within the first hours, it pushes a header with the model's types and a reader that opens the three generated decks in `tests/office/generated/`.
   - Until then, p1-render and p1-show build against that header's draft and the generated decks.
   - p1-view starts on the tab's frame (panes, thumbnails list, zoom) with placeholder rendering.
2. p1-corpus starts at once: the corpus (ask Roy first), the reference renders and the slide compare tool.
3. p1-render draws through `src/draw` (DrawingML shapes, the preset geometry, pictures, D2D), `src/text` (text, fonts, line layout) and `src/chart`. Everything Word's and Excel's drawings use is shared, not copied.
4. p1-show uses the shared Direct3D 11/D2D device (`src/draw/D2DDevice`) and the PDF reader's presentation mode (input, cursor hiding). Media Foundation (video) is P3, not P1.

### Reuse (share, don't copy)

- `src/pkg` (OPC packages, encryption later), `src/draw` (DrawingML, preset shapes, pictures, Direct2D, PDF output, print jobs), `src/text` (fonts, shaping, line layout; Embedded OpenType goes in `src/draw/Sfnt` or `src/text/Fonts`), `src/chart` (charts, as S3 draws them).
- `src/doc` for DrawingML text bodies where it fits.
- `src/PagedView` and the PDF reader's presentation mode for the slide show's input.
- `FileKinds` already lists `.pptx`, `.pptm`, `.potx`, `.potm` and `.ppsx` as presentations (not yet opened).

### Shared files

- **Command ids:** p1-model 3200–3219, p1-render 3220–3239, p1-view 3240–3299, p1-show 3300–3349.
- **Code:** each piece puts its code in new files and touches shared ones only to hook in.
- **Library:** the model and reader live in `src/slide/` (`fwslide`). The rendering lives in `src/slide/render/`, the tab in `src/slide/view/`, the show in `src/slide/show/`.

## Rules

As S1–S4 (`docs/s3-plan.md`, "Rules"):
- never damage a file;
- nothing active (macros kept, never run);
- lightweight, with a ledger entry and a reason for every size step;
- Office automation rules (above);
- Roy's Featherweight is never closed;
- only Playwright's own headless Chromium.

- **Usage:** check `get_usage` before each step. Stop gracefully at **98 % of the 5-hour window** and at **98 % weekly**: no new step, commit and push (a WIP commit is fine), state note exact, end the turn. The coordinator resumes everyone after the reset.
- **Notes:** `D:\Documents\Codex\FeatherweightNotes\p1\<piece>-state.md`, current at every turn end.
- **Commits:** small, on your branch, no Co-Authored-By trailer. Push and say what's ready to merge.

## Decisions during P1

- **2026-11-22: PowerPoint references may be rendered over Remote Desktop.** Probed: the 18 public decks rendered in Roy's active Remote Desktop session match the console references (97 slides, SSIM ≥ 0.99992; only a date field differs). An active `rdp-tcp` session now counts like the console; a disconnected one doesn't. `reference-renders.ps1` checks before every deck.
- **2026-11-22: first corpus measurement, 59.22 %.**
  - 1,490 of 2,516 slides pass (SSIM ≥ 0.95 at 100 dpi), fwtool on p1-int `ee37031f`.
  - PowerPoint rendered 149 of the 152 decks; 3 fail PowerPoint's own export.
  - Per kind: briefings 75.0 %, charts 52.6 %, features 57.0 %, lectures 60.2 %, photos 59.0 %, school 74.4 %, shows 80.0 %, statistics 58.2 %, templates 79.5 %, training 34.2 %.
  - Mostly near misses: 21.5 % of the slides score 0.90–0.95, 12.9 % 0.80–0.90, 6.4 % below 0.80.
  - Causes, with a deck and slide for each: `FeatherweightNotes\p1\p1-corpus-findings.md`.
- **2026-11-22: re-render the references at full picture quality? Probed: PowerPoint can't, so they stay** (coordinator).
  - PowerPoint's Save As PDF resamples some pictures (a 1920×1080 full-slide picture came out 1536×864) and recompresses some as JPEG. On a noisy full-slide picture that costs about 0.05–0.1 SSIM.
  - Probe on two decks: ExportAsFixedFormat with the print intent (called early-bound through Office's interop assembly) writes exactly the same pictures as Save As; the screen intent writes smaller ones.
  - The resampled pictures are the ones with transparency (`alphaModFix`) or flattened transparent groups, which PowerPoint rasterizes at its own resolution in any PDF. Pictures without effects keep their size.
  - So the references stay as made, and this resampling is a known measurement artifact (p1-render reached the same conclusion for `alphaModFix` pictures).
  - Coordinator: Save As stays the reference. Featherweight's slide PDFs bake see-through (`alphaModFix`) pictures at most 1536 px across, as every PowerPoint PDF export does (p1-render `b27a4905`). That's PDF output only: the slide view keeps them at full resolution, as PowerPoint shows them. A light `slide-pages -Through app` cross-check (the public set and about 20 corpus decks) compares the app's own drawing with fwtool's.
- **2026-11-22: no criterion change yet** (coordinator).
  - p1-render and p1-model work through the cause list, biggest first: font fallback (sans faces drawn in Times New Roman), text 1–2 px off, texture backgrounds, `defaultTextStyle` bold/italic applied to placeholders, layout `clrMapOvr`, then charts, SmartArt and WordArt.
  - p1-corpus re-measures on the newest p1-int after each round of fixes.
  - The criterion is revisited, as S4 did, only if real fixes plateau well below 90 %.
- **2026-11-23: corpus at 75.44 % on p1-int `0317ed79`.**
  - 1,898 of 2,516 slides pass, after 67.01 % on `af87058b` (references unchanged). The public set is at 92/97.
  - Per kind: briefings 77.1 %, charts 72.6 %, features 72.5 %, lectures 81.8 %, photos 64.8 %, school 78.0 %, shows 80.0 %, statistics 66.4 %, templates 86.7 %, training 60.0 %.
  - Near misses: 15.9 % of the slides score 0.90–0.95, 6.4 % 0.80–0.90, 2.3 % below 0.80.
- **2026-11-23: cross-check of the slide view (`slide-pages -Through app`) against fwtool's PDF, same decks, same commit.**
  - 20 corpus decks (two per kind, 233 slides): 100 pass in the app, 163 through fwtool. 64 slides pass only through fwtool, 1 only in the app. Mean SSIM is 0.018 lower in the app.
  - Public set: 88/97 in the app, 92/97 through fwtool.
  - Largest gaps:
    - `backgrounds.pptx` slide 7: a pattern fill drawn at a coarser pitch, 0.127 against 0.810.
    - `backgrounds.pptx` slide 6, `shapes-groups.pptx` slide 1.
    - Every slide of `cpb-presentatie` (−0.08 to −0.13).
  - So the fwtool number overstates what the slide view shows.
- **2026-11-23: the gate's display measure** (coordinator).
  - The gate number stays the fwtool path: Featherweight's PDF and PowerPoint's PDF, both rasterized by pdfium. It compares like with like, and the reference is PowerPoint's PDF, not its screen.
  - Added to the gate: the app draws what its own PDF draws. The slide view (`pagepngs`, 100 dpi) is compared with Featherweight's own PDF (pdfium) on the 20-deck sample and the public set. Every slide differing by more than 0.05 SSIM is a slide-view bug, fixed or explained before the gate. A uniform rasterizer and antialiasing offset under that is accepted and recorded with its size.
  - A full `-Through app` corpus run only if that check finds more than a few root causes.
  - First check, on `0317ed79` (`FeatherweightNotes\p1\corpus-analysis\app-vs-own-pdf.ps1`):
    - Public set: median SSIM 0.9966, 3 slides under 0.95.
    - Corpus sample: median 0.972, 30 of 233 slides under 0.95.
    - Root causes:
      - Text in an embedded font isn't drawn in the slide view (`cpb-presentatie` slide 5, 0.796).
      - Pattern fills at a different pitch (`backgrounds.pptx` slide 7, 0.123; `shapes-groups.pptx` slide 1, 0.899).
      - Tiled picture fills at a different tile size and offset (`backgrounds.pptx` slide 6, 0.783).
      - A picture drawn darker (`overheid-migratieketen` slide 3, 0.915).
      - Pictures with an ellipse shape or soft edges drawn differently (`usgov-our-dynamic-sun-2025` slide 12, 0.927).
    - Text antialiasing: dense text slides drop to about 0.935 with nothing else different (`un-ccsa` slide 12), more than the accepted 0.05.
- **2026-11-23: corpus at 77.23 % on p1-int `9eb0c84d`** (the gate number), after p1-render fixed the five slide-view causes.
  - 1,943 of 2,516 slides pass. The public set is at 92/97.
  - Per kind: briefings 77.1 %, charts 74.1 %, features 85.2 %, lectures 82.3 %, photos 65.6 %, school 79.2 %, shows 80.0 %, statistics 67.3 %, templates 87.1 %, training 63.4 %.
  - Slide view against Featherweight's own PDF, app on `445d69b0` (p1-render `77b8d25c`: the view draws text as pdfium does; fwtool's PDF path unchanged):
    - Public set: median 0.998. Only `text-autofit.pptx` slide 1 (0.944) is under 0.95: explained, antialiasing.
    - Corpus sample: median 0.985.
    - The app's page export stops after slide 6 of `govuk-cybersprinters-lesson-3-suspicious-contact` (exit 0, 6 of 22 pages; all 22 on `9eb0c84d`), a regression with `77b8d25c`.
    - Four other slides score 0.932–0.949: `overheid-migratieketen` 7, `usgov-globe-widescreen` 8, `cpb-presentatie` 7, `zslbw-bnt-klasse-5-6` 17.
- **2026-11-23: `usgov-how-well-do-you-know-era-systems` from 13 to 54 of 58 slides with p1-render `161a2922`** (coordinator). The gain is mostly the resampling phase of its see-through background picture now matching PowerPoint's PDF, not a change anyone would see.
