<#
.SYNOPSIS
  The workbook friction fixes (docs/friction-review.md, "Workbooks (S4)": W1, W2, W4, W6), checked
  through Featherweight's --script mode, off-screen (snapshot mode):

  - W1: at 1366, 1280 and 1024 pixels the workbook toolbar has every item it has in a wide
    window, on the bar or in its overflow menu (dumptoolbar), and Data is in that menu at 1366.
  - W2: command search (Alt+Q) finds Sort, Filter, Table, Conditional formatting, Data validation
    and Format cells (dumpcommands).
  - W4: a "greater than" filter by mouse: the column's button, the condition, the value typed,
    Enter (no click in the value box, no Apply). The saved file has the filter.
  - W6: Ctrl+T, a style's preview in the gallery, Create table. The saved table has that style.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tests\workbook-friction-checks.ps1
  # snapshots and saved files in build\workbook-friction
#>
param(
  [string]$Exe = "build\Release\Featherweight.exe",
  [string]$Out = "build\workbook-friction"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path $Out).Path
$Exe = (Resolve-Path $Exe).Path
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.IO.Compression.FileSystem

# A list with a header row: Item, Quantity, Price, Total, Date in A1:E7.
$book = Join-Path $Out "book.xlsx"
Copy-Item "tests\office\edge\extlst.xlsx" $book -Force

# Runs Featherweight on a copy of the book with `script`; returns its stderr lines (the dumps).
function Invoke-Featherweight([string]$name, [string]$size, [string]$script) {
  $copy = Join-Path $Out "$name.xlsx"
  Copy-Item $book $copy -Force
  $png = Join-Path $Out "$name.png"
  $err = Join-Path $Out "$name.txt"
  $p = Start-Process -FilePath $Exe -Wait -PassThru -NoNewWindow -RedirectStandardError $err -ArgumentList @(
    "`"$copy`"", "--snapshot", "`"$png`"", "--size", $size, "--theme", "light", "--script", "`"$script`"")
  if ($p.ExitCode -ne 0) { throw "Featherweight exited with $($p.ExitCode) ($name)" }
  return @(Get-Content $err -Encoding UTF8)
}

function Read-Part([string]$xlsx, [string]$like) {
  $zip = [IO.Compression.ZipFile]::OpenRead($xlsx)
  try {
    $text = ""
    foreach ($e in $zip.Entries | Where-Object { $_.FullName -like $like }) {
      $r = New-Object IO.StreamReader($e.Open())
      $text += $r.ReadToEnd()
      $r.Close()
    }
    return $text
  } finally { $zip.Dispose() }
}

$failed = 0
function Report([string]$name, [string[]]$problems) {
  if ($problems.Count) {
    $script:failed++
    "{0,-4} FAIL" -f $name
    foreach ($p in $problems) { "     $p" }
  } else {
    "{0,-4} ok" -f $name
  }
}

# The flows' coordinates are for a client area of 1350x692 (--size 1366x700 where they were
# written): a first snapshot measures this display's window frame.
$probe = Join-Path $Out "probe.png"
Start-Process -FilePath $Exe -ArgumentList @("--snapshot", "`"$probe`"", "--size", "1366x700", "--theme", "light") -Wait -NoNewWindow
$img = [Drawing.Image]::FromFile($probe)
$frameW = 1366 - $img.Width
$size = "{0}x{1}" -f (1350 + $frameW), (700 + 692 - $img.Height)
$img.Dispose()
Remove-Item $probe

# W1: the toolbar's items at each width, against a window wide enough for all of them.
function Get-Toolbar([string]$name, [int]$width) {
  $lines = Invoke-Featherweight $name "${width}x400" "editors:all;dumptoolbar"
  $items = @{}
  foreach ($l in $lines) {
    if ($l -match '^toolbar: (shown|overflow) (\d+) (.*)$') { $items[[int]$Matches[2]] = @{ where = $Matches[1]; text = $Matches[3] } }
  }
  return $items
}
$all = Get-Toolbar "w1-wide" 2400
$problems = @()
if (($all.Values | Where-Object { $_.where -eq "overflow" }).Count) { $problems += "2400 px: items in the overflow menu" }
foreach ($w in 1366, 1280, 1024) {
  $now = Get-Toolbar "w1-$w" $w  # a window that wide, as the review's snapshots
  foreach ($cmd in $all.Keys) {
    if (-not $now.ContainsKey($cmd)) { $problems += "$w px: $($all[$cmd].text) ($cmd) is neither on the bar nor in its overflow menu" }
  }
  foreach ($cmd in $now.Keys) {
    if ($now[$cmd].where -eq "overflow" -and $now[$cmd].text -match '\(0 choices\)$' -and $cmd -in 2704, 2717, 2719, 2854) {
      $problems += "$w px: $($now[$cmd].text) has no choices in the overflow menu"
    }
  }
  if ($w -eq 1366 -and -not ($now.ContainsKey(2719) -and $now[2719].where -eq "overflow")) {
    $problems += "1366 px: Data (2719) isn't in the overflow menu (expected there at this width)"
  }
}
Report "W1" $problems

# W2: command search.
$queries = [ordered]@{
  "sort" = '^Sort A to Z\|'; "filter" = '^Filter\|Ctrl\+Shift\+L\|1$'; "table" = '^Table.\|Ctrl\+T\|1$'
  "conditional formatting" = '^Conditional formatting: new rule.\|\|1$'; "data validation" = '^Data validation.\|\|1$'
  "format cells" = '^Format cells.\|Ctrl\+1\|1$'
}
$script = "editors:all;" + (($queries.Keys | ForEach-Object { "dumpcommands:$_" }) -join ";")
$lines = Invoke-Featherweight "w2" $size $script
$problems = @()
foreach ($q in $queries.Keys) {
  $found = @($lines | Where-Object { $_.StartsWith("commands[$q]: ") } | ForEach-Object { $_.Substring("commands[$q]: ".Length) })
  if (-not ($found | Where-Object { $_ -match $queries[$q] })) { $problems += "Alt+Q `"$q`" finds: " + ($found -join "; ") }
}
Report "W2" $problems

# W4: Ctrl+Shift+L, Quantity's filter button, the condition box, "is greater than", 3, Enter.
$saved = Join-Path $Out "w4-saved.xlsx"
Remove-Item $saved -ErrorAction SilentlyContinue
$null = Invoke-Featherweight "w4" $size "editors:all;key:ctrl+shift+l;click:150,150;click:1125,491;click:1105,215;type:3;key:enter;saveas:$saved"
$problems = @()
if (-not (Test-Path $saved)) { $problems += "nothing was saved" }
elseif ((Read-Part $saved "xl/worksheets/sheet1.xml") -notmatch '<customFilter operator="greaterThan" val="3"/>') {
  $problems += "the saved sheet has no 'greater than 3' filter on Quantity"
}
Report "W4" $problems

# W6: Ctrl+T, Medium 9's preview (the Medium group's second row, second column), Create table.
$saved = Join-Path $Out "w6-saved.xlsx"
Remove-Item $saved -ErrorAction SilentlyContinue
$null = Invoke-Featherweight "w6" $size "editors:all;key:ctrl+t;click:1110,459;click:1115,633;saveas:$saved"
$problems = @()
if (-not (Test-Path $saved)) { $problems += "nothing was saved" }
elseif ((Read-Part $saved "xl/tables/*.xml") -notmatch '<tableStyleInfo name="TableStyleMedium9"') {
  $problems += "the saved table isn't in Medium 9"
}
Report "W6" $problems

"Snapshots and saved files are in $Out"
if ($failed) { exit 1 }
