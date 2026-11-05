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

## Word documents (D1–D3)

What the automated checks can't judge: how it looks and feels, real input devices, Word itself, and paper. Test documents: `tests/office/generated/`, `tests/office/torture/`, and any of your own `.docx` files. Keep a copy of your own files first.

### Opening and reading (D1)

- [ ] Double-click a `.docx` in Explorer, drag one onto the window, and open one with Ctrl+O. Each opens in its own tab next to PDFs.
- [ ] Open three of your own real documents (a letter, a report with a table of contents, something with pictures). Side by side with Word, pages break in the same places and nothing is missing.
- [ ] Scrolling, zooming (Ctrl+wheel, pinch) and the page layouts feel as smooth as with a PDF. A 300-page document's first page shows at once.
- [ ] A document with a font you don't have shows the font banner, and the text still looks reasonable.
- [ ] Comments and tracked changes are shown; a link opens the browser; Ctrl+F finds text and the count is right.
- [ ] Print a document with pictures and a table to a real printer: it matches the screen.
- [ ] Night reading, presentation (F5) and full screen work in a Word tab.

### Writing and editing (D2)

- [ ] Write a one-page letter from a blank document (New → Document): typing, Enter, Backspace, Delete, selecting with mouse and keyboard, undo and redo all feel instant.
- [ ] Bold, italic, underline, font, size and color from the toolbar and with Ctrl+B/I/U. The format painter copies formatting.
- [ ] Headings from the style box, then Enter after a heading continues in Normal. A bulleted and a numbered list: Tab and Shift+Tab change the level, Enter on an empty item ends the list, typing "1. " starts a list.
- [ ] AutoCorrect fixes "teh" and makes "(c)" a ©; Ctrl+Z right after puts it back. Typing a web address makes a link.
- [ ] Spelling: a misspelled word gets a red squiggle; right-click shows suggestions; Dutch text in a Dutch paragraph isn't flagged as English.
- [ ] Type with a second keyboard language, and with the emoji panel (Win+.). Dictation (Win+H) types at the caret.
- [ ] Copy formatted text from Word into Featherweight and back, and from a web page in Edge. Formatting and links come along; Ctrl+Shift+V pastes plain text.
- [ ] Find and replace (Ctrl+H): Replace All is one undo step.
- [ ] Save, close and reopen in Featherweight and in **Word**: everything is there and Word shows no repair prompt.
- [ ] Edit a document you received (one of your own), save, and open it in Word: only your edits changed.
- [ ] The dot on the tab shows unsaved changes; closing asks Save / Don't save / Cancel. Undo back to the saved state clears the dot.
- [ ] End Featherweight from Task Manager while a document has unsaved changes. On the next start, recovery offers them back.
- [ ] Open a document that's open in Word too: Featherweight says who has it open. Change it in Word and save: Featherweight offers to reload.

### Tables, pictures and layout (D3)

- [ ] Insert a table from the grid, type in it, Tab through cells (Tab in the last cell adds a row), insert and delete rows and columns, merge and split cells, drag a column border, apply a table style. Save and open in Word: it looks the same.
- [ ] Fix a table in a received document: change a row, widen a column, delete a column.
- [ ] Insert a picture from a file, by pasting and by dragging it in. Resize with the corners (keeps its shape), drag it, change the text wrap, crop it, rotate it. Open in Word: same place and size.
- [ ] Page setup: change margins, orientation and paper size; two columns for part of the document; insert a section break and a page break.
- [ ] The ruler: drag the margins, the indents and a tab stop; a tab stop with a dot leader.
- [ ] Double-click a header to edit it; add page numbers ("Page 1 of 3"); a different first page. Open in Word: the header and numbers are right.
- [ ] Insert a link (Ctrl+K) to a web page and to a heading; insert a bookmark and go to it.
- [ ] The navigation pane lists the headings and follows the caret; clicking one goes there.
- [ ] Formatting marks (Ctrl+Shift+8) show ¶, spaces and tabs; change case (Shift+F3); the Paragraph panel sets spacing and "keep with next".
- [ ] Format a CV from scratch: name as a heading, sections, a two-column table for dates, a photo. It takes no more steps than in Word.

### Speed and feel (record anything that feels slow)

- [ ] Typing in the 300-page document never stutters, also at its start.
- [ ] Opening a Word document, switching tabs and opening menus feel instant.
