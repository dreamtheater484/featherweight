<#
.SYNOPSIS
  P2's "build from scratch" check (docs\p2-plan.md): a 10-slide deck (tests\office\build\deck.steps:
  a title slide, bullets, a picture, a table, a chart, shapes, a theme, notes, slide numbers) is built
  from a blank presentation by Featherweight alone, through the presentation tab's commands as the
  keyboard and mouse use them, and saved. Then:
  1. fwtool check and the Open XML SDK validator: no errors in the saved deck.
  2. Featherweight draws every slide (the script op pagepngs:, as the screen does).
  3. PowerPoint (scripts\powerpoint-open-check.ps1) opens the saved deck without a repair and saves
     it as PDF.
  4. fwtool pdf-compare --pngs: every slide at SSIM >= 0.95 between PowerPoint's PDF and what
     Featherweight drew; and the deck has the steps' number of slides.
  The exit code is 1 when anything failed, 3 when PowerPoint couldn't be used (no active session, or
  someone's PowerPoint running): run it again with -Reuse then.

  Featherweight runs as a --snapshot run of its own (hidden, its own settings and clipboard), never
  Roy's instance. The app's script ops each step becomes are in ConvertTo-AppScript below (and
  FeatherweightNotes\p2\p2-corpus-build-ops.md, where the pieces that own them are named); -DryRun
  writes the script without running anything.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\build-deck.ps1 -Featherweight build\Release\Featherweight.exe
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\build-deck.ps1 -DryRun
#>
param(
  [string]$Steps = "tests\office\build\deck.steps",
  [string]$Out = "build\build-deck",
  [string]$Featherweight = "build\Release\Featherweight.exe",
  [string]$Fwtool = "build\tools\fwtool\Release\fwtool.exe",
  [int]$TimeoutSeconds = 180,
  [switch]$DryRun,
  # Keep the saved deck and Featherweight's slides already in -Out: only PowerPoint and the comparison.
  [switch]$Reuse
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
foreach ($v in "Steps", "Out", "Featherweight", "Fwtool") {
  $p = Get-Variable $v -ValueOnly
  if (-not [IO.Path]::IsPathRooted($p)) { Set-Variable $v (Join-Path $root $p) }
}
New-Item -ItemType Directory -Force $Out | Out-Null
$name = [IO.Path]::GetFileNameWithoutExtension($Steps)
$target = Join-Path $Out "$name.pptx"
$slidesDir = Join-Path $Out "slides"

# Reads the quoted texts of a step ('\"' is a quote, '\\' a backslash; other backslashes, as in a
# Windows path, are kept).
function Get-Quoted([string]$rest) { @([regex]::Matches($rest, '"((?:[^"\\]|\\.)*)"') | ForEach-Object { $_.Groups[1].Value -replace '\\(["\\])', '$1' }) }

# ---- The steps as the app's script ops (App::RunScript; ';' separates ops, so a typed ';' is "\;").
# Generic ops: key:, type: ('|' types Enter), picture:, saveas:, pagepngs:. The presentation tab's
# (ScriptOp in src\slide\view, by piece):
#   new:presentation                          a blank deck, 16:9, Office Theme (p2-slides)
#   layout:<name>                             the current slide's layout (p2-slides); Ctrl+M a new slide
#   theme:<name>                              a built-in theme (p2-slides)
#   notestext:<text>                          the current slide's notes, as typed in the notes pane (p2-slides)
#   headerfooter:slidenumber=1|footer=<text>|title=0   Header & Footer, Apply to All (p2-slides)
#   edit:title|subtitle|body|body2            the caret into that placeholder, at its end (p2-text)
#   fontsize:<points>, fontcolor:<RRGGBB>     the selected text (p2-text); Ctrl+B, Ctrl+I as keys
#   insert:table|<rows>x<cols>                Insert > Table, the caret in its first cell (p2-objects)
#   insert:chart|<column|bar|line|pie>        Insert > Chart with its sample data (p2-objects)
#   insert:shape|<preset>|x,y,w,h             Insert > Shapes drawn there, in points, selected (p2-objects)
#   insert:textbox|x,y,w,h                    a text box there, the caret in it (p2-objects)
#   shapefill:<RRGGBB>                        the selected shape's fill (p2-objects)
function ConvertTo-AppScript([string]$stepsPath, [string]$saveTo, [string]$pngDir) {
  $ops = New-Object Collections.Generic.List[string]
  $ops.Add("new:presentation")
  $editing = $false  # the caret is in a text: Esc leaves it before the next step that isn't about that text
  $first = $true
  function Add-Typed([string]$text) { $ops.Add("type:" + ($text -replace ';', '\;')) }
  function Stop-Editing { if ($script:editing) { $ops.Add("key:esc"); $script:editing = $false } }
  $script:editing = $false
  $n = 0
  foreach ($line in Get-Content -LiteralPath $stepsPath -Encoding UTF8) {
    $n++
    $line = $line.Trim()
    if (-not $line -or $line.StartsWith("#")) { continue }
    if ($line -notmatch '^(\S+)\s*(.*)$') { throw "line ${n}: can't read '$line'" }
    $op = $Matches[1]; $rest = $Matches[2]
    $quotes = @(Get-Quoted $rest)
    $words = @(($rest -replace '"((?:[^"\\]|\\.)*)"', '') -split '\s+' | Where-Object { $_ })
    if ($op -ne "format") { Stop-Editing }
    switch ($op) {
      "theme" { $ops.Add("theme:" + $quotes[0]) }
      "slide" {
        if (-not $first) { $ops.Add("key:ctrl+m") }
        $ops.Add("layout:" + $quotes[0])
        $first = $false
      }
      "text" {
        $ops.Add("edit:" + $words[0])
        Add-Typed $quotes[0]
        $script:editing = $true
      }
      "bullets" {
        $ops.Add("edit:" + $words[0])
        $level = 0
        for ($i = 0; $i -lt $quotes.Count; $i++) {
          $item = $quotes[$i]
          $want = ([regex]::Match($item, '^>*')).Value.Length
          if ($i) { $ops.Add("key:enter") }  # a new paragraph at the level of the one before
          for (; $level -lt $want; $level++) { $ops.Add("key:tab") }
          for (; $level -gt $want; $level--) { $ops.Add("key:shift+tab") }
          Add-Typed $item.Substring($want)
        }
        $script:editing = $true
      }
      "format" {
        if (-not $script:editing) { throw "line ${n}: format needs text just typed" }
        $ops.Add("key:ctrl+a")
        switch -regex ($words[0]) {
          '^bold$' { $ops.Add("key:ctrl+b"); break }
          '^italic$' { $ops.Add("key:ctrl+i"); break }
          '^size=(\d+)$' { $ops.Add("fontsize:" + $Matches[1]); break }
          '^color=([0-9A-Fa-f]{6})$' { $ops.Add("fontcolor:" + $Matches[1]); break }
          default { throw "line ${n}: unknown format '$($words[0])'" }
        }
      }
      "notes" { $ops.Add("notestext:" + ($quotes[0] -replace ';', '\;')) }
      "picture" {
        $file = if ([IO.Path]::IsPathRooted($quotes[0])) { $quotes[0] } else { Join-Path $root $quotes[0] }
        if (-not (Test-Path -LiteralPath $file)) { throw "line ${n}: no picture $file" }
        $ops.Add("picture:" + $file)
      }
      "table" {
        $ops.Add("insert:table|" + $words[0])
        for ($r = 0; $r -lt $quotes.Count; $r++) {
          $cells = $quotes[$r] -split '\|'
          for ($c = 0; $c -lt $cells.Count; $c++) {
            if ($r -or $c) { $ops.Add("key:tab") }  # the next cell (Tab at a row's end goes to the next row)
            Add-Typed $cells[$c]
          }
        }
        $script:editing = $true
      }
      "chart" { $ops.Add("insert:chart|" + $words[0]) }
      "shape" {
        $ops.Add("insert:shape|" + $words[0] + "|" + $words[1])
        foreach ($w in $words[2..($words.Count)] | Where-Object { $_ }) {
          if ($w -match '^fill=([0-9A-Fa-f]{6})$') { $ops.Add("shapefill:" + $Matches[1]) }
          else { throw "line ${n}: unknown shape option '$w'" }
        }
        if ($quotes.Count) { Add-Typed $quotes[0]; $ops.Add("key:esc") }  # typing into a selected shape is its text
        $ops.Add("key:esc")  # nothing selected
      }
      "textbox" {
        $ops.Add("insert:textbox|" + $words[0])
        Add-Typed $quotes[0]
        $script:editing = $true
      }
      "footer" {
        $hf = "headerfooter:title=0"
        if ($words -contains "slide-numbers") { $hf += "|slidenumber=1" }
        if ($rest -match 'footer="((?:[^"\\]|\\.)*)"') { $hf += "|footer=" + ($Matches[1] -replace '\\(["\\])', '$1' -replace ';', '\;') }
        $ops.Add($hf)
      }
      "save" { $ops.Add("saveas:" + $saveTo) }
      default { throw "line ${n}: unknown step '$op'" }
    }
  }
  Stop-Editing
  # Every slide as Featherweight draws it, at the PDF comparison's 100 dpi.
  $ops.Add("pagepngs:$pngDir|100")
  $ops -join ";"
}

function Get-StepSlides([string]$stepsPath) { @(Get-Content -LiteralPath $stepsPath -Encoding UTF8 | Where-Object { $_ -match '^\s*slide\s' }).Count }

$scriptPath = Join-Path $Out "$name.script.txt"
[IO.File]::WriteAllText($scriptPath, (ConvertTo-AppScript $Steps $target $slidesDir), [Text.UTF8Encoding]::new($false))
if ($DryRun) {
  "The app script for $name ($(Get-StepSlides $Steps) slides): $scriptPath"
  (Get-Content $scriptPath -Raw) -split '(?<!\\);' | ForEach-Object { "  $_" }
  exit 0
}
if (-not (Test-Path $Fwtool)) { throw "fwtool.exe not found at $Fwtool. Build it, or pass -Fwtool." }

$failures = New-Object Collections.Generic.List[string]
# 1. Featherweight builds and saves the deck, and draws its slides.
if (-not $Reuse) {
  if (-not (Test-Path $Featherweight)) { throw "Featherweight.exe not found at $Featherweight. Build it (target Featherweight), or pass -Featherweight." }
  foreach ($old in $target, $slidesDir) { if (Test-Path $old) { Remove-Item -Recurse -Force $old } }
  New-Item -ItemType Directory -Force $slidesDir | Out-Null
  $log = Join-Path $Out "$name.log"
  "== Featherweight builds $name from a blank presentation"
  $p = Start-Process -FilePath $Featherweight -PassThru -RedirectStandardError $log -ArgumentList @(
    "--snapshot", "`"$(Join-Path $Out "$name.png")`"", "--size", "1400x900", "--script", "`"@$scriptPath`"")
  $null = $p.Handle
  if (-not $p.WaitForExit($TimeoutSeconds * 1000)) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue; $failures.Add("Featherweight didn't finish in $TimeoutSeconds s") }
  $printed = @(Get-Content $log -ErrorAction SilentlyContinue | Where-Object { $_ })
  foreach ($l in $printed) { "  Featherweight: $l" }
  if ($printed) { $failures.Add("Featherweight printed $($printed.Count) problems (a script op it didn't take, or a failed save): $Out\$name.log") }
}
if (-not (Test-Path $target)) { $failures.Add("Featherweight saved no deck ($target)") }
$want = Get-StepSlides $Steps
$pngs = @(Get-ChildItem $slidesDir -Filter "page-*.png" -ErrorAction SilentlyContinue)
if ((Test-Path $target) -and $pngs.Count -ne $want) { $failures.Add("Featherweight drew $($pngs.Count) slides; the steps make $want") }

$ppStopped = $false
if (Test-Path $target) {
  # 2. fwtool check and the validator.
  "== fwtool check"
  & $Fwtool check $target | Select-Object -Last 3
  if ($LASTEXITCODE) { $failures.Add("fwtool check finds errors in the saved deck") }
  "== Validator (Open XML SDK)"
  $validator = Join-Path $root "build\validate\fwvalidate.exe"
  & dotnet build (Join-Path $root "tools\validate\Validate.csproj") -c Release -o (Join-Path $root "build\validate") --nologo -v q | Out-Null
  if ($LASTEXITCODE) { throw "building tools\validate failed" }
  $vjson = Join-Path $Out "validate.json"
  $vlist = Join-Path $Out "validate-files.txt"
  [IO.File]::WriteAllLines($vlist, [string[]]@($target), [Text.UTF8Encoding]::new($false))
  & $validator --json $vjson --files $vlist | Select-Object -Last 3
  $verr = @((Get-Content $vjson -Raw -Encoding UTF8 | ConvertFrom-Json).files | ForEach-Object { $_.errors } | Where-Object { $_ })
  if ($verr.Count) { $failures.Add("the validator finds $($verr.Count) errors: $($verr[0].Part) $($verr[0].Description)") }

  # 3. PowerPoint opens it without a repair and saves it as PDF.
  "== PowerPoint: opens the saved deck and saves it as PDF"
  $pdfDir = Join-Path $Out "powerpoint"
  $ppJson = Join-Path $Out "powerpoint.json"
  & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "powerpoint-open-check.ps1") $target -Json $ppJson -Pdf $pdfDir | Out-Host
  if ($LASTEXITCODE -eq 3 -or $LASTEXITCODE -eq 4) { $ppStopped = $true }
  $pp = if (Test-Path $ppJson) { (Get-Content $ppJson -Raw -Encoding UTF8 | ConvertFrom-Json).files | Select-Object -First 1 } else { $null }
  if (-not $ppStopped) {
    if (-not $pp -or $pp.status -ne "ok") { $failures.Add("PowerPoint: $(if ($pp) { $pp.status } else { 'no result' })") }
    elseif ($pp.slides -ne $want) { $failures.Add("PowerPoint shows $($pp.slides) slides; the steps make $want") }
    # 4. Each slide as Featherweight drew it against PowerPoint's PDF.
    if ($pp -and $pp.pdf) {
      "== Each slide: PowerPoint's PDF against Featherweight's drawing (SSIM >= 0.95)"
      & $Fwtool pdf-compare --pngs $slidesDir --out (Join-Path $Out "diff") --json (Join-Path $Out "compare.json") $pp.pdf
      if ($LASTEXITCODE) { $failures.Add("slides below SSIM 0.95: see $Out\compare.json and $Out\diff") }
    }
  }
}

""
if ($failures.Count) { foreach ($f in $failures) { Write-Host "FAIL: $f" -ForegroundColor Red }; exit 1 }
if ($ppStopped) { "PowerPoint couldn't run now (no active session, or someone's PowerPoint is open): run this again with -Reuse then."; exit 3 }
Write-Host "Built from scratch: $want slides, no check or validator errors, PowerPoint opens it cleanly, every slide at SSIM >= 0.95." -ForegroundColor Green
