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
- **2026-11-23: no page fails to draw on p1-int `0df66c49`** (after p1-render `ffec52c0`, which fixed whole slides failing in Direct2D with `D2DERR_BAD_NUMBER`).
  - `fwtool slides --png` over all 152 corpus decks and the 18 public decks drew 2,652 pages: every deck its full slide count, no errors.
  - 17 pages are one flat colour, and all 17 score SSIM 1.0 against PowerPoint: empty slides in the decks (templates' spacer slides, LibreOffice transition tests, `layouts-placeholders.pptx` slide 7).
  - Script: `FeatherweightNotes\p1\corpus-analysis\png-sweep.py`.
- **2026-11-23: corpus at 85.93 % on p1-int `bc26651a`** (about 20 render fixes since `9eb0c84d`).
  - 2,162 of 2,516 slides pass; 90 % needs 102 more. The public set is at 92/97.
  - Per kind: briefings 83.6 %, charts 80.7 %, features 88.0 %, lectures 87.5 %, photos 80.3 %, school 82.0 %, shows 92.0 %, statistics 72.7 %, templates 91.0 %, training 88.7 %.
  - Of the 354 failing slides, 250 are near misses (0.90–0.95).
  - Slide view against Featherweight's own PDF: no slide under 0.95. Sample median 0.989 (min 0.955), public 0.9987 (min 0.953). The view part of the gate holds; `govuk-cybersprinters` draws 22 of 22.
  - Failing slides by what they contain (first match: chart, SmartArt, table, equation, picture, text, other; a proxy, not a proven cause):
    - Slides with pictures: 133 failing, 37.6 % (86.9 % of those slides pass).
    - Text only: 131, 37.0 % (87.5 % pass).
    - Tables: 25, 7.1 % (75.5 % pass).
    - Equations: 25, 7.1 % (80.8 % pass).
    - Charts: 20, 5.6 % (16.7 % pass).
    - SmartArt: 13, 3.7 % (66.7 % pass).
    - Other: 7.
    - Script: `FeatherweightNotes\p1\corpus-analysis\causes.py`.
  - One regression: `un-country-presentation-morocco` fell from 2 to 0 of 15 (median 0.862).
- **On p1-int `d630a3c6`: corpus at 86.41 %** (p1-render `c28a07f7`: charts, columns, Arabic in Tahoma; p1-model `a3ff3d68`: table styles, cell fills, nested groups).
  - 2,174 of 2,516 slides pass, +12 net. 9 decks gained 35 slides; morocco is back to 7 of 15.
  - Chart slides now pass 12 of 24, table slides 89 %.
  - In this run `fwtool slides` crashed with 0xC0000005 on `who-health-in-all-policies-training-slide-template`, so all 23 of its slides counted as failing. Without the crash the total would be about 87.3 %.
  - The crash is intermittent: none in 2 runs alone, 2 of 20 with four runs in parallel, 1 of 32 with `--png`. That run stopped after slide 7, so it likely died drawing slide 8 (a group with custom geometry, a picture and an outer shadow).
  - Script: `FeatherweightNotes\p1\corpus-analysis\crash-repro.ps1`.
- **2026-11-23: slide PDFs write pictures as PowerPoint's Save As PDF does** (coordinator: agreed as PowerPoint's export behaviour; the gate report counts these gains apart from drawing gains).
  - The rules, measured on its PDFs:
    - Every JPEG picture is encoded again at quality 75: byte for byte libjpeg's quality 75, which WIC's encoder at 0.75 gives (`draw::EncodeJpeg`, WIC, in the exe since `6217503c`: no new encoder).
    - A picture's place is written in page coordinates (y up), each number to 5 significant digits, from its bottom left, about 0.00006 pt high (its float arithmetic), through the transforms as the file has them.
    - The page box is a whole number of 600-dpi pixels (961 pt is 960.96, 960.125 is 960.12); a PDF viewer's page width in pixels, and so its whole page's scale, follows it.
    - Not modelled: which pictures it downsamples (mostly to about 200 ppi where placed; some 220, 150 or 96; full-slide pictures at most 1536 px).
  - Export-matching gains: `7d70c42c` +18 slides on 14 photo decks (p1-corpus, full corpus: +11 net, 13 gained, 2 lost: unimuenster's template slides 23 and 20, 0.953 to 0.945); `a78d3c9e` (page box) +1, goettingen 3 (0.92 to 0.99). Earlier, `161a2922`'s era-systems gain (above) is of the same kind.
  - Drawing gains in the same round, for contrast: slide text laid out with GDI's advances in PowerPoint's master units (`babda789`, +70 over 46 near-miss decks), a baseline shift that doesn't make its line taller (`23b4d98f`, +17), Proxima Nova in Tahoma (`ce9971d6`, +10).
  - The app's slide view against its own PDF (pagepngs at 100 dpi, the coordinator's check that the gap stays within 0.05): acuk 41/41. On goettingen the export matching opened gaps, and they are explained, not view bugs: the view didn't change (2/7 against PowerPoint before and after), the PDF moved to PowerPoint's export (3/7 to 5/7).
    - The page box (slides 4 and 5, 0.97 to 0.94): the PDF's 960.12 is 1333 pixels, the view drew 1334. Fixed in the harness: pagepngs draws each page at the PDF's box (`PdfDocument::PowerPointBox`), so the two compare like with like (coordinator's decision).
    - The JPEG at quality 75 and its resampling (slide 3: view against PDF 0.98 to 0.87; the PDF against PowerPoint 0.84 to 0.99): an export-matching gap, kept. The view draws the file's own picture, as PowerPoint's screen does; only the exported PDF holds the re-encoded one.
- **2026-11-23: native Office math in slides** (coordinator, unparked). Slides lay out a14:m themselves, reusing fwdoclayout's typesetter (src/doc/layout/Math.cpp, already in the exe for Word: a few KB of glue), instead of Office's fallback picture.
  - Conditions: it stays behind a switch that is off (`FW_SLIDE_MATH=1` turns it on; `draw::xml::SlideMathLaidOut`) until it measures better on the corpus's math slides as a set, and no slide that passes with the fallback drops below 0.90.
  - p1-model owns the math path in `TextBody.cpp` (agreed with p1-render). Typesetter changes in src/doc/layout keep Word's math tests and the D-milestone checks green.
  - First step (p1-model, measured on the 137 corpus slides with a14:m): fallback 111 pass, native 68. Display integrals now take PowerPoint's variant (glyph 3505 of Cambria Math, one past DisplayOperatorMinHeight); lines of math are 1.2 times their font size unless the equation is taller.
  - **Parked again (coordinator's checkpoint, 2026-11-23):** native stays at 68 of 137 against the fallback's 111, so `FW_SLIDE_MATH` stays off. Lowering math baselines by 0.15 em went to 62 (enee-205 7 to 1) and was dropped. What's left is fine typesetting calibration against PowerPoint, not one big cause. Our math sets wider than PowerPoint's: cs-444's f_K(h_{K-1}, w_K) overflows its oval. A line after a display equation sits about 2 pt low. Baselines are 0.15-0.2 em high on probability-for-computing but not on enee-205. cs-444 vision 8 has a red display paragraph that PowerPoint's PDF doesn't show. Measuring and comparison scripts: `FeatherweightNotes\p1\corpus-analysis\mathcmp.py` and `eqpos.py`; state in `p1-model-state.md`.
- **On p1-int `cb26c73e`: corpus at 88.31 %** (p1-render: camera rev, spcFirstLastPara, chart margins, pies and doughnuts, hanging indents, empty charts, and `7d70c42c` export matching; p1-model: SmartArt text style, native math off).
  - 2,222 of 2,516 slides pass, +48 against `d630a3c6`. 14 decks gained 50 slides, 2 lost one each. The public set is at 92/97.
  - The gain splits three ways, measured against p1-int `34b12323` (`cb26c73e` without `7d70c42c` and the empty-chart fix):
    - `7d70c42c`'s export matching (picture placement, page box, JPEG at q75): +11 net, 13 gained and 2 lost. All 13 gains are slides with their own pictures: acuk general +4, unisaarland +3, acuk dual-gaelic +2, and 1 each on morocco, goettingen, cs-2110 graphs and course-100. The 2 losses are layout-picture slides in the two unimuenster templates (en 23, de 20: 0.953 → 0.945). 256 slides moved up by more than 0.001, 28 down.
    - who-health didn't crash this time: +23. The bug is still in `cb26c73e`. p1-render fixed it in `a2210fdf` (a use-after-free in `PdfPainter::DrawBitmap`), which isn't in p1-int yet.
    - Drawing fixes: +14, none lost: virtual-memory +7, rlf-webinar-i +2, business bodies +2, and 1 each on physik 7-8, the poi line chart and the overheid webinar.
    - The empty-chart fix flips no corpus slide: goettingen 2 already passed.
  - Without export matching the corpus is at 2,211 (87.88 %).
  - Per kind: briefings 85.7 %, charts 86.7 %, features 88.7 %, lectures 89.3 %, photos 83.6 %, school 86.8 %, shows 92.0 %, statistics 80.0 %, templates 92.9 %, training 89.2 %.
  - Of the 294 failing slides, 223 are near misses (0.90–0.95).
  - Failing slides by content:
    - Pictures: 116 (88.5 % of picture slides pass).
    - Text: 112 (89.4 % pass).
    - Equations: 24.
    - SmartArt: 13.
    - Tables: 11.
    - Charts: 9 (15 of 24 now pass).
    - Other: 9.
  - 90 % needs 43 more slides. Script for the split: `FeatherweightNotes\p1\corpus-analysis\split.py`.
- **2026-11-23: five corpus references re-exported (stale cloud fonts)** (p1-model's finding, coordinator's go).
  - Why: Office downloads a missing cloud font during the first export that needs it, and that export finishes with a substitute. These five PDFs were written 0–3 s after their font arrived in `%LOCALAPPDATA%\Microsoft\FontCache\4\CloudFonts`:
    - worldbank-evolution (Open Sans SemiBold/ExtraBold);
    - course-100 (Source Sans Pro);
    - our-dynamic-sun (Montserrat);
    - cybersprinters-1 (Poppins);
    - cybersprinters-3 (Libre Baskerville).
  - Re-exported from PowerPoint on 23 Nov at 22:11 from Roy's active session, under the usual rules (quiet-pc.lock, our own instance, AutomationSecurity 3, read-only). The old PDFs are kept beside the new ones as `<name>.stale-cloudfont.pdf`, and the old manifest as `reference\manifest.before-cloudfont.json`.
  - The new PDFs use the cloud faces: Open Sans SemiBold/ExtraBold, Source Sans Pro, Montserrat, and Poppins in place of Nirmala UI. cybersprinters-3's new PDF draws the same pages as before (no page under 0.999 against the old one), so it wasn't really stale.
  - **Gate on `cb26c73e`, the reference fix alone:** 2,222 → 2,225 of 2,516 (88.31 % → 88.43 %).
    - our-dynamic-sun 12 → 14/14 (slide 2 .879 → .960), cybersprinters-1 23 → 24/24.
    - 19 slides moved up, none down. worldbank-evolution stays at 0/8: its slides fail for other reasons.
  - Script: `FeatherweightNotes\p1\corpus-analysis\stale-refs.ps1` (`p1-queued-renders.ps1 -Stale`).
- **2026-11-24: the slide show's first transition warms up before it moves; the frame bench ignores input** (p1-show).
  - Why: at the console (240 Hz, 5120×1440, gate head `ea3fe681`), all 3 missed refreshes of a 50-transition run were in the first transition. Its second and third frames came 16.2 and 12.9 ms apart, each waiting ~16 ms for a buffer, while the compositor took up a full-screen window shown a moment before. Every later transition starts on a window that is already presenting, and none missed a refresh.
  - Which: a warm-up, not a second pre-render. The next slide is already prepared and finished on the GPU before every transition, the first included. What only the first lacks is a window that has presented before. So the first transition holds at its start for 100 ms and at least 4 frames before its clock starts. Its start is black (it comes from black), so nobody sees the wait. The bench leaves the held frames out (`warmupFrames` in its report) and times the transition from when it moves.
  - The bench ignores the mouse and the keyboard (Roy, 24 Nov: a mouse move or a click shouldn't throw a run away). Only Esc, Alt+F4, closing it, or another program taking the focus stop it. Its own ending no longer counts as losing the focus: destroying the window hands the focus to another program, which had made two complete runs end as aborted.
  - Report: DXGI's frame statistics are gone, since they give nothing for this windowed swap chain (4,073,665 "missed"). `lateFrameStarts` now counts only starts more than 1.8 refreshes apart (`lateStartThresholdMs`). The 6.3 ms starts at 240 Hz (1.5 refreshes) are on time: the frame queued ahead keeps a new frame at every refresh, and `missedRefreshes`, counted on DWM's vblank clock, is the pass criterion.
