# Featherweight: Implementation Plan

Featherweight grows from a fast PDF reader into a light office suite for Windows: documents, spreadsheets and presentations, alongside PDFs, in one small app. The promise stays the same: it opens instantly, stays out of the way, and has no bloat, subscriptions, ads, accounts or cloud.

- **Status:** draft, 2026-11-03, revised after an independent review the same day. Replaces the PDF reader plan. The reader (0.9.4) is finished, and its code is the starting point.
- **Order:** documents (Word files) first, then spreadsheets (Excel), then presentations (PowerPoint). Each app ships first as a viewer, then as an editor.
- **Repository:** https://github.com/dreamtheater484/featherweight (public)
- **License:** MIT

---

## 1. Goals

**Who it's for:** a regular consumer, or a slightly more advanced user, who wants to open, edit and send Word, Excel and PowerPoint files without Office. They should find every basic feature they use and a good share of the advanced ones, and never have to worry that Featherweight will damage a file.

**The compatibility promise**, in order of priority:

1. **Never damage a file.** Everything Featherweight doesn't understand is kept and written back, and the file stays consistent after edits. This is absolute, and tested on every build.
2. **Look right.** Common documents look the same as in Office: same fonts, same line breaks, same page count. This is measured against Office's own output, not guessed.
3. **Do the job.** All basic features and selected advanced ones (§6.2, §7.2, §8.2). Anything else is shown if possible, kept, and explained in plain language.

**The lightweight promise**, held to the same standard:

1. **Small.** One small exe and a small download. Nothing installed beside it, and nothing running in the background.
2. **Fast.** It opens near-instantly, and everyday actions respond at once: scrolling, zooming, switching tabs, opening menus, searching and typing.
3. **No bloat.** Every feature and every dependency earns its weight. Test-only code never ships.
4. **Zero friction.** The fewest possible steps and decisions between you and the document (§2).

Both promises are gates. No milestone or release is done while either is broken, and both are measured on every build, not assumed: the compatibility contract in §3, the lightweight contract in §11. When the two conflict, never damaging a file comes first.

100 % compatibility and full feature parity are not goals. Nobody reaches them, not even Microsoft's own web and Mac versions of Office.

**Not goals:** running macros, co-authoring or cloud storage, add-ins, AI features, and platforms other than Windows (for now).

---

## 2. Design principles

The bar stays **zero friction**: the fewest possible steps and decisions, and nothing between you and the document. The reader's ten principles still apply. Principles 11–15 are new for editing.

1. **The document is the app.** It opens straight to the document. The chrome is slim and quiet, and the page gets the space.
2. **No settings screen.** The few real preferences are single-click toggles in the ⋯ menu. Document settings (page size, margins, styles) belong to the document and are edited in a side panel, not in an Options dialog.
3. **Never ask what the app can figure out.**
   - Ctrl+S on a new document suggests a file name from its first line.
   - Double-clicking a template creates a new document from it.
   - Ctrl+N creates a new file of the same kind as the current tab.
4. **Never interrupt.** Information goes in a slim banner. Formatting and properties open in a side panel next to the document, not in a modal dialog. The only dialogs are Windows' own (Open, Save As, Print) and the unsaved-changes question.
5. **Updates happen by themselves.**
6. **Plain language.** No error codes. For example: *"budget.xlsx uses 3 functions Featherweight can't calculate yet. Their last saved values are shown."*
7. **Discoverable, then fast.**
   - Every button has a tooltip with its shortcut.
   - **Alt+Q** finds any command by typing its name.
   - Shortcuts match Office's wherever Office has one, so muscle memory works.
8. **Polished and native.** Windows 11 look, Direct2D everywhere, light and dark themes designed side by side.
9. **Forgiving.**
   - Undo has no practical limit.
   - Saving never puts the original at risk.
   - If Featherweight or Windows crashes, your unsaved changes come back the next time you open the file.
   - The first time Featherweight saves over a file, it keeps a copy of the original for 30 days.
10. **One-time offer, never a nag.** The first time you open each kind of file, a banner offers once to make Featherweight its default app.
11. **Never damage a file** (see §3). A feature is editable only when Featherweight can write it back completely and consistently. Otherwise it's shown, locked, and kept.
12. **Honest about limits.** When a file contains something Featherweight can't show, calculate or edit, a banner says what it is and what that means. Nothing is dropped silently.
13. **Familiar, not a clone.** People know the words (Heading 1, Wrap text, Insert table) and the shortcuts, and Featherweight uses them. But there's no Ribbon with 200 buttons: one contextual toolbar, a side panel and command search.
14. **Light and fast are the product.** Opening stays near-instant and the download small. Nothing new loads or runs at startup: engines, DLLs and features wait until a file needs them. A feature has to earn its weight, and the simplest design that works wins. The lightweight contract (§11) sets the budgets, and they're checked on every build, at every milestone, and on a typical consumer PC before every release.
15. **Private.**
    - Documents never leave the PC.
    - Nothing in an Office document can make Featherweight go online, run code or open other files.
    - PDF form scripts keep working as today: they run inside PDFium's own JavaScript engine and can't open files or programs.

---

## 3. Compatibility contract

### Never damage a file

- **Round trip:** open and save without edits, and the file is equivalent to the original. Parts of the package that weren't changed are copied byte for byte from a private snapshot taken when the file was opened, never from the live file (§4.3).
- **Edits stay local:** after an edit, only what you changed is different. Everything else is written back, including:
  - unknown elements and attributes
  - extension lists (`extLst`) and `mc:AlternateContent` blocks
  - custom XML and embedded objects
  - macros (`vbaProject.bin`)
- **Edits keep the file consistent:** Office files are full of cross-references, and most "this file needs repair" prompts come from references that no longer match. Two rules prevent this (§4.3 rule 2):
  - Every reference is updated wherever it can live, including inside content Featherweight otherwise keeps unchanged.
  - Data that's derived from other data, such as Excel's calculation chain, is updated or removed.
- **Locked, not lost:** some things Featherweight can show but couldn't write back correctly after a change, for example SmartArt. You can move, copy or delete them as a whole, but not edit inside them, and a tooltip says why. An edit that would leave content Featherweight doesn't understand pointing at the wrong place is blocked, and a banner says why.
- **Changed on disk:** if another program changed the file after Featherweight opened it, Save becomes Save As and a banner explains, so two versions are never mixed.
- **Older formats are imported, not overwritten.**
  - `.doc`, `.xls`, `.ppt` and OpenDocument files open, but Save goes to Save As with the modern format. A file is never replaced by one in a different format, and a banner explains this once.
  - RTF files are saved in place only if Featherweight understood everything in them.
  - CSV files keep only values: if a workbook holds more than that, the first save offers Save as `.xlsx` (§7.1).
- **Signatures:** saving a digitally signed file removes its signature, as in Office. A banner says so before the first save.
- **Validation** (§10):
  - Every saved file must have no new errors in the Open XML SDK validator.
  - Every saved file must pass Featherweight's own consistency checker.
  - On the Office reference PC, edited files must open in Office without a repair prompt. This is a gate for every editor milestone and every release.

### Look right

- **Measured, not guessed:** real Office exports a corpus of real documents to PDF once, on a reference PC. Featherweight renders the same files, and the two are compared on page count, line breaks and pixels (§10).
- **Fonts:**
  - Featherweight uses the fonts on the PC and fonts embedded in the document, within each font's embedding permissions.
  - A missing font is replaced by the closest available one, and a banner says the document may look slightly different.
  - The main case is Aptos, the default font of new Microsoft 365 documents since 2024. Aptos comes with Office, not with Windows, so most target users don't have it (decision 11).
  - Fidelity is therefore measured twice: with the reference PC's fonts, and with only the fonts that ship with Windows.
- **Previews:** many objects carry a preview made by Office: SmartArt drawings, embedded objects, newer chart types, and SVG images with a PNG copy. When Featherweight can't draw an object itself, it draws that preview.
- **Placeholders:** an object with no preview keeps its size and position and shows a neat placeholder with its name.

### Do the job

Each app's features come in three lists:

- **Basic:** all included before that app's editor release.
- **Advanced:** the ones included, in later milestones.
- **Kept but not editable:** shown where possible, never damaged.

Anything on none of the lists is not included, but it's still kept and never damaged.

---

## 4. Architecture

### 4.1 One app

It stays one window with tabs, as today. Any tab can hold a PDF, a document, a spreadsheet or a presentation. Opening any supported file from Explorer adds a tab to the running window.

```
Featherweight.exe
├─ shell     window, title bar and tabs, toolbar host, side panel, status bar, banners,
│            command search, settings, updater, file associations, printing host
├─ fwui      Direct2D on Direct3D 11, theme, animation, widgets (with UI Automation),
│            Text Services Framework input
├─ fwcore    pure logic: strings, INI, JSON, page layout math, units, versions (exists)
├─ pdf       the PDF reader as it is today ────────────────────────► pdfium.dll
├─ fwpkg     snapshots, zip, XML (DOM and streaming), package parts and relationships,
│            round-trip store, reference registry, Markup Compatibility, limits
├─ fwdraw    Painter interface with Direct2D and PDF backends; DrawingML and a VML
│            subset: themes, colors, fills, lines, effects, shapes, pictures
├─ fwtext    rich text: line layout on DirectWrite, hyphenation, editing, spell check
├─ fwchart   charts (basic types in D5, completed in S1, used by all three apps)
├─ fwdoc     documents: model, file formats, pagination, view and editor
├─ fwsheet   spreadsheets: model, formula engine, file formats, grid view and editor
└─ fwslide   presentations: model, file formats, renderer, editor, slide show
```

- **One exe, engines linked in.** The office engines are static libraries linked into `Featherweight.exe`. PDFium stays a separate DLL, loaded on first use as now.
  - With the static C runtime, every DLL has its own heap, so C++ objects can't safely cross DLL boundaries. Linking the engines in avoids that problem.
  - The updater still has only two files to replace.
  - Windows loads only the code pages that actually run, so opening a PDF never touches the spreadsheet code.
- **Nothing runs until it's needed.** Engines do no work at startup: no global objects whose constructors do work, and factories are created on first use. Launch time stays within budget (§11).
- **Engines are headless.** These don't depend on the shell or a window:
  - `fwpkg`, `fwdraw`, `fwtext` and `fwchart`
  - the model, file-format and layout halves of `fwdoc`, `fwsheet` and `fwslide`

  They're unit-tested, fuzzed and run by `fwtool` (§10) with no window. Only the view halves depend on `fwui`, and that's also where window-bound services live, such as Text Services Framework input.

### 4.2 The shell: from PDF reader to host

Today `App` is tied to the PDF view:

- `tabs_` is a `vector<unique_ptr<DocumentView>>`.
- `ViewHost` has PDF-only callbacks (cover page, form-field highlight, notes).
- `App.cpp` uses `DocumentView` directly in about 50 places.

