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

# The window the steps' coordinates are for: --size 1100x700, light theme. The style box is at
# 308,66; its list has Normal, No Spacing, Heading 1-3, Title... 31 px apart from 102.
$styleBox = "click:308,66"
$styleNoSpacing = "click:300,132"
$styleTitle = "click:279,255"
$dash = [string][char]0x2013  # the script stays ASCII: PowerShell 5.1 reads it as ANSI
$dot = [string][char]0x00B7

# A flow: its steps as "script step|count". The count is what the review counts: C (clicks),
# K (keys), both ("2C"), or nothing for typing the content. A count ending in "?" is a step only
# a reported finding needs (run with -Workarounds); "!", one the workaround replaces. `expect` are regular expressions the
# lines of `fwtool dump --model` must match, in order. `budget` is Word's count plus the extras
# the review accepts (docs/friction-review.md).
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
  }
)
# "Fix a table in a received document" joins when table editing lands (D3, claude/d3-tables).

$enc = New-Object System.Text.UTF8Encoding $false
$failed = 0
foreach ($f in $flows) {
  $docx = Join-Path $Out "$($f.name).docx"
  Remove-Item $docx -ErrorAction SilentlyContinue
  $script = @()
  $clicks = 0; $keys = 0
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
    $script += $step
  }
  $scriptFile = Join-Path $Out "$($f.name).script"
  [IO.File]::WriteAllText($scriptFile, ($script -join ";"), $enc)
  $png = Join-Path $Out "$($f.name).png"
  $p = Start-Process -FilePath $Exe -ArgumentList @("--snapshot", "`"$png`"", "--size", "1100x700", "--theme", "light",
    "--script", "`"@$scriptFile`"") -Wait -PassThru -NoNewWindow
  $problems = @()
  if ($p.ExitCode -ne 0) { $problems += "Featherweight exited with $($p.ExitCode)" }
  $steps = $clicks + $keys
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
  }
  $status = if ($problems.Count) { "FAIL" } else { "ok" }
  if ($problems.Count) { $failed++ }
  "{0,-8} {1,-4} {2} clicks + {3} keys = {4} (budget {5})" -f $f.name, $status, $clicks, $keys, $steps, $f.budget
  foreach ($pr in $problems) { "         $pr" }
  if ($problems.Count -and $lines.Count) { "         the saved document:"; $lines | ForEach-Object { "           $($_.Trim())" } }
}
"Images and saved files are in $Out"
if ($failed) { exit 1 }
