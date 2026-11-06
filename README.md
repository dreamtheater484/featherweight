<p align="center">
  <img src="docs/logo.png" width="104" alt="Featherweight logo">
</p>

<h1 align="center">Featherweight</h1>

<p align="center">
  <b>A light, fast PDF reader for Windows.</b><br>
  Now opening and editing Word documents too, in beta.<br>
  Zero bloat, zero subscriptions, zero cloud.
</p>

<p align="center">
  <a href="https://github.com/dreamtheater484/featherweight/releases/latest"><img src="https://img.shields.io/github/v/release/dreamtheater484/featherweight?label=latest&color=4f6bed" alt="Latest release"></a>
  <a href="https://github.com/dreamtheater484/featherweight/releases"><img src="https://img.shields.io/github/downloads/dreamtheater484/featherweight/total?color=4f6bed" alt="Downloads"></a>
  <img src="https://img.shields.io/badge/Windows-10%20%7C%2011-4f6bed" alt="Windows 10 and 11">
  <a href="LICENSE"><img src="https://img.shields.io/github/license/dreamtheater484/featherweight?color=4f6bed" alt="MIT license"></a>
</p>

<p align="center">
  <a href="https://github.com/dreamtheater484/featherweight/releases/latest"><b>⬇ Download for Windows</b></a>
  &nbsp;·&nbsp;
  <a href="https://github.com/dreamtheater484/featherweight/releases/tag/v0.95.0"><b>Try the 0.95 beta with Word documents</b></a>
</p>

![Featherweight with a highlight, a note preview and note markers](docs/screenshots/hero.png)

Featherweight opens PDFs instantly, fills in and saves forms, lets you highlight and add notes, searches,
and prints. Then it stays out of your way. No account, no ads, no telemetry, no background services,
and a 0.7 MB program.

## Beta: Word documents (0.95)