The first milestone turns it into a host:

- **`TabContent`** is the interface every tab implements:
  - title, path, kind and modified state
  - viewport, drawing and idle work
  - input: mouse, wheel, keys, characters and IME; cursor and tooltips
  - commands, toolbar, context menu, find, print and save
- **Commands:** `QueryCommand(id)` returns enabled, checked, visible and label; `Execute(id, args)` runs the command.
  - Global commands (open, close tab, new, theme) stay in `App`.
  - Each kind of content owns its own keyboard map, so Ctrl+I can mean night reading in a PDF and italic in a document.
- **`PagedView`** is extracted from today's `DocumentView`. It's shared by the PDF view, the document view and the spreadsheet print preview, and covers:
  - page rectangles from `fw::Layout`, and zoom modes
  - smooth scrolling, overlay scrollbars and page shadows
  - the page box and presentation mode
- **Toolbar model:**
  - Each kind of content provides groups of buttons, dropdowns, split buttons, combo boxes and color pickers.
  - A contextual group appears when you select a table, picture or chart.
  - In a narrow window, groups fold into overflow menus in a fixed priority order.
- **Side panel:** a 320 px panel that slides in from the right for formatting, page setup, comments, styles and the navigation pane. Only one panel is open at a time, and Esc closes it.
- **Status bar** (editors only): the page and word count, or the sum and count of the selected cells, plus the language and zoom.
- **Rendering:** the window moves from `ID2D1HwndRenderTarget` to a Direct3D 11 device with a flip-model DXGI swap chain and an `ID2D1DeviceContext`, with WARP as the fallback (decision 20). Video in slide shows needs a Direct3D 11 device shared with Direct2D, and smooth transitions and low typing latency benefit too. Doing this in D0 avoids reworking the backend in P3.

### 4.3 Document models: shared rules

All three engines follow the same rules. Getting these right from the start is what makes "never damage a file" possible.

1. **Round-trip store.**
   - The reader turns the elements it knows into the typed model.
   - Everything else (unknown elements, attributes and namespaces) is kept as XML fragments attached to the nearest known neighbor, along with its position. The writer puts these fragments back in place.
   - Unchanged parts aren't re-serialized at all: the zip writer copies them from the snapshot without decompressing them.
   - **Fragment ownership:** a fragment belongs to one model node.
     - When the node is split, every fragment keeps its place in the document: fragments before the split point stay with the first half, and fragments after it move to the second half. That includes fragments that follow the node's end tag, which now follow the second half.
     - When nodes merge, their fragments are kept in order.
     - When the node is deleted, its fragments go with it, unless the fragment is one end of a pair, which rule 2 handles.
   - **Repeated attributes:** unknown attributes that repeat on every row or cell, such as `x14ac:dyDescent`, are stored once as an interned set and referenced by id, not as a fragment per element.
   - **Memory:** the store has a budget per document (§11).
   - **Markup Compatibility** (ECMA-376 Part 3):
     - In `mc:AlternateContent`, the first Choice whose requirements are all understood is used.
     - Namespace declarations used only by `mc:Ignorable` are kept.
     - A part with an `mc:MustUnderstand` namespace that Featherweight doesn't understand is shown but not edited.
     - When Featherweight edits the content of an AlternateContent block, the other branch is regenerated where Featherweight can write it (for example a VML fallback for a text box) and dropped otherwise. A stale branch is never kept next to an edited one.
2. **Consistency.** Keeping unknown content isn't enough: it must still be correct after an edit.
   - **Reference registry:** for every kind of reference, a list of every place it can live, including the extensions Office writes. Every structural operation updates all of them. The kinds include:
     - cell ranges and sheet names, and slide ids
     - paragraph ids (`w14:paraId`, `w14:textId`) and relationship ids
     - bookmark, comment-range, field, permission and move-range pairs
     - shape ids and VML anchors
   - **Unknown extensions:** these are classified by URI as reference-free, handled by the registry, or unknown. The extensions Office writes are listed in [MS-DOCX], [MS-XLSX], [MS-PPTX] and [MS-ODRAWXML]. A structural edit that would leave an unknown extension pointing at the wrong place is blocked, with a banner explaining why.
   - **Derived data:** each format has a list of data computed from other data, and what happens to it on save. Examples:
     - Excel's calculation chain is dropped when any formula changes.
     - Chart caches and the workbook embedded in a chart are updated together.
     - The other branch of an edited AlternateContent block follows rule 1.
   - **Pasting** gives pasted content new ids where ids must be unique: paragraph ids, bookmark names, comment ids, shape ids.
   - `fwtool check` verifies all of this independently of the model (§10).
3. **A private snapshot of the original.**
   - At open, the file is read completely into memory. Files over 256 MB are copied into the recovery folder (§13) instead.
   - The live file is never read again and never memory-mapped, so a sync client or another program changing it can't crash Featherweight or mix two versions into one file.
   - Before saving, the file on disk is compared with the snapshot: size, last-write time, and a hash. If it changed, Save becomes Save As, with a banner: *"report.docx was changed by another program after you opened it."*
4. **Edits are operations.** Every change is an operation with an inverse: insert text, set properties, split a paragraph, insert a row. Undo and redo, typing coalescing and crash recovery all come from this. No code changes the model directly.
5. **Copy-on-write models.**
   - Model data lives in copy-on-write blocks: paragraphs, blocks of rows, slides.
   - Taking a snapshot for saving or recovery costs almost nothing, so serialization runs on a worker thread while you keep editing, even for million-cell workbooks.
6. **Crash recovery from the operation log.**
   - At the first edit, a copy of the original snapshot goes into the recovery folder (§13).
   - Operations are appended to a journal there every few seconds.
   - Journals are encrypted with Windows' data protection API (DPAPI), so a password-protected document's contents are never stored in clear.
   - The journal format is versioned, and each version can replay the previous version's journals, because the updater can replace the exe between a crash and the next start.
   - After a crash, reopening the file offers to recover the changes by replaying the journal. A clean save or close deletes both files.
7. **Incremental work.** Layout, pagination and recalculation redo only what changed. Undo and redo cost about as much as the edit itself.
8. **First screen first.**
   - Opening reads the package on a worker thread.
   - What's visible is laid out on the UI thread, and the rest continues at idle, like today's time-sliced search.
   - The one-thread rule for PDFium applies only to PDFs.
9. **Typed units.** Office files use EMUs, twips, half-points, eighths of a point and fiftieths of a percent. `fwcore` gets strongly typed units, so they can't be mixed up.
10. **One painter.** Engines draw through `fwdraw::Painter`, never through Direct2D directly.
    - The Direct2D backend draws on screen, into bitmaps for tests and thumbnails, and to the printer.
    - The PDF backend writes exported PDFs.
    - Effects that PDF can't express, such as soft shadows and glow, are drawn as images. So are EMF, WMF and SVG pictures (decision 9).

### 4.4 Text

Documents, slides and cells all need rich text, so `fwtext` is built once, in the documents phase, and shared.

- **Line layout:** Featherweight has its own line breaker, built on DirectWrite's text analyzer (`IDWriteTextAnalyzer`). The analyzer provides scripts, bidi, line-break opportunities, shaping, glyph placement and justification. `IDWriteTextLayout` isn't used for documents, because it can't do any of these:
  - Word's tab stops with leaders
  - lines of different widths around floating pictures
  - Word's justification
- **Hyphenation:**
  - Word's automatic hyphenation (`w:autoHyphenation`) is common in Dutch and German documents and in narrow justified columns. Without it, line breaks and page counts drift for the whole document.
  - Windows has no hyphenation API, so `fwtext` uses Liang-style hyphenation patterns.
  - Compressed patterns ship for about 10 languages, from hyph-utf8 and OpenTaal, with each language's license checked. That's a few hundred KB in total.
  - Optional hyphens (soft hyphens) and non-breaking hyphens are handled too.
- **Fonts:**
  - system fonts, and font fallback (`IDWriteFontFallback`)
  - fonts embedded in documents, loaded in memory with `IDWriteFactory5`: obfuscated fonts in Word files, and Embedded OpenType (`.fntdata`, sometimes MicroType Express-compressed) in PowerPoint files
  - each font's embedding permissions (`fsType`) are respected, both for embedded fonts and when exporting PDFs
  - theme fonts (`+mj`/`+mn`), and the substitution table (§3)
- **Editing:** the caret, selection, keyboard navigation (logical and visual, for right-to-left text) and the clipboard are written once, against a `TextStory` interface that each app implements.
- **Word boundaries, case mapping and sorting:** Windows' built-in ICU (`icu.dll`, Windows 10 1903 and later), which adds nothing to the download.
- **Input:** Text Services Framework, in `fwui`, so IMEs compose inline, and the touch keyboard, dictation (Win+H) and emoji panel (Win+.) all work.
- **Spelling:** the Windows Spell Checking API, in every language the user has installed, following each run's language. No spelling dictionaries ship with Featherweight. Grammar checking is not included.

### 4.5 Graphics

`fwdraw` covers the drawing formats the three file types share.

**DrawingML:**

- **Themes:** theme colors, fonts and effects, and color transforms (tint, shade, luminance)
- **Fills and lines:** solid, gradient, picture and pattern fills; dashed lines, arrows and compound lines
- **Shapes:** the roughly 190 preset shapes (from the formulas in the standard), custom geometry, groups and connectors
- **Pictures:** cropping, recoloring and transparency
- **Effects:** shadow, glow, soft edges and reflection, drawn with Direct2D effects (best effort)

**A VML subset.** Office still writes VML for basic content:

- Word watermarks
- text boxes, shapes and pictures in Word 2007-era and generated files
- Excel notes, including the legacy copy behind every threaded comment
- legacy form controls

Featherweight reads the shape types Office writes (rect, roundrect, oval, line, textbox, textpath, image and the common `o:spt` types) from D1. It writes VML for watermarks (D4), text-box fallbacks, and notes (S3). VML is in the reference registry.

**Image formats:**

- PNG, JPEG, GIF, BMP, TIFF and ICO through WIC. Decoders are created from Windows' built-in codec classes only, never by letting WIC pick any codec installed on the PC, so third-party codecs never see untrusted images.
- EMF and WMF through Direct2D's GDI metafile support.
- SVG through Direct2D's SVG support, falling back to the PNG copy Office stores next to it.

### 4.6 Printing and PDF export

- **Printing:** the Windows print dialog, as today. Pages are drawn by the same `Painter` code into Direct2D command lists, and `ID2D1PrintControl` sends them to the printer as vectors.
- **PDF export:** Featherweight's own small PDF writer, behind `Painter`.
  - Fonts are embedded as subsets, so text stays selectable. TrueType fonts are subset with Windows' font-subsetting API; other fonts are embedded whole.
  - Fonts whose license forbids embedding are drawn as outlines, with invisible text on top so the PDF stays searchable.
  - Hyperlinks work, and headings become PDF bookmarks.
  - EMF, WMF and SVG pictures are rasterized at 300 dpi, because Direct2D can only draw them into its own device context (decision 9).
  - Compression uses miniz.

