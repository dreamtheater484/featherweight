# Lightweight budgets

Plan §11 makes small and fast a promise, with budgets that are checked on every build. CI checks the size; the development PC also checks the timings. `scripts/budgets.ps1` does both, against the ledger in this folder.

## Running it

Measure the shipped configuration, which `scripts/release.ps1` builds in `build-release`: test-only screens such as the widget gallery are left out.

```powershell
cmake -S . -B build-release -DFW_TEST_SCREENS=OFF
cmake --build build-release --config Release
```

Then, in Windows PowerShell, which compresses the zip the way `release.ps1` does:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\budgets.ps1             # the size
powershell -ExecutionPolicy Bypass -File scripts\budgets.ps1 -Timings    # the size and the timings
```

The timings take about a minute and need the desktop: close Featherweight first, and leave the mouse and keyboard alone while it runs. `-Runs N` sets the runs per scenario (5), `-Json FILE` writes the results, and `-Build DIR` measures another build.

## What it measures

**Size:**
- **`Featherweight.exe`.** The budget is that of the release being built (decision 22): under 4 MB until 1.1, under 7 MB for 2.0, which D2 to D5 aim at.
- **The download.** This is the portable zip, staged and compressed as `release.ps1` does it: the exe, `pdfium.dll`, the license and the notices. The budget is under 25 MB. The installer is a little smaller (10.8 MB against 13.1 MB at 0.9.4), but making it needs Inno Setup.

**Timings:** `fwtool bench-app` starts the build alone, and with each fixed file (`tests/pdfs/text.pdf`, `big-600-pages.pdf`, `form.pdf`, `tests/office/layout/report.docx`, and a 300-page Word document that `tests/office/make_long_docs.py` makes when Python is installed). Each number is the median of several fresh starts after a warm-up, in ms from the launch:

| Column | What it is |
|---|---|
| window | The main window shown. |
| ready | The app's UI thread is done: the window drawn and, with a file, its first page shown. It's read from the thread's CPU cycles, which watching doesn't disturb. Launch to window (300 ms budget) and opening a file use this. |
| on screen | The window first looks the way it settles. This includes Windows' own animation of a window opening (about 200 ms), so it's reported but not gated. |
| memory | The private working set once settled (Task Manager's Memory column), and the private bytes. |
| idle | CPU time and thread wakeups in the 3 s after settling. Nothing should run: a timer firing once a second already shows 3 wakeups. |

**Typing:** `fwtool bench --typing` types in the 300-page document through the editor, at its start, middle and end and in a table cell: 44 letters, 10 Backspaces, and an Enter joined again. Each keystroke is timed from the edit to the page on screen painted again on the graphics adapter: the edit, laying out again what it moves on screen, placing the caret, and painting. The rest is laid out again untimed, as in idle time. **typing** is the median of letters and Backspaces; **worst key** is the slowest of all, Enter included.

**How it stays out of your way:**
- **A private copy.** It runs a copy of the build in a temporary folder, where settings and recent files stay (portable mode), and deletes the folder afterwards.
- **`--sandbox`.** The copy starts with this flag, so it reads and saves no settings or recent files. It also skips the update check, notices and registration, and never hands files to, or takes files from, another Featherweight.
- **Builds without the flag (0.9.4).** These get settings in the copy's folder that turn off the update check and the notices.
- **When it refuses.** It won't start while Featherweight is running, because an older build would hand its file over. It also won't start while the file types are registered to an exe that no longer exists, because an older build would register the copy.
- **One at a time.** Runs are sequential. Each window is kept on top while it's measured and then closed.

## The ledger

`ledger.json` records each accepted baseline, with the reason, one entry per line:

- **`sizes`.** The exe and the download, per compiler. Compilers make different sizes: Visual Studio 2019 (`v142`) is used locally, and 2022 (`v143`) on CI (decision 14). The compiler is read from the exe itself.
- **`timings`.** Each scenario's numbers, per computer.
- **`typing`.** Typing in the 300-page document, per computer.

**The rules:**
- **Size.** An increase of more than 50 KB in the exe or the download fails, unless the ledger records it. The baseline is the last entry made with the same compiler, plus the increases accepted since with the other one. That way an increase recorded locally also covers CI.
- **Timings, on a computer with a baseline.** These fail:
  - a time more than 10 % slower (and over 5 ms more);
  - memory more than 10 % higher (and over 2 MB more);
  - memory after launch of 40 MB or more;
  - a launch of 300 ms or more;
  - paginating the 300-page document taking 3 s or more (its "ready" includes the background pagination);
  - typing in it of 16 ms or more, or a worst key of 50 ms or more, or either more than 10 % slower (and over 1 ms more);
  - anything running while idle (over 5 ms of CPU or 2 wakeups).
- **Smaller decreases.** These need no entry. The baseline stays where it was recorded, so a later increase is measured from there.

**Recording an increase.** When it's worth it, say why, and commit the ledger with the change:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\budgets.ps1 -Timings -Record "D1: the docx layout engine"
```

**CI.** CI fails on an unrecorded size increase, and tracks the timings without gating them. Its numbers are uploaded as the `budgets` artifact. Until the ledger has a `v143` entry, the size step passes and its log prints the command that records CI's sizes as the baseline.
