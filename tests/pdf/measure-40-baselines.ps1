<#
.SYNOPSIS
  4.0's numbers for the gate's "no lower than 4.0's" fidelity checks that 4.0's own gate didn't record
  (F1, f1-gate): printed workbook pages (scripts\sheet-pages.ps1) and cell text (fwtool compare --cells),
  measured with 4.0's binaries exactly as scripts\f1-gate.ps1's fidelity.print and fidelity.cells
  measure the head. Writes tests\pdf\f1-gate-baselines.json (commit it).

.DESCRIPTION
  -Build40: a build of 4.0's final code (57930657, the same code as v4.0.0 but its version number) in
  the shipped configuration with fwtool: by default the coordinator's v40-final worktree. Between steps
  it waits while another session holds FeatherweightNotes\quiet-pc.lock (their quiet benches).
#>
param(
  [string]$Build40 = "D:\Documents\Codex\Featherweight\.claude\worktrees\v40-final\build-release",
  [string]$Corpus = $(if ($env:FW_CORPUS) { $env:FW_CORPUS } else { "D:\Documents\Codex\FeatherweightCorpus" }),
  [string]$Out = "build\baselines-40"
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $root
$exe = Join-Path $Build40 "Release\Featherweight.exe"
$fwtool = Join-Path $Build40 "tools\fwtool\Release\fwtool.exe"
foreach ($f in $exe, $fwtool) { if (-not (Test-Path $f)) { throw "$f isn't there" } }
$Out = Join-Path $root $Out
New-Item -ItemType Directory -Force $Out | Out-Null
$lock = "D:\Documents\Codex\FeatherweightNotes\quiet-pc.lock"
function Wait-Quiet { while (Test-Path $lock) { "  waiting: quiet-pc.lock is held ($((Get-Content $lock -Raw -ErrorAction SilentlyContinue) -replace '\s+', ' '))"; Start-Sleep -Seconds 120 } }

$result = [ordered]@{ _measured = "$(Get-Date -Format s) with $exe ($((Get-FileHash $exe).Hash.Substring(0, 16))...), by tests\pdf\measure-40-baselines.ps1" }
$sets = @(
  @{ key = "printPublic"; ref = (Join-Path $root "tests\office\reference"); folders = "excel" },
  @{ key = "printXlsx"; ref = (Join-Path $Corpus "reference"); folders = "xlsx" },
  @{ key = "printXlsOds"; ref = (Join-Path $Corpus "reference"); folders = "xls,ods" })
foreach ($set in $sets) {
  Wait-Quiet
  "== $($set.key): sheet-pages $($set.folders)"
  $o = Join-Path $Out $set.key
  & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root "scripts\sheet-pages.ps1") -Reference $set.ref -Folders $set.folders `
    -Featherweight $exe -Fwtool $fwtool -Out $o -MinPages 0 *>&1 | Out-File (Join-Path $Out "$($set.key).log") -Encoding utf8
  $s = Get-Content (Join-Path $o "summary.json") -Raw -Encoding UTF8 | ConvertFrom-Json
  $pages = (@($s.files) | Measure-Object pages -Sum).Sum; $passing = (@($s.files) | Measure-Object passing -Sum).Sum
  $result[$set.key] = [ordered]@{ passing = [int]$passing; pages = [int]$pages }
  "   $passing of $pages"
}
foreach ($f in "xlsx", "xls", "ods") {
  Wait-Quiet
  "== cells $f"
  $j = Join-Path $Out "cells-$f.json"
  $prev = $ErrorActionPreference; $ErrorActionPreference = "Continue"
  & $fwtool compare --cells --folders $f --json $j (Join-Path $Corpus "reference") 2>&1 | Out-File (Join-Path $Out "cells-$f.log") -Encoding utf8
  $ErrorActionPreference = $prev
  $c = Get-Content $j -Raw -Encoding UTF8 | ConvertFrom-Json
  $result["cells" + $f.Substring(0, 1).ToUpper() + $f.Substring(1)] = [ordered]@{ percent = [double]$c.percent; cells = [long]$c.cells; same = [long]$c.same }
  "   $($c.percent) % of $($c.cells) cells"
}
$target = Join-Path $root "tests\pdf\f1-gate-baselines.json"
[IO.File]::WriteAllText($target, (ConvertTo-Json $result -Depth 4), (New-Object Text.UTF8Encoding $false))
"Wrote $target"
