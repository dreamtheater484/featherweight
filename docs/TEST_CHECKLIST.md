# Release test checklist

Run before every release. Test PDFs are in `tests/pdfs/` (regenerate with `python tests/make_test_pdfs.py`).

## Automated

- [ ] `ctest --test-dir build -C Release` passes
- [ ] `powershell -ExecutionPolicy Bypass -File tests\visual-checks.ps1` renders every important state (welcome screen, reading, search, forms, highlights, note hover, markers and editing, password, errors, presentation) to `build\visual\`. Look through all the images.

## Window and chrome

- [ ] Title bar: drag to move, double-click to maximize/restore, right-click shows the system menu
- [ ] Hovering the maximize button shows Windows 11 Snap Layouts
- [ ] Resize from all edges and corners, including the top edge
- [ ] Maximized: nothing is cut off at the top, and caption buttons work
- [ ] Moving between monitors with different scaling stays sharp
- [ ] Display scaling 100 %, 150 % and 200 %
- [ ] Light, dark and High Contrast themes. Switching Windows' theme while running updates the app.
- [ ] Animation effects off (Windows Settings → Accessibility → Visual effects): no animations
- [ ] Tooltips appear on every toolbar button, with shortcuts

## Opening files

- [ ] Double-click a PDF in Explorer (after "Make default"), drag-and-drop with the "Drop to open" overlay, and Ctrl+O with multiple files selected
- [ ] Opening a file that's already open switches to its tab
- [ ] Opening a PDF while Featherweight runs adds a tab to the existing window
- [ ] `password-secret.pdf`: an inline prompt appears, a wrong password shakes the field, "secret" opens it
- [ ] `damaged.pdf` and `not-a-pdf.pdf`: a friendly error banner, no crash
- [ ] Recent files appear on the start screen, and × removes one
- [ ] Reopening a file returns to the same page, zoom and layout

## Viewing

- [ ] Smooth scrolling with the mouse wheel. The touchpad follows the fingers.
- [ ] Ctrl+scroll and pinch zoom around the cursor, with no blank flashes
- [ ] Single page, continuous and two-page views, with cover page on its own on and off
- [ ] Rotate right and left. `mixed-sizes.pdf` page 5 shows correctly rotated.
- [ ] Internal link in `text.pdf` jumps to chapter 5, and the web link opens the browser
- [ ] `big-600-pages.pdf` scrolls smoothly from start to end, and End jumps instantly

## Forms

- [ ] `form.pdf`: type in fields, Tab/Shift+Tab move between fields and on to page 2
- [ ] Checkbox, radio buttons, dropdown and list box work
- [ ] Ctrl+C/X/V/A/Z work inside fields
- [ ] Tab shows a ● for unsaved changes. Closing asks Save / Don't save / Cancel.
- [ ] Ctrl+S saves. Reopen in Featherweight and in another reader (Edge, Acrobat): values are there.
- [ ] Saving to a read-only folder opens Save As automatically

## Text and search

- [ ] Drag to select text across lines and pages. Ctrl+C copies, and accented text is intact.
- [ ] Double-click selects a word
- [ ] Ctrl+F: the match count updates while typing, Enter/Shift+Enter step through, Esc closes
- [ ] Search in `big-600-pages.pdf` for "needle" finds 12 matches without freezing

## Printing

- [ ] Print to "Microsoft Print to PDF": all pages, a range (`2-3`), current page
- [ ] Filled form values appear in the printout
- [ ] Landscape pages print in landscape. Fit to page and actual size both work.
- [ ] `no-print.pdf`: Print is disabled and the tooltip explains why

## Reading modes

- [ ] Night reading (Ctrl+I) turns pages dark. Colors stay recognizable.
- [ ] Full screen (F11): moving the mouse to the top edge reveals the toolbar. Esc leaves.
- [ ] Presentation (F5): black background, click/Space/→ for next, ←/Backspace for previous, cursor hides, Esc leaves

## Install and update

- [ ] Installer: one click, no admin prompt, Featherweight opens afterwards, and it's in the Start menu
- [ ] Uninstall from Settings → Apps removes everything, and asks before deleting settings
- [ ] Portable zip runs from any folder, including a read-only one
- [ ] Second start offers "Make default" once. The button opens Windows' Default apps.
- [ ] Update: publish a test release, start an older build, quit, start again. You get the "Updated to …" banner.
- [ ] "Check for updates now" in the ⋯ menu reports the result

## Performance (record the numbers)

- [ ] Launch to window: ___ ms (target < 300 ms)
- [ ] Launch with a 10 MB PDF to first page: ___ ms (target < 500 ms)
- [ ] Memory with one typical document: ___ MB (target < 100 MB)
