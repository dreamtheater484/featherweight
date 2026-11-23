# R1: the ribbon — work plan

R1 replaces the editors' single toolbar and long ⋯ menus with a ribbon placed as in Office. The design is in IMPLEMENTATION_PLAN.md §5.6, the done-when in §5.6's milestone table, and the reasoning in decision 27. R1 started on 2026-11-23, alongside P1's fidelity work. It lands in `main` after P1 and P2 are merged and Roy has checked them, and before P3.

## Integration

- Branch `claude/r1-int`, started from `claude/p2-int` 4a89ffc9, which has all of P1 and P2. The coordinator merges P1's and P2's later fixes in regularly.
- Each session works on its own branch and pushes ready heads. The coordinator merges a head into `claude/r1-int` only after a Release build and a 100 % ctest pass.
- The PDF reader keeps its slim toolbar. Only the document, workbook and presentation tabs get the ribbon.

## Pieces

| Piece | Owns | Command ids | Size budget |
|---|---|---|---|
| **r1-control** | `src/ui/Ribbon*`, `src/ui/RibbonModel.h`, the File page (`src/ui/FilePage*`), quick access in `src/ui/TitleBar.cpp`, the shell integration in `src/App*.cpp`, `src/TabContent.h` (`BuildRibbon`), custom icons in `src/ui/Icons.h`, the coverage test framework, ribbon snapshot tests | 3600–3649 | 160 KB |
| **r1-word** | Word's tabs, contextual tabs and File-page entries in `src/word/` (`BuildRibbon`); retires Word's toolbar model and ⋯ items | 3650–3699 | 30 KB |
| **r1-sheet** | Excel's tabs in `src/sheet/view/`; retires the workbook toolbar model and ⋯ items | 3700–3749 | 30 KB |
| **r1-slide** | PowerPoint's tabs in `src/slide/view/`; retires the presentation toolbar model and ⋯ items | 3750–3799 | 30 KB |
| **coordinator** | merges, P1 and P2 merged in, the final gate, the friction review summary, the checklist section | — | — |

The ids are for new commands only, such as Ctrl+F1 or a group's dialog launcher where no command exists yet. Existing commands keep their ids. In `src/Commands.h`, each piece adds its ids in its own block, under a comment naming the piece.

## Order

1. **r1-control first defines `RibbonModel.h`**, in its first push. It's the data the editors fill in: tabs, groups, items (large or small, KeyTip, label, the existing `ToolItem` for kind and state), galleries, dialog launchers, contextual tabs. It also adds `TabContent::BuildRibbon`. Until that lands, the module pieces write their tab layouts as tables in their state notes: each Office tab, group and button mapped to the existing command it runs, or "not in Featherweight" (left out).
2. The module pieces fill `BuildRibbon`, while r1-control builds the control, the File page, KeyTips, shrinking groups, collapsing, quick access and the theme.
3. Once a module's ribbon works end to end, that piece retires its toolbar model and the ⋯ items the ribbon or File page now covers. Right-click menus stay.

## Rules

- **Placement as in Office** (Word, Excel and PowerPoint 2021/365, classic ribbon): tab names, group order, and button order and size within groups. A button appears only for a command Featherweight has; nothing is greyed out for missing features.
- **Icons** come from Segoe Fluent Icons (Segoe MDL2 Assets on Windows 10), so they cost no exe size. Where the font has no fitting glyph, draw one with `Painter` in `src/ui/Icons.h` (r1-control) instead of using an unrelated glyph. Never copy Office's icon artwork.
- **Shortcuts, right-click menus, side panels and Alt+Q don't change.** Command search lists ribbon commands under their tab and group names.
- **Lightweight:** the size budgets above, with R1 at most 250 KB in total. Launch time and memory after launch no worse. Switching ribbon tabs and relayout on resize within one frame (16 ms). No CPU use when idle. Measure sizes with the map build (`$env:LINK='/MAP'` from PowerShell, see the link-map memory) and add a ledger line to `tests/budgets/ledger.json`.
- **Tests:**
  - Each module adds a coverage test: every ribbon item runs an existing command, and every command the old toolbar and ⋯ menu reached is reachable again from the ribbon, the File page, a right-click menu or Alt+Q.
  - Snapshot tests at 1366, 1024 and 800 px, in light and dark.
  - Full `fw_tests` and ctest before every push.
- **Never close Roy's Featherweight.** Run your own instances only, for example with `--snapshot`.
- **Usage:** run to 100 % of the windows (Roy, 2026-11-23). Push after every working step (WIP ok) and keep a "Resume here" block at the top of your state note.
- **Questions:** decide for yourself or ask the coordinator, not Roy.
- **State notes:** `D:\Documents\Codex\FeatherweightNotes\r1\<piece>-state.md`.

## Decisions during R1

