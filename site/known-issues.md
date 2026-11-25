---
title: Known issues
description: What doesn't work yet in Featherweight, and what it doesn't do on purpose.
---

# Known issues

For the latest release. Something not listed here? [Report it]({{ site.repo }}/issues/new): in Featherweight, **⋯** →
**Help** → **Report a problem** fills in the versions.

## Not yet

- **Word documents' pages:** pages break where Word breaks them for about 94 % of documents. Lines within a page can
  wrap a word earlier or later than in Word. The goal for release 4.1 is 97 %.
- **Older `.doc` files:** grouped drawings and plain lines aren't brought in yet.
- **Older `.xls` files and OpenDocument `.ods` files:** an `.xls` file's charts, and an `.ods` file's charts and
  shapes, aren't read yet. The file opens without them.
- **Animations:** some effects Featherweight can't edit yet are kept, and play as before, but can't be changed.
- **Signing:** downloads aren't code-signed yet, so Windows SmartScreen may warn the first time you run one. Choose
  **More info** → **Run anyway**.
- **Screen readers** aren't fully supported yet.
- **Windows 10 and 11, 64-bit (x64) only.**

## On purpose

These keep documents from doing harm, and keep Featherweight off the network:
- **Macros** are kept when you save, but never run.
- **Embedded objects** show their picture, but are never started.
- **Links to other files and data connections** aren't updated; the last stored values show.
- **Pictures and content on the web** that a document links to aren't fetched.
- **Links** open only when they're web or mail links (`http`, `https`, `mailto`). Links to files or programs are
  blocked.
