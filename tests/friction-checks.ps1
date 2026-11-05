<#
.SYNOPSIS
  The friction review's flows (docs/friction-review.md, plan section 11), run through Featherweight's
  --script mode as checks: each flow's steps as a person would take them, off-screen (snapshot
  mode), then the saved file is read back with fwtool and must say what the person wrote.

  Each step is counted as the review counts it (a click, a key or chord; typing the content itself
  isn't counted), and a flow fails when it takes more steps than its budget: Word's count, plus any
  extra the review accepts with a reason. So a flow can't gain a step without someone noticing.

  The scripts are the flows as they should go. Until a reported friction finding is fixed, the
  flow it touches fails on what the saved file says.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tests\friction-checks.ps1
  # images and saved files in build\friction
#>
param(
  [string]$Exe = "build\Release\Featherweight.exe",
  [string]$Fwtool = "build\tools\fwtool\Release\fwtool.exe",
  [string]$Out = "build\friction",
  # Also take the steps the reported findings need today (marked "?"): the content then checks out,
  # and the counts show what the friction costs.
  [switch]$Workarounds
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path $Out).Path

# The window the steps' coordinates are for: a client area of 1072x686 (--size 1100x700 where they
# were written), light theme. The style box is at 308,66; its list has Normal, No Spacing,
# Heading 1-3, Title... 31 px apart from 102.
$styleBox = "click:308,66"
$styleNoSpacing = "click:300,132"
$styleTitle = "click:279,255"
$dash = [string][char]0x2013  # the script stays ASCII: PowerShell 5.1 reads it as ANSI
$dot = [string][char]0x00B7

# A flow: its steps as "script step|count". The count is what the review counts: C (clicks),
# K (keys), D (drags), or nothing for typing the content; a step with no script step is one the
# script doesn't take (opening the file it starts with). A count ending in "?" is a step only a
# reported finding needs (run with -Workarounds); "!", one the workaround replaces. `expect` are
# regular expressions the lines of `fwtool dump --model` must match, in order; `xml`, ones the
# saved word/document.xml must match. `open` is the file it starts with (a copy), else a new
# document. `budget` is Word's count plus the extras the review accepts (docs/friction-review.md).
# The context menu is Windows' own and a script can't click it: a right-click is a click there
# (it puts the caret there too) and its menu items are the command they run (cmd:<id>), counted.
$flows = @(
  @{
    name = "letter"
    budget = 9  # Word: 6 clicks, 3 keys
    steps = @(
      "new|1C",                                   # the start screen's New document (or Ctrl+N)
      "$styleBox|1C", "$styleNoSpacing|1C",       # the address lines without space between them
      "key:esc|1K?",                              # finding F1: back to the page
      "type:Jane Doe|", "key:enter|", "type:1 Main Street|", "key:enter|", "type:3511 AB Utrecht|", "key:enter|", "key:enter|",
      "cmd:1045|2C",                              # Insert > the date (two clicks in the Insert box)
      "key:enter|", "key:enter|",
      "type:Acme B.V.|", "key:enter|", "type:Postbus 12|", "key:enter|", "type:1000 AA Amsterdam|", "key:enter|", "key:enter|",
      "key:ctrl+shift+n|1K",                      # Normal for the body
      "type:Dear Mr Jansen,|", "key:enter|",
      "type:Thank you for your letter of 2 November. I would be glad to meet next week.|", "key:enter|",
      "type:Yours sincerely,|", "key:enter|", "type:Jane Doe|",
      "save:{out}|2K"                             # Ctrl+S, Enter in the Save dialog
    )
    expect = @(
      '^\s*NoSpacing\s+\S+pt \| Jane Doe$', '^\s*NoSpacing\s+\S+pt \| 1 Main Street$', '^\s*NoSpacing\s+\S+pt \| 3511 AB Utrecht$',
      '^\s*NoSpacing\s+\S+pt \| $', '^\s*NoSpacing\s+\S+pt \| \S*\d{4}\S*$', '^\s*NoSpacing\s+\S+pt \| $',
      '^\s*NoSpacing\s+\S+pt \| Acme B\.V\.$', '^\s*NoSpacing\s+\S+pt \| Postbus 12$', '^\s*NoSpacing\s+\S+pt \| 1000 AA Amsterdam$',
      '^\s*NoSpacing\s+\S+pt \| $',
      '^\s*Normal\s+\S+pt \| Dear Mr Jansen,$', '^\s*Normal\s+\S+pt \| Thank you for your letter', '^\s*Normal\s+\S+pt \| Yours sincerely,$',
      '^\s*Normal\s+\S+pt \| Jane Doe$'
    )
  },
  @{
    name = "cv"
    budget = 12  # Word: 2 clicks, 9 keys; +1 click accepted: the style box is a list, not a gallery (plan section 5.2)
    steps = @(
      "new|1C",
      "$styleBox|1C", "$styleTitle|1C",           # the name as Title
      "key:esc|1K?",                              # finding F1
      "type:Jane Doe|", "key:enter|",             # Enter: the next paragraph is Normal (Title's w:next)
      "key:ctrl+shift+n|1K?",                     # finding F2
      "type:jane@example.com $dot Utrecht|", "key:ctrl+e|1K", "key:enter|",
      "key:ctrl+alt+1|1K", "type:Experience|", "key:enter|", "key:ctrl+shift+n|1K?",
      "key:ctrl+b|1K", "type:Product designer, Acme|", "key:ctrl+b|1K", "key:tab|1K", "type:2022$($dash)now|", "key:enter|",
      "key:ctrl+shift+l|1K", "type:Led the redesign of the app|", "key:enter|", "type:Cut onboarding time in half|", "key:enter|",
      "key:enter|!",                              # Enter on the empty item ends the list
      "key:ctrl+shift+l|1K?",                     # finding F3: the Bullets key instead
      "type:Skills: research, prototyping, Figma|", "key:enter|",
      "key:ctrl+alt+1|1K", "type:Education|", "key:enter|", "key:ctrl+shift+n|1K?",
      "type:Master of Design, TU Delft|",
      "save:{out}|2K"
    )
    expect = @(
      '^\s*Title\s+\S+pt \| Jane Doe$',
      '^\s*Normal\s+\S+pt \| jane@example\.com',
      '^\s*Heading1\s+\S+pt \| Experience$',
      "^\s*Normal\s+\S+pt \| Product designer, Acme\{tab\}2022$($dash)now$",
      '^\s*\S+\s+\S+pt \S+ \| Led the redesign of the app$', '^\s*\S+\s+\S+pt \S+ \| Cut onboarding time in half$',
      '^\s*\S+\s+\S+pt \| Skills: research, prototyping, Figma$',
      '^\s*Heading1\s+\S+pt \| Education$',
      '^\s*Normal\s+\S+pt \| Master of Design, TU Delft$'
    )
  },
  @{
    name = "table"
    open = "tests\office\torture\received-table.docx"
    budget = 16  # Word: 10 clicks, 2 keys, 2 drags; +2 clicks accepted: a row in the middle from the context menu, not a handle
    steps = @(
      "|1C",                                      # double-click the attachment: it opens ready to edit
      "dblclick:189,298|1C", "type:Chair|",       # the typo: double-click the word, type
      "click:785,340|1C", "key:tab|1K", "type:Rug|",  # a row at the end: Tab in the last cell
      "click:250,277|1C", "cmd:1156|2C",          # a row in the middle: right-click, Insert, Rows Below
      "click:250,340|1C", "cmd:1159|2C",          # delete Lamp: right-click, Delete, Rows
      "drag:560,256,760,256|1D", "cmd:1162|2C",   # merge Price and Total: select them, right-click, Merge Cells
      "drag:410,300,480,300|1D!",                 # widen Item: drag its border
      "click:250,277|1C?", "cmd:1173|1C?",        # finding F7: right-click, Properties (the Table panel)...
      "click:905,390|1C?", "type:7|?", "key:enter|1K?",  # ...Column width: 7 cm
      "save:{out}|1K"                             # Ctrl+S
    )
    expect = @(
      '^\s*\S+\s+\S+pt \| Quote for the new office$',
      '\| Item$', '\| Quantity$', '\| Price$', '\| Total$',
      '\| Desk$', '\| 2$', '\| 250$', '\| 500$', '\| $', '\| $', '\| $', '\| $',
      '\| Chair$', '\| 4$', '\| 80$', '\| 320$',
      '\| Shelf$', '\| 1$', '\| 150$', '\| 150$',
      '\| Rug$', '\| $', '\| $', '\| $',
      '\| Prices in euros, without VAT\.$'
    )
    xml = @(
      '<w:tblGrid><w:gridCol w:w="(3[5-9]\d\d|[4-9]\d\d\d)"/>',  # Item wider than its 3000 twips
      '<w:gridSpan w:val="2"/>',                                      # Price and Total merged
      '^(?!.*>Lamp<)'                                                 # Lamp's row gone
    )
  }
)

$enc = New-Object System.Text.UTF8Encoding $false
[Console]::OutputEncoding = $enc  # fwtool writes UTF-8 (the CV's en dash), whatever the console's code page

# --size is the window's; its frame, and so the client area the coordinates are in, depends on the
# display's scale. A first snapshot measures it, and the window is sized for a 1072x686 client area.
Add-Type -AssemblyName System.Drawing
$probe = Join-Path $Out "probe.png"
Start-Process -FilePath $Exe -ArgumentList @("--snapshot", "`"$probe`"", "--size", "1100x700", "--theme", "light") -Wait -NoNewWindow
$img = [Drawing.Image]::FromFile($probe)
$size = "{0}x{1}" -f (1100 + 1072 - $img.Width), (700 + 686 - $img.Height)
$img.Dispose()
Remove-Item $probe

$failed = 0
foreach ($f in $flows) {
  $docx = Join-Path $Out "$($f.name).docx"
  Remove-Item $docx -ErrorAction SilentlyContinue
  $script = @()
  $clicks = 0; $keys = 0; $drags = 0
  foreach ($s in $f.steps) {
    $bar = $s.LastIndexOf('|')
    $step = $s.Substring(0, $bar).Replace("{out}", $docx)
    $count = $s.Substring($bar + 1)
    if ($count.EndsWith("?")) {
      if (-not $Workarounds) { continue }
      $count = $count.TrimEnd("?")
    } elseif ($count.EndsWith("!")) {
      if ($Workarounds) { continue }
      $count = $count.TrimEnd("!")
    }
    if ($count -match '^(\d+)C$') { $clicks += [int]$Matches[1] }
    elseif ($count -match '^(\d+)K$') { $keys += [int]$Matches[1] }
    elseif ($count -match '^(\d+)D$') { $drags += [int]$Matches[1] }
    if ($step) { $script += $step }
  }
  $files = @()
  if ($f.open) {
    $copy = Join-Path $Out (Split-Path -Leaf $f.open)
    Copy-Item $f.open $copy -Force
    $files = @("`"$copy`"")
  }
  $scriptFile = Join-Path $Out "$($f.name).script"
  [IO.File]::WriteAllText($scriptFile, ($script -join ";"), $enc)
  $png = Join-Path $Out "$($f.name).png"
  $p = Start-Process -FilePath $Exe -ArgumentList ($files + @("--snapshot", "`"$png`"", "--size", $size, "--theme", "light",
    "--script", "`"@$scriptFile`"")) -Wait -PassThru -NoNewWindow
  $problems = @()
  if ($p.ExitCode -ne 0) { $problems += "Featherweight exited with $($p.ExitCode)" }
  $steps = $clicks + $keys + $drags
  if ($steps -gt $f.budget) { $problems += "$steps steps, more than the budget of $($f.budget)" }
  $lines = @()
  if (-not (Test-Path $docx)) {
    $problems += "nothing was saved"
  } else {
    $lines = @(& $Fwtool dump --model $docx 2>&1 | ForEach-Object { "$_" } | Where-Object { $_ -match 'pt (\S+ )?\| ' })
    $check = @(& $Fwtool check $docx 2>&1 | ForEach-Object { "$_" })
    if ($LASTEXITCODE -ne 0) { $problems += "fwtool check: " + ($check -join " ") }
    $at = 0
    foreach ($e in $f.expect) {
      while ($at -lt $lines.Count -and $lines[$at] -notmatch $e) { $at++ }
      if ($at -ge $lines.Count) { $problems += "no paragraph like /$e/ (in order)"; break }
      $at++
    }
    if ($f.xml) {
      Add-Type -AssemblyName System.IO.Compression.FileSystem
      $zip = [IO.Compression.ZipFile]::OpenRead($docx)
      try {
        $reader = New-Object IO.StreamReader($zip.GetEntry("word/document.xml").Open())
        $xml = $reader.ReadToEnd()
        $reader.Close()
      } finally { $zip.Dispose() }
      foreach ($x in $f.xml) { if ($xml -notmatch $x) { $problems += "word/document.xml has nothing like /$x/" } }
    }
  }
  $status = if ($problems.Count) { "FAIL" } else { "ok" }
  if ($problems.Count) { $failed++ }
  "{0,-8} {1,-4} {2} clicks + {3} keys + {4} drags = {5} (budget {6})" -f $f.name, $status, $clicks, $keys, $drags, $steps, $f.budget
  foreach ($pr in $problems) { "         $pr" }
  if ($problems.Count -and $lines.Count) { "         the saved document:"; $lines | ForEach-Object { "           $($_.Trim())" } }
}
"Images and saved files are in $Out"
if ($failed) { exit 1 }