(Each piece adds its decisions here.)

- **r1-slide (PowerPoint):** Transitions, Animations, Table Design and Chart Design are left out until their commands exist (P3); View's Presentation Views too (only Normal exists). Print layout (slides, notes, handouts) goes to File > Print's settings, as in PowerPoint. Menu buttons with no command of their own (Align ▾, Group ▾, Rotate ▾, Slide Size ▾…) get ids 3786–3796, enabled while a command in the menu is. Shape Outline's weights and dashes stay a Weight dropdown beside it. Date & Time and Slide Number open Header & footer, as PowerPoint's open its dialog. Slide text gets a format painter (CmdFormatPainter), and its commands are in Alt+Q.
- **r1-sheet, Excel's layout:** read from Excel's own ribbon through UI Automation (Excel 365 build 20326 on the PC, full width): tab, group and button names, order, sizes and KeyTips. The table is in `FeatherweightNotes\r1\r1-sheet-state.md`.
- **r1-sheet, buttons for what Featherweight already does:** where Excel's button is one click on something Featherweight did only in Format Cells or the Page Setup panel, it gets a command (ids 3700–3708): Wrap Text, Top/Middle/Bottom Align, Accounting Number Format, Comma Style, the Font and Alignment launchers, View › Normal. Existing ids are reused where the meaning is the same: `CmdGrowFont`/`CmdShrinkFont`, `CmdIndent`/`CmdOutdent`, `CmdPortrait`/`CmdLandscape`, `CmdMarginsNormal`/`Wide`/`Narrow`, `CmdObjectFill`/`CmdObjectOutline`. Menus are ids 3720–3739, filled by `WorkbookView::FillMenu` with Excel's words and mnemonics.
- **r1-sheet, print preview:** a contextual tab, Print Preview, as Excel 2007 had (later Excel moved preview into File › Print; Featherweight's preview is a mode of the tab): Print, Page Setup, Print What, Scaling, Zoom, Next and Previous Page, Close Print Preview.
- **r1-sheet, panels behind several buttons:** Chart Design's Add Chart Element, Change Colors, Select Data and Change Chart Type all open the chart panel, where Featherweight does each of them; Picture and Shape Format's launchers and Alt Text open the Format panel.
- **r1-sheet, Review:** Excel 365's threaded layout (Comments, and Notes ▾), since Featherweight has both.
- **Coordinator, script steps:** the script op `ribbon:<tab name>` (with collapse, expand and open) belongs to the control, in `App::RibbonScriptOp`, which runs before an editor's own ops. Module steps use their own prefix (for example `docribbon:`), and coverage goes through the control's `fw_ribbon_test` and `ribboncheck` against `tests/ribbon/<module>.txt`.
- **r1-word (Word):** the layout is read from Word's own ribbon through UI Automation (Word 365 16.0.20326 at full width: tab, group and button order, sizes, KeyTips; table in `FeatherweightNotes\r1\r1-word-state.md`). Word 365 now has a Markup group (Display for Review, Reviewing Pane, Change Tracking Options) between Comments and Tracking, and Featherweight follows it. Featherweight's own commands take Word's nearest places: Present at Read Mode's, Full Screen at Focus's, Night Reading beside it (Word 365's Switch Modes), Side to Side is one page at a time, Multiple Pages two pages, the reviewer's name is the Markup launcher and a File > Options entry. Content controls get a Developer tab (Controls only), since Word has them nowhere else; as in Word it is hidden until File › Options › Show Developer tab (`CmdDeveloperTab` 3660, `Settings::developerTab`, saved as DeveloperTab in [General]; coordinator's decision), and Alt+Q finds the content controls either way. Spelling and AutoCorrect as you type are the File page's own Editing options, so the document adds only Show Developer tab and User name. Where Word's menu only offers designs Featherweight lacks (Header, Footer, Table of Contents, Pictures), the button runs the command at once. New ids 3650–3655 for what only a panel did: Layout › Paragraph's indent and spacing boxes, Table Design's cell Shading and Borders; menu buttons reuse a command of their menu (Margins is `CmdMarginsNormal`), labels in Word's words. Ids 3656–3659 for Word's Height and Width boxes (Picture and Shape Format: a picture keeps its aspect ratio; Table Layout › Cell Size). Coverage is r1-control's `ribboncheck` with `tests/ribbon/word.txt` (162 commands: dumpreach before R1 plus the table, picture and header states), ctest `ribbon_word`. The document's toolbar model and the ⋯ menu's document block are retired (only Save, Print and the view buttons are left, for when there's no ribbon); right-click menus and shortcuts stay. Word's own pictures are r1-control's drawn icons; spin boxes are 100 DIPs wide so "15,24 cm" fits beside the arrows. The Styles gallery folds into a large Styles button (Aa) in a narrow window.
