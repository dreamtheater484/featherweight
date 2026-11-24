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

Test documents: `tests/office/doc/`, `tests/office/odt/`, `tests/office/dotx/`, `tests/office/protected/`, and `tests/office/script/` (`charts`, `equations`, `rtl` and `eastasia.docx`). Keep a copy of your own files first. The protected test files' password is `featherweight` (`ecma376-standard.docx`: `Password1234_`).

- [ ] **Opening a `.doc`.** Open an old Word 97–2003 `.doc` (one of yours, if you have one).
  - It opens in a tab and looks as it does in Word. Known gap: a `.doc`'s grouped drawings and plain lines and rectangles aren't brought in yet (its pictures and text boxes are).
  - A banner says that saving makes a new `.docx`.
  - Edit it and press Ctrl+S. Save As suggests the same name as a `.docx` in the same folder.
  - The `.doc` is unchanged. The `.docx` opens in **Word** without a repair prompt.
- [ ] **Opening an `.odt`.** Open an `.odt` from LibreOffice, or one saved by Word as OpenDocument.
  - Its styles, lists, tables, pictures, text boxes, headers and footers, footnotes, links, bookmarks, comments, tracked changes and table of contents are there.
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
- [ ] **Charts.** A document with column, bar, line, pie, doughnut, area and scatter charts shows each as Word does: titles, axes, gridlines, legend and data labels, with numbers in their formats. (A pie of pie shows as one pie: known.)
  - They print, and they're in Export PDF.
  - Edit text elsewhere and save: in Word, each chart is still there and can still be edited.
- [ ] **Equations.** A document with fractions, roots, sums and integrals, a matrix and an equation array looks as it does in Word (Cambria Math).
  - The equations print, are in Export PDF, and can still be edited in Word after a save.
  - Type before and after an equation, and in a paragraph with one: the equation stays as it was.
- [ ] **Right-to-left text.** Open an Arabic or Hebrew document, or type some with a second keyboard language.
  - Text runs from the right. English words and numbers inside it are in the right order.
  - The caret, selection, Home, End and the arrow keys move as they do in Word.
- [ ] **East Asian text.** In a Chinese or Japanese document, lines break where Word breaks them: no line starts with a closing bracket or "。".
  - A Japanese document with a character grid lines up as it does in Word.
- [ ] **Huge documents.** Make them with `python tests/office/make_long_docs.py <folder> 1000 3000 table-5000 pictures-300` (or use your own). Open the 1,000-page one.
  - The first page shows at once.
  - Ctrl+End, scrolling, and typing at the start and the end all feel instant.
  - So do Bold, a paragraph centered, Heading 1, bullets, Enter, and Undo after each of them, at the start, in the middle and at the end.
  - Layout ▾ → Margins: Narrow, then back to Normal: the page on screen changes at once, and the page count settles within a few seconds.
  - Write down the memory Task Manager shows.
  - Do the same with the 3,000-page one, the 5,000-row table and the 300 pictures. Known: in the 3,000-page document, Enter at the very start can take up to about 50 ms, which may just be noticeable.

### Speed and feel (record anything that feels slow)

- [ ] Typing in the 300-page document never stutters, also at its start.
- [ ] Opening a Word document, switching tabs and opening menus feel instant.

## Spreadsheets (S1)

Featherweight shows workbooks; it doesn't edit them yet (S2). What the automated checks can't judge: how it looks and feels next to Excel, real input devices, and paper. Test files: `tests/office/sheet/`, `tests/office/excel/`, `tests/office/csv/`, the real-world corpus in `D:\Documents\Codex\FeatherweightCorpus\xlsx` and `csv`, and your own workbooks. Make the two big files with `.venv\Scripts\python tests/office/make_budget_files.py` (into `build\budgets`).

### Opening and looking

- [ ] Open a `.xlsx` and a `.csv` from Explorer (Open with Featherweight), drag one onto the window, and open one with Ctrl+O. Each opens in its own tab next to PDFs and documents, and shows in Recent.
- [ ] Open three of your own workbooks (a budget, a list, something with charts) side by side with Excel. Values, number formats (dates `dd-mm-yyyy`, € amounts, percentages), fonts, fills, borders, merged cells, column widths and row heights look the same.
- [ ] Frozen panes stay put while scrolling; hidden rows and columns stay hidden; outline groups expand and collapse with their + and − buttons and the level buttons (1, 2, …).
- [ ] The sheet tabs at the bottom: click one, and Ctrl+PgUp/PgDn. Hidden sheets aren't listed; tab colors show.
- [ ] A right-to-left sheet (`tests/office/excel/sheets.xlsx`) runs from the right, as in Excel.
- [ ] Conditional formats (`conditional-formats.xlsx`): colored cells, color scales, data bars and icons as in Excel. A table (`tables.xlsx`) shows its style and banded rows.
- [ ] Pictures, shapes and charts (`pictures-shapes.xlsx`, `charts.xlsx`) sit on the same cells as in Excel. A chart sheet shows its chart.
- [ ] A cell with a note has a red corner, one with a threaded comment a purple one (`notes.xlsx`); hovering shows the note, or the comment with its replies.
- [ ] Zoom with Ctrl+wheel, Ctrl+= and Ctrl+−, Ctrl+0 (100 %) and the status bar's slider; scrolling a big sheet with the wheel, the scroll bar and the keyboard feels smooth.

### Formulas

- [ ] The formula bar shows the selected cell's formula (read-only).
- [ ] A workbook with `=TODAY()` shows today's date shortly after opening; one set to manual calculation shows what was saved.
- [ ] A formula Featherweight can't calculate yet, when opening recalculates it (it depends on `=TODAY()`, say), keeps Excel's value and gets a small gray triangle in its top-left corner; selected, the status bar says its value is possibly out of date.
- [ ] A workbook with macros (`.xlsm`) or links to other workbooks opens without running anything and without asking to update links.