### 4.7 Security and privacy

Documents are untrusted input, and Featherweight is a C++ program.

- **No active content:**
  - Macros are kept but never run.
  - Embedded objects show their preview but are never started.
  - DDE and other fields that could run code are never updated.
  - External content is never fetched or opened: linked pictures, remote templates, links to other workbooks, data connections. Its last stored value or preview is shown instead.
- **No network:** opening, editing or pasting never makes a network request on its own. Pictures in pasted web content are only downloaded when you click a button for it (decision 12).
- **Links:** only http, https and mailto links open, with Ctrl+click as in Office, through the same filter as PDF links.
- **Limits:** the zip and XML readers cap these, so a malicious file fails with a plain-language message instead of hanging or using up memory:
  - uncompressed size and compression ratio
  - number of entries, nesting depth and string length

  OOXML forbids DTDs, so they're rejected, which also blocks entity-expansion attacks.
- **Image codecs:** built-in WIC decoders only (§4.5).
- **Data at rest:**
  - Recovery journals are encrypted with DPAPI.
  - Backups and recovery copies of password-protected files stay encrypted, because they're copies of the encrypted original.
  - The README's privacy section mentions backups, and ⋯ → **Delete backups** removes them.
- **Updates:** once releases are signed, the updater checks each downloaded file's signature with `WinVerifyTrust`, and the expected publisher, before swapping it in (§12).
- **Fuzzing:** every parser is fuzzed with libFuzzer and AddressSanitizer (§10).

### 4.8 Source layout

```
Featherweight/
├─ CMakeLists.txt
├─ cmake/            FetchPdfium.cmake, FetchDeps.cmake (miniz, pugixml, hyphenation patterns:
│                    pinned, SHA-256 checked)
├─ src/
│  ├─ main.cpp, App.*  the shell
│  ├─ ui/            Gfx, Theme, Anim, Widgets, TitleBar, Toolbar, Banner, Tooltip (moved from src/),
│  │                 plus SwapChain, SidePanel, StatusBar, CommandSearch, Uia, Tsf
│  ├─ core/          pure logic (exists), plus Units
│  ├─ pdf/           Document, PdfView, FormFill, RenderCache, TextSelect, Search, Print
│  ├─ pkg/           Snapshot, Zip, XmlDom, XmlPull, Package, RoundTrip, References, Mce, Limits,
│  │                 Cfb, Crypto
│  ├─ draw/          Painter, D2DPainter, PdfPainter, DrawingML, Vml, Shapes, Images
│  ├─ text/          Story, LineLayout, Hyphenation, Fonts, Eot, Editing, SpellCheck, Html
│  ├─ chart/
│  ├─ doc/           model/, docx/, rtf/, txt/, layout/, view/
│  ├─ sheet/         model/, calc/, xlsx/, csv/, view/
│  └─ slide/         model/, pptx/, render/, view/, show/
├─ tests/
│  ├─ unit/          the existing harness, one file per module
│  ├─ fuzz/          one target per parser
│  ├─ office/        public corpus: generated files and permissively licensed test files
│  └─ pdfs/          (exists)
├─ tools/fwtool/     headless command line, not shipped: render, roundtrip, check, compare, dump, bench
├─ docs/, installer/, scripts/, packaging/
└─ third_party/      downloaded at configure time, not committed
```

`text/Html` is a small parser for the subset of HTML and CSS that browsers put on the clipboard, used when pasting into documents and slides.

### 4.9 Third-party code

Only small, permissively licensed code, each pinned to a version:

| Code | Use | License | Adds (§11 size ledger) |
|---|---|---|---|
| PDFium | PDFs (exists) | BSD-style | `pdfium.dll`, about 31 MB unpacked (the V8 build) |
| miniz | zip reading and writing, compression for PDF export | MIT | recorded in the ledger |
| pugixml | XML DOM for most parts. Large parts (sheet data, shared strings) use Featherweight's own streaming reader in `fwpkg`. | MIT | recorded in the ledger |
| Hyphenation patterns | hyph-utf8 and OpenTaal, about 10 languages | per language, checked one by one | a few hundred KB, compressed in the exe |

Everything else comes from Windows:

- Direct3D 11, Direct2D, DirectWrite and WIC
- ICU and Text Services Framework
- the Spell Checking API and the font-subsetting API
- CNG and DPAPI (encryption), WinVerifyTrust (signatures)
- Media Foundation (video in slides)
- UI Automation

**Considered and rejected:**

- **LibreOffice and ONLYOFFICE:** far too heavy, and ONLYOFFICE is AGPL.
- **Commercial SDKs:** paid licenses, large binaries, and little editing UI.
- **IronCalc** as the formula engine: see decision 8.

---

## 5. User interface

### 5.1 Start screen

```
                     🪶  Featherweight

              [   Open…   ]    or drop a file anywhere

        New   [ ▤ Document ]  [ ▦ Spreadsheet ]  [ ▭ Presentation ]

        Recent
        ▤ Report Q3.docx       Documents\Work        2 hours ago
        ▦ Budget 2026.xlsx     Documents             yesterday
        ⬚ Lease.pdf            Downloads             3 days ago
```

- The ＋ in the title bar offers the same choices: a new document, spreadsheet or presentation, or Open.
- Each "New" tile appears once that app's editor ships.
- A few built-in templates arrive in each app's last milestone: a letter, CV and report; a budget and invoice; a plain presentation. They're small and embedded in the exe.

### 5.2 Documents

```
┌───────────────────────────────────────────────────────────────────────────────────┐
│ 🪶 │ report.docx ● │ budget.xlsx × │ ＋                              ─    ▢    ✕ │ title bar + tabs
├───────────────────────────────────────────────────────────────────────────────────┤
│ 💾▾ ↶ ↷ 🖨 │ [Normal ▾] [Aptos ▾] [11 ▾] B I U A▾ 🖍▾ 🖌 │ ≡▾ •▾ 1.▾ ⇤ ⇥ │ ＋Insert ▾ │ 🔍 💬 ⋯ │ toolbar
├───────────────────────────────────────────────────────────────────────────────────┤
│                 ·····|·····|·····|·····|·····|·····|·····                       │ ruler (on demand)
│                        ┌──────────────────────┐                 ║ Comments      ✕ ║
│                        │                      │                 ║                 ║ side panel
│                        │        page          │                 ║                 ║ (when open)
│                        └──────────────────────┘                 ║                 ║
├───────────────────────────────────────────────────────────────────────────────────┤
│ Page 3 of 12 · 1,234 words · English (UK)                          −  ──●──  +  110 % │ status bar
└───────────────────────────────────────────────────────────────────────────────────┘
```

- **Contextual groups:** Table (rows and columns, merge, borders), Picture (wrap, position, crop, alt text), Header and footer.
- **＋ Insert ▾:** picture, table, link, page break, section break, page numbers, date and time, header and footer, footnote, comment, symbol, table of contents, text box, shape.
- **Right-click:** cut, copy and paste options, then whatever fits the spot: spelling suggestions, table, picture or link.
- **⋯ menu:**
  - Save As and Export PDF
  - page setup, hyphenation, ruler, formatting marks, the navigation pane, track changes, word count and properties
  - the app toggles

### 5.3 Spreadsheets

```
│ 💾▾ ↶ ↷ 🖨 │ [Aptos ▾][11 ▾] B I U A▾ ▦▾ ⊞▾ │ ≡▾ ↵ ⊟ │ [General ▾] % , .0 │ Σ▾ ⇅ ⏷ │ ＋Insert ▾ │ 🔍 ⋯ │
├───────────────────────────────────────────────────────────────────────────────────┤
│ B12 ▾ │ fx │ =SUM(B2:B11)                                                       │ formula bar
├───────┬─────────────┬──────────┬──────────┬─────────────────────────────────────────┤
│       │ A           │ B        │ C        │                                         │
│     1 │ Item        │ Amount   │          │                                         │ grid
├───────┴─────────────┴──────────┴──────────┴─────────────────────────────────────────┤
│ ◂ ▸ │ Sheet1 │ Sheet2 │ ＋              Sum: 1,234.00 · Count: 10    −  ──●──  +  100 % │ sheet tabs + status
```

Print preview (in ⋯) shows the sheet as it will print, page by page, using `PagedView`.

### 5.4 Presentations

```
│ 💾▾ ↶ ↷ │ ＋Slide ▾ Layout ▾ │ [Aptos ▾][24 ▾] B I U A▾ ≡▾ •▾ │ ＋Insert ▾ │ Arrange ▾ │ ▶ Present ▾ │ ⋯ │
├─────────┬─────────────────────────────────────────────────────────────────────────┤
│  ▭ 1    │                                                                         │
│  ▭ 2    │                               slide                                     │
│  ▭ 3    │                                                                         │
│         ├─────────────────────────────────────────────────────────────────────────┤
│         │ Click to add speaker notes                                              │
```

### 5.5 Shared

- **Command search (Alt+Q):** type "page num" to find Insert page numbers. Results show their shortcut.
- **Dark mode:** the chrome follows the app theme. Pages, cells and slides stay as their author made them, unless **night reading** is on. Night reading uses the same GPU color matrix as for PDFs: white pages turn dark and colors keep their hue. It's on the ◐ button for PDFs and in ⋯ for the editors.
- **Keyboard:**
  - Office's shortcuts in each app (the essential ones are listed in §6–8), plus Featherweight's own: Ctrl+Tab, Ctrl+W, Alt+Q.
  - Ctrl+Tab always switches tabs. Word uses Ctrl+Tab to type a tab character in a table cell, so Featherweight puts **Insert tab character** in the right-click menu instead.
- **Accessibility:** keyboard access to everything, and text contrast of at least 4.5:1. The basic UI Automation the widgets got in D0 stays; its DLL loads only when a client asks. There's no screen-reader work beyond that (decision 21).
- **Language:** the UI is in English, with all strings in one table (decision 13). Numbers, dates and formula separators follow Windows' regional settings (decision 17).

---

## 6. Documents (Word files)

### 6.1 Formats

| Format | Open | Save |
|---|---|---|
| `.docx` | ✅ | ✅ |
| `.docm` | ✅ macros kept, never run | ✅ macros kept |
| `.dotx`, `.dotm` | ✅ as a new document based on the template | ✅ via Save As |
| `.rtf` | ✅ | ✅ in place if everything was understood, otherwise Save As `.docx` |
| `.txt` | ✅ encoding detected | ✅ in its original encoding and line endings |
| `.doc` (Word 97–2003) | ✅ imported (D5) | Save As `.docx` |
| `.odt` | ✅ imported (D5) | Save As `.docx` |
| `.pdf` | the PDF reader | ✅ Export PDF |