[Featherweight 0.95 beta](https://github.com/dreamtheater484/featherweight/releases/tag/v0.95.0) opens and edits
Word documents, with its own engine. It doesn't use Word or any Office component, and it's still one small program
(5.7 MB). It's a beta: try it on copies of documents that matter. The stable download above stays the PDF reader.

| Start screen: new documents and templates | Charts, as Word draws them |
|---|---|
| ![The start screen with New document and templates](docs/screenshots/word-start.png) | ![A Word document with a column chart](docs/screenshots/word-charts.png) |
| **Equations** | **Every script, right to left and East Asian** |
| ![A Word document with equations](docs/screenshots/word-equations.png) | ![Accents, Japanese, Chinese, Korean, Arabic, Hebrew and emoji in one document](docs/screenshots/word-languages.png) |

**Write and edit**
- New documents (Ctrl+N), or start from a letter, CV or report template
- Formatting, styles, bulleted and numbered lists, find and replace, spelling (in each paragraph's language) and AutoCorrect
- Tables, pictures, text boxes, shapes and watermarks; page setup, the ruler, columns, sections, headers and footers
- Links, bookmarks, a table of contents, footnotes and endnotes, and the navigation pane
- Track changes and comments that Word shows exactly as you left them
- Fill in forms: content controls and protected legacy forms

**Open and save**
- `.docx`, `.docm`, `.dotx` and `.dotm`, and password-protected documents (saved with the same password)
- Older `.doc` and OpenDocument `.odt` files open too, and are saved as a new `.docx`, so the original is never overwritten
- Export PDF (with selectable text, links and bookmarks), and save as RTF or plain text
- Charts and equations are shown as in Word, and kept exactly as they are when you save

**Never damages a file:** every change is checked against Word. Saved documents open in Word without a repair prompt,
and the parts Featherweight doesn't edit are kept as they were. Before this beta, hundreds of documents went through
tens of thousands of random edits, each saved copy then opened in Word.

**Known in this beta:**
- Pages break where Word breaks them for about 94 % of documents. The goal for 2.0 is 97 %.
- A `.doc` file's grouped drawings and plain lines aren't brought in yet.

## What's new in 0.9.4

- **Note previews:** hover a note to read it in a clean card. Click to edit.
- **Note markers:** small amber marks along the right edge show where every note is. Click one to jump there.
- **New note icon:** a modern amber speech bubble that other PDF readers show too.
- Hovering a note no longer opens an old-style yellow popup.

See [all releases](https://github.com/dreamtheater484/featherweight/releases) for earlier changes.

## Features

**Read**
- Single page, continuous or two-page view, with optional book-style cover page
- Smooth zoom (Ctrl+scroll, touchpad pinch), fit width, fit page, rotate
- Clickable links. Web links open in your browser; links to files or programs are blocked for your safety.
- Remembers the page, zoom and layout of every file you open

**Fill in forms**
- Text fields, checkboxes, radio buttons, dropdowns and lists
- Forms that use JavaScript for formatting and totals work too
- Tab and Shift+Tab move between fields, across pages
- **Ctrl+S** saves in place. **Save as** is in the arrow next to the Save button (Ctrl+Shift+S).

**Highlight and add notes**
- Select text and a small bar appears. Click **Highlight**, or **Copy**.
- Right-click anywhere on a page → **Add note**. Hover a note to read it, and click to edit it.
- Amber markers along the right edge show where every note is in the document
- Saved as standard PDF annotations, so Adobe Reader, Edge and others show them too

**Find and print**
- **Ctrl+F**: every match is highlighted as you type, even in 600-page documents
- Printing uses the normal Windows print dialog. Filled-in form values are printed as sharp text.

**Comfortable**
- Tabs for multiple documents. Opening a PDF from Explorer adds a tab to the running window.
- Dark mode that follows Windows, plus **night reading** (Ctrl+I), which turns the pages themselves dark
- Full screen (F11) and presentation mode (F5)
- Updates itself quietly; the new version starts the next time you open it

| Fill in forms | Find in document |
|---|---|
| ![Filling in a form](docs/screenshots/form.png) | ![Search with highlighted matches](docs/screenshots/search.png) |
| **Night reading, two pages** | **Start screen** |
| ![Night reading in two-page view](docs/screenshots/night.png) | ![Start screen with recent files](docs/screenshots/welcome.png) |

## Install

Download from the [latest release](https://github.com/dreamtheater484/featherweight/releases/latest):

- **`Featherweight-x.y.z-Setup.exe`** installs for your user account only, with no administrator rights needed. It adds a Start menu entry and can be uninstalled from Windows Settings.
- **`Featherweight-x.y.z-Portable.zip`**: unzip anywhere (even a USB stick) and run `Featherweight.exe`. Settings are stored next to it.

Requires Windows 10 (version 1903 or later) or Windows 11, 64-bit.

> Featherweight isn't code-signed, so Windows SmartScreen may warn the first time you run a downloaded copy.
> Choose **More info → Run anyway**. Updates installed by the app itself don't trigger this.

## Keyboard shortcuts

| Action | Keys |
|---|---|
| Open, Save, Save as, Print | Ctrl+O, Ctrl+S, Ctrl+Shift+S, Ctrl+P |
| Close tab, next/previous tab | Ctrl+W, Ctrl+Tab / Ctrl+Shift+Tab |
| Find, next/previous match | Ctrl+F, Enter / Shift+Enter (or F3 / Shift+F3) |
| Copy selected text | Ctrl+C |
| Go to page | Ctrl+G |
| Next/previous page | Page Down / Page Up, Alt+→ / Alt+← |
| First/last page | Home / End |
| Zoom in/out | Ctrl+= / Ctrl+−, Ctrl+scroll |
| Automatic, actual size, fit page, fit width | Ctrl+0, Ctrl+1, Ctrl+2, Ctrl+3 |
| Single page, continuous, two pages | Ctrl+6, Ctrl+7, Ctrl+8 |
| Rotate right/left | Ctrl+Shift+= / Ctrl+Shift+− |
| Night reading | Ctrl+I |
| Full screen, presentation | F11, F5 (Esc to leave) |
| Save or close a note | Ctrl+Enter, Esc |

In Word documents (0.95 beta):

| Action | Keys |
|---|---|
| New document | Ctrl+N |
| Bold, italic, underline | Ctrl+B, Ctrl+I, Ctrl+U |
| Undo, redo | Ctrl+Z, Ctrl+Y |
| Find and replace | Ctrl+H |
| Insert a link | Ctrl+K |
| Track changes on or off | Ctrl+Shift+E |
| New comment | Ctrl+Alt+M |
| Footnote, endnote | Ctrl+Alt+F, Ctrl+Alt+D |
| Paste as plain text | Ctrl+Shift+V |
| Formatting marks | Ctrl+Shift+8 |
| Command search | Alt+Q |

## Privacy

Featherweight never sends your documents anywhere. Its only network access is a check of
`api.github.com` for a new release, at most once a day. You can turn off automatic updates in the ⋯ menu.

Two folders keep your work safe, in `%LOCALAPPDATA%\Featherweight` (or beside a portable copy's exe):

- **Backups:** the first time Featherweight saves over a document, the original is kept here for
  30 days, up to 1 GB in all. ⋯ → **Restore original** opens it; ⋯ → **Delete backups** removes them all.
- **Recovery:** while a document has unsaved changes, a copy is kept here, encrypted for your Windows
  account, so they can be recovered if Featherweight closes unexpectedly. Saving or closing the
  document removes it.

## Building from source

Requirements: Visual Studio 2019 or 2022 with the C++ desktop workload (or the Build Tools), and CMake 3.21 or later.

```bash
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release
```

The first configure downloads the pinned PDFium build and checks it against its SHA-256 hash.
To build without form JavaScript (7.5 MB engine instead of 32 MB): `cmake -S . -B build -DFW_WITH_V8=OFF`.

- `tests\visual-checks.ps1` renders every important screen to `build\visual\` for review.
- `scripts\release.ps1` builds the installer (needs [Inno Setup 6](https://jrsoftware.org/isinfo.php)), the portable zip and checksums, and publishes a release.
- The design is described in [IMPLEMENTATION_PLAN.md](IMPLEMENTATION_PLAN.md), and release testing in [docs/TEST_CHECKLIST.md](docs/TEST_CHECKLIST.md).

## License

MIT. See [LICENSE](LICENSE). Featherweight uses [PDFium](https://pdfium.googlesource.com/pdfium/); see
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
