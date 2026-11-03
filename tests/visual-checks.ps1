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

# name, theme, extra args, script, files
$checks = @(
  @("welcome-light", "light", "", "recent:C:\Docs\Annual report.pdf;recent:C:\Docs\Rental agreement.pdf", @()),
  @("welcome-dark", "dark", "", "", @()),
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
  @("gallery-toolbar-font", "light", "--widget-gallery", "click:316,64;move:250,200", @())
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