RTF is needed anyway: it's how formatted text travels to and from Word through the clipboard. `.txt` files open as documents. `.csv` and `.tsv` open as spreadsheets.

### 6.2 Features

**Basic** (all in place before 2.0):

- **Text:**
  - typing, and selecting with the mouse and keyboard (words, lines, paragraphs)
  - cut, copy and paste (keep formatting, merge formatting, text only), and drag and drop
  - undo and redo, find and replace (match case, whole words), go to page
- **Characters:** font, size, grow and shrink, bold, italic, underline, strikethrough, subscript and superscript, font color, highlight, change case, clear formatting, format painter
- **Paragraphs:**
  - alignment, and indents (left, right, first line, hanging)
  - spacing before and after, and line spacing
  - tab stops: left, center, right and decimal, with leaders
  - a ruler, shown on demand, for dragging indents and tab stops
  - borders and shading
  - keep with next, keep lines together, page break before, widow control
- **Styles:** a gallery of built-in and document styles (Normal, Title, Heading 1–9, Quote and so on); modify a style; create a style from the selection; theme fonts and colors
- **Lists:**
  - bullets, numbering and multilevel lists
  - Tab and Shift+Tab change the level
  - restart or continue numbering
  - typing "1. " or "- " starts a list
- **Tables:**
  - insert a table, and Tab through its cells (a new row at the end)
  - insert and delete rows and columns, merge and split cells
  - drag column borders, autofit
  - borders and shading, table styles, repeating header row, vertical alignment in cells
- **Pictures:**
  - insert from a file, the clipboard, or drag and drop
  - resize, rotate and crop
  - in line with text or floating (square, tight, top and bottom, behind, in front)
  - alt text
- **Page layout:**
  - page size, orientation and margins
  - columns (1–3), page breaks, and section breaks (next page, continuous)
  - a different first page, and different odd and even pages
  - hyphenation: automatic hyphenation on or off per document, optional hyphens (Ctrl+-) and non-breaking hyphens (Ctrl+Shift+-)
- **Headers and footers:** edit them, add page numbers (including "Page X of Y"), link to the previous section
- **Links and fields:**
  - insert, edit and remove hyperlinks
  - insert the date and time
  - page number, page count and date fields update; other fields show their last result
- **Review:**
  - comments: show, add, reply, resolve and delete
  - tracked changes: show them, and accept or reject them one by one or all at once
- **Proofing:**
  - spelling as you type, with suggestions, add to dictionary and ignore
  - set the language of the selection
  - word count
  - AutoCorrect basics: smart quotes, dashes, (c) → ©, a capital at the start of a sentence, automatic lists and links. Each correction can be undone with one Ctrl+Z.
- **View:** page view with zoom (fit width, one page, side by side), a navigation pane with headings, formatting marks (¶)
- **Output:** print and Export PDF
- **Safety:** crash recovery, a backup of the original on the first save, and a banner when another program changes the file

**Advanced** (included):

- Recording tracked changes, with your name (taken from Windows, changeable in ⋯)
- Table of contents: insert it from headings, and update it
- Footnotes and endnotes: insert and edit
- Content controls and legacy form fields: fill in text, check boxes, dropdowns and dates, like forms in the PDF reader
- Text boxes and basic shapes: insert and edit
- Bookmarks, and links to them
- Text watermark, page color, page borders
- Charts shown (basic types, from D5)
- Equations shown (editing them is not included)
- Password-protected documents: open, and save with the same password
- Right-to-left and complex scripts (Arabic, Hebrew, Indic), and East Asian line breaking

**Kept but not editable:**

- SmartArt (shown through its stored drawing)
- Charts (basic types shown from D5, others as a placeholder; charts are edited only in spreadsheets)
- Equations
- Citations and bibliography, indexes, mail merge fields
- Embedded objects, ActiveX controls, ink and macros

**Not included:** running macros, mail merge, grammar checking, translation, co-authoring, add-ins, master documents, and outline and draft views.

### 6.3 Engine

**Model**

- **Stories:** the main text, headers, footers, footnotes, endnotes, comments and text boxes. Each story is a list of blocks: paragraphs, tables and block-level content controls.
- **Paragraphs:** each has properties (its style plus direct formatting), its text as UTF-16 with attribute runs, and inline items:
  - fields, as begin, separator and end markers, which may span several paragraphs
  - bookmarks, comment ranges, revision marks, hyperlinks and content controls
  - pictures and other drawings (DrawingML or VML), inline or anchored

  Each paragraph has a stable id for caches and operations, and keeps its `w14:paraId`.
- **Pairs:** field markers, bookmark and comment-range ends, permission ranges and move ranges are tracked as pairs in the reference registry. Deleting text that contains one end deletes or repairs the whole pair, so no orphan ends are written.
- **Sections:** properties are stored where Word stores them: in the last paragraph of each section, and at the end of the body.
- **Property resolution** follows Word: ECMA-376 §17.7.2 as amended by [MS-OI29500], with a unit test for each departure from the standard. The order is:
  1. document defaults
  2. table style, with conditional formatting (header row, banding and so on)
  3. numbering
  4. the paragraph style chain
  5. character style
  6. direct formatting

  Toggle properties such as bold combine across the style chain: a bold character style inside a bold paragraph style turns bold off. Direct formatting always sets an absolute value. Resolved properties are cached per run, and style edits invalidate the cache.
- **Compatibility settings:** `compatibilityMode` and the `w:compat` flags change how Word lays out a document.
  - Mode 15 (Word 2013 and later) comes first.
  - The flags that matter most for older documents come in D5, chosen by measuring the corpus.

**Layout**

- **Lines:** `fwtext` line layout with Word's rules:
  - tab stops, and justification that stretches only spaces
  - line height (auto, at least, exactly, multiple)
  - spacing before and after, including contextual spacing
  - automatic hyphenation when the document asks for it
  - East Asian rules (D5)
- **Pages:**
  - sections and columns, headers and footers
  - footnotes: space reserved at the bottom of the page, continued on the next page
  - floating pictures and text boxes with text wrapping, anchored to a paragraph, the page or the margin
  - keep-with-next chains, and widow and orphan control
  - tables split across pages, including rows that can't split and repeating header rows
  - page and section breaks
- **Incremental:** each paragraph's lines are cached by paragraph version, width and wrap zones. After an edit, pagination restarts from the first changed page and stops once the page starts match the previous layout again.
- **Fast open:** the first visible pages are laid out first, and the rest are paginated at idle. The page count shows once it's known.

**Editing**

- **Positions** are (story, paragraph id, offset). A selection can be a range of text, a rectangle of table cells, or an object.
- **Clipboard:**
  - Copying puts an internal format (an exact piece of the model) on the clipboard, plus RTF, HTML and plain text.
  - Pasting takes RTF from Word, HTML from browsers, or plain text. Pasted content gets new unique ids (§4.3 rule 2).
  - Pictures in pasted web content follow decision 12.
- **Typing:** each keystroke re-lays out one paragraph and repaints. If the paragraph's height changes, repagination continues in the background.
- **Text boxes and shapes** come from Word as a DrawingML Choice with a VML Fallback, each with its own copy of the text. After an edit, Featherweight writes a new VML fallback for text boxes and simple shapes, and drops it for anything else (§4.3 rule 1).

**Saving**

1. A copy-on-write snapshot of the model is taken on the UI thread, which costs almost nothing.
2. A worker thread serializes the snapshot and the round-trip store, copies unchanged parts from the original snapshot, and compresses the result.
3. It checks whether the file on disk has changed (§4.3 rule 3).
4. It writes a temporary file and swaps it in with `ReplaceFileW`, as today.

- Revision ids (`w:rsid`) are kept but never added. Document properties (modified date, last modified by) are updated.
- **Working alongside Word:**
  - If Word has the file open (its `~$` owner file exists, or the file is locked), a banner says so before you start editing, and Save falls back to Save As.
  - While Featherweight has unsaved edits, it writes a Word-compatible `~$` owner file, so Word users see the file is in use.

**Essential shortcuts:**

- Ctrl+B, Ctrl+I, Ctrl+U: bold, italic, underline
- Ctrl+L, Ctrl+E, Ctrl+R, Ctrl+J: align left, center, right, justify
- Ctrl+1, Ctrl+2, Ctrl+5: single, double and 1.5 line spacing
- Ctrl+Alt+1 to Ctrl+Alt+3: Heading 1 to 3
- Ctrl+Shift+N: Normal style
- Ctrl+Space: clear character formatting
- Ctrl+Shift+C, Ctrl+Shift+V: copy and paste formatting
- Ctrl+Enter: page break
- Ctrl+K: insert a link
- Ctrl+H: replace
- Ctrl+Shift+8: formatting marks
- F7: spelling
- Shift+F3: change case

### 6.4 Milestones

Every milestone also passes the lightweight gate (§11): its budgets are met, and nothing is slower or larger than at the previous milestone without a recorded reason.

