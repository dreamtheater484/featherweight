# Featherweight: Implementation Plan

Featherweight is a small, fast, free PDF reader for Windows. It reads, fills in forms, saves and prints, and does nothing else. No bloat, no subscriptions, no cloud.

- **Status:** approved 2026-10-10, in progress
- **Target:** version 1.0, built and released in one go (no separate v1/v2 releases)
- **Repository:** https://github.com/dreamtheater484/featherweight (public)
- **License:** MIT

---

## 1. Scope

### Included in 1.0

**Opening files**
- Double-click (file association), drag-and-drop onto the window, Open dialog (multi-select), command line (`Featherweight.exe file.pdf`)
- Recent files list
- Password-protected PDFs

**Viewing**
- Single page and continuous scrolling
- Two-page (facing) view, with an option to show the cover page alone
- Zoom: automatic, fit width, fit page, actual size, preset levels, Ctrl+scroll and touchpad pinch (anchored at the cursor), Ctrl +/−
- Page navigation: Page Up/Down, Home/End, "go to page" box, "current of total" display
- Clicking links: internal jumps (table of contents, cross-references) and web links
- Rotate the view 90° left or right (the file itself is not changed)
- Sharp rendering on high-DPI screens, including moving between monitors with different scaling

**Form filling**
- Text fields (single and multi-line), checkboxes, radio buttons, dropdowns, list boxes
- Tab / Shift+Tab between fields
- Fillable fields are subtly highlighted
- Form JavaScript: automatic formatting (dates, numbers) and calculated fields, plus basic support for XFA forms
- Save (Ctrl+S), Save As (Ctrl+Shift+S), unsaved-changes prompt when closing

**Printing**
- Windows print dialog: printer, page range, copies
- Fit to page or actual size
- Filled-in form values are printed
- PDFs that forbid printing are respected, with a clear message

**Text**
- Select and copy text
- Search (Ctrl+F) with all matches highlighted, next/previous, match count, match-case option

**Window and appearance**
- Tabs for multiple documents
- Dark mode for the app (follows Windows by default)
- Inverted-colors reading mode for pages
- Full-screen mode (F11) and presentation mode (F5)

**Installing**
- **Installer:** a one-click install per user, with no admin rights or UAC prompt. It adds a Start menu entry, registers Featherweight for PDFs, and appears in Windows' Installed apps for uninstalling.
- **Portable zip:** unzip and run from anywhere, including a USB stick. Settings live next to the exe.

**Housekeeping**
- Remembers the window, and each file's page, zoom, view mode and rotation
- Keyboard shortcuts for everything
- Automatic updates from GitHub Releases

### Not included

