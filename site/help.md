---
title: Help
description: Getting started with Featherweight, and how to do the common things.
---

# Help

Featherweight opens PDFs, Word documents, Excel workbooks and PowerPoint presentations in tabs, in one window. In
Featherweight, **F1** opens this page.

## Getting started

- **Install:** download `Featherweight-x.y.z-Setup.exe` from the [latest release]({{ site.repo }}/releases/latest). It
  installs for your user account only, without administrator rights, and can be uninstalled from Windows Settings →
  Apps. Or unzip `Featherweight-x.y.z-Portable.zip` anywhere and run `Featherweight.exe`; a portable copy keeps its
  settings beside it.
- **Open a file:** **Ctrl+O**, drag it onto the window, or double-click it in Explorer once Featherweight opens that
  kind of file. A file opened from Explorer adds a tab to the window that's already open.
- **The start page** shows new documents, workbooks and presentations, your templates and your recent files. Pin a
  file to keep it at the top.
- **Make it your default app:** Featherweight offers it when you open a file; you can also choose it in the **⋯** menu,
  or in Windows Settings → Apps → Default apps.
- **Find a command:** **Alt+Q**, then type what you're looking for. Documents, workbooks and presentations have a
  ribbon; **Alt** shows its KeyTips and **Ctrl+F1** collapses it. **Alt+F** opens the File page: New, Open, Info, Save,
  Save as, Print, Export, Close and Options.

## PDFs

- **Read:** single page, continuous or two pages (**Ctrl+6**, **Ctrl+7**, **Ctrl+8**), zoom with **Ctrl+scroll** or a
  pinch, full screen (**F11**) and presentation mode (**F5**). Night reading (**Ctrl+I**) makes the pages dark.
- **Fill in a form:** click a field and type; **Tab** goes to the next field, across pages. **Ctrl+S** saves in place.
- **Highlight:** select text and click **Highlight** in the small bar that appears.
- **Add a note:** right-click the page → **Add note**. Hover a note to read it; click it to edit it.
- **Find:** **Ctrl+F** highlights every match as you type. **Enter** and **Shift+Enter** go to the next and previous one.
- **Print:** **Ctrl+P** opens Windows' print dialog.

Highlights and notes are saved as standard PDF annotations, so other PDF readers show them too.

## Documents, workbooks and presentations

- **New:** **Ctrl+N** makes a new document; the start page and File → New have workbooks, presentations and templates.
- **Save:** **Ctrl+S**. A `.doc`, `.odt`, `.xls`, `.ods`, `.ppt` or `.odp` file is saved as a new Office file beside
  it, so the original is never overwritten. A password-protected file stays protected with the same password.
- **Export PDF:** File → Export.
- **Present:** in a presentation, **F5** starts the slide show. The presenter view shows the next slide, your notes and
  a timer.
- **Macros** in a file are kept when you save, but never run. Embedded objects show their picture but are never
  started, and links to other files aren't updated: documents can't run code or reach the network through Featherweight.

## Your work is kept safe

- **Backups:** the first time Featherweight saves over a document, the original is kept for 30 days (up to 1 GB in all).
  File → Info → **Restore original** opens it. **⋯** → **Delete backups** removes them all.
- **Recovery:** while a document has unsaved changes, a copy is kept, encrypted for your Windows account. If
  Featherweight closes unexpectedly, the next start offers to recover it.
- Both are in `%LOCALAPPDATA%\Featherweight`, or beside a portable copy.

## Updates

Featherweight checks for a new release at most once a day and installs it; the new version starts the next time you
open Featherweight. Turn this off with **⋯** → **Update automatically**, and check by hand with **⋯** → **Check for
updates now**. The check is Featherweight's only network access ([privacy](../privacy/)).

## When something goes wrong

- **Look at the [known issues](../known-issues/)** first.
- **Report a problem:** **⋯** → **Help** → **Report a problem** opens a new GitHub issue in your browser, with
  Featherweight's and Windows' versions filled in. A file that shows the problem helps most, if it isn't private.
- **If Featherweight crashes,** it saves a crash report on your PC, in `%LOCALAPPDATA%\Featherweight\Crashes` (the 5
  newest are kept). The next start offers to report it on GitHub and shows you the file to attach. Nothing is sent
  by Featherweight itself. The report may hold bits of the documents that were open, so don't attach it if any of
  them were private.
- **Windows SmartScreen** may warn the first time you run a download that isn't signed yet: choose **More info** →
  **Run anyway**. Updates installed by Featherweight itself don't trigger this.
- **A security problem?** Please report it privately: see [security](../security/).
