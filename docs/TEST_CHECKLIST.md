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

## Word documents (D1–D5)

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

### Review and advanced (D4)

- [ ] Review a contract you received with tracked changes and comments: step through the changes, accept some, reject others, reply to a comment and resolve another. Save and open in **Word**: Word shows exactly the changes and comments you left.
- [ ] Turn on Track Changes (Ctrl+Shift+E), type, delete and make something bold. Open in Word: the changes carry your name and can be accepted there. Switch between Simple markup, All markup, No markup and Original.
- [ ] Add a comment on a selection, reply to it, and resolve it.
- [ ] Insert a table of contents in a document with headings, add a heading, then update the table (page numbers only, then the entire table). The entries are links.
- [ ] Insert a footnote (Ctrl+Alt+F) and an endnote (Ctrl+Alt+D), edit them, and delete one: the numbers follow.
- [ ] Fill in a form with content controls (text, dropdown, date, checkbox), and a protected form with legacy fields: Tab moves between fields, and nothing else can be edited.
- [ ] Insert a text box and a few shapes, type in them, resize and move them, change the fill and outline. Add a text watermark ("DRAFT") and remove it.
- [ ] Export a document to PDF (File → Export PDF). In Edge or Acrobat: text can be selected and searched, links work, and the bookmarks list the headings. It looks like the screen.
- [ ] Save as RTF and as plain text, and open both in Word or Notepad.

### Breadth (D5)

Test documents: `tests/office/odt/`, `tests/office/dotx/`, and the `.doc`, password-protected, chart and equation copies in `tests/office/`. Keep a copy of your own files first.

- [ ] **Opening a `.doc`.** Open an old Word 97–2003 `.doc` (one of yours, if you have one).
  - It opens in a tab and looks as it does in Word.
  - A banner says that saving makes a new `.docx`.
  - Edit it and press Ctrl+S. Save As suggests the same name as a `.docx` in the same folder.
  - The `.doc` is unchanged. The `.docx` opens in **Word** without a repair prompt.
- [ ] **Opening an `.odt`.** Open an `.odt` from LibreOffice, or one saved by Word as OpenDocument.
  - Its styles, lists, tables, pictures, headers and footers, footnotes and links are there.
  - Ctrl+S goes to Save As `.docx`, and the `.odt` is unchanged.
  - The `.docx` opens in Word without a prompt.
- [ ] **The other ways in.** Open a `.doc` and an `.odt` by double-clicking them in Explorer (Open with Featherweight), by dragging them onto the window, and with Ctrl+O.
  - Each opens in its own tab, shows in Recent, and reopens from there.
  - Opening one takes no more steps than in Word (Word adds Protected View's Enable Editing).
- [ ] **A password-protected `.docx`.**
  - Featherweight asks for the password, with the same prompt as for PDFs. A wrong password says so and asks again; Cancel leaves the other tabs as they are.
  - Edit and save, then open it in Word: Word asks for the same password and shows the edit.
  - The backup and the recovery copy can't be read without the password. End Featherweight from Task Manager with unsaved changes: at the next start, recovery offers the document back.
- [ ] **A password-protected `.doc`.** It opens after the password, or says plainly why it can't.
- [ ] **Templates.** On the start screen, Templates → Letter, CV and Report each open a new "Document N" with placeholder text in brackets.
  - Select a placeholder and type over it.
  - The + menu and the … menu have "New from template" with the same three.
  - Ctrl+S suggests a name from the first words, in Documents. The saved file opens in Word, with its bullets, the report's table and its page numbers.
- [ ] **Template files.** Double-click a `.dotx`, such as `tests/office/dotx/simple.dotx` or one of your own.
  - A new untitled document opens with the template's text and styles.
  - Ctrl+S asks where to save, and the `.dotx` is unchanged.
  - A document made from a `.dotm` saves as a `.docm`. An `.ott` opens like a `.dotx`.
- [ ] **Charts.** A document with column, bar, line, pie, area and scatter charts shows each as Word does: titles, axes, gridlines, legend and data labels.
  - They print, and they're in Export PDF.
  - Edit text elsewhere and save: in Word, each chart is still there and can still be edited.
- [ ] **Equations.** A document with fractions, roots, sums and integrals, a matrix and an equation array looks as it does in Word (Cambria Math).
  - The equations print, are in Export PDF, and can still be edited in Word after a save.
- [ ] **Right-to-left text.** Open an Arabic or Hebrew document, or type some with a second keyboard language.
  - Text runs from the right. English words and numbers inside it are in the right order.
  - The caret, selection, Home, End and the arrow keys move as they do in Word.
- [ ] **East Asian text.** In a Chinese or Japanese document, lines break where Word breaks them: no line starts with a closing bracket or "。".
  - A Japanese document with a character grid lines up as it does in Word.
- [ ] **Huge documents.** Open a 1,000-page document.
  - The first page shows at once.
  - Ctrl+End, scrolling, and typing at the start and the end all feel instant.
  - Write down the memory Task Manager shows.
  - Do the same with a document that has a 5,000-row table and one with 300 pictures.

### Speed and feel (record anything that feels slow)

- [ ] Typing in the 300-page document never stutters, also at its start.
- [ ] Opening a Word document, switching tabs and opening menus feel instant.