- Digital signatures, editing page content, annotations
- Bookmark/outline sidebar, page thumbnails
- Telemetry, accounts, cloud, background services
- Full XFA support (XFA forms get PDFium's basic support only, see §11)
- Platforms other than Windows (for now)

---

## 2. Design principles: zero friction

The bar is **zero friction**. Every interaction should need the fewest possible steps and decisions, and nothing should get in the way of the document.

1. **The document is the app.** It opens straight to the document: no splash screen, no welcome wizard, no tips, no "rate us". The UI chrome is slim and quiet, and the page gets the space.
2. **No settings screen.** Defaults are right for nearly everyone. The few real preferences sit in the ⋯ menu as single-click toggles. Everything else is remembered automatically: window size and position, plus page, zoom and view mode per file.
3. **Never ask what the app can figure out.**
   - Opening a file that's already open switches to its tab.
   - Ctrl+S saves in place. If the location can't be written to, Save As opens automatically, already pointing at a sensible folder.
   - Zoom starts at **Automatic**: fit width for portrait pages, but never above 125 %, so text isn't giant on wide screens. Landscape pages use fit page.
   - Printing defaults to all pages, fit to page, and the right orientation for each page.
4. **Never interrupt.**
   - Information appears in a slim banner that doesn't block the document and goes away by itself, never in a dialog.
   - The only dialogs are ones Windows requires (Open, Save As, Print) and the unsaved-changes question, which uses plain buttons: **Save**, **Don't save**, **Cancel**.
5. **Updates happen by themselves.** New versions download quietly in the background and take effect the next time Featherweight starts, followed by a small "Updated to 1.1" note. There are no prompts and no restarts forced on you.
6. **Plain language.** Every error says what happened and what to do next, with no error codes. For example: *"form.pdf couldn't be opened — the file seems to be damaged."*
7. **Discoverable, then fast.** Every button has a tooltip with its keyboard shortcut. Mouse-only use is fully possible, and keyboard users never need the mouse.
8. **Polished and native.**
   - Looks at home on Windows 11, and good on Windows 10: Segoe UI Variable, Fluent icons, the Windows accent color, rounded corners, soft page shadows.
   - Smooth scrolling and zooming, and gentle hover and press feedback.
   - Light and dark themes designed side by side.
   - Everything drawn with Direct2D, so it's anti-aliased and sharp at any scaling.
9. **Forgiving.**
   - Saving never puts the original at risk: the app writes a temporary file first and swaps it in.
   - Closing with unsaved changes always asks.
   - Damaged PDFs show an error, never a crash.
10. **One-time offer, never a nag.** On first run only, a banner offers to make Featherweight the default PDF app: **[Make default] [Not now]**. After that, it's only in the ⋯ menu.

---

## 3. Technical choices

| Area | Choice | Why |
|---|---|---|
| Language | C++20 | Smallest and fastest result, with no runtime to install |
| UI | Win32 window, with all drawing done in **Direct2D + DirectWrite** | Anti-aliased, sharp at any DPI, smooth scaling. Built into Windows, so it adds no size. |
| Title bar | Custom: tabs live in the title bar (like Windows Terminal and Notepad), with Windows 11 Snap Layouts kept | More room for the document, modern look |
| Icons | **Segoe Fluent Icons** font glyphs (Win 11), falling back to **Segoe MDL2 Assets** (Win 10) | Zero bytes of icon assets, sharp at any size, recolored per theme |
| PDF engine | **PDFium** (Chrome's PDF engine) with V8 (form JavaScript) and XFA, prebuilt by [bblanchon/pdfium-binaries](https://github.com/bblanchon/pdfium-binaries), currently `chromium/8086` | Very good form-filling support, BSD-style license |
| PDFium version | Pinned and verified against a SHA-256 hash at build time | Reproducible builds, and engine updates are always deliberate |
| C runtime | Static (`/MT`) | No Visual C++ Redistributable needed |
| Build | CMake + MSVC | Local: VS 2019 Build Tools 16.11. CI: GitHub Actions `windows-latest`, build and unit tests only. |
| Releases | Built **locally** by `scripts/release.ps1`: builds, tests, packages the installer and zip, computes checksums, and publishes the GitHub Release with `gh` | Packaging doesn't depend on GitHub's build machines |
| Platform | Windows 10 (1903+) and 11, x64 | x64 builds also run on ARM64 Windows through emulation |
| Packages | **Installer** (`Featherweight-X.Y.Z-Setup.exe`, built with Inno Setup) and **portable zip** (`Featherweight-X.Y.Z-Portable.zip`). Both contain `Featherweight.exe` + `pdfium.dll` + licenses. | Inno Setup is free, small and widely trusted. |
| Install location | `%LOCALAPPDATA%\Programs\Featherweight` (per user) | No admin rights or UAC prompt needed, and the auto-updater can write there |
| Settings | Installed: `%APPDATA%\Featherweight\Featherweight.ini`. Portable: `Featherweight.ini` next to the exe (falls back to `%APPDATA%` if that folder is read-only). | The app knows it's installed when its folder matches the install path the installer recorded in `HKCU\Software\Featherweight`. |
| Updates | GitHub Releases API for `dreamtheater484/featherweight` | Public repo, so no token needed |

### Ground rule: all PDFium calls run on one thread

PDFium is not thread-safe, so every PDFium call runs on the UI thread. The app stays responsive through three techniques instead of threads:

- **Progressive rendering** (`FPDF_RenderPageBitmap_Start` / `_Continue`), with a pause callback that hands control back to the UI after about 8 ms
- **Tiled bitmap cache**, so already-rendered tiles are shown instantly
- **Time-sliced search** that processes a few pages per idle tick

Only the updater's network and file work runs on a worker thread, and it never touches PDFium.

---

## 4. Architecture

### Source layout

```
Featherweight/
├─ CMakeLists.txt
├─ cmake/FetchPdfium.cmake    download pinned PDFium build, verify SHA-256
├─ src/
│  ├─ main.cpp                entry point, single-instance handoff, command line
│  ├─ App.*                   main window, custom title bar, layout, commands, shortcuts
│  ├─ Gfx.*                   Direct2D/DirectWrite device, text formats, icon font, helpers
│  ├─ Theme.*                 light/dark palettes, accent color, dark title bar & menus
│  ├─ Anim.*                  tiny animation helper (eased values driven by a frame timer)
│  ├─ TabStrip.*              tabs in the title bar, caption buttons
│  ├─ Toolbar.*               toolbar, page box, zoom box
│  ├─ SearchBar.*             Ctrl+F bar
│  ├─ Banner.*                slim notifications (update done, errors, default-app offer)
│  ├─ Tooltip.*               custom-drawn tooltips with shortcut hints
│  ├─ EmptyState.*            start screen: open button, drop hint, recent files
│  ├─ DocumentView.*          one per tab: layout, scrolling, zoom, hit-testing, input
│  ├─ Document.*              PDFium wrapper: load, pages, links, save, permissions
│  ├─ FormFill.*              FPDF_FORMFILLINFO callbacks, routing form events
│  ├─ RenderCache.*           progressive tile rendering, LRU cache, invert pass
│  ├─ TextSelect.*            selection and copy
│  ├─ Search.*                time-sliced search, match rectangles
│  ├─ Print.*                 print dialog and print job
│  ├─ FullScreen.*            full-screen and presentation modes
│  ├─ Settings.*              INI, recent files, per-file state
│  ├─ FileAssoc.*             register/unregister as PDF app (HKCU only)
│  ├─ Updater.*               check, download, verify, swap
│  ├─ Util.*                  strings, paths, hashing, small helpers
│  └─ resource.rc, Featherweight.manifest, Featherweight.ico
├─ tests/unit/, tests/pdfs/, tests/make_test_pdfs.py
├─ docs/TEST_CHECKLIST.md
├─ installer/Featherweight.iss  Inno Setup script
├─ scripts/release.ps1        local build + package + publish
├─ .github/workflows/build.yml  CI: build and unit tests on every push
└─ LICENSE, THIRD_PARTY_NOTICES.md, README.md
```

### Key flows

**Opening a file**
1. `FPDF_LoadCustomDocument` reads on demand from a file handle opened with full sharing, so other programs can still read the file.
2. If the file needs a password, an inline password field appears over the document area. There's no modal dialog, and a wrong password shakes gently and stays focused.
3. Set up the form environment (`FPDFDOC_InitFormFillEnvironment`, field highlight colors, document open actions).
4. Restore the saved page, zoom, view mode and rotation for this file.

**Rendering**
- The layout engine computes page rectangles from `FPDF_GetPageSizeByIndexF`, so it doesn't need to load pages.
- Visible pages are drawn as 512×512 device-pixel tiles:
  1. `FPDF_RenderPageBitmap_Start` renders the page (with annotations and LCD text off; Direct2D handles smoothing).
  2. `FPDF_FFLDraw` adds the live form fields.
  3. If inverted mode is on, the tile's pixels are inverted.
  4. The tile is uploaded as an `ID2D1Bitmap`.
- Tiles live in an LRU cache with a 128 MB cap.
- While zooming or scrolling, cached tiles from the nearest zoom level are stretched until sharp ones are ready, so pages are never blank.
- Pages are drawn on a soft shadow against the background.

**Form input**
- Mouse events over a form field (`FPDFPage_HasFormFieldAtPoint`) go to the `FORM_On*` functions. Mouse events over a link follow the link. Everything else goes to text selection.
- The `FFI_Invalidate` callback re-renders only the affected tiles, so typing feels instant.
- `FFI_SetCursor` sets the mouse cursor.
- Dropdowns are drawn by PDFium inside the page.
- Tab moves between fields on a page (handled by PDFium). The app adds the jump to the next page's first field.

**Saving**
1. `FORM_ForceToKillFocus` commits the field being edited.
2. `FPDF_SaveAsCopy(..., FPDF_INCREMENTAL)` writes to a temporary file in the same folder.
3. Close the document, swap the file in with `ReplaceFileW`, reopen it, and restore the position.
4. If the file is locked by another program, the document stays open with your changes, and Save As opens automatically.

**Printing**
- `PrintDlgEx`, defaulting to all pages and fit to page.
- Vector output via `FPDF_RenderPage(hdc, …, FPDF_ANNOT | FPDF_PRINTING)`.
- Orientation is chosen per page.

**Search**
- `FPDFText_FindStart` / `FPDFText_FindNext` with match rectangles from `FPDFText_GetRect`.
- Starts at the current page and wraps around, with a live match count.

**Links**
- `FPDFLink_GetLinkAtPoint`, then either the internal destination (`FPDFLink_GetDest` → `FPDFDest_GetDestPageIndex` and location) or the URI action (`FPDFAction_GetURIPath`). Web links open in the default browser.
- The hand cursor and a tooltip show the target URL.
- Only `http`, `https` and `mailto` links open. Anything else (for example a local program) is blocked with a short message.

---

## 5. User interface

### Layout

```
┌──────────────────────────────────────────────────────────────────────────┐
│ 🪶 │ form.pdf  ● │ manual.pdf  × │ ＋                      ─    ▢    ✕ │ title bar + tabs (40 px)
├──────────────────────────────────────────────────────────────────────────┤
│ 📂  💾  🖨     ‹  [ 3 ] of 12  ›     −  [ Auto ▾ ]  +     ▯▯ ▾   ⟳     🔍  ◐  ⛶  ⋯ │ toolbar (44 px)
├──────────────────────────────────────────────────────────────────────────┤
│  🔍 [ search text          ]  4 of 17   ˄ ˅   Aa                     ✕ │ only after Ctrl+F
│  ✓ Featherweight was updated to 1.1.0                                  ✕ │ banner, when needed
│                                                                          │
│                          ┌─────────────┐                                ┃│ thin overlay scrollbar
│                          │    page     │                                ┃│
│                          └─────────────┘                                 │
└──────────────────────────────────────────────────────────────────────────┘
```

- **Title bar:** app icon, tabs, a ＋ (open) button, and Windows-style caption buttons. Hovering the maximize button shows Windows 11 Snap Layouts. Double-clicking empty title-bar space maximizes, and dragging it moves the window.
- **Toolbar:** three groups. File actions on the left, page and zoom in the middle (centered), view and more on the right. Icons only, each with a tooltip such as *"Print (Ctrl+P)"*. Buttons that do nothing right now are hidden (for example Save when there are no changes) or dimmed.
- **⋯ menu:** Open recent ▸, Save As, Theme ▸ (System / Light / Dark), Show cover page alone, Highlight form fields, Update automatically, Make default PDF app, About Featherweight.
- **Right-click on the page:** Copy (if text is selected), Select all on page, Rotate, view modes, zoom presets.
- **Scrollbars:** thin overlays that grow when hovered and fade when idle, like Windows 11.
- **Empty window:**
  - A centered welcome area with the feather logo, a large **Open PDF** button and *"or drop a PDF anywhere"*.
  - Recent files listed with name, folder and "2 days ago", each one click away. Files that no longer exist are dropped quietly.
- **Drag-and-drop:** dragging a file over the window shows a soft accent overlay saying *"Drop to open"*.
- **Tabs:**
  - A ● marks unsaved changes. × and middle-click close a tab.
  - When many tabs are open, they shrink evenly, and the full path shows in a tooltip.
  - Opening a file that's already open switches to its tab.
- **Single instance:** opening a PDF from Explorer while Featherweight runs adds a tab to the existing window and brings it forward.
- **Page box:** click to type a number and press Enter. Scroll over it to step through pages.
- **Zoom box:**
  - Click to pick Automatic, Fit width, Fit page, Actual size or a preset (25–1600 %), or type a number.
  - Ctrl+scroll and pinch zoom around the cursor, with a smooth stretched preview that sharpens a moment later.

### Keyboard shortcuts

| Action | Keys |
|---|---|
| Open | Ctrl+O |
| Save / Save As | Ctrl+S / Ctrl+Shift+S |
| Print | Ctrl+P |
| Close tab | Ctrl+W, Ctrl+F4 |
| Next / previous tab | Ctrl+Tab / Ctrl+Shift+Tab |
| Find / next / previous | Ctrl+F / Enter or F3 / Shift+Enter or Shift+F3 |
| Copy selection | Ctrl+C |
| Go to page | Ctrl+G |
| Next / previous page | Page Down / Page Up (→ / ← when the page fits horizontally and no field is focused) |
| First / last page | Home / End |
| Zoom in / out | Ctrl+= / Ctrl+−, Ctrl+scroll, pinch |
| Automatic / actual size / fit page / fit width | Ctrl+0 / Ctrl+1 / Ctrl+2 / Ctrl+3 |
| Single / continuous / facing | Ctrl+6 / Ctrl+7 / Ctrl+8 |
| Rotate left / right | Ctrl+Shift+− / Ctrl+Shift+= |
| Inverted reading mode | Ctrl+I |
| Full screen / presentation | F11 / F5 |
| Leave full screen, presentation or search | Esc |

When a form field has keyboard focus, keys go to the field first.

### Visual design

- **Type:** Segoe UI Variable (Windows 11), falling back to Segoe UI. Body text 13 px (scaled for DPI) and tab titles 12 px.
- **Colors:** two hand-tuned palettes based on Windows 11. Light: chrome `#F3F3F3`, document background `#E9E9EB`. Dark: chrome `#202020`, document background `#191919`. The Windows accent color is used for focus, selection and primary buttons.
- **Shape:** 4 px corner radius on controls and 8 px on banners and popups. Pages have square corners and a soft, multi-layer shadow.
- **Motion:** hover fades about 100 ms. Banners and the search bar slide in over about 150 ms with ease-out. Smooth scrolling for the mouse wheel; touchpads are followed 1:1. All motion is turned off when Windows "Animation effects" is off.
- **Accessibility:** contrast at least 4.5:1 for text, visible keyboard focus rings, and High Contrast mode respected (system colors are used when it's on).

### Dark mode and inverted reading mode

- **App theme:** System (default), Light or Dark. The title bar and all chrome follow it, and so do the ⋯ and right-click menus (through uxtheme `SetPreferredAppMode`, which falls back to light menus if unavailable). The app follows Windows theme changes live.
- **Inverted reading mode (Ctrl+I, or the ◐ button):**
  - Turns white pages dark. It inverts brightness but keeps hues, so photos and charts stay recognizable.
  - It's independent of the app theme.
  - It's remembered globally, not per file, so night reading stays on.

### Full screen and presentation

- **Full screen (F11):** the document fills the monitor. Moving the mouse to the top edge slides the toolbar back into view.
- **Presentation (F5):** black background, one page at a time at fit page. Click, Space or → for the next page; ← or Backspace for the previous one. The cursor hides after 2 seconds without movement.
- Esc leaves either mode and restores the previous view.

---

## 6. Settings and saved state

`Featherweight.ini` in plain text, always written atomically:

- `[General]`: theme, inverted mode, cover page alone, highlight fields, auto-update, last update check, first-run flags, window placement
- `[Recent]`: the last 10 files
- `[State]`: per file (keyed by a hash of the full path) page, zoom, view mode, rotation. Capped at 200 entries, oldest dropped first.

### Installer

The installer is designed to be one click:

1. It opens straight to an **Install** button. There are no license, folder or Start menu pages, and no admin prompt.
2. It installs the files, adds the Start menu entry, and registers Featherweight as a PDF app by running `Featherweight.exe --register`.
3. It finishes with Featherweight already open.
4. Running a newer installer over an existing install upgrades it in place, and settings are kept.
5. Uninstalling (from Windows Settings → Installed apps) runs `Featherweight.exe --unregister` and removes the files. Settings are kept unless you choose to delete them.

### Registering as the default PDF app

The installer, and the ⋯ menu in the portable version, use the same registration code:

1. Writes only under `HKCU` (`Software\Classes\Featherweight.PDF`, `.pdf\OpenWithProgids`, `RegisteredApplications` + `Capabilities`), so no admin rights are needed.
2. Opens Windows' Default apps page for Featherweight, where you confirm with one click. Windows doesn't allow apps to make themselves the default silently.
3. If the exe is moved later, Featherweight updates its own registration automatically on the next start.

---

## 7. Automatic updates

No service, no scheduled task, no separate updater exe, and no prompts.

1. **Check:** on startup, at most once every 24 hours, a worker thread asks the GitHub API for the latest release (WinHTTP + a minimal JSON reader).
2. **Download:** if a newer version exists, the changed files are downloaded quietly (`Featherweight.exe` and/or `pdfium.dll`, which are separate release assets). Each one is checked against `SHA256SUMS.txt`. Installed and portable copies update the same way.
3. **Swap:** the running files are renamed to `*.old` (Windows allows that), and the new files are moved into place. If anything fails, everything is rolled back and the app tries again next day.
4. **Next start:** the new version runs, deletes the `*.old` files, updates the version shown in Windows' Installed apps (installed copies only), and shows a small banner, *"Updated to 1.1.0"*, that fades after a few seconds.
5. **Opting out:** "Update automatically" in the ⋯ menu turns this off. The app then shows a one-line banner when an update exists, with an **Update** button.
6. **Read-only folder** (for example Program Files): the banner offers a link to the download page instead.

**Security note:** the exe and installer aren't code-signed, so Windows SmartScreen warns once when they're first downloaded in a browser. Updates fetched by the app itself don't trigger this. The checksums catch damaged downloads but not a compromised GitHub account, so keep two-factor authentication on that account.

---

## 8. Build order

Everything ships together as 1.0. Each phase ends with something that runs.

| Phase | Work | Done when |
|---|---|---|
| **0. Foundation** | CMake, PDFium download and verification, static CRT, manifest (per-monitor DPI v2, UTF-8, Common Controls 6), license files, CI build, unit test harness | An empty window builds locally and in CI. ✅ PDFium sizes measured and the JavaScript build chosen (see §11). |
| **1. Shell and design system** | Direct2D/DirectWrite setup, theme palettes, accent color, custom title bar with tabs and Snap Layouts, toolbar, tooltips, empty state, animation helper | The empty app looks finished in light and dark at 100–200 % scaling. |
| **2. Core viewer** | Loading (including inline password), layout engine, zoom modes (including Automatic), rotation, smooth scrolling, overlay scrollbars, tiled progressive rendering, cache, page shadows | Large and odd PDFs scroll and zoom smoothly. |
| **3. Documents and tabs** | Single instance, command line, drag-and-drop with overlay, recent files, per-file state, all shortcuts, page box, zoom box, links | Several documents open in tabs and reopen where you left off. |
| **4. Forms and saving** | Form environment and callbacks, input routing, field highlight, tabbing across pages, Save / Save As, unsaved-changes prompt, locked-file handling | Forms fill in, save, and reopen correctly in Featherweight and in another reader. |
| **5. Text** | Selection and copy, search bar, time-sliced search, highlights | Searching 500+ pages stays smooth. Accented and CJK text copies correctly. |
| **6. Printing** | Print dialog, ranges, copies, fit / actual size, per-page orientation, permissions | Filled forms print correctly on a real printer and on "Microsoft Print to PDF". |
| **7. Reading modes** | Inverted mode, full screen, presentation | Works on multiple monitors with different scaling. |
| **8. Install, updates and release** | Inno Setup installer, portable zip, updater, local release script, default-app registration and first-run offer | The installer installs and uninstalls cleanly without admin rights. A test release (`v0.9.0` → `v0.9.1`) updates both an installed and a portable copy end to end. |
| **9. Polish and 1.0** | Full checklist, a friction review of every flow, damaged files, performance budgets, README, `v1.0.0` | Everything in §9 passes. |

---

## 9. Testing

**Test PDF corpus (`tests/pdfs/`)**
- Multi-page text, a 500+ page document, mixed page sizes and rotations, a scanned document
- Forms covering every field type, generated by `make_test_pdfs.py` (reportlab) and committed
- A few real-world public forms
- Encrypted with a password, and encrypted with "no printing"
- Truncated and corrupted files
- Non-ASCII file names and contents
- Documents with internal links and web links

**Unit tests:** pure logic, run by `ctest` in CI. Covers layout and zoom math, page-range parsing, version comparison, INI parsing, JSON field extraction and list capping.

**Friction review:** for each common task, count the clicks and decisions and remove whatever can be removed. The tasks: open a PDF, fill in and save a form, print two pages, find a word, read at night, present.

**Manual checklist (`docs/TEST_CHECKLIST.md`):**
- Windows 10 and 11
- Display scaling 100 / 150 / 200 %, and two monitors with different scaling
- Light, dark and High Contrast themes
- Animation effects off
- A real printer and "Microsoft Print to PDF"

**Performance budgets** (measured on this PC):

| Metric | Target |
|---|---|
| Launch to window | < 300 ms |
| Launch with a typical 10 MB PDF to first page shown | < 500 ms |
| Memory with one typical document | < 100 MB |
| `Featherweight.exe` size | < 1.5 MB |
| Download size (installer or zip) | ≈ 13.5 MB, almost all of it PDFium with form JavaScript |
| Typing in a form field | No visible delay |

---

## 10. Risks

| Risk | Mitigation |
|---|---|
| Custom title bar edge cases (Snap Layouts, maximized borders, multi-monitor DPI) | Follows the pattern Windows Terminal uses, and gets its own checklist items |
| Dark popup menus rely on undocumented APIs | Falls back to light menus if unavailable |
| Saving over a file another program has open | Temporary file first, and Save As if the swap fails. Your changes are never lost. |
| PDFium API changes | Version pinned. Updates are deliberate and re-tested against the corpus. |
| Unsigned exe and installer trigger SmartScreen and antivirus warnings | Documented in the README. Code signing is possible later. |
| Form JavaScript adds attack surface | Scripts run inside PDFium's own JavaScript engine (as in Chrome). They can't open files or programs, and links they request go through the same http/https/mailto filter. |
| Local VS 2019 vs CI VS 2022 | Both build through CMake. Code stays within the C++20 subset VS 2019 16.11 supports. |

---

## 11. Decisions

| # | Decision | Outcome |
|---|---|---|
| 1 | Form JavaScript (V8) | **Included.** Measured with PDFium `chromium/8086`: `pdfium.dll` is 32 MB (12.8 MB zipped) with JavaScript, against 7.5 MB without. It also brings basic **XFA** form support. The build option `FW_WITH_V8` (default ON) can still produce a build without it. |
| 2 | Clicking links in PDFs | **Yes**, included (only http, https and mailto open externally) |
| 3 | PDFs that forbid printing | **Respected**, with a clear message |
| 4 | License | **MIT** |
| 5 | UI guideline | **Polished, beautiful, intuitive, zero friction.** See §2. |
| 6 | Distribution | **Installer and portable zip**, Windows only for now |