### Selecting, copying and finding

- [ ] Arrows, Ctrl+arrows, Home, Ctrl+Home, Ctrl+End, PgUp/PgDn and Alt+PgUp/PgDn (a screen across), with and without Shift, move and select as in Excel; Tab and Enter move on, Shift+Tab and Shift+Enter back. Shift+Space selects the row, Ctrl+Space the column, Ctrl+A everything. Click a row or column heading to select it whole, the corner for all.
- [ ] The status bar shows Average, Count and Sum (Excel's order) for two or more cells; text only, just Count.
- [ ] Copy a range (Ctrl+C) and paste it into Excel (the values as shown, with fonts, colors, fills and alignment; not borders, column widths or merged cells), Word (a table) and Notepad (tab-separated text).
- [ ] Ctrl+F finds text and numbers as they're shown on the sheet you're looking at (switch sheets and it searches that one), and the count is right.

### CSV

- [ ] Open a comma-separated, a semicolon-separated, a tab-separated and a pipe-separated file (`tests/office/csv/semicolon.csv`, `tab.tsv`, `pipe.csv`). Columns split correctly.
- [ ] Nothing changes: codes with leading zeros (`00123`), long numbers (IBANs, phone numbers), and dates show exactly as in the file.
- [ ] A file from an old Windows program (`tests/office/csv/ansi-1252.csv`) shows "Crème brûlée" and "€4,50" correctly.

### Printing

- [ ] Open print preview (Ctrl+F2; Esc closes it) and print (Ctrl+P) a sheet to the Epson, and print the same sheet from Excel. Featherweight prints the sheet shown, as Excel's Print Active Sheets does. Page breaks, print area, repeated titles, fit to page, orientation, margins, headers and footers, and gridlines match.
- [ ] Print a sheet with pictures and a chart: they're on the paper where they are on screen.

### Big files and speed (record the numbers)

- [ ] `million-cells.xlsx`: first screen in ___ s (target < 1.5 s). Ctrl+End and scrolling feel instant.
- [ ] `100mb.csv`: first screen in ___ s (target < 1 s), fully loaded in ___ s (target < 5 s).
- [ ] Memory with one typical workbook open (one of yours): ___ MB in Task Manager (target < 150 MB).
- [ ] After opening a workbook, Featherweight uses no CPU while you do nothing (Task Manager).
- [ ] Opening a PDF and a Word document is as quick as before.

## Spreadsheet editing (S2)

Featherweight edits workbooks. Check it next to Excel on the same file, keystroke for keystroke; every edit is one undo step. Work on copies (the S1 test files, `million-cells.xlsx` from `build\budgets`, and your own workbooks).

### Editing in the grid

- [ ] Typing replaces a cell; F2 edits it with the caret at the end; the formula bar edits too. Enter, Tab, Shift+Enter, Shift+Tab and the arrows move as in Excel (Enter after a run of Tabs goes back to the first column); Esc cancels; Ctrl+Enter fills the selection; Alt+Enter is a line break; Delete clears contents.
- [ ] What you type reads as Excel reads it in your region: `1,5`, `12%`, `1-2-2024`, `12:30`, `'007`, `TRUE`. Typing into a cell that already has a format keeps it as Excel does (`1/2/2024` into 0.00 stays 0.00; `50%` into a date cell becomes a percentage); a number typed into a 0% cell gets its % (5 gives 5%).
- [ ] AutoComplete offers the column's own entries; Ctrl+; and Ctrl+Shift+; type the date and the time.
- [ ] A formula: clicking cells or using the arrows inserts references (point mode); each reference gets a colored border you can drag and resize; F4 cycles $A$1, A$1, $A1, A1; function names autocomplete with their argument tip. A formula that can't be entered says why and stays open.
- [ ] The fill handle: dragging copies, or continues 1, 2 / dates / Mon / Jan / Item 1; double-click fills down beside the data; Ctrl+D and Ctrl+R. Formulas move their references.
- [ ] Cut, copy and paste in a workbook and between two workbooks: formulas move their references; the dashed border stays until the next edit or Esc. Paste special: values (Ctrl+Shift+V), formulas, formats, transposed.
- [ ] Copy a range in Excel, in Word (a table) and from a web page, and paste it here: values, number formats, bold, colors, fills and merged cells come along; paragraphs become rows. Copy from here into Excel: the values (not just the text shown) and formats arrive.
- [ ] Insert and delete rows, columns and cells (Ctrl+Shift+= and Ctrl+-, and the cell menu's Insert and Delete with their choices). Formulas, merged cells, notes and links follow as in Excel; deleted references become #REF!.
- [ ] Drag a row or column heading's edge to size it (the tooltip shows Excel's width, e.g. 8.43); double-click it to fit; Row height and Column width in the menu. Open the file in Excel: the sizes are the same.
- [ ] Hide and unhide rows and columns; merge & center (asks before discarding values; toggles off), merge across, unmerge; group and ungroup (Alt+Shift+Right and Left) and the outline buttons; freeze panes at the active cell, top row, first column, and unfreeze.
- [ ] Sheet tabs: Shift+F11 adds a sheet; double-click renames; drag moves; the tab menu deletes, colors, hides and unhides.
- [ ] Find and replace (Ctrl+F, Ctrl+H, Replace all), Go to (Ctrl+G, F5, the name box: B12, A1:C5, Sheet2!A1, a name), AutoSum (Alt+=) over the numbers above or left.
- [ ] Undo and redo (Ctrl+Z, Ctrl+Y) and their lists on the toolbar: picking a step undoes it and everything after.
- [ ] F9, Shift+F9 and Ctrl+Alt+F9 in a workbook set to manual calculation; a circular reference warns once and shows "Circular References: B2" in the status bar.
- [ ] Saving: the tab's dot appears with the first edit and goes when you undo back; Ctrl+S saves without freezing the window ("Saving…" in the status bar); Save As offers .xlsx, .xlsm, .xltx and .xltm and asks before dropping macros; closing asks to save. Open every saved file in Excel: no repair prompt.
- [ ] Recovery: end Featherweight in Task Manager after editing a workbook; start it again: the workbook is offered and comes back with your edits.
- [ ] Change the open file in Excel and save it there: Featherweight says so and offers to reload.
- [ ] The two everyday tasks take no more steps than in Excel: **total a column** (click below the numbers, Alt+=, Enter) and **fix a value in a received workbook** (open it, click the cell or Ctrl+G to it, type the value, Enter, Ctrl+S).

### Editing speed (record the numbers)

- [ ] `million-cells.xlsx`: typing in a cell feels instant (measured 3.75 ms a key, 31 ms at worst; plan: under 16 and 50). Inserting a row ___ ms (measured 23–28 ms; plan: under 100 ms), and undoing it as quick.

### Fewer steps (S4's friction fixes)

`tests\workbook-friction-checks.ps1` checks these off-screen; the menus are Windows' own, so try them by hand too.

- [ ] Narrow the window to about 1366, 1280 and 1024 pixels: the toolbar ends in **›** where items no longer fit. Its menu has each of them: Merge, Styles and Data with their choices, the colors (Automatic, the theme's, the standard ones, More colors…), the format painter. Each works as on the bar. In a document the same: Insert table opens the Table panel, Page layout's menu opens under **›**.
- [ ] Alt+Q finds Sort, Filter, Table, Conditional formatting, Data validation, Format cells, Name Manager and Refresh PivotTable, with their shortcuts.
- [ ] A filter's **Condition** picked from its list puts the caret in the Value box; type the number and press Enter: the filter applies, without a click on Apply.
- [ ] Ctrl+T, and in a table **Table design…**: the styles show as small tables under Light, Medium and Dark, as Excel's gallery (in the workbook's theme colors); a click picks one, the arrows move between them, and the wheel scrolls when the panel is short.

## Protection, links and notes (S3)

Check next to Excel on copies of your own workbooks. Every change is one undo step.

### Protecting

- [ ] Right-click a sheet tab, **Protect sheet…**: a password (optional) typed twice, and the list of what everyone may still do (Excel's: select locked and unlocked cells, format cells, columns and rows, insert and delete columns and rows, links, sort, AutoFilter, PivotTables, objects, scenarios). Two different passwords: Excel's "Confirmation password is not identical." Clicking a permission (or Space on it) turns it on and off once per press; the ones turned on are what Excel's Protect Sheet shows ticked for the saved file.
- [ ] On the protected sheet, typing into a locked cell (every cell is, until Format cells › Protect › Locked is cleared) shows Excel's message at once; unlocked cells take typing, pasting and Delete. Merging, inserting cells, grouping and turning the filter on are refused with Excel's other message; formatting, column widths, row heights, inserting and deleting rows and columns, links, notes and sorting work only when allowed (deleting rows and sorting only over unlocked cells).
- [ ] A cell marked Hidden (Format cells › Protect) shows nothing in the formula bar while the sheet is protected. Tab goes to the next unlocked cell. Without "Select locked cells", the cursor stays off them.
- [ ] **Unprotect sheet…** asks for the password; a wrong one gives Excel's "The password you supplied is not correct…". Without a password it unprotects at once.
- [ ] **Protect workbook…** (password twice): sheets can't be added, deleted, moved, renamed, hidden or recolored until it's unprotected. Saved, the file opens in Excel protected the same way, and the same passwords unprotect it there; a file protected in Excel is protected here too.

### Links

- [ ] Ctrl+K on an empty cell: type `www.example.com` (or an e-mail address, or pick a place like `Sheet2!A1`), Enter. The cell shows the text, blue and underlined. Clicking a web or e-mail link opens it; a place in the workbook goes there; other kinds aren't opened.
- [ ] Ctrl+K on a link edits it (Update) or removes it (Remove link); the cell menu has Insert/Edit link and Remove link. Saved, Excel opens the file without a repair and follows the same links.

### Notes

- [ ] Shift+F2 adds a note to the cell (with your Windows user name in bold, as Excel shows it); the red corner appears; hovering shows the note. Shift+F2 on it edits the text; Delete note removes it; Show note / Show all notes keep them shown, and so does the panel's **Keep shown** (one press turns it on). A ¶ in the box is a line break.
- [ ] Saved, Excel opens the file without a repair and shows the same notes, in the same place, shown or hidden as set here.
- [ ] The note panel's width and height (points) resize the box: it moves beside the cell at the new size, here and in Excel. Editing the text of a note made or resized in Excel keeps its box where it was.

### Threaded comments

- [ ] Ctrl+Alt+M (or New comment in the cell menu) on a cell: type, Enter. The purple corner appears; hovering shows "Name: text". Ctrl+Alt+M again (or Shift+F2) opens the thread: type a reply and Enter; pick a comment in the list to edit it (Save) or delete it (Delete reply; the first one deletes the thread); Resolve and Reopen. A cell with a note offers no New comment, and a cell with a thread no New note.
- [ ] Saved, the file opens in Excel without a repair. Excel 365 shows the thread with its replies and authors, resolved as set here; Excel 2021 shows it as a note starting "[Threaded comment]", as for a file Excel 365 saved.

## Page setup, print preview and Export PDF (S3)

Check next to Excel on copies of your own workbooks, with sheets long and wide enough for several pages.

- [ ] Right-click a sheet tab, **Page setup…** (or the gear in print preview): the Page tab (portrait or landscape, paper size, no scaling or fit sheet, all columns or all rows on one page, adjust to %, fit to pages wide by tall, first page number), Margins (Normal, Wide, Narrow, or each margin in your units, centered horizontally and vertically), Header (Excel's presets, or left, center and right with &[Page], &[Pages], &[Date], &[File], &[Tab]; different odd and even, different first page) and Sheet (print area, rows and columns to repeat, gridlines, headings, page order). Each change shows at once in print preview beside it; each is one undo step.
- [ ] From the sheet (no print preview): the toolbar's gear beside Print opens Page setup, and its arrow offers No scaling, Fit sheet on one page, Fit all columns on one page and Fit all rows on one page, the sheet's own marked; ⋯ › Page setup… and Alt+Q ("fit all", "page setup") reach the same. Fit all columns on one page, then Ctrl+P, Print: the columns print on one page wide.
- [ ] A header or footer holding a carriage return (Excel writes it as `_x000D_`; some .xls files have one): opened, set up (say landscape) and saved, Excel shows the same header text; print preview never shows "_x000D_".
- [ ] In print preview, Print: Active sheet, Entire workbook (one job: page numbers and &[Pages] through all sheets) or Selection; printing and Export PDF follow the choice and the setup.
- [ ] Set print area, Add to print area, Clear print area (cell menu and command search); Insert page break above and left of the active cell, Remove page break, Reset all page breaks.
- [ ] Page break preview (sheet tab menu): what doesn't print is grey, each page shows "Page N", automatic breaks are dashed blue lines and manual ones solid; dragging a break moves it (it becomes manual). Saved, Excel opens in Page Break Preview with the same pages.
- [ ] In Page break preview, dragging the blue edge of the printed area sets the print area to the cells inside it (Sheet tab shows it; Undo names Set Print Area).
- [ ] Header tab with Different first page and/or Different odd and even pages on: "Header and footer for" picks the odd (other), even or first page's header and footer, each typed or picked from the presets; printed and exported, the first printed page gets the first page's (even with a first page number other than 1) and the 2nd, 4th... pages the even ones, as Excel prints them.
- [ ] On a chart sheet, Page setup (sheet tab menu, print preview) has Page (orientation, paper, first page number), Margins and Header only; printed and exported, the chart keeps its size from the top left margin (cut off at the margins when the page is smaller, as Excel prints it). Saved, Excel shows the same setup.
- [ ] Export PDF: the PDF's text (cells, headers and footers, headings, notes) can be searched and copied in a PDF reader.
- [ ] Sheet tab: Black and white prints no fills and black text and borders; Draft quality prints no gridlines, pictures or charts; Notes (none, at end of sheet, as displayed); Cell errors as displayed, blank, "--" or "#N/A" (centered), as Excel prints them. Saved, Excel shows the same.
- [ ] Saved, Excel opens the file without a repair and shows the same page setup (File › Print, Page Setup), print area, print titles and breaks; its PDF export has the same pages as Featherweight's Export PDF (on this PC Excel exports on Letter even for A4 sheets).

## Tables, conditional formatting, data validation and names (S3)

Check next to Excel on copies of your own workbooks, in your region (`1,5`, `;` between arguments). Every change is one undo step; after each block, save, open the file in Excel (no repair prompt) and compare.

### Tables

- [ ] Select a block of data with a header row and press Ctrl+T (or Data › **Table…**): "Where is the data for your table?" shows the selection; with **My table has headers** on, **Create table** makes it in the chosen style. Without headers, a row of Column1, Column2… goes in above the data, as in Excel.
- [ ] **Table design…** (on a cell of the table): rename it (a name with a space is refused), pick another style, and turn on and off Total row, Banded rows, Banded columns, First column, Last column and Filter button. The total row shows "Total" and the last column's SUBTOTAL (a sum of numbers, a count of text), as Excel adds it.
- [ ] Type in the row just below the table, or the column just right of it: the table grows. A formula typed in an empty column of the table fills down the column (a calculated column).
- [ ] A formula outside the table using `=SUM(Table1[Q1])` (with the table's own name) gives the column's total; inside, `[@Q1]` uses the row's value.
- [ ] **Convert to range**: the table goes, its cells stay, and formulas that used `Table1[Q1]` now refer to the cells.
- [ ] Saved, Excel shows the same table (name, range, style, options, total row) and its formulas give the same results.

### Conditional formatting

- [ ] Select numbers, toolbar **Conditional › New rule…**: try each of "Format cells that are": Greater than, Less than, Between, Equal to (with a value or a cell), Text that contains, A date occurring, Duplicate values, Top N items, Above average, each with a format from the list ("Light Red Fill with Dark Red Text"…). The cells change at once, as Excel colors them.
- [ ] Data bars, Color scales and Icon sets, a few presets each: the bars, colors and icons look like Excel's on the same values.
- [ ] Use a formula: `=$C2>100` over A2:D20 colors whole rows; typed in your region's form (`=AND($B2>0;$C2<5)`).
- [ ] **Manage rules…**: the list shows each rule and where it applies; Move up and Move down change which applies first; Stop if true; Delete; change "Applies to". Clear rules from the selected cells, and from the sheet.
- [ ] Change a value the rules look at: the colors follow at once. Saved, Excel shows the same colors, bars and icons, and lists the same rules in its Manage Rules.

### Data validation

- [ ] Data › **Data validation…** on a range: Allow a list (`Yes;No` in your region, or a range such as `=$F$1:$F$5`, also on another sheet): the cell shows its arrow; Alt+Down opens the choices. Whole number between 1 and 10, decimal, date, time, text length and a custom formula each refuse what they should.
- [ ] Input message (Title and Input message) shows beside the selected cell. The error alert: Stop refuses the typed value with your message; Warning and Information ask and let you keep it. Pasting works as in Excel: the copied cells' rules come along in place of the cells' own, and pasted values aren't checked.
- [ ] **Circle invalid data** circles cells that break the rule (typed before it was set); Clear circles removes them. Clear all removes the rule from the selection.
- [ ] Saved, Excel shows the same rules (its Data Validation dialog), dropdowns and messages, and refuses the same values.

### Names

- [ ] Ctrl+F3 (**Name Manager**): New: a Name, a Scope (the workbook or one sheet) and what it Refers to (`=Sheet1!$B$2`, in your region's form); Save; Delete. The list shows each name's value and where it refers.
- [ ] Type a new name in the Name Box with cells selected: it's defined. Ctrl+Shift+F3 (**Create names from selection**) with a header row: each column gets its header's name. F3 pastes a name into a formula; Paste list writes them all out.
- [ ] Formulas using names (`=B4*(1+Rate)`, `=SUM(Net)`) calculate; a deleted name gives #NAME?. Saved, Excel's Name Manager shows the same names, scopes and references.
- [ ] In the Name Manager, the filter (All names, Scoped to the workbook, Scoped to this sheet, Names with errors, Names without errors) narrows the list. Typing `=Ra` in a cell offers `Rate` in AutoComplete beside the functions.

### Dynamic arrays and S3's functions

- [ ] Type `=SORT(UNIQUE(A2:A20))` beside a column with repeats: the sorted list spills down from the cell, with a thin blue border around the spill range while a cell in it is selected. A spilled cell shows the first cell's formula greyed in the formula bar.
- [ ] Type something into a cell the array would spill into: the first cell shows #SPILL!, and selecting it shows why in the status bar ("Spill range isn't blank", as Excel's error menu says it). Clear the cell: the array spills again. A merged cell in the way, or the sheet's edge, gives #SPILL! too.
- [ ] `=SEQUENCE(5;3)`, `=FILTER(A2:B20;B2:B20>3)` and `=XLOOKUP(…)` in your region's form; `=SUM(D2#)` and `=ROWS(D2#)` use the whole spill range and follow it when it grows or shrinks.
- [ ] Saved, Excel 365 opens the file without a repair, shows the same arrays spilled (not as {curly-bracket} array formulas) and gives the same results; Excel 2021 too.
- [ ] Spot-check functions your own workbooks use (`TEXTJOIN`, `IFS`, `XLOOKUP`, `LET`, `TEXTSPLIT`, date and financial ones): the same results as Excel, in your region's form.

## Charts and objects (S3)

Check next to Excel on copies of your own workbooks.

- [ ] Select a block with a header row and a first column of text, Alt+F1: a clustered column chart of it appears in the middle of the window, as Excel puts it, its series named by the headers and its categories from the first column, as Excel guesses them (series down the columns when there are at least as many rows as columns). The toolbar's **Chart** list makes a column, bar, line, pie, doughnut, area, scatter or combo chart instead. A combo shows the last series as a line over the others' columns; a scatter of two numeric columns takes the first as the X values. Cells with no numbers: "Those cells have no numbers to chart."
- [ ] Double-click a chart (or Ctrl+1 on it) for **Format chart**: Chart type; Colors (Colorful, or Blue, Orange, Gray or Green shades, as Excel's palettes); Chart title and Axis titles on or off with their text (Apply); Legend (None, Bottom, Right, Top, Left); Data labels. Each change shows at once and is one undo step.
- [ ] In the same panel, the data: type another **Chart data range** (`A1:C9` or `Sheet1!A1:C9`) and **Use range**; pick a series to change its name, values, categories and color; **Add** and **Remove** series and move them up and down. Saved, Excel's Select Data shows the same series in the same order.
- [ ] Click a chart, picture or shape to select it: drag it to move it; drag its handles to resize it (Shift keeps a corner's proportions); the arrow keys nudge it; Delete removes it; Ctrl+X, Ctrl+C and Ctrl+V cut, copy and paste it (a pasted chart refers to the same cells). Each is one undo step.
- [ ] Change a value the chart shows: the chart follows at once. Insert or delete rows above it: it moves with its cells; insert rows inside its data: the chart takes them in.
- [ ] **Move chart to a new sheet** (the chart's menu): the chart gets a chart sheet of its own (Chart1), before the sheet it was on; on the chart sheet, **Move chart onto a sheet** puts it back on a worksheet. Saved, Excel shows the chart sheet the same way, and its Page Setup as set here.
- [ ] Saved, Excel opens the file without a repair and shows the same charts, in the same place and size, with the same series (Select Data) and look.

### Pictures and shapes

- [ ] The toolbar's **Insert** list, **Pictures…**: a PNG, JPEG, GIF or BMP file goes in at the active cell at its own size; another kind of file gets "That isn't a picture Featherweight adds: PNG, JPEG, GIF or BMP." Copy a picture in another program (Paint, a browser) and Ctrl+V in a cell: it's pasted there. The same picture inserted twice is stored once (the saved file is no bigger by a second copy).
- [ ] **Insert** › Rectangle, Rounded rectangle, Oval, Line, Arrow and Text box: each goes in at the active cell in Excel's default look (blue fill, darker outline; a text box white with a grey outline).
- [ ] Ctrl+1 on a selected picture or shape (**Format object…**): Fill (a color, No fill, Automatic), Outline (a color, No line) and its weight, the shape's Text, and Alt text. Each is one undo step; typing in a shape shows the text centred in it.
- [ ] Pictures and shapes move, resize, nudge, cut, copy, paste and delete like charts, and move with their cells when rows and columns are inserted or deleted above or left of them.
- [ ] Saved, Excel opens the file without a repair and shows the same pictures and shapes in the same place and size, with the same fill, outline, text and alt text (Excel's Format Shape and Alt Text panes).

## S3's gate (record the numbers)

- [ ] `scripts\build-from-scratch.ps1 -Fixtures -Featherweight build\Release\Featherweight.exe` passes: every fixture in `tests\office\fixtures` (formatted cells, S3's functions, names, dynamic arrays, tables and rules, notes, comments, links and protection, every chart kind and a chart sheet, pictures and shapes, page setup) is built through the app's commands, Excel reads back every expected object, gives the same results when it recalculates, its PDF matches Featherweight's (SSIM ≥ 0.95), and opens the file without a repair.
- [ ] `scripts\sheet-torture.ps1 -Corpus D:\Documents\Codex\FeatherweightCorpus`: the 246 workbooks, with S3's edits (names, protection, links, notes, comments, tables, rules, validation, charts, pictures and shapes, page setup and breaks) mixed in: ___ failures (target 0), ___ new validator errors (target 0), ___ Excel repairs (target 0).
- [ ] `fwtool bench --sheet-edits --runs 12 tests\office\generated\simple.xlsx`: a table, a rule and a chart from 1,000 rows each to screen in ___ / ___ / ___ ms (target < 100); the chart drawn again after an edit to its data in ___ ms typical, ___ ms worst (target < 16 and < 50). S2's lines all "met".
- [ ] `fwtool bench-app build\Release\Featherweight.exe build\budgets\million-cells.xlsx build\budgets\100mb.csv` on a quiet PC: million-cells ready in ___ ms (< 1,500), 100mb.csv ready in ___ ms (< 1,000); nothing runs when idle (idle CPU 0 ms, no wake-ups).
- [ ] The shipped exe (`scripts\budgets.ps1` on `build-release`, see `tests\budgets\README.md`): ___ MB, at most 1.35 MB more than S2's 7.91 MB (so ≤ 9.26 MB).
- [ ] `fuzz_xlsx` (with S3's edits, undo, redo and save) and `fuzz_formula` 60 s each: no findings.
- [ ] Make a workbook with a table, a chart, a few rules, a list validation, a note, a comment, a link, a name and a protected sheet; save it; open it in Excel: no repair, and everything is there and looks the same.

## More files, pivots and big files (S4)

Check next to Excel. Test files: `tests\fuzz\seeds\xls` (Excel's `.xls` copies of ten of the S1 test workbooks), the corpus copies in `D:\Documents\Codex\FeatherweightCorpus\xls` and `\ods` (Excel saved the real-world `.xlsx` corpus as `.xls` and `.ods`), `tests\office\sheet\pivots`, `tests\office\sparklines\sparklines.xlsx`, and the big files from `.venv\Scripts\python tests\office\make_budget_files.py` (`build\budgets`, now with `fifty-mb.xlsx`).

### .xls and .ods

- [ ] Open an `.xls` and an `.ods` from Explorer, by dragging and with Ctrl+O. Each opens in a workbook tab as a copy, with the banner "… is an Excel 97–2003 workbook. Saving makes a new ….xlsx and leaves it as it is." (for `.ods`, "… is an OpenDocument spreadsheet."). Ctrl+S asks where to save the `.xlsx`.
- [ ] Open a few `.xls` from `tests\fuzz\seeds\xls` next to their `.xlsx` originals in `tests\office\excel`, and a few corpus `.xls` and `.ods` next to Excel: values, number formats, fonts (a font that isn't installed shows in Arial, as in Excel), text with several formats in one cell, fills, borders, merged cells, widths and heights, frozen panes, hidden rows and sheets, conditional formats, pictures, notes and print preview look the same.
- [ ] Edit an `.xls` and an `.ods` (a value, a formula, a format), save them as `.xlsx`, and open the result in Excel: no repair, and the edits are there. The original `.xls` or `.ods` is unchanged.
- [ ] An `.ots` (OpenDocument template) opens as a new workbook.
- [ ] A damaged file (rename a `.txt` to `.ods`) says it "seems to be damaged or isn't an OpenDocument spreadsheet" and nothing else happens.
- [ ] An `.xls` with pictures, text boxes and shapes shows them where Excel does; one with a chart sheet keeps the sheet in its place, with an empty chart where Excel draws its chart, and prints a page for it.
- Known: an `.xls`'s charts aren't read (a chart sheet keeps an empty chart), its macro sheets and macros are dropped (references to them become `#REF!`), and formulas that refer to other workbooks keep their values; an `.ods`'s sheet protection comes across without its password, and its data validations aren't read. Printed, the `.xls` copies pass about as many pages as their `.xlsx` originals (21.99 % against 21.51 % on 198 workbooks, one workbook Excel prints differently left out), but by workbook they're behind on average (27.70 % against 29.56 %): a follow-up.

### Password-protected workbooks

- [ ] Open `tests\fuzz\seeds\xls\text-password-feather1.xls`: the password card says it's protected and asks for the password. A wrong one: "That password isn't right. Try again."; `Feather1` opens it. Save it: the `.xlsx` is encrypted with the same password (Excel asks for it, and opens it without a repair).
- [ ] Encrypt a copy of one of your own workbooks in Excel (File › Info › Protect Workbook › Encrypt with Password). Featherweight asks for the password, opens it, and after an edit and Ctrl+S Excel still asks for the same password and opens it without a repair.

### New workbooks and templates

- [ ] The + menu and the … menu have **New spreadsheet** (a blank "Book1"); the start screen has Spreadsheet under New.
- [ ] Save an Excel template (`.xltx`) into `Documents\Custom Office Templates` (or your own templates folder, as Word and Excel have it). **New from template** lists it; picking it opens a new workbook named after it ("Budget1") with its contents and formats; Ctrl+S asks where (as `.xlsx`; `.xlsm` from an `.xltm`). **More templates…** opens that folder.

### PivotTables

- [ ] Open `tests\office\sheet\pivots\pivots-stale.xlsx` in Featherweight and in Excel. The toolbar's **Data** list: **Refresh all PivotTables** (Ctrl+Alt+F5) in Featherweight, Data › Refresh All in Excel: the pivots show the same values. **Refresh PivotTable** (Alt+F5) refreshes the one the cursor is in.
- [ ] Select a block with a header row, **Data** › **PivotTable…**: the Create PivotTable panel takes the range (or a table), where to put it (New worksheet), and each field as Rows, Columns, Filters, Sum, Count or Average. **Create PivotTable** adds a sheet before the active one with the filters at A1 and the pivot at A3, in Excel's default style; it's one undo step. Saved, Excel opens it without a repair, and Refresh All gives the same values.
- [ ] Change a value in the pivot's source and refresh: the pivot follows. A pivot with features Featherweight keeps but doesn't refresh (calculated fields, grouping, slicers) says so instead.
- Known: pivot labels sit about 11 pixels left of Excel's (no room is left for the expand and collapse buttons); custom pivot styles aren't drawn.

### Sparklines

- [ ] Open `tests\office\sparklines\sparklines.xlsx` next to Excel: line, column and win/loss sparklines, markers, high, low, first and last points, negative points, gaps, the axis, custom minimum and maximum, right to left, weights and colors look the same, in the grid and in print preview. Insert rows above them: they move with their cells. (Featherweight shows sparklines; it doesn't create or edit them.)

### Big files and speed (record the numbers)

- [ ] `fifty-mb.xlsx` (384,000 rows, 52 MB): first screen in ___ s (target < 1.5 s), fully loaded in ___ s (target < 5 s), memory ___ MB. Scroll while it loads; when it's loaded, Ctrl+End reaches row 384,000. Edit a cell and save: the window doesn't freeze.
- [ ] `100mb.csv`: first screen in ___ s (target < 1 s), fully loaded in ___ s. `million-cells.xlsx`: first screen in ___ s (target < 1.5 s).
- [ ] A workbook with many formulas (`million-cells.xlsx`, or one of yours): editing a value that feeds them recalculates without a noticeable pause; Task Manager shows several cores busy during a full recalculation (Ctrl+Alt+F9), then nothing when idle.

## S4's gate (record the numbers)

- [ ] `scripts\sheet-torture.ps1 -Corpus D:\Documents\Codex\FeatherweightCorpus` over the 249 `.xlsx` (44 public, 205 real-world), the 204 `.xls` and the 171 `.ods` copies: ___ failures, ___ new validator errors, ___ Excel repairs (target 0 each); the converted `.xls` and `.ods` saved as `.xlsx` with no edits: ___ validator errors, ___ Excel repairs (target 0 each).
- [ ] `fwtool compare --cells --folders xls <corpus>\reference` and the same for `ods`: ___ % and ___ % of the cells show Excel's text (target ≥ 99 %).
- [ ] Printed pages (`scripts\sheet-pages.ps1`): the public set (`-Reference tests\office\reference -Folders excel`) ___ of 239 (target ≥ 90 %); the real-world `.xlsx` (`-Folders xlsx`) ___ % (recorded); the `.xls` and `.ods` copies (`-Folders xls,ods -Originals <the xlsx run's summary.json>`) ___ % and ___ % against their originals' ___ % and ___ % on the same workbooks (target: at least the originals'; `govuk-english-indices-of-deprivation-2019-file-8` is left out of the `.xls` comparison, as the plan says, because Excel prints its own `.xls` differently from its `.xlsx`).
- [ ] PivotTables: every corpus pivot refreshes to Excel's values; the created ones open in Excel without a repair and refresh to the same values.
- [ ] `fwtool bench --sheet-edits`: full recalculation of 100,000 formulas ___ ms on 1 thread, ___ ms on 4 (speed-up ___×, target ≥ 3×, 0 values differ).
- [ ] The shipped exe (`scripts\budgets.ps1` on `build-release`): ___ MB (target < 11 MB, 3.0's budget).
- [ ] On a quiet PC with no Featherweight open (the script won't start its copies beside one): `scripts\budgets.ps1 -Build build-release -Timings` (launch, ready, memory, idle) and `fwtool bench-app build-release\Release\Featherweight.exe build\budgets\million-cells.xlsx build\budgets\100mb.csv build\budgets\fifty-mb.xlsx`: million-cells ready in ___ ms (< 1,500), 100mb.csv ready in ___ ms (< 1,000), fifty-mb on screen in ___ ms (< 1,500) and loaded in ___ ms (< 5,000); then record them (`-Timings -Record "…"`) before release 3.0.
- [ ] `fuzz_xls`, `fuzz_ods`, the pivot cache, and `fuzz_xlsx` with pivots, 60 s each: no findings.

## Presentations (P1)

Decks to open: the public set in `tests\office\presentations` (one per feature, `tests\office\README.md`), PowerPoint's PDFs of them in `tests\office\reference\presentations`, and the real-world decks in `FeatherweightCorpus\pptx\<kind>\`. Open each one next to PowerPoint (or its PDF).

Roy passed these checks on 2026-11-24, with the gate build (`ea3fe681` plus the slide-show bench's own fixes) as a portable copy. The test set has no `.pptm`, so the first row's `.pptm` part wasn't tried.

### Opening and viewing

- [x] Double-click a `.pptx`, a `.potx` and a `.pptm` in Explorer: each opens read-only in a presentation tab, the first slide shown at once; the `.pptm` says its macros are kept and never run.
- [x] `show.ppsx` opens straight into the slide show; Esc leaves it to the tab.
- [x] The thumbnails pane fills in while you look at the first slide; scrolling it is smooth; clicking a thumbnail shows that slide; hidden slides (`hidden-slides.pptx`) are marked and dimmed.
- [x] Page Up/Down, arrows, Home and End move between slides; zoom (Ctrl+wheel, fit slide) keeps the slide sharp; the notes pane shows `notes.pptx`'s notes, and the slide with none shows an empty pane.
- [x] Find (Ctrl+F) finds text on slides and in notes, jumps to the slide and marks the paragraph (and the words in the notes); Copy (Ctrl+C) copies the slide: paste into Word gives its picture, into Notepad its text. Right-click has Copy slide text, Copy slide as picture and Copy speaker notes.
- [x] `hyperlinks.pptx` in the tab: the pointer becomes a hand over a link, its tooltip says where it goes; Ctrl+click opens a web link in the browser and jumps to a linked slide.
- [x] The notes pane: drag its top edge to size it; the toolbar's notes button (or a double-click on the edge) hides and shows it.
- [x] Each public deck looks like PowerPoint's PDF: layouts and placeholders (an empty placeholder shows nothing), the theme's colors and fonts and the dark slide, autofit text at PowerPoint's size, bullets and numbering, tables and their styles, every chart kind, pictures (crop, rotation, flips, transparency, the SVG), groups, effects, backgrounds (master, layout, slide, picture, pattern), WordArt.
- [x] `embedded-fonts.pptx` shows its text in the pixel font (blocky 5×7 letters), not in Calibri; a real-world deck that embeds a font shows it too.
- [x] Slide sizes: 4:3, 16:9 and the portrait `hidden-slides.pptx` each fit the view.

### Slide show

- [x] F5 starts from the first slide, Shift+F5 from the current one, the ▶ button too; on a second monitor, the show is on the chosen one.
- [x] Click, Space, Enter, the arrow keys, Page Down move on; Backspace and Page Up go back; a number and Enter jumps; B and W black and white the screen; Esc ends the show; the cursor hides when the mouse is still.
- [x] `transitions.pptx`: fade, push, wipe, split, cover, uncover and zoom play smoothly at the display's refresh rate; dissolve, random bars, vortex and morph play as a fade; the push with a time moves on by itself after 2 s.
- [x] `animations.pptx`: the entrances, emphasis, exit and motion path play on click, with and after the previous ones; the first effect on slide 3 starts by itself; bullets fly in one by one; wheel and checkerboard play as appear; the trigger shows the cloud only when you click its button; the picture, chart and table animate.
- [x] `hyperlinks.pptx` during the show: the slide links jump (and the home buttons back), next, previous, first, last and end show do what they say, the mouse-over link jumps on hover, web and mail links open the browser and mail app (after the usual question).
- [x] `sections-custom-shows.pptx`: the button plays the custom show (slides 1, 4, 6) and comes back.
- [x] A 50-slide deck from the corpus presents without a stutter in its transitions (record the frame rate below). Measured: p1-show's frame bench (its 50-slide `bench50.pptx`, 50 transitions) at the console presents every transition at the display's 240 Hz; see P1's gate below and p1-plan's frame-bench decision of 2026-11-24.

### Printing and PDF

- [x] Print (Ctrl+P), with the toolbar's print layout box: full page slides (landscape for a wide deck), handouts (1, 2, 3, 4, 6, 9 per page; 3 with lines beside the slides) and notes pages (portrait), hidden slides left out unless ⋯ Print hidden slides is on; the dialog's page range picks slides ("2-4" prints those slides in the chosen layout), Current page prints the shown slide.
- [x] Export PDF: the same layouts; the PDF opens in Featherweight's PDF reader and in another reader, with the slides' text selectable and the links working.

### Never damage a file

- [x] Open a deck, look around, play the show, close it: the file's bytes and date haven't changed (P1 doesn't save decks).

### P1's gate (record the numbers)

Measured on the gate code, p1-int `ea3fe681` (later P1 commits are docs and the ledger), on 2026-11-24.

- [x] Slides against PowerPoint's (`scripts\slide-pages.ps1`): the real-world corpus (`-Reference D:\Documents\Codex\FeatherweightCorpus\reference -Folders pptx`) 2,369 of 2,516 slides, 94.16 % (target ≥ 90 %); the public set (`-Reference tests\office\reference -Folders presentations`) 92 of 97 slides, 94.85 %.
- [x] `fwtool check` and the Open XML SDK validator (`build\validate\fwvalidate.exe`) on every corpus and public deck: `fwtool check` 0 failed on all 170 (152 corpus, 18 public); the validator 0 errors on the 18 public decks and on 141 of the 152 corpus decks (target 0). The other 11 have 71 schema errors in their downloaded originals (P1 never saves a deck). By the coordinator's decision of 2026-11-24 in p1-plan, they're P2's baseline, not P1 failures: P2's check is that a saved deck adds no errors to its original's (`fwvalidate --compare`).
- [x] `fuzz_pptx` and the animation timing fuzzer (`fuzz_timing`), 60 s each: no findings (CI on p1-int `9797b268`: 1,929 and 243,759 runs, "All fuzz targets ran without findings").
- [x] On a quiet PC with no Featherweight open: a 50-slide `.pptx` to its first slide in 273 ms, on screen at 107 ms (target < 500 ms); memory with a typical deck 30.3 MB (target < 150 MB); nothing runs when idle (0 % CPU, no timers) with a deck open: 0 ms of idle CPU and no wake-ups with each deck; the slide show at 240 fps on a 240 Hz display, 0 dropped frames during transitions 2–50 (target: the display's rate, none dropped). From `scripts\budgets.ps1 -Timings` at the console (5120×1440), 2026-11-24 09:29 (ledger `c7db3b25`), and p1-plan's frame-bench decision of 2026-11-24: 50 transitions, 12,085 frames, worst frame interval 7.28 ms (under 2 refreshes), CPU per frame p99 0.48 ms. Slide 1's 3–4 counted misses are the bench's queue model, not frames; a second run had one 23 ms present stall from outside (other sessions' builds winding down).
- [x] The shipped exe (`scripts\budgets.ps1` on `build-release`): 10.63 MB, 11,149,824 bytes (target ≤ 11.05 MB: 10.05 MB after S4 plus ≤ 1.0 MB in P1), 597 KB over S4.
- [x] Opening a PDF, a document and a workbook is no slower than at S4 (`scripts\budgets.ps1 -Timings` against S4's numbers): PDF 102.7 ms, document 125.9 ms, workbook 598 ms (`text.pdf`, `report.docx` and `million-cells.xlsx` ready), against 101.8, 128.9 and 594 ms for S4 final's exe (`e00f6ec`) in the same quiet window; the other PDFs, `long-300.docx`, `100mb.csv` and `fifty-mb.xlsx` are within 2 % either way, and memory is the same.
