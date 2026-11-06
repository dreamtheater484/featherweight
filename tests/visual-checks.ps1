<#
.SYNOPSIS
  Renders every important state of Featherweight to PNG files for review before a release.
  Uses snapshot mode (off-screen rendering), so nothing appears on screen.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tests\visual-checks.ps1
  # then look through build\visual\*.png
#>
param([string]$Exe = "build\Release\Featherweight.exe")

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$out = Join-Path $root "build\visual"
New-Item -ItemType Directory -Force $out | Out-Null
$pdfs = Join-Path $root "tests\pdfs"

# Recent files of every kind, opened from minutes to days ago (recent:path|seconds ago). Folders in
# the profile show without it, so the images look the same for every user.
$h = $env:USERPROFILE
$recent = "recent:$h\Downloads\Lease.pdf|259200;recent:$h\Documents\Budget 2026.xlsx|90000;" +
  "recent:$h\Documents\Work\Report Q3.docx|7200;recent:D:\Projects\2026\Clients\Acme Corporation\Decks\Kickoff.pptx|1500;" +
  "recent:C:\Docs\Meeting notes.txt|20"

# name, theme, extra args, script, files
$checks = @(
  # The start screen. editors:all shows the New row as it will be once the editors ship.
  @("welcome-light", "light", "", $recent, @()),
  @("welcome-dark", "dark", "", $recent, @()),
  @("welcome-empty", "light", "", "", @()),
  @("welcome-keys-open", "dark", "", "$recent;key:tab", @()),
  @("welcome-keys-list", "light", "", "$recent;key:tab;key:tab;key:down;key:down", @()),
  @("welcome-keys-delete", "dark", "", "$recent;key:tab;key:tab;key:down;key:del", @()),
  @("welcome-new", "light", "", "$recent;editors:all;key:tab;key:tab;key:right", @()),
  @("welcome-narrow", "light", "--size 480x700", "$recent;editors:all", @()),
  @("welcome-light-200", "light", "--scale 2 --size 2200x1400", "$recent;key:tab;key:tab", @()),
  @("welcome-dark-200", "dark", "--scale 2 --size 2200x1400", "$recent;editors:all", @()),
  @("text-light", "light", "", "", @("text.pdf")),
  @("night-reading", "dark", "--invert", "view:facing;cmd:fit-page", @("text.pdf")),
  @("two-pages", "light", "", "view:facing;cmd:fit-page", @("text.pdf")),
  @("mixed-sizes", "dark", "", "view:facing;cmd:fit-page;page:4", @("mixed-sizes.pdf")),
  @("search", "light", "", "cmd:find;type:fox;wait:400", @("text.pdf")),
  @("search-600-pages", "light", "", "cmd:find;type:needle;wait:2000", @("big-600-pages.pdf")),
  @("selection-bar", "light", "", "drag:200,300,600,360", @("text.pdf")),
  @("highlight", "light", "", "drag:200,300,600,360;highlight", @("text.pdf")),
  @("form-typing", "light", "", "click:560,285;type:Jane Doe;key:tab;type:jane@example.com", @("form.pdf")),
  @("note-hover-light", "light", "", "note:760,210,Check this paragraph before signing.|Payment terms need a closer look.;move:770,222;wait:600", @("text.pdf")),
  @("note-hover-night", "dark", "--invert", "note:760,300,Bring the passports.;move:770,312;wait:600", @("form.pdf")),
  @("note-markers", "dark", "", "note:760,210,One;page:3;note:300,500,Two;page:8;note:500,300,Three;page:1;move:1070,206;wait:300", @("text.pdf")),
  @("note-new", "light", "", "opennote:500,300;type:Typing works|Second line", @("text.pdf")),
  @("note-edit-existing", "dark", "", "note:760,210,Existing note|with two lines;click:772,222;type: and more", @("text.pdf")),
  @("password-wrong", "light", "", "type:wrong;key:enter", @("password-secret.pdf")),
  @("damaged-files", "dark", "", "", @("damaged.pdf", "not-a-pdf.pdf")),
  @("presentation", "light", "", "cmd:present;key:right", @("text.pdf")),
  # The widget gallery (--widget-gallery): every control, by mouse and by keyboard, light and dark, 100% and 200%.
  @("gallery-light", "light", "--widget-gallery", "", @()),
  @("gallery-dark", "dark", "--widget-gallery", "", @()),
  @("gallery-light-200", "light", "--widget-gallery --scale 2 --size 2200x1400", "", @()),
  @("gallery-dark-200", "dark", "--widget-gallery --scale 2 --size 2200x1400", "", @()),
  @("gallery-dropdown", "light", "--widget-gallery", "click:243,268;move:243,420", @()),
  @("gallery-dropdown-200", "dark", "--widget-gallery --scale 2 --size 2200x1400", "click:486,536;move:486,760", @()),
  @("gallery-combo-typing", "dark", "--widget-gallery", "click:560,268;type:Ge;key:alt+down", @()),
  @("gallery-split-colors", "light", "--widget-gallery", "click:895,268;move:930,330", @()),
  @("gallery-split-keys", "light", "--widget-gallery", "key:tab;key:tab;key:tab;key:tab;key:tab;key:down", @()),
  @("gallery-picker-keys", "dark", "--widget-gallery", "key:tab;key:tab;key:tab;key:tab;key:tab;key:tab;key:enter;key:down;key:down;key:right;key:right", @()),
  @("gallery-spin-list", "light", "--widget-gallery", "click:560,510;key:up;key:up;click:820,541;type:in", @()),
  @("gallery-toolbar-font", "light", "--widget-gallery", "click:316,64;move:250,200", @()),
  # The shell: side panels (Format is controls, Comments is drawn by the tab), the status bar and
  # command search (Alt+Q), by mouse and by keyboard. Panels need a wider window for three cards.
  # (Clicks are in client pixels: the client area is 16 pixels narrower than --size.)
  @("gallery-panel-light", "light", "--widget-gallery --size 1400x800", "click:1282,64;move:1356,114", @()),
  @("gallery-panel-dark", "dark", "--widget-gallery --size 1400x800", "key:alt+q;type:format;key:enter;key:f6", @()),
  @("gallery-panel-light-200", "light", "--widget-gallery --scale 2 --size 2800x1600", "click:2580,128;move:2400,800", @()),
  @("gallery-panel-dark-200", "dark", "--widget-gallery --scale 2 --size 2800x1600", "key:alt+q;type:format;key:enter;key:f6;key:tab;key:alt+down", @()),
  @("gallery-comments-light", "light", "--widget-gallery --size 1400x800", "click:1320,64;key:f6;key:down", @()),
  @("gallery-comments-dark-200", "dark", "--widget-gallery --scale 2 --size 2800x1600", "click:2656,128;key:f6;key:end", @()),
  @("gallery-status-light", "light", "--widget-gallery", "key:f6;key:right;key:right;key:right", @()),
  @("gallery-status-dark", "dark", "--widget-gallery", "drag:936,678,966,678", @()),
  @("gallery-status-light-200", "light", "--widget-gallery --scale 2 --size 2200x1400", "key:f6;key:left;key:left", @()),
  @("gallery-status-dark-200", "dark", "--widget-gallery --scale 2 --size 2200x1400", "click:2028,1364;click:2028,1364", @()),
  @("gallery-commands-light", "light", "--widget-gallery", "key:alt+q;type:fi", @()),
  @("gallery-commands-dark", "dark", "--widget-gallery", "key:alt+q;type:theme;key:down", @()),
  @("gallery-commands-light-200", "light", "--widget-gallery --scale 2 --size 2200x1400", "key:alt+q;type:pa;move:1092,288", @()),
  @("gallery-commands-dark-200", "dark", "--widget-gallery --scale 2 --size 2200x1400", "key:alt+q;type:fi;key:down;key:down", @()),
  @("commands-pdf", "light", "", "key:alt+q;type:page", @("text.pdf")),
  # Word documents (D1): the document tab, two pages at night, find, a selection, command search.
  @("doc-report", "light", "", "", @("..\office\layout\report.docx")),
  @("doc-night", "dark", "--invert", "view:facing;cmd:fit-page", @("..\office\layout\tabs-columns.docx")),
  @("doc-search", "light", "", "cmd:find;type:the;wait:400", @("..\office\layout\report.docx")),
  @("doc-selection", "light", "", "drag:200,300,600,360", @("..\office\layout\report.docx")),
  @("doc-commands", "dark", "", "key:alt+q;type:page", @("..\office\layout\footnotes.docx")),
  # Editing (D2): typing with the caret, typing at night, replacing every match.
  @("doc-typing", "light", "", "key:ctrl+end;key:enter;type:Typed in Featherweight, with a caret.|And a second paragraph", @("..\office\layout\report.docx")),
  @("doc-typing-night", "dark", "--invert", "key:ctrl+home;type:Night typing ", @("..\office\layout\report.docx")),
  @("doc-replace", "light", "", "cmd:replace;type:margin;key:tab;type:MARGIN;cmd:replaceall;wait:300", @("..\office\layout\report.docx")),
  # Files (D2): a new document, and an edited one (the tab's dot, Save enabled).
  @("doc-new", "light", "", "new", @()),
  @("doc-password-wrong", "dark", "", "type:wrong;key:enter", @("..\office\protected\report-featherweight.docx")),
  @("doc-password-open", "light", "", "type:featherweight;key:enter;wait:300", @("..\office\protected\report-featherweight.docx")),
  @("doc-edited", "dark", "", "edited", @("..\office\layout\report.docx")),
  # Proofing (D2): typing with AutoCorrect ("teh", "(c)", "--", capitals, a link), red squiggles, words and language.
  @("doc-proofing", "light", "", "type:teh quikc brown fox (c) jumps over teh dog -- it said ok, see www.example.com now;wait:600", @("..\office\layout\report.docx")),
  # Pictures (D3): inserted and selected (handles, the picture tools), square wrapping after a
  # drag, cropping, the Picture panel at night.
  @("doc-picture", "light", "", "key:ctrl+home;picture:$root\docs\logo.png", @("..\office\layout\report.docx")),
  @("doc-picture-moved", "light", "", "key:ctrl+home;picture:$root\docs\logo.png;cmd:902;drag:300,400,600,330", @("..\office\layout\report.docx")),
  @("doc-picture-crop", "light", "", "key:ctrl+home;picture:$root\docs\logo.png;cmd:908;drag:466,414,380,414", @("..\office\layout\report.docx")),
  @("doc-picture-panel", "dark", "", "key:ctrl+home;picture:$root\docs\logo.png;cmd:911", @("..\office\layout\report.docx")),
  # Tables (D3): text made a table, a column selected (the Table menu in the toolbar); the Table panel at night.
  @("doc-table", "light", "", "new;type:Name;key:tab;type:Qty|Apples;key:tab;type:3|Pears;key:tab;type:5;cmd:selectall;cmd:1153;key:ctrl+home;key:tab;key:tab;cmd:1166", @()),
  @("doc-table-panel", "dark", "", "key:ctrl+end;key:enter;type:Item;key:tab;type:Price;key:shift+home;cmd:1153;cmd:1173", @("..\office\layout\report.docx")),
  # Page layout (D3): landscape in two columns, hyphenated, with the Page setup panel; at night, a
  # page break (Ctrl+Enter) and a next-page section break near the start.
  @("doc-page-setup", "light", "", "cmd:1253;cmd:1259;cmd:1268;cmd:1250;wait:400", @("..\office\layout\report.docx")),
  @("doc-page-breaks", "dark", "", "key:ctrl+home;key:right;key:ctrl+enter;cmd:1263;view:facing;wait:300", @("..\office\layout\report.docx")),
  # The Paragraph panel on a justified paragraph with exact line spacing.
  @("doc-paragraph", "dark", "", "key:ctrl+home;key:down;key:down;cmd:1271;wait:300", @("..\office\layout\report.docx")),
  # Optional and non-breaking hyphens typed in the heading (Ctrl+-, Ctrl+Shift+-), formatting marks shown.
  @("doc-hyphens", "light", "", "key:ctrl+home;key:right;key:right;key:ctrl+minus;key:right;key:ctrl+shift+minus;key:ctrl+shift+8;wait:200", @("..\office\layout\report.docx")),
  # Footnotes and tables of contents (D4): a footnote (Ctrl+Alt+F) typed in place at the foot of the
  # page; at night, a table of contents inserted at the top, its entries on the pages the layout gives.
  @("doc-footnote", "light", "", "key:ctrl+home;key:end;key:ctrl+alt+f;type:A footnote, typed in place.;wait:300", @("..\office\layout\report.docx")),
  @("doc-toc", "dark", "", "key:ctrl+home;cmd:1322;key:ctrl+home;wait:400", @("..\office\layout\report.docx")),
  # Printing a document (vector, through Microsoft Print to PDF), then the PDF it made.
  @("doc-print", "light", "", "print:$out\doc-print.pdf", @("..\office\layout\textbox-table.docx")),
  @("doc-printed", "light", "", "view:facing;cmd:fit-page", @("..\..\build\visual\doc-print.pdf")),
  # Export PDF (D4): the drawings document exported (and its pages as the export draws them, for
  # fwtool pdf-check --pngs), then the PDF it made, with its bookmarks.
  @("doc-export", "light", "", "exportpdf:$out\doc-export.pdf;pagepngs:$out\doc-export", @("..\office\generated\drawings-dml.docx")),
  @("doc-exported", "light", "", "view:facing;cmd:fit-page", @("..\..\build\visual\doc-export.pdf"))
)

foreach ($c in $checks) {
  $name, $theme, $extra, $script, $files = $c
  $args = @("--snapshot", (Join-Path $out "$name.png"), "--size", "1100x700", "--theme", $theme)
  if ($extra) { $args += $extra -split ' ' }
  if ($script) { $args += @("--script", $script) }
  foreach ($f in $files) { $args += (Join-Path $pdfs $f) }
  $p = Start-Process -FilePath $Exe -ArgumentList ($args | ForEach-Object { if ($_ -match '\s') { "`"$_`"" } else { $_ } }) -Wait -PassThru -NoNewWindow
  $status = if ($p.ExitCode -eq 0 -and (Test-Path (Join-Path $out "$name.png"))) { "ok" } else { "FAILED ($($p.ExitCode))" }
  "{0,-22} {1}" -f $name, $status
}
"Images are in $out"