| Milestone | Work | Done when |
|---|---|---|
| **D0. Host and foundations** | **Shell:** `TabContent`, `PagedView`, the command and toolbar model, side panel, status bar, command search. Rendering on Direct3D 11 with a flip-model swap chain. New widgets (dropdown, combo box, split button, color picker, spin box, list), with UI Automation for every widget. The file association code generalized for all kinds of file (each kind is registered when its viewer ships, §12), the start screen and recent files.<br><br>**Engines:** `Painter` with the Direct2D backend and `ID2D1PrintControl`. `fwpkg`: snapshot, zip with raw copy, XML (DOM and streaming), parts, relationships, content types, round-trip store with fragment rules, the reference registry framework, Markup Compatibility, limits. Typed units.<br><br>**Testing:** `fwtool` (`roundtrip`, `check`, `dump`), the corpus and reference-render pipeline, fuzzing in CI. | The PDF reader works exactly as before. The manual checklist passes. `visual-checks.ps1` images are pixel-identical, except the start screen, which is reviewed by hand.<br><br>`fwtool roundtrip` copies every corpus file's untouched parts byte for byte.<br><br>The zip and XML fuzzers run 8 hours without findings.<br><br>Reference PDFs exist for the public corpus.<br><br>Launch to window, opening a PDF and memory after launch are no worse than 0.9.4. The widget gallery is compiled only into test builds, and the size ledger has its first baseline. |
| **D1. Document viewer** (release 1.1) | **Model:** the `.docx` model, reader and writer (the writer is used by round-trip tests; saving comes in D2), styles and numbering.<br><br>**Drawing:** the VML subset (reading).<br><br>**Layout:** `fwtext` line layout with hyphenation. Pagination: sections, columns, headers and footers, footnotes, tables, inline and floating pictures, shapes and text boxes.<br><br>**View:** fields show their last results; comments and tracked changes shown; links; selection and copy; find; print; fonts and fallbacks; banners for unsupported content. | Across the corpus, with Office's fonts: the page count matches Word for ≥ 90 % of documents, ≥ 95 % of lines break where Word breaks them, and nothing crashes. Targets for Windows-only fonts are set from this first measurement.<br><br>The open-and-save round trip produces no new validator errors and passes `fwtool check` for 100 % of files.<br><br>In re-serialize-all mode (§10), every part equals the original after canonicalization, with each exception documented.<br><br>The D1 budgets in §11 are met: opening a typical and a 300-page `.docx`, memory, and the exe under 4 MB. |
| **D2. Editing** | **Editing:** caret, selection, TSF input, clipboard (internal, RTF, HTML, text), undo and redo, typing and deleting, find and replace.<br><br>**Formatting:** character and paragraph formatting, format painter, styles, lists, date and time.<br><br>**Files:** new document, save, crash recovery, backups, change detection, the `~$` owner file.<br><br>**Proofing:** spelling and the selection's language, AutoCorrect, word count, status bar. | A 5-page report using text, styles and lists can be written from scratch, and the text of a received document edited.<br><br>Every corpus file, after scripted random edits (including deletes across fields, bookmarks and comments) and a save: no new validator errors, passes `fwtool check`, and opens in Word without a repair prompt.<br><br>The typing budget is met in a 300-page document. |
| **D3. Tables, pictures and layout** | Table editing; pictures (insert, resize, wrap, position, crop); page setup, columns, section breaks and hyphenation; the ruler; editing headers and footers; page numbers; hyperlinks and bookmarks; navigation pane; formatting marks | Every item under "basic" in §6.2 works, except review and Export PDF, which come in D4. The friction review passes for writing a letter, formatting a CV, and fixing a table in a received document. |
| **D4. Review and advanced** | **Review:** comments (add, reply, resolve), tracked changes (accept, reject, record).<br><br>**Advanced:** table of contents, footnotes and endnotes, content controls and form fields, text boxes and shapes, watermark (writing VML).<br><br>**Output:** `PdfPainter` and Export PDF; saving RTF and TXT. | Everything under "basic" in §6.2 works.<br><br>A contract with tracked changes and comments can be reviewed end to end, and Word shows the result correctly.<br><br>Exported PDFs have selectable text, links and bookmarks. |
| **D5. Breadth, and release 2.0** | **Files:** `.doc` and `.odt` import, password-protected files, templates.<br><br>**Layout:** charts shown (`fwchart`, read-only: column, bar, line, pie, area and scatter, from the chart's cached values); equations shown; compatibility modes for older documents; right-to-left and East Asian checks.<br><br>**Quality:** performance on huge documents, a full corpus pass, a friction review of every flow. | Final targets with Office's fonts: page count matches for ≥ 97 % of documents, and ≥ 98 % of lines break where Word breaks them. The Windows-only-fonts targets set in D1 are met.<br><br>All budgets met, on the typical consumer PC. The checklist passes.<br><br>Release **2.0**. |

The fidelity numbers are starting targets. They're adjusted once D1 has measured the corpus, and after 2.0 they only go up.

---

## 7. Spreadsheets (Excel files)

### 7.1 Formats

| Format | Open | Save |
|---|---|---|
| `.xlsx` | ✅ | ✅ |
| `.xlsm` | ✅ macros kept, never run | ✅ macros kept |
| `.xltx`, `.xltm` | ✅ as a new workbook | ✅ via Save As |
| `.csv`, `.tsv` | ✅ | ✅ (see below) |
| `.xls` (Excel 97–2003) | ✅ imported (S4) | Save As `.xlsx` |
| `.ods` | ✅ imported (S4) | Save As `.xlsx` |
| `.pdf` | — | ✅ Export PDF |

`.xlsb` is not included.

**CSV without damage.** Excel is known for changing CSV data: leading zeros vanish, long numbers turn into 1.23E+15, and codes that look like dates become dates.

- **Opening:**
  - A value is kept as text whenever converting it would lose information.
  - The encoding and the delimiter (comma, semicolon, tab or pipe) are detected.
- **Saving an existing CSV:**
  - It keeps its encoding, byte-order mark, delimiter, quoting and line endings.
  - Untouched rows are written exactly as they were read.
- **When the workbook holds more than CSV can store** (formatting, more sheets, formulas you want to keep), the first save shows a banner: *"CSV keeps only the values of the first sheet."* with **Save as .xlsx** and **Keep CSV**.
- **New CSV files:** UTF-8 with a byte-order mark, so Excel shows accented letters correctly, and the regional list separator (decision 17).

### 7.2 Features

**Basic** (all in place before 3.0):

- **Grid:**
  - 1,048,576 rows × 16,384 columns
  - sheets: add, rename, move, delete, color, hide
  - freeze panes, hide rows and columns, merged cells, gridlines, zoom
  - row heights and column widths, with double-click to fit
  - outline groups: expand and collapse them, group and ungroup rows and columns
- **Entering data:**
  - type values and formulas; F2 edits in the cell; a formula bar
  - Enter, Tab and the arrow keys behave as in Excel
  - AutoComplete from the column, and Ctrl+Enter fills the selection
  - the fill handle: copies, and series of numbers, dates, weekdays and months
- **Formula entry:**
  - while typing a formula, clicking or using the arrow keys inserts a reference
  - referenced ranges get colored borders
  - function names autocomplete, with argument tips
- **Editing:**
  - cut, copy and paste, plus paste special (values, formats, formulas, transpose)
  - insert and delete rows, columns and cells
  - find and replace, go to, undo and redo
- **Formulas:**
  - references (relative, absolute, mixed, other sheets), names and operators
  - about 150 functions in the first editor release: the most used ones, counted in the corpus
  - AutoSum, and error values
- **Formatting:**
  - number formats: general, number, currency, accounting, date, time, percent, fraction, scientific, text and custom
  - font, fill and borders
  - alignment: horizontal, vertical, wrap and indent
  - cell styles and format painter
- **Data:** sort by one or more columns; filter (value lists, text and number filters); remove duplicates
- **Charts:** column, bar, line, pie, doughnut, area, scatter and combo. Create one from a selection, and change its type, title, legend, data labels and colors.
- **Comments and notes:** show, add, reply, delete
- **Printing:**
  - print preview
  - print area, print titles (rows or columns repeated on every page)
  - fit to page width or to one page, orientation, margins
  - headers and footers, gridlines
  - Export PDF
- **Status bar:** sum, average and count of the selection

**Advanced** (included):

- About 350 functions in total, including:
  - dynamic arrays with spilling (FILTER, SORT, UNIQUE, SEQUENCE, XLOOKUP)
  - LET, and text functions such as TEXTSPLIT
- Tables: create them, with styles, a total row and structured references (`Table1[Amount]`)
- Conditional formatting, shown and created: highlight rules, top and bottom, data bars, color scales, icon sets
- Data validation: dropdown lists shown and created; other rules enforced
- Named ranges: define and manage
- Pivot tables: refresh them, and create simple ones (rows, columns, filters; sum, count, average) (decision 18)
- Sheet protection: respected, and unprotect with a password
- Hyperlinks; inserting pictures and shapes
- Sparklines (shown)
- Password-protected workbooks

**Kept but not editable:**

- Pivot table features beyond the simple ones: calculated fields, grouping, and pivots on the data model
- Slicers and timelines
- Power Query and data connections
- Links to other workbooks (their last values are shown)
- Macros and form controls
- Chart types beyond those listed above, shown through Office's preview where the file has one
- What-if analysis and Solver

**Not included:** running macros, refreshing external data, Power Pivot, 3D Maps, co-authoring, `.xlsb`.

### 7.3 Engine

**Model**

- **Cells:**
  - Sheets store cells sparsely, in copy-on-write blocks of rows, aiming for about 16–32 bytes per cell with a value.
  - Strings live in a shared table, and formats are deduplicated.
  - Unknown attributes that repeat on every row or cell are interned sets (§4.3 rule 1).
  - Formulas are shared where the file shares them.
- **Cached values:** an `.xlsx` file stores the value Excel last calculated for every formula, and Featherweight shows those values when it opens the file. In automatic calculation mode, Excel recalculates some cells every time it opens a file, so Featherweight then recalculates the same ones in the background:
  - volatile functions (TODAY, NOW, RAND, OFFSET, INDIRECT and so on) and everything that depends on them, so a due-date column shows today's result, not the date of the last save
  - formulas with no cached value, which is common in files written by other programs
  - the whole workbook when the file sets `fullCalcOnLoad`

  Manual calculation mode is respected.
- **Unsupported functions:** a formula that uses one keeps its last value, marked as possibly out of date, and a banner says so.
  - On save, the formula and its value are written back, and Excel is told to recalculate on open (`fullCalcOnLoad`).
  - Featherweight never invents a value.

**Writing formulas the way Excel expects:**

- Newer functions are written with their `_xlfn.` prefix (and `_xlws.` where needed), as Excel stores them. Otherwise Excel shows #NAME?.
- Dynamic-array formulas carry the cell-metadata attribute (`cm`) and the matching `metadata.xml` entries. Otherwise Excel treats them as old-style array formulas.
- The calculation chain (`calcChain.xml`) is dropped whenever a formula changes. Excel rebuilds it.

**Structural edits:** inserting or deleting rows and columns, and renaming, moving or deleting sheets, go through the reference registry. It covers:

- formulas and defined names
- print areas and titles
- merged cells, hyperlinks, tables and filters
- conditional formats and data validations, including the newer ones stored in extension lists
- sparklines, chart series, pivot sources, and note anchors in VML

If an unknown extension in the workbook could hold references, the edit is blocked with a banner (§4.3 rule 2).

**Formula engine**

- **Parser:** A1 and R1C1 references, sheets, names, structured references and arrays. References to other workbooks are kept but not resolved.
- **Evaluation:** formulas compile to compact bytecode. Ranges are evaluated lazily, without copying cells.
- **Dependency graph:**
  - Ranges are tracked as ranges, so `SUM(A:A)` is one dependency, not a million.
  - Dirty propagation and recalculation in topological order; volatile functions.
  - Circular references are detected and shown in a banner, and iterative calculation runs when the file asks for it.
- **Large recalculations** are time-sliced on the UI thread at first, with "Calculating…" in the status bar. Multi-threaded recalculation comes in S4.
- **Excel's behavior is matched where it matters:**
  - 15 significant digits
  - the 1900 and 1904 date systems, including 1900 as a leap year
  - error propagation and implicit intersection
  - comparisons between text and numbers

**Locale** (decision 17):

- Files always store formulas in Excel's invariant form.
- The formula bar and cell entry use English function names, and Windows' decimal and list separators, as Excel does. With Dutch regional settings, for example, you type `=SUM(A1;2,5)`.
- Dates are typed in the regional order.

**Number formats:** one engine for all of Excel's format codes: sections, conditions, colors, locale-aware separators, dates and times, fractions, and "####" when a value doesn't fit.

**Loading:**

- Sheet data and shared strings are read with `fwpkg`'s streaming XML reader, not a DOM, starting with the active sheet.
- CSV files are read into memory once and parsed in parallel chunks.

**Grid view:**

- It draws only visible cells, with cached text layouts.
- Text overflows into empty neighboring cells, as in Excel.
- Column widths follow Excel's rule, based on the widest digit in the default font, so they match.

**Essential shortcuts:**

- F2: edit the cell
- Ctrl+Enter: fill the selection
- Ctrl+; and Ctrl+Shift+;: today's date and the current time
- Alt+=: AutoSum
- Ctrl+1: format cells
- Ctrl+Shift+L: filter
- Ctrl+arrow keys: jump to the edge of the data
- Ctrl+Space, Shift+Space: select the column, the row
- Ctrl+PgUp, Ctrl+PgDn: previous and next sheet
- F4: switch between relative and absolute references
- Alt+Shift+→, Alt+Shift+←: group, ungroup

### 7.4 Milestones

Every milestone also passes the lightweight gate (§11): its budgets are met, and nothing is slower or larger than at the previous milestone without a recorded reason.

| Milestone | Work | Done when |
|---|---|---|
| **S1. Spreadsheet viewer** (release 2.1) | **Files:** reading `.xlsx` and CSV, using the streaming XML reader.<br><br>**Display:** styles and number formats, the grid, merged cells, frozen panes, outline groups (expand and collapse), sheets, conditional formatting and tables, pictures and shapes, VML notes and comments.<br><br>**Charts:** `fwchart` completed: every type in §7.2, in workbooks and documents.<br><br>**View:** selection and copy, find, print.<br><br>**Formulas:** the parser, the dependency graph, background recalculation after opening (§7.3), and the first ~50 functions. | ≥ 99 % of non-empty cells show the same text as Excel's PDF export, and ≥ 90 % of pages pass the pixel threshold (§10).<br><br>CSV files open without any value changing.<br><br>Of the corpus formulas that use only implemented, non-volatile functions, ≥ 99.5 % reproduce Excel's cached values, and every mismatch is documented. |
| **S2. Editing** | **Editing:** cells and the formula bar, formula entry (point mode, colored ranges, function tips), the fill handle, clipboard with paste special, inserting and deleting with the reference registry.<br><br>**Formulas:** about 150 functions, AutoSum, the `_xlfn` and calculation-chain rules.<br><br>**Formatting:** formats, column widths and row heights, format painter.<br><br>**Workbook:** sheets, sort and filter, freeze panes, outline groups.<br><br>**Files:** saving `.xlsx` and CSV, crash recovery. | A household budget and a small invoice can be built from scratch.<br><br>Every corpus workbook, after scripted random edits (including inserting and deleting rows, columns and sheets) and a save: no new validator errors, passes `fwtool check`, and opens in Excel without a repair prompt.<br><br>Recalculation budgets met. |
| **S3. Charts, tables and more** | **Create:** charts (also edited), tables, conditional formatting, data validation.<br><br>**Formulas:** dynamic arrays, about 350 functions, names.<br><br>**More:** protection, print preview and setup (print area, titles, scaling), comments and notes (writing VML), hyperlinks, pictures, Export PDF. | Everything under "basic" in §7.2 works, and everything under "advanced" except S4's items. |
| **S4. Breadth, and release 3.0** | **Files:** `.xls` and `.ods` import, password-protected files, templates.<br><br>**Data:** refreshing pivot tables and creating simple ones.<br><br>**Scale:** multi-threaded recalculation, huge files.<br><br>**Quality:** sparklines, a full corpus pass, a friction review. | All budgets and targets met, on the typical consumer PC. Release **3.0**. |

---

## 8. Presentations (PowerPoint files)

### 8.1 Formats

| Format | Open | Save |
|---|---|---|
| `.pptx` | ✅ | ✅ |
| `.pptm` | ✅ macros kept, never run | ✅ macros kept |
| `.potx`, `.potm` | ✅ as a new presentation | ✅ via Save As |
| `.ppsx` | ✅ straight into the slide show; Esc leaves to the editor | ✅ |
| `.ppt` (PowerPoint 97–2003) | ✅ imported (P3) | Save As `.pptx` |
| `.odp` | ✅ imported (P3) | Save As `.pptx` |
| `.pdf` | — | ✅ Export PDF (slides, handouts, notes) |

### 8.2 Features

**Basic** (all in place before 4.0):

- **Slides:** new slide with a layout, duplicate, delete, reorder (drag in the thumbnails), hide, change layout, slide size (16:9 or 4:3)
- **Text:**
  - placeholders and text boxes with everything `fwtext` offers (formatting, bullets, numbering)
  - autofit: text shrinks when it overflows
  - find and replace
  - spelling as you type
- **Objects:**
  - insert pictures, shapes, text boxes, tables and charts
  - move, resize and rotate, with smart guides and snapping
  - align and distribute, group, bring forward and send back
  - fill, outline and shadow; crop pictures
- **Design:** a few built-in themes, theme colors and backgrounds
- **Slide numbers, footer and date**
- **Hyperlinks:** insert and edit; they work during the slide show
- **Notes:** speaker notes
- **Slide show:**
  - start from the beginning (F5) or the current slide (Shift+F5)
  - click or use the arrow keys to move on, type a number and press Enter to jump, B or W for a black or white screen
  - common transitions: fade, push, wipe, split, cover, uncover, zoom. Others play as a fade.
  - common animations: appear, fade, fly in, wipe, zoom, float, split, grow and shrink, spin. Others play as appear.
  - animations on click, with the previous one, or after the previous one
- **Copying slides between presentations,** keeping the source design or using the destination's
- **Output:** print slides and handouts; Export PDF

**Advanced** (included):

- Presenter view on a second monitor: the current and next slide, notes and a timer
- Adding and changing transitions and the common animations, with their order and timing
- Video and audio in slide shows (Media Foundation, any format Windows can play)
- Comments: show, add, reply
- Password-protected presentations
- Exporting slides as pictures (PNG)

**Kept but not editable:**

- SmartArt (shown through its stored drawing)
- Morph and other complex transitions (played as a fade)
- Motion paths and complex animation sequences (played in a simplified form)
- 3D models (shown as their preview)
- Zoom slides, embedded objects, ink and macros

**Not included:** recording slide shows or narration, live captions, design suggestions, co-authoring, broadcasting.

### 8.3 Engine

- **Inheritance:** slide → layout → master → theme, for placeholders, text styles, backgrounds and colors. These are resolved and cached like document styles.
- **Rendering:** `fwdraw` and `fwtext`, including fonts embedded as Embedded OpenType (§4.4). Autofit uses the scale PowerPoint stored in the file (`normAutofit`) until the text is edited; after that, Featherweight fits the text itself.
- **Slide operations** go through the reference registry. Deleting or reordering a slide updates the presentation's slide list, sections (stored in an extension list), custom shows, and links between slides.
- **Thumbnails** are rendered at idle and cached as bitmaps.
- **Slide show:**
  - full screen on the chosen monitor, with the next slide pre-rendered
  - transitions as Direct2D animations at the display's refresh rate
  - an interpreter for the common subset of PowerPoint's animation timing
  - video through Media Foundation into the shared Direct3D 11 device (D0)
  - input and cursor hiding reused from the PDF reader's presentation mode

**Essential shortcuts:**

- Ctrl+M: new slide
- Ctrl+D: duplicate
- Ctrl+G and Ctrl+Shift+G: group and ungroup
- F5 and Shift+F5: slide show
- Esc: end the show
- B and W: black and white screen during the show
- plus the text shortcuts from §6.3

### 8.4 Milestones

Every milestone also passes the lightweight gate (§11): its budgets are met, and nothing is slower or larger than at the previous milestone without a recorded reason.

| Milestone | Work | Done when |
|---|---|---|
| **P1. Viewer and slide show** (release 3.1) | **Files:** reading `.pptx` (and `.ppsx`), including Embedded OpenType fonts.<br><br>**Display:** inheritance, rendering, thumbnails, notes.<br><br>**Slide show:** common transitions and animations, hyperlinks.<br><br>**Output:** print, Export PDF. | ≥ 90 % of corpus slides pass the pixel threshold against PowerPoint's PDF exports (§10).<br><br>On the typical consumer PC, a 50-slide deck presents at the display's refresh rate with no dropped frames during transitions. |
| **P2. Editing** | **Slides:** slide operations with the reference registry, notes, themes, slide size, slide numbers, footer and date, copying slides between decks.<br><br>**Content:** text editing with autofit, find and replace, spelling, hyperlinks; objects (insert, move, resize, rotate, smart guides), formatting, arrange.<br><br>**Files:** save, crash recovery. | A 10-slide deck can be built from scratch.<br><br>Every corpus deck, after scripted random edits (including deleting and moving slides) and a save: no new validator errors, passes `fwtool check`, and opens in PowerPoint without a repair prompt. |
| **P3. Advanced, and release 4.0** | **Presenting:** presenter view, editing transitions and animations, video and audio.<br><br>**Review:** comments.<br><br>**Files:** `.ppt` and `.odp` import, password-protected files, templates, picture export.<br><br>**Quality:** a friction review. | All targets met, on the typical consumer PC. Release **4.0**. |

---

## 9. Phases at a glance

| Release | What it adds | Milestones | Estimated size |
|---|---|---|---|
| **1.0** | The PDF reader as it is today | (exists) | 12,000 lines |
| **1.1** | Opens Word documents | D0, D1 | ~50,000 lines |
| **2.0** | Edits Word documents | D2–D5 | ~95,000 lines |
| **2.1** | Opens Excel workbooks and CSV files | S1 | ~30,000 lines |
| **3.0** | Edits spreadsheets | S2–S4 | ~90,000 lines |
| **3.1** | Opens and presents PowerPoint decks | P1 | ~20,000 lines |
| **4.0** | Edits presentations | P2, P3 | ~45,000 lines |

The sizes are rough estimates of new C++ code, about 330,000 lines in total, or roughly 27 times the PDF reader. They're there to show the scale and the order, not to set a schedule.

---

## 10. Testing

### Test machines

- **Development PC:** this PC (AMD Ryzen 7 7800X3D, 192 GB RAM, RTX 5090). It's much faster than what most users have, so it catches regressions but doesn't prove the budgets.
- **Typical consumer PC:** 4 cores, 8 GB RAM, integrated graphics and an SSD, for example an inexpensive laptop from a few years ago. The lightweight contract's budgets (§11) apply here (decision 19).
- **Office reference PC:** the development PC, which has Microsoft 365 (current channel) (decision 19). It makes the reference renders and checks for repair prompts.

### Corpus

- **Public corpus, in `tests/office/`:**
  - files generated by scripts (python-docx, openpyxl, python-pptx), like `make_test_pdfs.py`
  - permissively licensed test files, for example Apache POI's test data (Apache-2.0)

  Every feature in §6–8 has at least one file.
- **Private corpus, outside the repository** (its path is in `FW_CORPUS`): real-world documents that can't be published.
  - Aim for at least 1,000 files per format by each viewer release.
  - Weight it toward what consumers actually receive: letters, CVs, reports, forms, invoices, budgets, school assignments and decks.
  - Legacy formats come from public collections such as Govdocs1.
- **Tags:** files are tagged by the structures they contain, so tests can pick them. Examples: fields, bookmarks, comments, tracked changes, extension-list conditional formats, sparklines, sections, charts, VML, hyphenation, right-to-left text.
- **Reference renders:** Office exports each corpus file to PDF once on the Office reference PC, with its fonts recorded (`scripts/reference-renders.ps1`). The PDFs and a manifest are stored beside the corpus, in `tests/office/reference/` for the public corpus, and re-exported when Office changes noticeably.

### Fidelity metrics

`fwtool compare` computes these. It reads the reference PDFs with PDFium.

- **Page count:** do they match?
- **Line breaks:**
  - For each line in the reference PDF (from PDFium's text API), does Featherweight's line start with the same word? This is the best single measure of Word compatibility.
  - Both texts are aligned again at the start of every paragraph, so one difference can't shift all the comparisons after it.
  - Lines in columns, table cells and text boxes are matched by their text and the region of the page they're in.
- **Pixels:** both renders at 100 dpi, page origins aligned, compared per page with structural similarity (SSIM). A page passes at ≥ 0.95. This catches missing objects, wrong colors and wrong positions.
- **Spreadsheets:** displayed cell values (the formatted text) must match, as well as the pixels.
- **Two font sets:** every metric is computed with the reference PC's fonts, and again with only the fonts that ship with Windows (plus fonts embedded in the file). Releases are gated on both.

Results are tracked per build, and a drop fails CI. The worst 20 files per format are reviewed at every milestone.

### Never-damage tests

These run on every build, for every corpus file:

- **Open and save, no edits:** untouched parts are byte-identical, the validator reports no new errors, and `fwtool check` passes.
- **Re-serialize all:** every part is written from the model, even untouched ones, and must equal the original after XML canonicalization (namespace prefixes, attribute order and whitespace normalized). This tests the writer on all content, not just edited content.
- **Open, edit and save:** random scripted edits, then save, validate, check and reopen. The reopened model must equal the edited one.
  - The edits include the structural operations from §4.3 rule 2: deleting text across fields, bookmarks and comments; pasting; inserting and deleting rows, columns and sheets; deleting and moving slides.
  - They run on corpus files tagged with those structures.
- **`fwtool check`, the consistency checker:**
  - It's written independently of the model, from the reference registry's rules, so it can see damage to content the model doesn't understand.
  - It checks that every reference resolves, that ids are unique, that pairs are complete, and that derived data such as the calculation chain matches the content.
- **Office repair check:** edited files are opened in Office through automation on the Office reference PC, and any repair prompts are recorded.
  - It's a hard gate for D2, S2 and P2 and for every release, and there must be none.
  - Between gates, it runs weekly as an early warning.

### Other tests

- **Formulas:** every formula in the corpus is recalculated and compared with Excel's cached value, excluding volatile functions. A mismatch is a bug unless it's documented.
- **Unit tests:** the existing harness, per module: units, property resolution (including each [MS-OI29500] departure), numbering, line breaking and hyphenation (with fixtures), the formula parser, number formats, CSV detection, the dependency graph, the round-trip store and fragment rules, and Markup Compatibility.
- **Fuzzing:** libFuzzer with AddressSanitizer, seeded from the corpus.
  - Targets: zip, XML (DOM and streaming), CFB, `.docx`, `.xlsx`, `.pptx`, VML, RTF, HTML, CSV, Embedded OpenType, the legacy formats and the formula parser.
  - Short runs in CI on every push, and long runs nightly.
  - Every crash becomes a regression test.
- **Visual checks and smoke tests:** `visual-checks.ps1` grows to cover every new screen. Snapshot mode (`--snapshot`) works for every kind of file, and CI renders one file of each kind.
- **Size and speed (§11):** `fwtool bench` and the budget script measure the exe and download size, launch, opening, layout, recalculation, typing, memory and idle CPU on fixed files, without touching the user's settings. CI gates on size and tracks timings; the development PC gates timings; the typical consumer PC checks the whole table before each release.
- **Keyboard and UI Automation:** `tests/uia-checks.ps1` drives every widget through UI Automation and the keyboard. No manual screen-reader passes (decision 21).
- **Manual checklist:** `docs/TEST_CHECKLIST.md` gets a section per app.
- **Real use:** from each editor milestone on, Featherweight is used for real work every day, and every surprise becomes a test.

---

## 11. Lightweight contract

Small, fast, no bloat and zero friction are the lightweight promise (§1). They're held to the same standard as the compatibility contract (§3): written down as budgets, measured on every build, and a gate for every milestone and release. The budgets are decision 22.

### Small

| Budget | Target |
|---|---|
| `Featherweight.exe` | < 4 MB at 1.1, < 7 MB at 2.0, < 11 MB at 3.0, < 15 MB at 4.0 |
| Download (installer or zip, with PDFium) | < 25 MB |
| Memory right after launch, no file open | < 40 MB |
| Memory with a typical document, workbook or deck | < 150 MB |
| Round-trip store for kept content | < 25 % of the model's own memory |

- **One exe.** PDFium is the only separate DLL, loaded on first use. Nothing else is installed beside it: no runtimes (the C runtime is linked in), no .NET, no web view, no fonts.
- **Nothing in the background.** No services, scheduled tasks, tray icons or helper processes. The update check runs only while the app is open, as today.
- **Nothing that grows on disk** beyond settings, recovery journals (§13) and the 30-day backups (§2 principle 9).
- **A size ledger:** a committed baseline records the exe and download size, and the reason for every accepted increase, so growth is a decision and never an accident.

### Fast

On the typical consumer PC (§10).

| Budget | Target |
|---|---|
| Launch to window | < 300 ms |
| Opening a PDF | No slower than 0.9.4 |
| Typical 20-page `.docx` to first page shown | < 400 ms |
| 300-page `.docx`: first page / fully paginated | < 700 ms / < 3 s |
| 1-million-cell `.xlsx` to first screen | < 1.5 s |
| 100 MB CSV: first screen / fully loaded | < 1 s / < 5 s |
| 50-slide `.pptx` to first slide | < 500 ms |
| Scrolling and zooming | Every frame within the display's refresh interval |
| Switching tabs, opening a menu, the side panel or command search | < 50 ms |
| Command search results, after each keystroke | < 16 ms |
| Keystroke to screen, 300-page document | < 16 ms typical, < 50 ms worst |
| Recalculation after one edit, 100,000-formula workbook | < 100 ms typical |
| Slide show frame rate | The display's refresh rate, no dropped frames |
| Idle, with nothing animating | No CPU use and no timers firing |

### No bloat

- Every feature passes the friction review and earns its weight in size, memory and complexity. The "not included" lists in §6.2, §7.2 and §8.2 change only deliberately.
- Every third-party dependency states what it adds to the download (§4.9), and is chosen over writing the code only when that's clearly smaller or safer.
- Engines, DLLs and features load nothing until a file needs them (§4.1).
- Test-only code never ships: the widget gallery and other test screens are compiled only into test builds.

### Zero friction

- The principles in §2.
- **The friction review:** each core flow (open and read, find, print, share as PDF, write and save a letter, fix a table in a received document, total a column, present a deck) is written down with its steps (clicks, keystrokes, decisions). A flow never gains a step without a reason in the plan. It's reviewed at every editor milestone.

### How it's checked

- **Every build** (CI and local), by `fwtool bench` and the budget script (§10):
  - The exe and download size against the size ledger. CI fails on any unrecorded increase over 50 KB.
  - Launch to window, opening fixed files of each kind to the first page, memory after launch and after opening, and the idle check.
  - Timings on CI runners are noisy, so CI gates on size and tracks timings as a trend. The development PC gates timings: more than 10 % slower than the baseline fails, unless the ledger records why.
- **Every milestone:** its budgets are met on the development PC, and nothing is slower or larger than at the previous milestone without a recorded reason.
- **Every release:** the whole table is checked on the typical consumer PC.

---

## 12. Building, releasing and updating

- **Toolchain:** C++20, CMake, MSVC, the static C runtime, x64, Windows 10 1903+ and 11, all unchanged. Local builds move to VS 2022 Build Tools (decision 14).
- **Dependencies:** miniz, pugixml and the hyphenation patterns are downloaded at configure time, pinned and checked against SHA-256 hashes, like PDFium.
- **CI:** builds, unit tests, short fuzz runs, round-trip, consistency and fidelity checks on the public corpus, and smoke tests. The private corpus, the reference renders and the Office repair check run locally (`scripts/corpus.ps1`) before each release.
- **Releases:** `scripts/release.ps1`, as today. There are still two files to update, `Featherweight.exe` and `pdfium.dll`. Templates, hyphenation patterns and other assets are resources inside the exe, so the updater doesn't change.
- **Code signing (decision 16):**
  - An office suite asks people to trust it with important files, and unsigned programs that read documents attract antivirus false positives. Sign before 1.1 if possible, and before 2.0 at the latest.
  - Once releases are signed, the updater checks each downloaded file with `WinVerifyTrust` and requires the expected publisher before swapping it in. Then a compromised GitHub account alone can't push code to every install.
- **File associations:**
  - one ProgID per kind of file: `Featherweight.Document`, `Featherweight.Workbook` and `Featherweight.Presentation`, plus the existing `Featherweight.PDF`
  - registered under HKCU for each extension, each with its own file icon
  - each kind is added to `--register` when its viewer ships, with the one-time default-app offer per kind (§2)
- **README and description:**
  - At 1.1, the README, screenshots, winget manifest and product description say "a light, fast PDF reader that also opens Word documents".
  - The office-suite wording comes at 2.0, when Featherweight edits documents.

---

## 13. Settings and saved state

`Featherweight.ini`, as today, plus:

- **`[General]`:** your name for comments and tracked changes (taken from Windows on first use), spelling on or off, AutoCorrect on or off, and the default-app offers per kind of file.
- **`[Recent]`:** files of every kind.
- **`[State]`:** per file, the page or scroll position and zoom, plus the active sheet and selected cell, or the current slide.

**Recovery and backups:**

- Snapshots of large files and crash-recovery journals go in `%LOCALAPPDATA%\Featherweight\Recovery`. Journals are DPAPI-encrypted and versioned (§4.3).
- Backups of originals go in `%LOCALAPPDATA%\Featherweight\Backups`, kept for 30 days and capped at 1 GB.
  - ⋯ → **Restore original** opens the backup.
  - ⋯ → **Delete backups** removes all of them.
  - The README's privacy section explains both folders.
- Portable copies keep both folders next to the exe, like their settings, and fall back to `%LOCALAPPDATA%` when that folder is read-only.

---

## 14. Risks

| Risk | Mitigation |
|---|---|
| **Word layout fidelity.** Word's line-breaking and pagination rules aren't published, and a small difference shifts every page after it. | Measured line by line against the reference renders from D1 on; compatibility settings; hyphenation; font metrics checked; an honest banner for missing fonts |
| **Losing or damaging user data** | The round-trip store and private snapshot from day one; never-damage tests on every build; the validator and `fwtool check`; the Office repair check as a release gate; safe saving, backups and crash recovery |
| **Stale references inside kept content.** Unknown content survives an edit but points at the wrong cells, slides or ids. | The reference registry and derived-data rules (§4.3 rule 2); blocking edits that would affect unknown extensions; `fwtool check` after every scripted edit |
| **The file changes on disk while it's open** (sync clients, Word) | The private snapshot, change detection before every save, and the `~$` owner file |
| **Fidelity looks better in tests than for users,** because the reference PC has Office's fonts | Every metric measured twice, with Windows-only fonts as a release gate |
| **Scope and effort:** roughly 27 times the PDF reader | Each app ships as a viewer first, so it's useful early; strict basic and advanced lists; shared engines (`fwtext`, `fwdraw`, `fwchart`) built once |
| **Bloat creeping in** | The lightweight contract (§11): size and speed checked on every build against a committed ledger, a gate at every milestone, the friction review for every flow, and the "not included" lists, which change only deliberately |
| **Malicious documents** | No active content and no network, size limits, built-in image codecs only, fuzzing with AddressSanitizer, crash recovery |
| **A crash in one tab loses work in all tabs** (everything runs in one process) | Crash-recovery journals for every open file, and fuzzing. One process keeps memory low and sharing simple; revisit if crashes show up in practice. |
| **Excel calculation quirks** | Every corpus formula checked against Excel's cached values |
| **Missing fonts** (Aptos and others) | A substitution table tuned by measured line-break matches, a banner, decision 11 |
| **PowerPoint animations are complex** | The common subset plays exactly; others play simplified, never skipped silently |
| **Using the Office formats legally** | OOXML is an ECMA and ISO standard, and Microsoft's Open Specifications Promise covers its format documentation ([MS-DOCX], [MS-DOC] and others). No Microsoft fonts, icons or names are shipped. "Opens Word documents" is purely descriptive. |
| **Unsigned exe, antivirus warnings, and a compromised release account** | Code signing (decision 16), and signature checks in the updater |

---

## 15. Decisions

| # | Decision | Outcome |
|---|---|---|
| 1 | Engines | **Own engines.** LibreOffice and ONLYOFFICE are far too heavy, and ONLYOFFICE is AGPL. *Decided.* |
| 2 | Compatibility | **Never damage a file, and look right on common documents, measured.** No claim of 100 %. *Decided.* |
| 3 | Order | **Documents, then spreadsheets, then presentations.** *Decided.* |
| 4 | Scope | **All basic features plus selected advanced ones,** as listed in §6.2, §7.2 and §8.2. *Decided; the lists are to be confirmed.* |
| 5 | Packaging | **One exe with the engines linked in.** PDFium stays a separate DLL. *Decided (as proposed; Roy delegated the remaining D0 and D1 decisions, 2026-11-03).* |
| 6 | Releases | **Viewer first, then editor,** as in §9. The PDF reader ships as 1.0 now. *Proposed.* |
| 7 | Document line layout | **Own line breaker on DirectWrite's text analyzer,** not `IDWriteTextLayout`. *Proposed.* |
| 8 | Formula engine | **Own engine, in C++.** IronCalc (Rust, MIT/Apache) was considered. It brings its own workbook model and file reader, which conflict with the round-trip store, and a second language toolchain. *Proposed.* |
| 9 | PDF export | **Own small PDF writer** behind `Painter`, so exports have links and bookmarks and don't depend on Microsoft Print to PDF being installed. EMF, WMF and SVG pictures are rasterized at 300 dpi in exports; a metafile player that draws through `Painter` could replace that later. *Proposed.* |
| 10 | Older and OpenDocument formats | **Import only, in each app's last milestone,** and Save As the modern format. RTF is saved in place only when it was fully understood. *Proposed.* |
| 11 | Fonts | **No bundled fonts.** Missing fonts get substitutes tuned by measured line-break matches, and a banner. *Open:* no free font has Aptos's metrics. One option is a one-time banner linking to Microsoft's own Aptos download, if Microsoft offers one whose license allows this use. To settle in D1. |
| 12 | Pictures in pasted web content | **Only pictures the clipboard itself carries are inserted.** For the others, a banner offers **Download pictures**, so nothing is fetched unless you click (pasted webmail can contain tracking pixels). *Proposed.* |
| 13 | UI language | **English,** with all strings in one table so translations can follow. *Proposed.* |
| 14 | Local toolchain | **VS 2019 16.11 Build Tools stay the local toolchain; CI builds with VS 2022.** libFuzzer with AddressSanitizer works on VS 2019 (`tests/fuzz/README.md`), so VS 2022 is installed only if a C++20 feature or a tool needs it. *Decided (Roy delegated the remaining D0 and D1 decisions, 2026-11-03).* |
| 15 | Backups | **Keep the original for 30 days on the first save,** capped at 1 GB, with Delete backups in ⋯. *Proposed.* |
| 16 | Code signing | **Before 1.1 if possible, before 2.0 at the latest.** Candidates: SignPath Foundation (free for open-source projects), a Certum open-source certificate, or Microsoft's Trusted Signing, which hasn't always accepted individual developers outside the US and Canada. *Open: check eligibility and cost.* |
| 17 | Number and formula locale | **Files are stored in Excel's invariant form; formulas are entered with English function names; decimal and list separators and date order follow Windows' regional settings,** as in Excel. New CSV files use the regional list separator. *Proposed; needed before S1.* |
| 18 | Pivot tables | **Refresh them, and create simple ones, in S4.** For the slightly more advanced user, pivot tables are the most likely advanced request. *Proposed.* |
| 19 | Test machines | **A typical consumer PC** (4 cores, 8 GB RAM, integrated graphics) for the budgets, and **an Office reference PC with a Microsoft 365 subscription** for reference renders and repair checks. *The Office reference PC is the development PC (decided 2026-11-03). Open: the typical consumer PC, needed before the 1.1 budgets are checked.* |
| 20 | Rendering | **Direct3D 11 with a flip-model swap chain** in D0, replacing `ID2D1HwndRenderTarget`, so video, smooth transitions and low latency are possible later without rework. *Decided; done in D0.* |
| 21 | Accessibility | **Keyboard access and the basic UI Automation from D0 only.** No accessibility gates, no Narrator or NVDA passes, no screen-reader models of document, grid or slide content. *Decided (Roy, 2026-11-03).* |
| 22 | Lightweight budgets | **The lightweight promise is a gate, like never damaging a file** (§1, §11), with these budgets added to the existing ones: exe size per release (< 4, 7, 11 and 15 MB at 1.1, 2.0, 3.0 and 4.0), memory after launch < 40 MB, menus, panels and tab switches < 50 ms, command search < 16 ms per keystroke, and no CPU use when idle. *Decided (Roy, 2026-11-03).* |

---

## 16. Progress

*Updated 2026-11-03.*

| Milestone | State | Notes |
|---|---|---|
| PDF reader | ✅ Done | 0.9.4; see the README and git history |
| Release 1.0 | ⏳ Next | The reader as it is: a hands-on pass with `docs/TEST_CHECKLIST.md`, then tag `v1.0.0` |
| D0. Host and foundations | ✅ Done (2026-11-04) | On branch `d0/host`. Done: `TabContent`, `PagedView` and the command and toolbar model, with command labels; `fwpkg`, typed units, `fwtool` and the corpus; Direct3D 11 with a flip-model swap chain and WARP fallback; `Painter` and vector printing through `ID2D1PrintControl`; the widgets, with UI Automation for every element of the window (`tests/uia-checks.ps1`, 176 checks); the side panel, status bar and command search (Alt+Q); the start screen, recent files of every kind, and file associations per kind; the reference registry framework with `fwtool check`; fuzzing in CI (5 targets); reference renders for the public corpus (19 of 20 files: Word calls `edge/escaped-part-names.docx` corrupted). The first CI run on GitHub passed (998ffd5). Exit criteria met: the 8-hour zip and XML fuzz runs found nothing (8.9 and 27.2 million inputs); the manual checklist passed; the lightweight gate (§11) passed: launch, PDF opening and window shown 19–37 % faster than 0.9.4, memory after launch within 3 % (17.8 MB), no CPU or wakeups when idle, and the size ledger has its first baseline (on `claude/d1-measure`, 1e808e7); reference renders exist for the private corpus too (108 of 108). The widget gallery is already out of release builds (`FW_TEST_SCREENS=OFF`; the shipped exe is 1.14 MB, 0.75 MB at 0.9.4). Known: printed group opacity is flattened per shape (see `Painter::BeginGroup`); `password-wrong.png` can differ by the password box's caret from run to run |
| D1–D5. Documents | 🔄 D1 in progress | Phase 1, in parallel sessions from `d0/host`: the `.docx` model, reader and writer; `fwtext` line layout and hyphenation; DrawingML, VML and pictures; the fidelity measurement and the size and speed checks. Phase 2: pagination and the document tab. The fidelity targets need the private corpus (`FW_CORPUS`, §10) and its reference renders |
| S1–S4. Spreadsheets | Not started | |
| P1–P3. Presentations | Not started | |

---

## References

- **Standards:**
  - ECMA-376 (Office Open XML), 5th edition, parts 1–4. Part 3 covers Markup Compatibility.
  - OpenDocument 1.3 (OASIS)
- **Microsoft Open Specifications:**
  - [MS-OI29500]: how Office implements ISO/IEC 29500, including where it departs from the standard. This matters as much as the standard itself.
  - [MS-OE376], [MS-DOCX], [MS-XLSX], [MS-PPTX], [MS-ODRAWXML]
  - [MS-OFFCRYPTO] (encryption), [MS-CFB] (compound files)
  - [MS-DOC], [MS-XLS], [MS-PPT] (legacy formats)
