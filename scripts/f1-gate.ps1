<#
.SYNOPSIS
  F1's automated gate for release 4.1 (docs/f1-plan.md, "The 4.1 gate"), instead of Roy's manual check.
  Every row is passed, failed, or waiting for one of Roy's steps, with its numbers, in gate-4.1.md and
  gate-4.1.json. release.ps1 -Publish refuses unless that report is for the commit being released and
  has no failed row (f1-gate.ps1 -Verify).

.DESCRIPTION
  The rows are the plan's: build, damage (never damage a file), fidelity, legal, lang, pdf, ship
  (product basics), review (external review), light (lightweight), friction and docs. Each row is a
  few checks. A check is:
    passed   its target is met, with the numbers;
    failed   its target isn't met, or what it checks isn't built yet (the piece that builds it is named);
    waiting  it waits for one of Roy's steps (the plan's "Roy's steps"); it doesn't block the release,
             and the release notes list it;
    not run  no result on this commit (a long check this run left out): counts as failed.
  A row is failed when one of its checks is failed or not run, waiting when one waits, else passed.

  Kinds of check:
    quick    seconds: run on every call.
    ci       reads GitHub (gh): the commit's CI runs and code scanning. Run on every call.
    long     minutes to hours: builds, corpus runs, torture, Office. Run when there's no result for this
             commit yet (or with -Fresh, or when named in -Checks); -Quick leaves them out.
    timing   the quiet-PC timings: run only with -Timings. The final timing run is the coordinator's
             (budgets.ps1 -Timings -Runs 3 -Record), which this gate reads from the ledger.
  A long check's result is kept in <Out>\results\<commit>\<check>.json. A result measured on an earlier
  commit still counts when only docs changed since (docs\, *.md, tests\budgets\ledger.json), and the
  report says which commit it's from. A run on a tree with uncommitted changes is marked "dirty": it
  counts for nothing, and its results aren't kept for reuse.

  Office (Word, Excel, PowerPoint): the open checks run in Roy's active session (at the console or
  over Remote Desktop) when the PC isn't locked, none of those programs is running, and no one holds
  D:\Documents\Codex\FeatherweightNotes\quiet-pc.lock. The gate takes the lock for them (and sets
  FW_HOLDS_QUIET_PC_LOCK=1), and releases it after each. It waits up to -OfficeWaitMinutes for that.

  Pieces run it as they go, for their own rows: -Rows pdf, or -Checks pdf.readers. The final run is on
  the final head's shipped build, all rows, with -Report D:\Documents\Codex\FeatherweightNotes\f1.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\f1-gate.ps1 -Quick
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\f1-gate.ps1 -Rows pdf,legal
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\f1-gate.ps1 -Report D:\Documents\Codex\FeatherweightNotes\f1
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\f1-gate.ps1 -Verify D:\Documents\Codex\FeatherweightNotes\f1\gate-4.1.json
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\f1-gate.ps1 -List
#>
param(
  # Rows to work on (build, damage, fidelity, legal, lang, pdf, ship, review, light, friction, docs);
  # the report always has every row, with the others' kept results.
  [string[]]$Rows = @(),
  # Checks to run again even when a result for this commit is kept (implies their rows).
  [string[]]$Checks = @(),
  [switch]$Quick,
  [switch]$Fresh,
  [switch]$Timings,
  # Build the shipped configuration from scratch (deletes -Build first).
  [switch]$CleanBuild,
  [string]$Out = "build\f1-gate",
  # Also write gate-4.1.md and gate-4.1.json here (the final run: D:\Documents\Codex\FeatherweightNotes\f1).
  [string]$Report,
  [string]$Corpus = $(if ($env:FW_CORPUS) { $env:FW_CORPUS } else { "D:\Documents\Codex\FeatherweightCorpus" }),
  # The shipped configuration (release.ps1's: FW_BUILD_TESTS=ON, FW_TEST_SCREENS=OFF).
  [string]$Build = "build-release",
  # A test build (FW_TEST_SCREENS on): lang.clipping runs its script step clipcheck, which the shipped exe hasn't.
  [string]$TestBuild = "build",
  [int]$OfficeWaitMinutes = 120,
  # Check a report instead of making one: exit 0 when it's for -Commit (default HEAD) and has no failed row.
  [string]$Verify,
  [string]$Commit,
  # A trial's CI rows read this commit's runs instead of the head's (a head that differs from it only in the
  # gate's own files, before CI has run on it). A report made with it is never publishable.
  [string]$CiCommit,
  # Print the rows and checks and exit.
  [switch]$List
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$Version = "4.1"
$utf8 = New-Object Text.UTF8Encoding $false

# ---------------------------------------------------------------------------------------------------
# Targets and 4.0's numbers (the baselines "no lower than 4.0's" is measured against).
# ---------------------------------------------------------------------------------------------------
$exe40 = 13167616                      # 4.0's Featherweight.exe (57930657 = v4.0.0 but the version)
$exeBudget = $exe40 + 1024 * 1024      # F1 grows the exe by at most 1,024 KB
$downloadBudget = 10 * 1024 * 1024     # the default download without V8 (18.2 MB at 4.0)
$catalogBudget = 40 * 1024             # each language, compressed, in the exe
$baselines = [ordered]@{
  # docs/TEST_CHECKLIST.md, P1's and P3's rows, on 4.0's final head (6fe39e23). Slides are compared with 4.0
  # measured without Office's cloud fonts (the coordinator, 2026-11-25): f1-word's slide-pages run on 04e9a4bc
  # with the cloud fonts gone and no line-breaker change (r1-word-5f2617\build\spB0, spPB0, spOA): .pptx 2,349 (2,370 with
  # them), .ppt 2,322 (2,344), .odp 2,042 (2,059).
  slidesPptx = @{ passing = 2349; slides = 2516; folder = "pptx"; through = "fwtool" }
  slidesPpt  = @{ passing = 2263; slides = 2577; folder = "ppt"; through = "fwtool" }  # f1-word's spPB0 through fwtool, as the gate measures: fwtool refuses the two encrypted decks (60 slides; P3 counted them through the app, 59 of 60, for 2,322)
  slidesOdp  = @{ passing = 2042; slides = 2547; folder = "odp"; through = "app" }  # f1-word: -Through app with the head's exe (spOA); on .pptx the head and the no-cloud build are slide for slide the same
}
# Measured on 4.0's binaries by f1-gate (-Checks fidelity.baseline); null until then.
$baselineFile = Join-Path $root "tests\pdf\f1-gate-baselines.json"
if (Test-Path $baselineFile) {
  $b = Get-Content $baselineFile -Raw -Encoding UTF8 | ConvertFrom-Json
  foreach ($p in $b.PSObject.Properties) { if ($p.Name -notlike "_*") { $baselines[$p.Name] = $p.Value } }
}
# A plain hashtable: an [ordered] one indexed by a number gives the entry at that position.
$roySteps = @{
  1 = "SignPath Foundation code signing"
  2 = "Microsoft Store (Partner Center)"
  3 = "Chocolatey account and Scoop pull request"
  4 = "private vulnerability reporting and GitHub Pages"
  5 = "VirusTotal and Hybrid Analysis keys"
  6 = "Coverity Scan token and OpenSSF Best Practices"
}
$rowTitles = [ordered]@{
  build = "Build and tests"; damage = "Never damage"; fidelity = "Fidelity"; legal = "Legal"; lang = "Languages"
  pdf = "PDF tools"; ship = "Product basics"; review = "External review"; light = "Lightweight"; friction = "Friction"; docs = "Docs"
}
$lockPath = "D:\Documents\Codex\FeatherweightNotes\quiet-pc.lock"

# ---------------------------------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------------------------------
# A value as plain data for the report. A string Get-Content read carries PSPath, PSDrive and PSProvider notes,
# and ConvertTo-Json -Depth 12 walks the provider's object graph: minutes of CPU and GBs for one string.
function ConvertTo-Plain($v) {
  if ($null -eq $v) { return $null }
  if ($v -is [string]) { return "$v" }
  if ($v -is [ValueType]) { return $v }
  if ($v -is [Collections.IDictionary]) {
    $d = [ordered]@{}
    foreach ($k in @($v.Keys)) { $d["$k"] = ConvertTo-Plain $v[$k] }
    return $d
  }
  if ($v -is [Collections.IEnumerable]) { return , @(foreach ($x in $v) { ConvertTo-Plain $x }) }
  if ($v -is [Management.Automation.PSCustomObject]) {
    $d = [ordered]@{}
    foreach ($p in $v.PSObject.Properties) { $d[$p.Name] = ConvertTo-Plain $p.Value }
    return $d
  }
  "$v"
}
function New-GateResult([string]$status, [string]$detail, $numbers, [int]$step = 0) {
  $h = @{ status = $status; detail = $detail; numbers = $(if ($numbers) { ConvertTo-Plain $numbers } else { [ordered]@{} }) }
  if ($step) { $h.step = $step }
  $h
}
function Passed([string]$detail, $numbers) { New-GateResult "passed" $detail $numbers }
function Failed([string]$detail, $numbers) { New-GateResult "failed" $detail $numbers }
function Waiting([int]$step, [string]$detail, $numbers) { New-GateResult "waiting" ("waiting for Roy's step $step ($($roySteps[$step])): " + $detail) $numbers $step }
function NotBuilt([string]$piece, [string]$what) { Failed "not built yet ($piece): $what" }

function Invoke-Git { $o = & git.exe @args 2>$null; if ($LASTEXITCODE) { return $null }; $o }
function Read-Json([string]$path) { if ($path -and (Test-Path $path)) { Get-Content $path -Raw -Encoding UTF8 | ConvertFrom-Json } }
function Write-Json([string]$path, $obj) {
  New-Item -ItemType Directory -Force (Split-Path -Parent $path) | Out-Null
  [IO.File]::WriteAllText($path, (ConvertTo-Json $obj -Depth 12), $utf8)
}
function Full([string]$p) { if ([IO.Path]::IsPathRooted($p)) { $p } else { Join-Path $root $p } }
function Say([string]$s) { Write-Host $s }
function Log([string]$id) { Join-Path $script:outDir "logs\$id.log" }

# Runs a PowerShell script as its own process, its output to the check's log. Returns the exit code.
function Invoke-Script([string]$id, [string]$file, [object[]]$arguments) {
  $log = Log $id
  New-Item -ItemType Directory -Force (Split-Path -Parent $log) | Out-Null
  "== $(Get-Date -Format s) $file $($arguments -join ' ')" | Out-File -FilePath $log -Append -Encoding utf8
  $prev = $ErrorActionPreference; $ErrorActionPreference = "Continue"
  & powershell -NoProfile -ExecutionPolicy Bypass -File (Full $file) @arguments *>&1 | ForEach-Object { "$_" } | Out-File -FilePath $log -Append -Encoding utf8
  $code = $LASTEXITCODE
  $ErrorActionPreference = $prev
  $code
}
# Runs a program, its output to the check's log. Returns the exit code.
function Invoke-Tool([string]$id, [string]$exe, [object[]]$arguments) {
  $log = Log $id
  New-Item -ItemType Directory -Force (Split-Path -Parent $log) | Out-Null
  "== $(Get-Date -Format s) $exe $($arguments -join ' ')" | Out-File -FilePath $log -Append -Encoding utf8
  $prev = $ErrorActionPreference; $ErrorActionPreference = "Continue"
  & $exe @arguments 2>&1 | ForEach-Object { "$_" } | Out-File -FilePath $log -Append -Encoding utf8
  $code = $LASTEXITCODE
  $ErrorActionPreference = $prev
  $code
}
# "| label | value |" rows of a summary.md, as numbers.
function Read-SummaryTable([string]$md) {
  $n = [ordered]@{}
  if (-not (Test-Path $md)) { return $n }
  foreach ($line in Get-Content $md -Encoding UTF8) {
    if ($line -match '^\|\s*([^|]+?)\s*\|\s*([^|]+?)\s*\|$' -and $Matches[1] -notmatch '^-+$' -and $Matches[2] -notmatch '^-+$') { $n[$Matches[1]] = $Matches[2] }
  }
  $n
}

# --- Office: Roy's active session, unlocked, none of the program running, the quiet-PC lock ---
function Get-OfficeBusy([string]$process) {
  $why = @()
  $me = @(qwinsta 2>$null | Where-Object { $_ -match '^>' }) | Select-Object -First 1
  if (-not $me -or $me -notmatch '^>(console|rdp-tcp#\d+)\s' -or $me -notmatch '\sActive\s*$') { $why += "no active session" }
  $sid = [Diagnostics.Process]::GetCurrentProcess().SessionId
  if (@(Get-Process LogonUI -ErrorAction SilentlyContinue | Where-Object { $_.SessionId -eq $sid }).Count) { $why += "the PC is locked" }
  if ($process -and (Get-Process $process -ErrorAction SilentlyContinue)) { $why += "a $process is running (never automate or close one we didn't start)" }
  if (Test-Path $lockPath) { $why += "quiet-pc.lock is held: " + ((Get-Content $lockPath -Raw -ErrorAction SilentlyContinue) -replace '\s+', ' ').Trim() }
  $why -join ", "
}
# Runs $body holding quiet-pc.lock, once Office can be used. Returns $body's value, or a failed result.
function Use-Office([string]$id, [string]$process, [scriptblock]$body) {
  $deadline = (Get-Date).AddMinutes($OfficeWaitMinutes)
  $last = $null
  while ($true) {
    $why = Get-OfficeBusy $process
    if (-not $why) {
      try {
        $fs = [IO.File]::Open($lockPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write)
        $bytes = [Text.Encoding]::UTF8.GetBytes(("f1-gate {0:yyyy-MM-dd HH:mm:ss} (pid {1}): {2}`r`n" -f (Get-Date), $PID, $id))
        $fs.Write($bytes, 0, $bytes.Length); $fs.Close()
        break
      } catch { $why = "the lock was taken just now" }
    }
    if ((Get-Date) -gt $deadline) { return Failed "not run: Office couldn't be used within $OfficeWaitMinutes minutes ($why)" }
    if ($why -ne $last) { Say "   waiting for Office: $why" }
    $last = $why
    Start-Sleep -Seconds 60
  }
  $env:FW_HOLDS_QUIET_PC_LOCK = "1"
  try { & $body }
  finally {
    Remove-Item Env:FW_HOLDS_QUIET_PC_LOCK -ErrorAction SilentlyContinue
    $held = Get-Content $lockPath -Raw -ErrorAction SilentlyContinue
    if ($held -match "^f1-gate .*\(pid $PID\)") { Remove-Item $lockPath -ErrorAction SilentlyContinue }
  }
}

# --- GitHub (gh) ---
$repoSlug = $null
$origin = Invoke-Git remote get-url origin
if ($origin -match 'github\.com[/:]([^/]+/[^/.]+?)(\.git)?$') { $repoSlug = $Matches[1] }
function Gh-Json([string]$path) {
  $prev = $ErrorActionPreference; $ErrorActionPreference = "Continue"
  $o = & gh api $path 2>&1
  $code = $LASTEXITCODE
  $ErrorActionPreference = $prev
  if ($code) { throw "gh api $path : $(($o | Out-String).Trim())" }
  $x = ($o -join "`n") | ConvertFrom-Json
  # An array one element at a time (PowerShell 5.1's ConvertFrom-Json hands it over whole).
  if ($x -is [array]) { $x | ForEach-Object { $_ } } else { $x }
}
$script:runsCache = $null
function Get-CiRuns {
  if ($null -eq $script:runsCache) {
    $j = Gh-Json "repos/$repoSlug/actions/runs?head_sha=$($script:ciHead)&per_page=100"
    $script:runsCache = @($j.workflow_runs)
  }
  $script:runsCache
}
# The latest run of a workflow (by its file name) on this commit, with its jobs.
$script:workflowCache = @{}
function Get-WorkflowRun([string]$file) {
  if ($script:workflowCache.ContainsKey($file)) { return $script:workflowCache[$file] }
  $runs = @(Get-CiRuns | Where-Object { $_.path -like "*/$file" } | Sort-Object { [datetime]$_.created_at } -Descending)
  $w = $null
  if ($runs.Count) {
    $r = $runs[0]
    $jobs = @((Gh-Json "repos/$repoSlug/actions/runs/$($r.id)/jobs?per_page=100").jobs)
    $w = [pscustomobject]@{ run = $r; jobs = $jobs; url = $r.html_url; status = $r.status; conclusion = $r.conclusion }
  }
  $script:workflowCache[$file] = $w
  $w
}
# An artifact of this commit's (finished) run of a workflow, downloaded once into <Out>\artifacts\<run id>\<name>.
# $null when there's no such run or artifact.
function Get-RunArtifact([string]$file, [string]$name) {
  $w = Get-WorkflowRun $file
  if (-not $w -or $w.status -ne "completed") { return $null }
  $dir = Join-Path $script:outDir "artifacts\$($w.run.id)\$name"
  if (-not (Test-Path $dir)) {
    $prev = $ErrorActionPreference; $ErrorActionPreference = "Continue"
    & gh run download $w.run.id -R $repoSlug -n $name -D $dir 2>&1 | Out-Null
    $code = $LASTEXITCODE
    $ErrorActionPreference = $prev
    if ($code) { Remove-Item -Recurse -Force $dir -ErrorAction SilentlyContinue; return $null }
  }
  [pscustomobject]@{ dir = $dir; run = $w.url }
}
# A workflow's run on this commit as a result: passed when it succeeded (and the named jobs did).
function Test-Workflow([string]$file, [string[]]$jobs = @(), [string]$piece = "", [string]$what = "") {
  if (-not (Test-Path (Join-Path $root ".github\workflows\$file"))) { return NotBuilt $piece "$what (.github\workflows\$file)" }
  $w = Get-WorkflowRun $file
  if (-not $w) { return Failed "no $file run on this commit (is it pushed?)" }
  $n = [ordered]@{ run = $w.url; status = $w.status; conclusion = $w.conclusion }
  $missing = @(); $bad = @()
  foreach ($j in $jobs) {
    $m = @($w.jobs | Where-Object { $_.name -match $j })
    if (-not $m.Count) { $missing += $j; continue }
    foreach ($x in $m) { $n["job: $($x.name)"] = "$($x.status) $($x.conclusion)".Trim(); if ($x.conclusion -ne "success") { $bad += $x.name } }
  }
  if ($w.status -ne "completed") { return Failed "$file is still $($w.status)" $n }
  if ($missing.Count) { return Failed "$file has no job matching $($missing -join ', ')" $n }
  if ($w.conclusion -ne "success" -or $bad.Count) { return Failed "$file : $($w.conclusion)$(if ($bad.Count) { '; failed jobs: ' + ($bad -join ', ') })" $n }
  Passed "$file succeeded on this commit" $n
}

# --- The shipped build ---
function Get-Exe { Join-Path (Full $Build) "Release\Featherweight.exe" }
function Get-Fwtool { Join-Path (Full $Build) "tools\fwtool\Release\fwtool.exe" }
function Need-Build {
  if (-not (Test-Path (Get-Exe)) -or -not (Test-Path (Get-Fwtool))) { throw "the shipped build isn't there ($Build): run the build.shipped check first" }
}

# --- Text of the source, the exe ---
function Find-InFiles([string[]]$dirs, [string]$pattern, [string[]]$include = @("*.cpp", "*.h", "*.rc", "*.txt", "*.cmake")) {
  $hits = @()
  foreach ($d in $dirs) {
    $p = Join-Path $root $d
    if (-not (Test-Path $p)) { continue }
    $hits += @(Get-ChildItem $p -Recurse -File -Include $include | Select-String -Pattern $pattern -SimpleMatch:$false |
      ForEach-Object { "$($_.Path.Substring($root.Length + 1)):$($_.LineNumber)" })
  }
  $hits
}
function Test-BinaryHasText([string]$file, [string]$text) {
  $bytes = [IO.File]::ReadAllBytes($file)
  $ascii = [Text.Encoding]::ASCII.GetString($bytes)
  if ($ascii.IndexOf($text, [StringComparison]::OrdinalIgnoreCase) -ge 0) { return $true }
  $wide = [Text.Encoding]::Unicode.GetString($bytes)
  if ($wide.IndexOf($text, [StringComparison]::OrdinalIgnoreCase) -ge 0) { return $true }
  # Shifted by one byte: a UTF-16 string at an odd offset.
  $wide2 = [Text.Encoding]::Unicode.GetString($bytes, 1, $bytes.Length - 1)
  $wide2.IndexOf($text, [StringComparison]::OrdinalIgnoreCase) -ge 0
}
# The cloud-font families: the folder names under Office's cloud-font cache (names only; no file in
# them is read), and the ones known at 4.0 in case the folder isn't on this PC.
function Get-CloudFamilies {
  $known = @("Aptos", "Aptos Display", "Aptos Narrow", "Aptos Serif", "Aptos Mono", "Grandview", "Seaford", "Skeena", "Tenorite", "Bierstadt")
  $dir = Join-Path $env:LOCALAPPDATA "Microsoft\FontCache\4\CloudFonts"
  $found = @(if (Test-Path $dir) { Get-ChildItem $dir -Directory | ForEach-Object Name })
  @($known + $found | Sort-Object -Unique)
}

# ---------------------------------------------------------------------------------------------------
# The checks, row by row. Each run block returns New-GateResult/Passed/Failed/Waiting/NotBuilt as its last value.
# ---------------------------------------------------------------------------------------------------
$all = New-Object Collections.ArrayList
function Check([string]$id, [string]$kind, [string]$title, [string]$pass, [scriptblock]$run) {
  [void]$all.Add([pscustomobject]@{ id = $id; row = $id.Split('.')[0]; kind = $kind; title = $title; pass = $pass; run = $run })
}

# ===== Build and tests =====
Check "build.tree" quick "The tree is clean (the shipped build is made from a commit)" "No uncommitted changes" {
  $s = @(Invoke-Git status --porcelain)
  if ($s.Count) { Failed "$($s.Count) uncommitted changes" ([ordered]@{ changes = $s.Count; first = $s[0] }) }
  else { Passed "clean at $($script:head.Substring(0, 12))" }
}
Check "build.shipped" long "The shipped configuration builds (release.ps1's: FW_TEST_SCREENS=OFF)" "Builds; Featherweight.exe and fwtool.exe made" {
  $b = Full $Build
  if ($CleanBuild -and (Test-Path $b)) { Remove-Item -Recurse -Force $b }
  $code = Invoke-Tool "build.shipped" "cmake" @("-S", $root, "-B", $b, "-DFW_BUILD_TESTS=ON", "-DFW_TEST_SCREENS=OFF")
  if ($code) { return Failed "configure failed (logs\build.shipped.log)" }
  $code = Invoke-Tool "build.shipped" "cmake" @("--build", $b, "--config", "Release", "--parallel", "--", "/nologo", "/verbosity:minimal")
  if ($code) { return Failed "build failed (logs\build.shipped.log)" }
  $cache = Get-Content (Join-Path $b "CMakeCache.txt") -Raw
  if ($cache -notmatch 'FW_TEST_SCREENS:BOOL=OFF') { return Failed "FW_TEST_SCREENS isn't OFF in $Build" }
  $exe = Get-Item (Get-Exe)
  $pdfium = Get-Item (Join-Path $b "Release\pdfium.dll") -ErrorAction SilentlyContinue
  Passed "built" ([ordered]@{ exeBytes = $exe.Length; exeSha256 = (Get-FileHash $exe.FullName -Algorithm SHA256).Hash
                              pdfiumBytes = $(if ($pdfium) { $pdfium.Length } else { $null }) })
}
Check "build.ctest" long "ctest on the shipped build (x64, locally)" "100 % of the tests pass" {
  Need-Build
  $junit = Join-Path $script:outDir "ctest.xml"
  $code = Invoke-Tool "build.ctest" "ctest" @("--test-dir", (Full $Build), "-C", "Release", "--output-on-failure", "--output-junit", $junit)
  $text = Get-Content (Log "build.ctest") -Raw
  $m = [regex]::Matches($text, '(\d+)% tests passed(?:, (\d+) tests? failed)? out of (\d+)')  # no failed count when none failed
  if (-not $m.Count) { return Failed "ctest printed no total (exit $code)" }
  $last = $m[$m.Count - 1]
  $n = [ordered]@{ total = [int]$last.Groups[3].Value; failed = $(if ($last.Groups[2].Success) { [int]$last.Groups[2].Value } else { 0 }) }
  $names = @()
  if (Test-Path $junit) { $names = @(([xml](Get-Content $junit -Raw)).SelectNodes("//testcase") | ForEach-Object { $_.name }) }
  $n.tests = $names
  if ($code -or $n.failed) { Failed "$($n.failed) of $($n.total) tests failed" $n } else { Passed "$($n.total) of $($n.total) pass" $n }
}
# ossf/scorecard-action runs only on the default branch (it refuses any other; f1-trust): this commit's run when it's
# on main, else main's latest finished run. None yet is "later", not a failure: it first runs once F1 is merged.
function Test-Scorecard {
  if (-not (Test-Path (Join-Path $root ".github\workflows\scorecard.yml"))) { return NotBuilt "f1-trust" "the OpenSSF Scorecard (.github\workflows\scorecard.yml)" }
  if (Get-WorkflowRun "scorecard.yml") { return Test-Workflow "scorecard.yml" @() "f1-trust" "the OpenSSF Scorecard" }
  try { $j = Gh-Json "repos/$repoSlug/actions/workflows/scorecard.yml/runs?branch=main&status=completed&per_page=1" } catch { $j = $null }
  $r = @($j.workflow_runs) | Select-Object -First 1
  if (-not $r) { return New-GateResult "later" "Scorecard runs only on main, where it hasn't run yet (F1 isn't merged to main)" }
  $n = [ordered]@{ run = $r.html_url; commit = "$($r.head_sha)".Substring(0, 8); conclusion = $r.conclusion }
  if ($r.conclusion -eq "success") { Passed "main's latest Scorecard run succeeded" $n } else { Failed "main's latest Scorecard run: $($r.conclusion)" $n }
}
Check "build.ci" ci "CI on this commit: build (with arm64's ctest), fuzz, CodeQL, Scorecard" "Every workflow and job succeeded" {
  if (-not $repoSlug) { return Failed "no GitHub remote" }
  if (-not @(Invoke-Git branch -r --contains $script:ciHead).Count) { return Failed "this commit isn't pushed" }
  $parts = [ordered]@{
    build = Test-Workflow "build.yml" @("^build", "fuzz", "arm64") "f1-ship" "the arm64 job"
    codeql = Test-Workflow "codeql.yml" @() "f1-trust" "CodeQL code scanning"
    scorecard = Test-Scorecard
  }
  $n = [ordered]@{}
  foreach ($k in $parts.Keys) { $n[$k] = "$($parts[$k].status): $($parts[$k].detail)" }
  $bad = @($parts.Keys | Where-Object { $parts[$_].status -notin "passed", "later" })
  if ($bad.Count) { Failed ("not green: " + ($bad -join ", ")) $n } else { Passed "all green" $n }
}
Check "build.codeql" ci "No open CodeQL alert of high or critical severity" "0 open high or critical alerts" {
  if (-not (Test-Path (Join-Path $root ".github\workflows\codeql.yml"))) { return NotBuilt "f1-trust" "CodeQL code scanning (.github\workflows\codeql.yml)" }
  # The branch gated: this checkout's, else the first of f1-int and main that has the commit (codeql.yml runs on
  # pushes to those). Without ref the API answers for the default branch only.
  $pushed = @(Invoke-Git branch -r --contains $script:ciHead | ForEach-Object { "$_".Trim() } | Where-Object { $_ -match '^origin/' -and $_ -notmatch ' -> ' } | ForEach-Object { $_ -replace '^origin/', '' })
  # With -CiCommit this checkout's branch only carries that commit; it was analysed where it was pushed.
  # (A list built with +=: an if's one-item result is a string, and string + array concatenates.)
  $tryBranches = @()
  if (-not $CiCommit) { $tryBranches += "$(Invoke-Git rev-parse --abbrev-ref HEAD)" }
  $tryBranches += @("claude/f1-int", "main") + $pushed
  $branch = @($tryBranches | Where-Object { $_ -and $_ -in $pushed }) | Select-Object -First 1
  if (-not $branch) { return Failed "this commit isn't on a pushed branch" }
  $ref = "refs/heads/$branch"
  # The alerts API answers "none open" for a branch CodeQL never analysed: the analysis of this commit (or of one
  # with only docs changed since) must be there first.
  try { $analyses = @(Gh-Json "repos/$repoSlug/code-scanning/analyses?ref=$ref&per_page=20") } catch { $analyses = @() }
  $a = @($analyses | Where-Object { $_.commit_sha -eq $script:ciHead }) | Select-Object -First 1
  if (-not $a) { $a = @($analyses | Where-Object { $_.commit_sha -and (Test-OnlyDocsSince $_.commit_sha) }) | Select-Object -First 1 }
  if (-not $a) {
    $last = @($analyses) | Select-Object -First 1
    return Failed ("not run yet: no CodeQL analysis of this commit on $ref" + $(if ($last) { " (the latest is of $("$($last.commit_sha)".Substring(0, 8)))" } else { " (none at all; codeql.yml runs on pushes to main and claude/f1-int)" }))
  }
  try { $alerts = @(Gh-Json "repos/$repoSlug/code-scanning/alerts?state=open&ref=$ref&per_page=100") }
  catch {
    if ("$($_.Exception.Message)" -match '(?i)no analysis found|404') { return Failed "not run yet: CodeQL hasn't analysed $ref (codeql.yml runs on pushes to main and claude/f1-int)" }
    return Failed "code scanning can't be read: $($_.Exception.Message)"
  }
  $severe = @($alerts | Where-Object { $_.rule.security_severity_level -in "high", "critical" })
  $n = [ordered]@{ ref = $ref; analysedCommit = "$($a.commit_sha)".Substring(0, 8); open = $alerts.Count; highOrCritical = $severe.Count }
  if ($severe.Count) { $n.first = "$($severe[0].rule.id) $($severe[0].most_recent_instance.location.path)"; Failed "$($severe.Count) open high or critical alerts" $n }
  else { Passed "$($alerts.Count) open alerts, none high or critical" $n }
}

# ===== Never damage =====
function Invoke-Torture([string]$id, [string]$script, [string]$office, [string]$process, [string]$noOffice, [switch]$canReuse, [object[]]$extra) {
  Need-Build
  $o = Join-Path $script:outDir $id
  $env:FW_CORPUS = $Corpus
  $common = @("-Fwtool", (Get-Fwtool), "-Out", $o)
  if ($extra) { $common += $extra }
  if ($canReuse) {
    $code = Invoke-Script $id $script ($common + @($noOffice))
    if (-not (Test-Path (Join-Path $o "summary.json"))) { return Failed "the torture run wrote no summary (exit $code; logs\$id.log)" }
    $code = Use-Office $id $process { Invoke-Script $id $script ($common + @("-Reuse")) }
  } else {
    $code = Use-Office $id $process { Invoke-Script $id $script $common }
  }
  if ($code -is [hashtable]) { return $code }  # Office couldn't be used
  $s = Read-Json (Join-Path $o "summary.json")
  if (-not $s) { return Failed "no summary (exit $code; logs\$id.log)" }
  $n = Read-SummaryTable (Join-Path $o "summary.md")
  $n.files = @($s.files).Count
  $n.summary = Join-Path $o "summary.md"
  if ($code -eq 0) { Passed "$(@($s.files).Count) files, nothing failed ($office checked)" $n }
  elseif ($code -eq 3 -or $code -eq 4) { Failed "$office couldn't finish (exit $code)" $n }
  else { Failed "failures (exit $code): see $($n.summary)" $n }
}
Check "damage.docs" long "Document torture with Word's open check (scripts\torture.ps1, the private corpus)" "0 failures, 0 new validator errors, 0 repair prompts" {
  Invoke-Torture "damage.docs" "scripts\torture.ps1" "Word" "WINWORD"
}
Check "damage.workbooks" long "Workbook torture with Excel's open check (scripts\sheet-torture.ps1)" "0 failures, 0 new validator errors, 0 repair prompts" {
  Invoke-Torture "damage.workbooks" "scripts\sheet-torture.ps1" "Excel" "EXCEL" "-NoExcel" -canReuse -extra @("-Corpus", $Corpus)
}
Check "damage.decks" long "Deck torture with PowerPoint's open check (scripts\deck-torture.ps1); unedited saves byte-identical" "0 failures, 0 new validator errors, 0 repair prompts; unedited saves byte-identical" {
  Invoke-Torture "damage.decks" "scripts\deck-torture.ps1" "PowerPoint" "POWERPNT" "-NoPowerPoint" -canReuse -extra @("-Corpus", $Corpus)
}
Check "damage.pdf" long "PDF torture: random page operations, OCR and a drawn signature, saved and reopened (tests\pdf\pdf-torture.ps1); unedited saves byte-identical" "0 failures; every output passes the PDF checks; unedited saves byte-identical" {
  $s = Join-Path $root "tests\pdf\pdf-torture.ps1"
  if (-not (Test-Path $s)) { return NotBuilt "f1-gate" "the PDF torture run (tests\pdf\pdf-torture.ps1)" }
  Need-Build
  $o = Join-Path $script:outDir "damage.pdf"
  $code = Invoke-Script "damage.pdf" "tests\pdf\pdf-torture.ps1" @("-Fwtool", (Get-Fwtool), "-Exe", (Get-Exe), "-Out", $o, "-Corpus", $Corpus)
  $s = Read-Json (Join-Path $o "summary.json")
  if (-not $s) { return Failed "no summary (exit $code; logs\damage.pdf.log)" }
  $n = [ordered]@{}
  foreach ($p in $s.totals.PSObject.Properties) { $n[$p.Name] = $p.Value }
  if ($code -eq 0 -and $s.pass -and $s.totals.through -ne "featherweight") { return NotBuilt "f1-pdf" "the torture through the page tools (fwtool built without fwpdftools: $($s.verdict))" }
  if ($code -eq 0 -and $s.pass) { Passed $s.verdict $n } else { Failed $s.verdict $n }
}

# ===== Fidelity =====
function Test-CloudFontsGone {
  if (Test-Path (Join-Path $root "src\draw\CloudFonts.cpp")) { return "src\draw\CloudFonts.cpp is still there (f1-word's round 1 removes it)" }
  $hits = @(Find-InFiles @("src") "CloudFonts")
  if ($hits.Count) { return "the source still names CloudFonts ($($hits[0]))" }
  $null
}
Check "fidelity.word" long "Word pages and lines against Word's renders (fwtool compare, without cloud fonts)" "Pages >= 148 of 152, lines >= 96.8 %" {
  Need-Build
  $why = Test-CloudFontsGone
  $j = Join-Path $script:outDir "fidelity-word.json"
  $code = Invoke-Tool "fidelity.word" (Get-Fwtool) @("compare", "--json", $j, (Join-Path $Corpus "reference"))
  $c = Read-Json $j
  if (-not $c) { return Failed "fwtool compare wrote nothing (exit $code)" }
  $n = [ordered]@{ pages = $c.pages.match; documents = $c.pages.documents; pagesPercent = $c.pages.percent
                   lines = $c.lines.match; linesTotal = $c.lines.lines; linesPercent = $c.lines.percent }
  $ok = $c.pages.match -ge 148 -and $c.lines.percent -ge 96.8
  if ($why) { return Failed "measured with cloud fonts: $why" $n }
  if ($ok) { Passed "$($c.pages.match) of $($c.pages.documents) page counts, $($c.lines.percent) % of lines" $n }
  else { Failed "$($c.pages.match) of $($c.pages.documents) page counts (148), $($c.lines.percent) % of lines (96.8)" $n }
}
# slide-pages.ps1 writes <out>\decks\<deck>\<deck>.pdf, the deck named by its corpus path (up to 77
# characters), and fwtool and pdf-compare read paths of at most 260 characters: so a short folder, and a
# check that it's short enough (a longer one loses decks silently: 11 decks, 150 slides on a trial run).
$maxSlideOut = 259 - "\decks\".Length - 77 - 1 - 77 - ".pdf".Length
function Invoke-SlidePages([string]$id, $base) {
  Need-Build
  $o = Join-Path $script:outDir "sl-$($base.folder)"
  if ($o.Length -gt $maxSlideOut) { return [ordered]@{ error = "the output folder's path is $($o.Length) characters; the decks' names need it at most $maxSlideOut (use a shorter -Out)" } }
  $code = Invoke-Script $id "scripts\slide-pages.ps1" @("-Reference", (Join-Path $Corpus "reference"), "-Folders", $base.folder,
    "-Featherweight", (Get-Exe), "-Fwtool", (Get-Fwtool), "-Out", $o, "-Through", $base.through, "-MinSlides", "0")
  $s = Read-Json (Join-Path $o "summary.json")
  if (-not $s) { return $null }
  [ordered]@{ passing = [int]$s.passing; slides = [int]$s.slides; percent = $s.percent; exit = $code }
}
Check "fidelity.slides" long "Slides against PowerPoint's (scripts\slide-pages.ps1): .pptx, .ppt, .odp" "No lower than 4.0's without cloud fonts: .pptx 2,349 of 2,516, .ppt 2,263 of 2,577 through fwtool (2,322 with the two encrypted decks the app opens), .odp 2,042 of 2,547 (SSIM >= 0.95)" {
  $n = [ordered]@{}; $bad = @()
  foreach ($k in "slidesPptx", "slidesPpt", "slidesOdp") {
    $base = $baselines[$k]
    $m = Invoke-SlidePages "fidelity.slides-$($base.folder)" $base
    if (-not $m) { $bad += "$($base.folder): no summary"; continue }
    if ($m.error) { $bad += "$($base.folder): $($m.error)"; continue }
    $n[$base.folder] = "$($m.passing) of $($m.slides) ($($m.percent) %); 4.0 without cloud fonts: $($base.passing) of $($base.slides)"
    # The same slides: no fewer passing. Another count of slides (decks added or dropped): no lower share.
    $okCount = if ($m.slides -eq $base.slides) { $m.passing -ge $base.passing } else { ($m.passing / [math]::Max(1, $m.slides)) -ge ($base.passing / $base.slides) }
    if (-not $okCount) { $bad += $base.folder }
  }
  if ($bad.Count) { Failed ("lower than 4.0's: " + ($bad -join ", ")) $n } else { Passed "no lower than 4.0's" $n }
}
Check "fidelity.print" long "Printed workbook pages against Excel's (scripts\sheet-pages.ps1): the public set, .xlsx, .xls, .ods" "No lower than 4.0's (measured on 4.0's binaries: tests\pdf\f1-gate-baselines.json)" {
  Need-Build
  $sets = @(
    @{ key = "printPublic"; ref = (Join-Path $root "tests\office\reference"); folders = "excel" },
    @{ key = "printXlsx"; ref = (Join-Path $Corpus "reference"); folders = "xlsx" },
    @{ key = "printXlsOds"; ref = (Join-Path $Corpus "reference"); folders = "xls,ods" })
  $n = [ordered]@{}; $bad = @()
  foreach ($set in $sets) {
    $o = Join-Path $script:outDir "pr-$($set.key)"  # short: sheet-pages names files after the workbooks
    $code = Invoke-Script "fidelity.print" "scripts\sheet-pages.ps1" @("-Reference", $set.ref, "-Folders", $set.folders,
      "-Featherweight", (Get-Exe), "-Fwtool", (Get-Fwtool), "-Out", $o, "-MinPages", "0")
    $s = Read-Json (Join-Path $o "summary.json")
    if (-not $s) { $bad += "$($set.key): no summary"; continue }
    $pages = (@($s.files) | Measure-Object pages -Sum).Sum; $passing = (@($s.files) | Measure-Object passing -Sum).Sum
    $base = $baselines[$set.key]
    $n[$set.key] = "$passing of $pages$(if ($base) { "; 4.0: $($base.passing) of $($base.pages)" } else { '; 4.0: not measured yet' })"
    if (-not $base) { $bad += "$($set.key): no 4.0 baseline"; continue }
    if ($pages -eq $base.pages) { if ($passing -lt $base.passing) { $bad += $set.key } }
    elseif (($passing / [math]::Max(1, $pages)) -lt ($base.passing / [math]::Max(1, $base.pages))) { $bad += $set.key }
  }
  if ($bad.Count) { Failed ("lower than 4.0's, or not measured: " + ($bad -join ", ")) $n } else { Passed "no lower than 4.0's" $n }
}
Check "fidelity.cells" long "Cell text against Excel's (fwtool compare --cells): .xlsx, .xls, .ods" "No lower than 4.0's (tests\pdf\f1-gate-baselines.json)" {
  Need-Build
  $n = [ordered]@{}; $bad = @()
  foreach ($f in "xlsx", "xls", "ods") {
    $j = Join-Path $script:outDir "fidelity-cells-$f.json"
    $code = Invoke-Tool "fidelity.cells" (Get-Fwtool) @("compare", "--cells", "--folders", $f, "--json", $j, (Join-Path $Corpus "reference"))
    $c = Read-Json $j
    if (-not $c) { $bad += "${f}: nothing written"; continue }
    $pct = if ($null -ne $c.percent) { [double]$c.percent } elseif ($c.cells) { [double]$c.cells.percent } else { $null }
    $base = $baselines["cells" + $f.Substring(0, 1).ToUpper() + $f.Substring(1)]
    $n[$f] = "$pct %$(if ($base) { "; 4.0: $($base.percent) %" } else { '; 4.0: not measured yet' })"
    if ($null -eq $pct -or -not $base -or $pct -lt [double]$base.percent) { $bad += $f }
  }
  if ($bad.Count) { Failed ("lower than 4.0's, or not measured: " + ($bad -join ", ")) $n } else { Passed "no lower than 4.0's" $n }
}

# ===== Legal =====
Check "legal.source" quick "The source and the build files don't read or name Office's cloud fonts" "No CloudFonts in src\ or CMakeLists.txt; src\draw\CloudFonts.* gone" {
  $hits = @(Find-InFiles @("src") "CloudFonts|FontCache") + @(Select-String -Path (Join-Path $root "CMakeLists.txt") -Pattern "CloudFonts" | ForEach-Object { "CMakeLists.txt:$($_.LineNumber)" })
  $files = @(Get-ChildItem (Join-Path $root "src\draw") -Filter "CloudFonts.*" -ErrorAction SilentlyContinue)
  $n = [ordered]@{ mentions = $hits.Count; files = $files.Count }
  if ($hits.Count) { $n.first = $hits[0] }
  if ($hits.Count -or $files.Count) { Failed "$($files.Count) CloudFonts files, $($hits.Count) mentions (f1-word's round 1)" $n } else { Passed "none" $n }
}
Check "legal.exe" quick "The shipped exe doesn't name the cloud-font folder" "No 'CloudFonts' or 'FontCache' in Featherweight.exe" {
  Need-Build
  $found = @("CloudFonts", "FontCache" | Where-Object { Test-BinaryHasText (Get-Exe) $_ })
  if ($found.Count) { Failed ("the exe names " + ($found -join ", ")) } else { Passed "not named" }
}
Check "legal.fileopen" long "No file under FontCache\4\CloudFonts opened or probed during the corpus runs (fwtool's file-open hook)" "0 paths under the cloud-font folder; the hook was on" {
  Need-Build
  $dir = Join-Path $script:outDir "filelog"
  # The usage text, without a file log of its own (a log here means a corpus run happened).
  $env:FW_FILE_LOG_DIR = ""
  try { $code = Invoke-Tool "legal.fileopen" (Get-Fwtool) @("--help") } finally { $env:FW_FILE_LOG_DIR = $dir }
  if ((Get-Content (Log "legal.fileopen") -Raw) -notmatch "FW_FILE_LOG_DIR") { return NotBuilt "f1-gate" "fwtool's file-open hook (FW_FILE_LOG_DIR)" }
  # The corpus runs of this gate log into $dir (FW_FILE_LOG_DIR); if none ran, render the documents and decks now.
  if (-not @(Get-ChildItem $dir -Filter "fwtool-*.txt" -ErrorAction SilentlyContinue).Count) {
    foreach ($kind in @(@("render", "docx"), @("slides", "pptx"))) {
      $src = Join-Path $Corpus $kind[1]
      if (Test-Path $src) { Invoke-Tool "legal.fileopen" (Get-Fwtool) @($kind[0], "--out", (Join-Path $script:outDir "fileopen-$($kind[1])"), $src) | Out-Null }
    }
  }
  # And text laid out in each cloud-font family, which makes the font lookup look for it (at 4.0 that
  # read the cloud-font folder and the family's files: the hook's check that it would see them).
  foreach ($fam in Get-CloudFamilies) { Invoke-Tool "legal.fileopen" (Get-Fwtool) @("wrap", "--font", $fam, "Featherweight 4.1 laid out in $fam") | Out-Null }
  $logs = @(Get-ChildItem $dir -Filter "fwtool-*.txt" -ErrorAction SilentlyContinue)
  if (-not $logs.Count) { return Failed "no file log was written" }
  $hooked = 0; $paths = 0; $cloud = @()
  foreach ($l in $logs) {
    $t = @(Get-Content $l.FullName -Encoding UTF8)
    if ($t.Count -and $t[0] -match '^hook: on') { $hooked++ }
    $paths += $t.Count - 1
    $cloud += @($t | Where-Object { $_ -match '\\FontCache\\4\\CloudFonts' })
  }
  $n = [ordered]@{ runs = $logs.Count; hooked = $hooked; paths = $paths; cloud = $cloud.Count }
  if ($cloud.Count) { $n.first = $cloud[0]; return Failed "$($cloud.Count) paths under the cloud-font folder" $n }
  if ($hooked -ne $logs.Count) { return Failed "the hook was off in $($logs.Count - $hooked) of $($logs.Count) runs" $n }
  Passed "$paths paths in $($logs.Count) runs, none under the cloud-font folder" $n
}
Check "legal.pdffonts" long "No cloud-font family embedded by Export PDF on the corpus" "0 PDFs with a cloud-font family embedded" {
  Need-Build
  $o = Join-Path $script:outDir "legal.pdffonts"
  $code = Invoke-Tool "legal.pdffonts" (Get-Fwtool) @("pdf-check", "--out", $o, (Join-Path $Corpus "docx"))
  $pdfs = @(Get-ChildItem $o -Recurse -Filter *.pdf -ErrorAction SilentlyContinue)
  if (-not $pdfs.Count) { return Failed "pdf-check exported no PDF (exit $code)" }
  $list = Join-Path $o "pdfs.txt"
  [IO.File]::WriteAllLines($list, [string[]]@($pdfs | ForEach-Object FullName), $utf8)
  $j = Join-Path $o "fonts.json"
  $code = Invoke-Tool "legal.pdffonts" (Get-Fwtool) @("pdf-valid", "--json", $j, "--samples", $list)
  $v = Read-Json $j
  if (-not $v) { return NotBuilt "f1-gate" "fwtool pdf-valid (the fonts a PDF embeds)" }
  $families = Get-CloudFamilies
  $hits = @()
  foreach ($f in @($v.files)) {
    foreach ($font in @($f.fonts)) {
      $name = ($font -replace '^[A-Z]{6}\+', '') -replace '[-,](Bold|Italic|BoldItalic|Regular|Light|SemiBold|Black).*$', ''
      foreach ($fam in $families) { if ($name -replace '\s', '' -ieq ($fam -replace '\s', '')) { $hits += "$($f.file): $font" } }
    }
  }
  $n = [ordered]@{ pdfs = $pdfs.Count; fonts = (@($v.files) | ForEach-Object { @($_.fonts).Count } | Measure-Object -Sum).Sum; cloud = $hits.Count }
  if ($hits.Count) { $n.first = $hits[0]; Failed "$($hits.Count) cloud-font embeddings" $n } else { Passed "none in $($pdfs.Count) PDFs" $n }
}
Check "legal.notices" quick "THIRD_PARTY_NOTICES matches the SBOM, and About shows it" "Every SBOM component in the notices and back; About shows the notices" {
  # No SBOM is committed (it would go stale): tools\sbom\make-sbom.ps1 writes it, and exits 1 when
  # THIRD_PARTY_NOTICES.md and the SBOM name different components (f1-trust).
  if (-not (Test-Path (Join-Path $root "tools\sbom\make-sbom.ps1"))) { return NotBuilt "f1-trust" "the SBOM (tools\sbom\make-sbom.ps1, CycloneDX)" }
  $out = Join-Path $script:outDir "sbom.cdx.json"
  Remove-Item $out -ErrorAction SilentlyContinue
  $code = Invoke-Script "legal.notices" "tools\sbom\make-sbom.ps1" @("-Out", $out)
  $doc = Read-Json $out
  if (-not $doc) { return Failed "make-sbom.ps1 wrote no SBOM (exit $code; its log)" }
  $notices = Get-Content (Join-Path $root "THIRD_PARTY_NOTICES.md") -Raw -Encoding UTF8
  $names = @($doc.components | ForEach-Object name)
  $missing = @($names | Where-Object { $notices -notmatch [regex]::Escape($_) })
  $about = @(Find-InFiles @("src") "THIRD_PARTY_NOTICES")
  $n = [ordered]@{ components = $names.Count; missingFromNotices = $missing.Count; aboutMentions = $about.Count; makeSbomExit = $code }
  if ($missing.Count) { $n.first = $missing[0]; return Failed "$($missing.Count) SBOM components not in THIRD_PARTY_NOTICES.md" $n }
  if ($code) { return Failed "make-sbom.ps1: THIRD_PARTY_NOTICES.md names components the SBOM hasn't (its log)" $n }
  if (-not $about.Count) { return Failed "About doesn't show THIRD_PARTY_NOTICES (no mention in src\)" $n }
  Passed "$($names.Count) components, all in the notices; About shows them" $n
}
Check "legal.hyphenation" quick "The Swedish (LPPL) and Finnish hyphenation patterns ship byte for byte, or are dropped" "As decided in docs\f1-plan.md (f1-word)" {
  $plan = Get-Content (Join-Path $root "docs\f1-plan.md") -Raw -Encoding UTF8
  # The latest decision line counts (f1-word dropped them first, then kept them byte for byte).
  $decided = @([regex]::Matches($plan, '(?im)^- \*\*f1-word[^\n]*hyphenation[^\n]*'))
  if (-not $decided.Count) { return NotBuilt "f1-word" "the hyphenation decision in docs\f1-plan.md" }
  $text = $decided[$decided.Count - 1].Value
  $cmake = (Get-ChildItem (Join-Path $root "cmake") -Filter *.cmake | Get-Content -Raw) -join "`n"
  $lists = $cmake + (Get-Content (Join-Path $root "CMakeLists.txt") -Raw)
  $n = [ordered]@{ decision = $text.Substring(0, [math]::Min(160, $text.Length)) }
  $sv = $lists -match 'hyph[-_]sv'; $fi = $lists -match 'hyph[-_]fi'
  $n.swedishInBuild = $sv; $n.finnishInBuild = $fi
  if ($text -match '(?i)dropp') { if ($sv -or $fi) { Failed "decided dropped, but the build still has them" $n } else { Passed "dropped" $n } }
  else {
    # Kept: fetched as they are, each pinned by its SHA-256 next to its name.
    $pinned = @("hyph-sv", "hyph-fi" | Where-Object { $cmake -match ([regex]::Escape($_) + '[\s\S]{0,400}?[0-9a-f]{64}') })
    $n.pinned = $pinned -join ", "
    if ($pinned.Count -eq 2) { Passed "kept byte for byte, each pinned by its SHA-256" $n } else { Failed "decided kept, but not both pinned by hash in cmake\" $n }
  }
}
Check "legal.names" quick "Word, Excel and PowerPoint only as descriptions; the README says the MIT licence doesn't cover the name and logo" "No product or feature named after them; the README's wording present" {
  $readme = Get-Content (Join-Path $root "README.md") -Raw -Encoding UTF8
  $n = [ordered]@{}
  $n.readmeNameAndLogo = [bool]($readme -match '(?is)(MIT|licen[cs]e).{0,200}(name|logo).{0,120}(logo|name)' -and $readme -match '(?i)not|doesn.t|don.t')
  # Feature names: a string a person reads that is "Word ..."/"Excel ..."/"PowerPoint ..." as a name
  # (not "Word documents", "Excel workbooks", "PowerPoint presentations", "Word 97-2003", "the Word format").
  # The strings are the catalog's msgids (strings\featherweight.pot: every string a person reads, and only
  # those; ctest strings_marked keeps it so), else the source's literals on one line each, without the
  # lines marked "// lang: no" (data such as registry key names). Strings reviewed as descriptions are in
  # tests\legal-names-reviewed.txt; the row lists those apart.
  $strings = @()
  $pot = Join-Path $root "strings\featherweight.pot"
  if (Test-Path $pot) {
    $n.source = "strings\featherweight.pot"
    $cur = $null
    foreach ($line in [IO.File]::ReadAllLines($pot, $utf8)) {
      if ($line -match '^msgid(_plural)? "(.*)"$') { if ($null -ne $cur) { $strings += $cur }; $cur = $Matches[2] }
      elseif ($null -ne $cur -and $line -match '^"(.*)"$') { $cur += $Matches[1] }
      else { if ($null -ne $cur) { $strings += $cur }; $cur = $null }
    }
    if ($null -ne $cur) { $strings += $cur }
    $strings = @($strings | Where-Object { $_ } | ForEach-Object { [pscustomobject]@{ where = "featherweight.pot"; text = $_ } })
  } else {
    $n.source = "src literals"
    foreach ($f in @(Get-ChildItem (Join-Path $root "src") -Recurse -File -Include *.cpp, *.h, *.rc)) {
      foreach ($line in [IO.File]::ReadAllLines($f.FullName)) {
        if ($line -match '//\s*lang:\s*no') { continue }
        foreach ($m in [regex]::Matches($line, 'L"([^"\r\n]*)"')) { $strings += [pscustomobject]@{ where = $f.FullName.Substring($root.Length + 1); text = $m.Groups[1].Value } }
      }
    }
  }
  $reviewedFile = Join-Path $root "tests\legal-names-reviewed.txt"
  $reviewed = @{}
  if (Test-Path $reviewedFile) {
    foreach ($line in [IO.File]::ReadAllLines($reviewedFile, $utf8)) { if ($line -and -not $line.StartsWith("#")) { $reviewed[($line -split "`t")[0]] = $true } }
  }
  $hits = @(); $accepted = @()
  foreach ($x in $strings) {
    $s = $x.text -replace '\\n', "`n" -replace '\\"', '"' -replace '\\\\', '\'
    if ($s -cnotmatch '\b(Word|Excel|PowerPoint)\b') { continue }  # the names, capitalized: not the English word "word"
    if ($s -match '\b(Word|Excel|PowerPoint)\s+(\d|document|documents|workbook|workbooks|presentation|presentations|file|files|format|formats|template|templates|show|show''s|97|2003|2007|binary|Macro|Add-in|XML|Open XML)' -or
        $s -match '(as|in|like|than|from|by|of|the)\s+(Word|Excel|PowerPoint)\b') { continue }
    if ($reviewed.ContainsKey($x.text)) { $accepted += $x.text; continue }
    $hits += "$($x.where): $($x.text)"
  }
  $n.possibleNames = $hits.Count
  $n.reviewedAsDescriptions = $accepted.Count
  if ($hits.Count) { $n.first = $hits[0] }
  $listFile = Join-Path $script:outDir "legal-names.txt"
  New-Item -ItemType Directory -Force $script:outDir | Out-Null
  [IO.File]::WriteAllLines($listFile, [string[]](@($hits) + @("", "# reviewed as descriptions (tests\legal-names-reviewed.txt):") + @($accepted)), $utf8)
  if (-not $n.readmeNameAndLogo) { return NotBuilt "f1-trust" "the README's line that the MIT licence doesn't cover the name and logo" }
  if ($hits.Count) { Failed "$($hits.Count) strings may use a program's name as a name, not yet reviewed (list: legal-names.txt; reviewed ones go in tests\legal-names-reviewed.txt)" $n } else { Passed "only as descriptions ($($accepted.Count) reviewed)" $n }
}

# ===== Languages =====
$langs = @("nl", "de", "fr", "es")
Check "lang.catalog" quick "The catalog check: NL, DE, FR, ES translated, placeholders and access keys consistent; each <= 40 KB" "100 % translated, 0 placeholder or access-key errors, <= 40 KB each" {
  $tool = @("tools\strings\check.py", "tools\strings\check.ps1") | Where-Object { Test-Path (Join-Path $root $_) } | Select-Object -First 1
  if (-not $tool) { return NotBuilt "f1-lang" "the catalog check (python tools\strings\check.py --build --json: see FeatherweightNotes\f1\gate-contract.md)" }
  $j = Join-Path $script:outDir "lang-catalog.json"
  if ($tool -like "*.ps1") { $code = Invoke-Script "lang.catalog" $tool @("-Build", (Full $Build), "-Json", $j) }
  else { $code = Invoke-Tool "lang.catalog" "python" @("-I", "-B", (Full $tool), "--build", (Full $Build), "--json", $j) }
  $c = Read-Json $j
  if (-not $c) { return Failed "the catalog check wrote nothing (exit $code)" }
  $n = [ordered]@{}; $bad = @()
  foreach ($l in $langs) {
    $x = $c.languages.$l
    if (-not $x) { $bad += "${l}: missing"; $n[$l] = "missing"; continue }
    $n[$l] = "$($x.translated) of $($x.strings), $($x.placeholderErrors) placeholder, $($x.accessKeyErrors) access-key and $([int]$x.otherErrors) other errors, $($x.compressedBytes) bytes"
    if ($x.translated -lt $x.strings -or $x.placeholderErrors -or $x.accessKeyErrors -or $x.otherErrors -or $x.compressedBytes -gt $catalogBudget) { $bad += $l }
  }
  if ($c.unmarked) { $n.unmarked = $c.unmarked; $bad += "$($c.unmarked) strings not through T()" }
  if ($code -and -not $bad.Count) { $bad += "check.py exited $code (its log)" }
  if ($bad.Count) { Failed ($bad -join ", ") $n } else { Passed "all four complete, each within 40 KB" $n }
}
Check "lang.clipping" long "The clipping check: every ribbon tab, panel and dialog at 1366 x 768 (100 %), in each language" "0 clipped labels" {
  $tool = @("tools\strings\clipping.ps1") | Where-Object { Test-Path (Join-Path $root $_) } | Select-Object -First 1
  if (-not $tool) { return NotBuilt "f1-lang" "the clipping check (tools\strings\clipping.ps1 -Exe -Json: see gate-contract.md)" }
  $testExe = Join-Path (Full $TestBuild) "Release\Featherweight.exe"
  if (-not (Test-Path $testExe)) { return Failed "not run: no test build at $TestBuild (FW_TEST_SCREENS on: clipcheck isn't in the shipped exe)" }
  $j = Join-Path $script:outDir "lang-clipping.json"
  $code = Invoke-Script "lang.clipping" $tool @("-Exe", $testExe, "-Json", $j, "-Out", (Join-Path $script:outDir "lang-clipping"), "-Languages", "en,nl,de,fr,es")
  $c = Read-Json $j
  if (-not $c) { return Failed "the clipping check wrote nothing (exit $code)" }
  $n = [ordered]@{ surfaces = $c.surfaces; languages = (@($c.languages) -join ","); clipped = @($c.clipped).Count }
  if (@($c.clipped).Count) { $n.first = "$(@($c.clipped)[0])"; Failed "$(@($c.clipped).Count) clipped labels" $n }
  elseif (@(@("en") + $langs | Where-Object { $_ -notin @($c.languages) }).Count) { Failed "not in use: $(@(@("en") + $langs | Where-Object { $_ -notin @($c.languages) }) -join ', ') (a catalog missing or damaged falls back to English)" $n }
  elseif ($code) { Failed "clipping.ps1 exited $code (its log)" $n }
  else { Passed "$($c.surfaces) surfaces in $(@($c.languages).Count) languages, none clipped" $n }
}
Check "lang.launch" timing "Launch to window in German no more than 5 ms slower than in English (fwtool bench-app)" "German - English <= 5 ms" {
  Need-Build
  $j = Join-Path $script:outDir "lang-launch.json"
  $code = Invoke-Tool "lang.launch" (Get-Fwtool) @("bench-app", "--runs", "10", "--languages", "en,de", "--json", $j, (Get-Exe))
  $c = Read-Json $j
  if (-not $c -or -not $c.languages) { return NotBuilt "f1-gate" "bench-app --languages (after f1-lang's language setting)" }
  $en = [double]$c.languages.en.launchMs; $de = [double]$c.languages.de.launchMs
  $n = [ordered]@{ englishMs = $en; germanMs = $de; differenceMs = [math]::Round($de - $en, 2) }
  if ($de - $en -le 5) { Passed "German $([math]::Round($de - $en, 2)) ms slower" $n } else { Failed "German $([math]::Round($de - $en, 2)) ms slower" $n }
}

# ===== PDF tools =====
Check "pdf.readers" long "Every PDF output (page tools, OCR, signatures, the torture run) reopens in PDFium (fwtool pdf-valid), Windows.Data.Pdf and Adobe Acrobat (a sample, our own instance)" "All open without errors" {
  $s = Join-Path $root "tests\pdf\pdf-readers.ps1"
  if (-not (Test-Path $s)) { return NotBuilt "f1-gate" "the PDF reader checks (tests\pdf\pdf-readers.ps1)" }
  Need-Build
  $o = Join-Path $script:outDir "pdf.readers"
  # The torture run's files (saved and extracted), and the made corpus with its refusals.
  $inputs = @((Join-Path $script:outDir "damage.pdf\saved"), (Join-Path $script:outDir "damage.pdf\extracted"))
  if (Test-Path (Join-Path $script:outDir "damage.pdf\signed")) { $inputs += Join-Path $script:outDir "damage.pdf\signed" }
  if (-not (Test-Path $inputs[0])) { return Failed "needs damage.pdf's outputs on this commit (run it first)" }
  $originals = Join-Path $script:outDir "damage.pdf\inputs.txt"
  $code = Use-Office "pdf.readers" "Acrobat" { Invoke-Script "pdf.readers" "tests\pdf\pdf-readers.ps1" @("-Fwtool", (Get-Fwtool), "-Out", $o, "-Paths", ($inputs -join ","), "-Originals", $originals) }
  if ($code -is [hashtable]) { return $code }
  $r = Read-Json (Join-Path $o "summary.json")
  if (-not $r) { return Failed "no summary (exit $code)" }
  $n = [ordered]@{}
  foreach ($p in $r.totals.PSObject.Properties) { $n[$p.Name] = $p.Value }
  if ($code -eq 0 -and $r.pass) { Passed $r.verdict $n } else { Failed $r.verdict $n }
}
Check "pdf.acrobat" quick "Adobe Acrobat (on the reference PC) opens the outputs too, in its own instance" "Opens without errors" {
  $acro = @("${env:ProgramFiles}\Adobe\Acrobat DC\Acrobat\Acrobat.exe", "${env:ProgramFiles(x86)}\Adobe\Acrobat Reader DC\Reader\AcroRd32.exe",
            "${env:ProgramFiles}\Adobe\Acrobat Reader DC\Reader\AcroRd32.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
  if (-not $acro) { return Failed "Acrobat isn't installed on this PC (the reference PC has Acrobat DC)" }
  $r = Read-Json (Join-Path $script:outDir "pdf.readers\summary.json")
  if ($r -and $null -ne $r.totals.acrobatFailed) { if ($r.totals.acrobatFailed) { Failed "$($r.totals.acrobatFailed) outputs fail in Acrobat" } else { Passed "all open in Acrobat" } }
  else { Failed "Acrobat is installed but pdf.readers didn't check it yet" }
}
Check "pdf.ocr" long "OCR on the generated scan set (English, 300 dpi): the text layer read back against the truth" ">= 95 % of words right" {
  $s = Join-Path $root "tests\pdf\ocr-accuracy.ps1"
  if (-not (Test-Path $s)) { return NotBuilt "f1-gate" "the OCR accuracy run (tests\pdf\ocr-accuracy.ps1)" }
  Need-Build
  $o = Join-Path $script:outDir "pdf.ocr"
  $code = Invoke-Script "pdf.ocr" "tests\pdf\ocr-accuracy.ps1" @("-Fwtool", (Get-Fwtool), "-Exe", (Get-Exe), "-Out", $o)
  $r = Read-Json (Join-Path $o "summary.json")
  if (-not $r) { return Failed "no summary (exit $code)" }
  $n = [ordered]@{ pages = $r.pages; words = $r.words; right = $r.right; percent = $r.percent; through = $r.through }
  if ($r.through -ne "featherweight") { return NotBuilt "f1-pdf" "OCR in Featherweight (the scan set measured Windows' OCR alone: $($r.percent) %)" }
  if ($r.percent -ge 95) { Passed "$($r.percent) % of $($r.words) words" $n } else { Failed "$($r.percent) % of $($r.words) words" $n }
}
Check "pdf.signatures" ci "Signatures through an independent validator (pyHanko on a CI runner): every signature valid, every change after signing caught" "All valid; all changes caught" {
  $r = Test-Workflow "pdf-signatures.yml" @("pyhanko|signatures") "f1-gate" "the pyHanko validation on CI"
  # Until the app signs, pyHanko sees only the samples made for it: the validator works, the row can't pass.
  if ($r.status -eq "passed" -and -not (Test-Path (Join-Path $root "src\pdf\sign"))) {
    return NotBuilt "f1-pdf" "certificate signing (src\pdf\sign): pyHanko checks the made samples only ($($r.detail))"
  }
  $r
}

# ===== Product basics =====
Check "ship.release" ci "The release workflow's dry run: test-signed (or SignPath-signed after step 1), signtool verify /pa on every file" "The dry run succeeds; every file verifies" {
  Test-Workflow "release.yml" @() "f1-ship" "the release workflow"
}
Check "ship.signpath" quick "SignPath signs the release (Roy's step 1)" "Signed by SignPath, or waiting for step 1" {
  $wf = Join-Path $root ".github\workflows\release.yml"
  if ((Test-Path $wf) -and (Get-Content $wf -Raw) -match '(?i)signpath' -and $env:FW_SIGNPATH_APPROVED) { Passed "SignPath in the workflow" }
  else { Waiting 1 "4.1 is test-signed in CI and released unsigned; the first signed build follows as 4.1.x" }
}
Check "ship.updater" long "The updater's tests (ctest, offline): unsigned and wrong-publisher files refused, channels, rollback, the policy" "Each kind has a passing test" {
  $c = $script:resultsById["build.ctest"]
  if (-not $c -or -not $c.numbers.tests) { return Failed "needs build.ctest's results on this commit" }
  $names = @($c.numbers.tests)
  $kinds = [ordered]@{ unsigned = "(?i)updater.*unsigned"; publisher = "(?i)updater.*publisher"; channel = "(?i)updater.*channel"; rollback = "(?i)updater.*rollback"; policy = "(?i)(updater|policy).*polic" }
  $n = [ordered]@{}; $missing = @()
  foreach ($k in $kinds.Keys) { $m = @($names | Where-Object { $_ -match $kinds[$k] }); $n[$k] = $m.Count; if (-not $m.Count) { $missing += $k } }
  if ($missing.Count) { return NotBuilt "f1-ship" ("updater tests named for: " + ($missing -join ", ") + " (gate-contract.md)") }
  if ($c.status -ne "passed") { return Failed "ctest didn't pass" $n }
  Passed "every kind tested and passing" $n
}
Check "ship.msix" ci "The MSIX through the Windows App Certification Kit; the Store waits for step 2" "WACK passes" {
  $r = Test-Workflow "release.yml" @("(?i)msix|wack") "f1-ship" "the MSIX and its WACK run"
  if ($r.status -eq "passed") { Waiting 2 "the MSIX passes WACK; the Store submission is Roy's" $r.numbers } else { $r }
}
Check "ship.install" ci "Silent per-machine install and uninstall of the MSI and the installer on a CI runner" "Both install and uninstall silently" {
  Test-Workflow "release.yml" @("(?i)install") "f1-ship" "the silent install job"
}
Check "ship.manifests" ci "The winget, Chocolatey and Scoop manifests (release.yml's manifests artifact on this commit): this version, the run's own files and hashes" "All three made and consistent; Chocolatey and Scoop wait for step 3" {
  # Before the release the manifests are the release workflow's (from its SHA256SUMS.txt); release.ps1 -Publish
  # commits them under packaging\ from the published release's (f1-ship).
  if (-not (Test-Path (Join-Path $root ".github\workflows\release.yml"))) { return NotBuilt "f1-ship" "the release workflow (.github\workflows\release.yml)" }
  $cm = Get-Content (Join-Path $root "CMakeLists.txt") -Raw
  $ver = if ($cm -match 'project\(Featherweight VERSION (\d+\.\d+\.\d+)') { $Matches[1] } else { "" }
  $m = Get-RunArtifact "release.yml" "manifests"
  if (-not $m) { return Failed "no manifests artifact from a finished release.yml run on this commit" }
  $p = Get-RunArtifact "release.yml" "packages"
  $sums = if ($p) { Get-ChildItem $p.dir -Recurse -Filter SHA256SUMS.txt -ErrorAction SilentlyContinue | Select-Object -First 1 }
  $runHashes = @(if ($sums) { [regex]::Matches((Get-Content $sums.FullName -Raw), '(?i)\b[0-9a-f]{64}\b') | ForEach-Object { $_.Value.ToLowerInvariant() } })
  $n = [ordered]@{ version = $ver; run = $m.run; runHashes = $runHashes.Count }; $missing = @()
  $expect = @{ winget = @("$ver\*.yaml"); chocolatey = @("featherweight.nuspec", "tools\chocolateyinstall.ps1"); scoop = @("featherweight.json") }
  foreach ($k in "winget", "chocolatey", "scoop") {
    $dir = Join-Path $m.dir $k
    $files = @(Get-ChildItem $dir -Recurse -File -ErrorAction SilentlyContinue)
    $n[$k] = $files.Count
    if (-not $files.Count) { $missing += "$k (none)"; continue }
    foreach ($e in $expect[$k]) { if (-not (Test-Path (Join-Path $dir $e))) { $missing += "$k ($e)" } }
    $text = ($files | ForEach-Object { Get-Content $_.FullName -Raw }) -join "`n"
    if ($ver -and $text -notmatch [regex]::Escape($ver)) { $missing += "$k (not $ver)" }
    $own = @([regex]::Matches($text, '(?i)\b[0-9a-f]{64}\b') | ForEach-Object { $_.Value.ToLowerInvariant() } | Sort-Object -Unique)
    if (-not $own.Count) { $missing += "$k (no SHA-256)" }
    elseif ($runHashes.Count) { $foreign = @($own | Where-Object { $_ -notin $runHashes }); if ($foreign.Count) { $missing += "$k ($($foreign.Count) hashes not in the run's SHA256SUMS.txt)" } }
    else { $missing += "$k (the run's SHA256SUMS.txt wasn't found to compare)" }
  }
  if ($missing.Count) { return Failed ("manifests: " + ($missing -join ", ")) $n }
  Waiting 3 "the manifests are ready; Chocolatey and Scoop need Roy's account and OK" $n
}
Check "ship.admx" quick "The ADMX policies match what the app reads (HKLM\SOFTWARE\Policies\Featherweight)" "Every ADMX value is read by src\Policy.cpp and back" {
  $admx = @(Get-ChildItem (Join-Path $root "packaging\admx") -Filter *.admx -ErrorAction SilentlyContinue)
  $policy = Join-Path $root "src\Policy.cpp"
  if (-not $admx.Count -or -not (Test-Path $policy)) { return NotBuilt "f1-ship" "packaging\admx\*.admx and src\Policy.cpp" }
  $names = @($admx | ForEach-Object { [regex]::Matches((Get-Content $_.FullName -Raw), 'valueName="([^"]+)"') } | ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique)
  $src = Get-Content $policy -Raw
  $read = @([regex]::Matches($src, 'L"([A-Za-z][A-Za-z0-9]+)"') | ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique)
  $notRead = @($names | Where-Object { $_ -notin $read })
  $n = [ordered]@{ admxValues = $names.Count; notRead = $notRead.Count }
  if ($notRead.Count) { $n.first = $notRead[0]; Failed "$($notRead.Count) ADMX values the app doesn't read" $n } else { Passed "$($names.Count) values, all read" $n }
}

# ===== External review =====
Check "review.repro" ci "Two CI builds of the commit, without their signatures, are byte-identical" "Byte-identical" {
  Test-Workflow "release.yml" @("(?i)reproduc") "f1-ship" "the reproducible-build comparison"
}
Check "review.defender" long "Microsoft Defender scans the release files clean" "No threat found" {
  Need-Build
  $mp = @("${env:ProgramFiles}\Windows Defender\MpCmdRun.exe") + @(Get-ChildItem "$env:ProgramData\Microsoft\Windows Defender\Platform\*\MpCmdRun.exe" -ErrorAction SilentlyContinue | Sort-Object FullName -Descending | ForEach-Object FullName) | Where-Object { Test-Path $_ } | Select-Object -First 1
  if (-not $mp) { return Failed "MpCmdRun.exe not found" }
  $files = @((Get-Exe), (Join-Path (Full $Build) "Release\pdfium.dll")) + @(Get-ChildItem (Join-Path $root "dist") -File -ErrorAction SilentlyContinue | ForEach-Object FullName)
  $n = [ordered]@{ files = $files.Count }; $bad = @()
  foreach ($f in $files) {
    $code = Invoke-Tool "review.defender" $mp @("-Scan", "-ScanType", "3", "-File", $f, "-DisableRemediation")
    if ($code -ne 0) { $bad += [IO.Path]::GetFileName($f) }
  }
  $n.flagged = $bad.Count
  if ($bad.Count) { Failed ("Defender flags " + ($bad -join ", ")) $n } else { Passed "$($files.Count) files clean" $n }
}
Check "review.scans" quick "VirusTotal and Hybrid Analysis (Roy's step 5): Defender clean, other detections listed, sandbox traffic only to GitHub" "Reports linked; traffic only to GitHub" {
  $s = Join-Path $root "scripts\scan-release.ps1"
  if (-not (Test-Path $s)) { return NotBuilt "f1-trust" "scripts\scan-release.ps1" }
  $o = Join-Path $script:outDir "scan-release"
  Remove-Item (Join-Path $o "summary.json") -ErrorAction SilentlyContinue
  $code = Invoke-Script "review.scans" "scripts\scan-release.ps1" @("-FromCi", "-Commit", $script:ciHead, "-OutDir", $o)
  $r = Read-Json (Join-Path $o "summary.json")
  if (-not $r) { return Failed "scan-release.ps1 -FromCi wrote no summary (exit $code; its log)" }
  $n = [ordered]@{ verdict = $r.verdict }
  if ($r.waiting) { return Waiting 5 "the scan workflow is ready; the keys are Roy's" $n }
  if ($r.pass) { Passed $r.verdict $n } else { Failed $r.verdict $n }
}
Check "review.coverity" ci "Coverity Scan runs weekly (Roy's step 6)" "A successful Coverity run, or waiting for step 6" {
  if (-not (Test-Path (Join-Path $root ".github\workflows\coverity.yml"))) { return NotBuilt "f1-trust" ".github\workflows\coverity.yml" }
  try { $j = Gh-Json "repos/$repoSlug/actions/workflows/coverity.yml/runs?status=completed&per_page=10" } catch { return Waiting 6 "no Coverity run yet" }
  # The run succeeds while its scan job is skipped (no token yet): only a scan job's success counts.
  foreach ($run in @($j.workflow_runs | Where-Object { $_.conclusion -eq "success" })) {
    $jobs = @((Gh-Json "repos/$repoSlug/actions/runs/$($run.id)/jobs?per_page=100").jobs)
    $scan = @($jobs | Where-Object { $_.name -match '^scan\b' })
    if (@($jobs | Where-Object { $_.name -match '^token\b' }).Count -and $scan.Count -and $scan[0].conclusion -eq "success") { return Passed "last scan $($run.created_at)" ([ordered]@{ run = $run.html_url }) }
  }
  Waiting 6 "the workflow is in; its scan job is skipped until the token is there"
}

# ===== Lightweight =====
Check "light.size" quick "Size (budgets.ps1 on the shipped build): the exe, and the default download" "Exe <= 4.0's + 1,024 KB (14,216,192 bytes); download <= 10 MB" {
  Need-Build
  $j = Join-Path $script:outDir "budgets.json"
  $code = Invoke-Script "light.size" "scripts\budgets.ps1" @("-Build", $Build, "-Json", $j)
  $b = Read-Json $j
  if (-not $b) { return Failed "budgets.ps1 wrote nothing (exit $code)" }
  $exe = [long]$b.size.exe
  # The default download is the x64 installer that release.yml's package job makes (its packages artifact).
  $p = Get-RunArtifact "release.yml" "packages"
  $setup = if ($p) { Get-ChildItem $p.dir -Recurse -Filter "Featherweight-*-Setup.exe" -ErrorAction SilentlyContinue | Select-Object -First 1 }
  $dl = if ($setup) { [long]$setup.Length } else { 0 }
  $n = [ordered]@{ exeBytes = $exe; exeGrowthKB = [math]::Round(($exe - $exe40) / 1024, 1); downloadBytes = $dl; downloadMB = [math]::Round($dl / 1MB, 2); budgetsExit = $code }
  $n.download = if ($setup) { "$($setup.Name) from $($p.run)" } else { "none" }
  $bad = @()
  if (-not $setup) { $bad += "the download: no installer from a finished release.yml run on this commit" }
  if ($exe -gt $exeBudget) { $bad += "exe $([math]::Round(($exe - $exe40) / 1024)) KB over 4.0's (1,024 allowed)" }
  if ($setup -and $dl -gt $downloadBudget) { $bad += "download $($n.downloadMB) MB (10 MB; f1-ship's V8-less PDFium)" }
  if ($code) { $bad += "budgets.ps1 failed (unrecorded growth?)" }
  if ($bad.Count) { Failed ($bad -join "; ") $n } else { Passed "exe +$($n.exeGrowthKB) KB, download $($n.downloadMB) MB" $n }
}
Check "light.timings" quick "Launch, memory and idle no worse than 4.0's; long-300.docx's worst key < 50 ms (the final timing run: budgets.ps1 -Timings -Runs 3 -Record)" "No worse than 4.0's; worst key < 50 ms" {
  # The ledger (tests\budgets\ledger.json): timings[] (scenarios: launch and each file) and typing[]
  # (long-300.docx), per commit and machine. 4.0's are the P3 final gate's, on 57930657.
  $ledger = Read-Json (Join-Path $root "tests\budgets\ledger.json")
  $machine = $env:COMPUTERNAME
  $forHead = {
    param($list)
    $m = @($list | Where-Object { $_.machine -eq $machine -and $_.commit -and $script:head.StartsWith($_.commit) })
    # An entry on an earlier commit counts when only docs changed since.
    if (-not $m.Count) { $m = @($list | Where-Object { $_.machine -eq $machine -and $_.commit -and (Test-OnlyDocsSince $_.commit) }) }
    if ($m.Count) { $m[-1] } else { $null }
  }
  $e = & $forHead @($ledger.timings)
  $t = & $forHead @($ledger.typing)
  $b = @($ledger.timings | Where-Object { $_.machine -eq $machine -and $_.commit -eq "57930657" }) | Select-Object -Last 1
  if (-not $e) { return Failed "no timing entry in the ledger for this commit on $machine (the coordinator's final run: budgets.ps1 -Timings -Runs 3 -Record)" }
  if (-not $b) { return Failed "no 4.0 timing entry (57930657) on $machine to compare with" }
  $n = [ordered]@{ entry = $e.commit; runs = $e.runs }
  $bad = @()
  # No worse than 4.0's, as budgets.ps1 counts it: more than 10 % slower, or 10 % more memory, fails.
  foreach ($s in @($e.scenarios)) {
    $o = @($b.scenarios | Where-Object name -eq $s.name) | Select-Object -First 1
    if (-not $o) { continue }
    foreach ($k in "windowMs", "readyMs", "memoryMB") {
      if ($null -ne $s.$k -and $null -ne $o.$k -and [double]$s.$k -gt [double]$o.$k * 1.10) { $bad += "$($s.name) $k $($s.$k) (4.0: $($o.$k))" }
    }
    if ([double]$s.idleCpuMs -gt 0) { $bad += "$($s.name) idle CPU $($s.idleCpuMs) ms" }
  }
  $launch = @($e.scenarios | Where-Object name -eq "launch") | Select-Object -First 1
  $launch0 = @($b.scenarios | Where-Object name -eq "launch") | Select-Object -First 1
  if ($launch) { $n.launchWindowMs = "$($launch.windowMs) (4.0: $($launch0.windowMs))"; $n.launchMemoryMB = "$($launch.memoryMB) (4.0: $($launch0.memoryMB))" }
  if ($t) { $n.long300WorstKeyMs = $t.worstMs; if ([double]$t.worstMs -ge 50) { $bad += "long-300's worst key $($t.worstMs) ms" } }
  else { $bad += "no long-300 typing entry for this commit" }
  $n.regressions = $bad.Count
  if ($bad.Count) { $n.first = $bad[0]; Failed ($bad -join "; ") $n } else { Passed "no worse than 4.0's; long-300's worst key $($t.worstMs) ms" $n }
}
Check "light.modules" quick "Nothing loads before it's used: the modules after launch (fwtool bench-app --modules)" "None of OCR, the share sheet, CNG signing, dbghelp; no module 4.0 didn't load" {
  Need-Build
  $j = Join-Path $script:outDir "modules.json"
  $code = Invoke-Tool "light.modules" (Get-Fwtool) @("bench-app", "--modules", "--runs", "1", "--json", $j, (Get-Exe))
  $m = Read-Json $j
  if (-not $m -or -not $m.modules) { return NotBuilt "f1-gate" "fwtool bench-app --modules" }
  $mods = @($m.modules | ForEach-Object { $_.ToLowerInvariant() })
  # OCR (Windows.Media.Ocr), the share sheet (DataTransferManager), CNG key storage for signing, dbghelp
  # for crash dumps. (dataexchange.dll is OLE's clipboard and drag and drop, loaded at 4.0 already.)
  $forbidden = @("windows.media.ocr.dll", "windows.globalization.dll", "windows.applicationmodel.dll", "windows.applicationmodel.datatransfer.dll",
                 "sharehost.dll", "ncrypt.dll", "ncryptprov.dll", "dbghelp.dll", "dbgcore.dll")
  $bad = @($mods | Where-Object { $_ -in $forbidden })
  $baseFile = Join-Path $root "tests\pdf\modules-4.0.txt"
  $new = @()
  if (Test-Path $baseFile) { $base = @(Get-Content $baseFile | ForEach-Object { $_.Trim().ToLowerInvariant() } | Where-Object { $_ -and $_ -notmatch '^#' }); $new = @($mods | Where-Object { $_ -notin $base }) }
  $n = [ordered]@{ modules = $mods.Count; forbidden = $bad.Count; newSince40 = $new.Count }
  if ($bad.Count) { $n.first = $bad[0] }
  if ($new.Count) { $n.new = ($new -join ", ") }
  if (-not (Test-Path $baseFile)) { return Failed "no 4.0 module list (tests\pdf\modules-4.0.txt)" $n }
  if ($bad.Count -or $new.Count) { Failed "$($bad.Count) loaded before use, $($new.Count) new since 4.0" $n } else { Passed "$($mods.Count) modules, as at 4.0" $n }
}

# ===== Friction =====
Check "friction.rf" long "RF2 (comments open with the Review pane) and RF7 (Title in the Styles gallery in one click at 1366 px)" "Word's steps or fewer" {
  $s = Join-Path $root "tests\f1-friction-checks.ps1"
  if (-not (Test-Path $s)) { return NotBuilt "f1-gate" "tests\f1-friction-checks.ps1" }
  Need-Build
  $o = Join-Path $script:outDir "friction"
  $code = Invoke-Script "friction.rf" "tests\f1-friction-checks.ps1" @("-Exe", (Get-Exe), "-Fwtool", (Get-Fwtool), "-Out", $o, "-Flows", "RF2,RF7")
  $r = Read-Json (Join-Path $o "summary.json")
  if (-not $r) { return Failed "no summary (exit $code)" }
  $n = [ordered]@{}; foreach ($f in @($r.flows)) { $n[$f.id] = "$($f.steps) steps (target $($f.target))$(if (-not $f.ok) { ': ' + $f.why })" }
  if ($code -eq 0 -and $r.pass) { Passed "both within Word's steps" $n } else { Failed "over Word's steps, or not built" $n }
}
Check "friction.flows" long "The new flows: merge two PDFs, sign a PDF, OCR a scan, change the language, share as PDF" "Within their written targets (docs\friction-review.md, F1): no more steps than Acrobat" {
  $s = Join-Path $root "tests\f1-friction-checks.ps1"
  if (-not (Test-Path $s)) { return NotBuilt "f1-gate" "tests\f1-friction-checks.ps1" }
  Need-Build
  $o = Join-Path $script:outDir "friction"
  $code = Invoke-Script "friction.flows" "tests\f1-friction-checks.ps1" @("-Exe", (Get-Exe), "-Fwtool", (Get-Fwtool), "-Out", $o, "-Flows", "merge,sign,certificate,ocr,language,share")
  $r = Read-Json (Join-Path $o "summary.json")
  if (-not $r) { return Failed "no summary (exit $code)" }
  $n = [ordered]@{}; foreach ($f in @($r.flows)) { $n[$f.id] = "$($f.steps) steps (target $($f.target))$(if (-not $f.ok) { ': ' + $f.why })" }
  if ($code -eq 0 -and $r.pass) { Passed "every flow within its target" $n } else { Failed "over target, or not built" $n }
}

# ===== Docs =====
Check "docs.readme" quick "The README: the network wording exactly, the badges (CodeQL, Scorecard, Best Practices), the name-and-logo line" "Present" {
  $readme = Get-Content (Join-Path $root "README.md") -Raw -Encoding UTF8
  $wording = "no telemetry, no account, no subscription; the only network access is the update check, which can be turned off"
  $n = [ordered]@{
    wording = $readme.IndexOf($wording, [StringComparison]::OrdinalIgnoreCase) -ge 0
    codeqlBadge = [bool]($readme -match '(?i)codeql[^)\n]*\.svg|badge[^)\n]*codeql')
    scorecardBadge = [bool]($readme -match '(?i)scorecard')
    bestPracticesBadge = [bool]($readme -match '(?i)bestpractices\.(coreinfrastructure\.org|dev)/[^)\s]*projects/\d+')
  }
  $missing = @($n.Keys | Where-Object { -not $n[$_] })
  if ($missing.Count -eq 1 -and $missing[0] -eq "bestPracticesBadge") { return Waiting 6 "the wording and the CodeQL and Scorecard badges are in; the Best Practices badge needs the project number step 6 makes" $n }
  if ($missing.Count) { NotBuilt "f1-trust" ("README: " + ($missing -join ", ")) } else { Passed "all present" $n }
}
Check "docs.security" quick "SECURITY.md: private reporting, response times, supported versions; reporting on (step 4)" "Present; the reporting link waits for step 4" {
  $p = Join-Path $root "SECURITY.md"
  if (-not (Test-Path $p)) { return NotBuilt "f1-trust" "SECURITY.md" }
  $t = Get-Content $p -Raw -Encoding UTF8
  $n = [ordered]@{ privateReporting = [bool]($t -match '(?i)privat'); responseTimes = [bool]($t -match '(?i)\b\d+\s*(day|business day|hour)s?\b'); supportedVersions = [bool]($t -match '(?i)supported') }
  $missing = @($n.Keys | Where-Object { -not $n[$_] })
  if ($missing.Count) { return Failed ("SECURITY.md lacks: " + ($missing -join ", ")) $n }
  Waiting 4 "SECURITY.md is complete; private reporting is a repository setting" $n
}
Check "docs.site" quick "The website (site\): help, known issues, the privacy statement; Help opens it; published (step 4)" "Present and linked from Help; publishing waits for step 4" {
  $site = Join-Path $root "site"
  if (-not (Test-Path $site)) { return NotBuilt "f1-trust" "site\" }
  $pages = @(Get-ChildItem $site -Recurse -File -Include *.md, *.html | ForEach-Object { $_.Name.ToLowerInvariant() })
  $n = [ordered]@{ pages = $pages.Count
                   help = [bool]($pages -match 'help'); knownIssues = [bool]($pages -match 'known'); privacy = [bool]($pages -match 'privacy')
                   helpLinked = [bool](@(Find-InFiles @("src") "github\.io").Count) }
  $missing = @($n.Keys | Where-Object { $_ -ne "pages" -and -not $n[$_] })
  if ($missing.Count) { return Failed ("site: " + ($missing -join ", ")) $n }
  Waiting 4 "the site is built; GitHub Pages from Actions is a repository setting" $n
}
Check "docs.notes" quick "The release notes: the size and timing ledger lines, the report links" "Present; scan links wait for step 5" {
  $cm = Get-Content (Join-Path $root "CMakeLists.txt") -Raw
  $ver = if ($cm -match 'project\(Featherweight VERSION (\d+\.\d+\.\d+)') { $Matches[1] } else { "" }
  # The tag, the file names and the manifests come from CMake's version, so it must be this gate's.
  if (-not $ver.StartsWith("$Version.")) { return Failed "CMakeLists.txt says VERSION ${ver}; $Version needs project(Featherweight VERSION $Version.0) before the final run (the notes, the manifests and the tag follow it)" ([ordered]@{ cmakeVersion = $ver }) }
  $p = @("docs\release-notes\$ver.md", "packaging\release-notes.md", "docs\release-notes.md") | ForEach-Object { Join-Path $root $_ } | Where-Object { Test-Path $_ } | Select-Object -First 1
  if (-not $p) { return NotBuilt "f1-ship" "the release notes (docs\release-notes\$ver.md: see gate-contract.md)" }
  $t = Get-Content $p -Raw -Encoding UTF8
  $n = [ordered]@{ file = $p.Substring($root.Length + 1); sizeLine = [bool]($t -match '(?i)Featherweight\.exe.*(bytes|KB|MB)'); timingLine = [bool]($t -match '(?i)launch.*ms'); gateLink = [bool]($t -match '(?i)gate') }
  $missing = @("sizeLine", "timingLine" | Where-Object { -not $n[$_] })
  if ($missing.Count) { Failed ("release notes lack: " + ($missing -join ", ")) $n } else { Passed "ledger lines present" $n }
}

# ---------------------------------------------------------------------------------------------------
# Reuse: results kept per commit.
# ---------------------------------------------------------------------------------------------------
$script:docsOnly = @{}
function Test-OnlyDocsSince([string]$from) {
  if ($script:docsOnly.ContainsKey($from)) { return $script:docsOnly[$from] }
  $ok = $false
  & git merge-base --is-ancestor $from $script:head 2>$null
  if ($LASTEXITCODE -eq 0) {
    $changed = @(Invoke-Git diff --name-only $from $script:head)
    $ok = -not @($changed | Where-Object { $_ -notmatch '^docs/' -and $_ -notmatch '\.md$' -and $_ -ne 'tests/budgets/ledger.json' }).Count
  }
  $script:docsOnly[$from] = $ok
  $ok
}
function Find-Kept([string]$id) {
  $dir = Join-Path $script:outDir "results"
  if (-not (Test-Path $dir)) { return $null }
  $mine = Join-Path $dir "$($script:head)\$id.json"
  if (Test-Path $mine) { return Read-Json $mine }
  foreach ($d in @(Get-ChildItem $dir -Directory | Sort-Object LastWriteTime -Descending)) {
    $f = Join-Path $d.FullName "$id.json"
    if ((Test-Path $f) -and $d.Name -match '^[0-9a-f]{40}$' -and (Test-OnlyDocsSince $d.Name)) { return Read-Json $f }
  }
  $null
}

# ---------------------------------------------------------------------------------------------------
# -Verify: release.ps1 -Publish's check of a report.
# ---------------------------------------------------------------------------------------------------
if ($Verify) {
  if (-not (Test-Path $Verify)) { Write-Host "No gate report at $Verify. Run scripts\f1-gate.ps1 on the final head first." -ForegroundColor Red; exit 1 }
  $rep = Read-Json $Verify
  $want = if ($Commit) { (Invoke-Git rev-parse $Commit) } else { (Invoke-Git rev-parse HEAD) }
  $problems = @()
  if ($rep.commit -ne $want) { $problems += "the report is for $($rep.commit), not $want" }
  if ($rep.dirty) { $problems += "the report was made on a tree with uncommitted changes" }
  $failed = @($rep.rows | Where-Object { $_.status -ne "passed" -and $_.status -ne "waiting" })
  if ($failed.Count) { $problems += "failed rows: " + (($failed | ForEach-Object { $_.title }) -join ", ") }
  $waiting = @($rep.rows | ForEach-Object { $_.checks } | Where-Object { $_.status -eq "waiting" })
  foreach ($w in $waiting) { Write-Host "  waiting: $($w.title): $($w.detail)" }
  if ($problems.Count) { foreach ($p in $problems) { Write-Host "Gate $($rep.version): $p" -ForegroundColor Red }; exit 1 }
  Write-Host "Gate $($rep.version) passed for $want ($($waiting.Count) checks waiting for Roy's steps)." -ForegroundColor Green
  exit 0
}

if ($List) {
  foreach ($r in $rowTitles.Keys) {
    "{0} ({1})" -f $rowTitles[$r], $r
    foreach ($c in @($all | Where-Object row -eq $r)) { "  {0,-18} {1,-6} {2}" -f $c.id, $c.kind, $c.title }
  }
  exit 0
}

# ---------------------------------------------------------------------------------------------------
# The run
# ---------------------------------------------------------------------------------------------------
$script:head = Invoke-Git rev-parse HEAD
$script:ciHead = if ($CiCommit) { Invoke-Git rev-parse $CiCommit } else { $script:head }
if (-not $script:ciHead) { throw "unknown -CiCommit $CiCommit" }
$dirty = [bool]@(Invoke-Git status --porcelain).Count
$branch = Invoke-Git rev-parse --abbrev-ref HEAD
$script:outDir = Full $Out
New-Item -ItemType Directory -Force $script:outDir | Out-Null
# fwtool's file-open hook (legal.fileopen): every fwtool this run starts logs the files it opens here.
$env:FW_FILE_LOG_DIR = Join-Path $script:outDir "filelog"
New-Item -ItemType Directory -Force $env:FW_FILE_LOG_DIR | Out-Null
$Rows = @($Rows | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$Checks = @($Checks | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
foreach ($r in $Rows) { if (-not $rowTitles.Contains($r)) { throw "unknown row $r (rows: $($rowTitles.Keys -join ', '))" } }
foreach ($c in $Checks) { if (-not @($all | Where-Object id -eq $c).Count) { throw "unknown check $c (-List shows them)" } }
$selected = @($all | Where-Object { (-not $Rows.Count -and -not $Checks.Count) -or $_.row -in $Rows -or $_.id -in $Checks })
Say "Gate $Version on $($script:head.Substring(0, 12)) ($branch$(if ($dirty) { ', uncommitted changes' }))"

$script:resultsById = @{}
$results = @{}
$resultDir = Join-Path $script:outDir "results\$($script:head)"
foreach ($c in $all) {
  $kept = Find-Kept $c.id
  $isSelected = $c -in $selected
  # Quick and CI checks run every time, whatever -Rows says: the report then shows every row as it is now.
  $run = $c.kind -in "quick", "ci" -or ($isSelected -and (
    $c.id -in $Checks -or
    ($c.kind -eq "long" -and -not $Quick -and ($Fresh -or -not $kept)) -or
    ($c.kind -eq "timing" -and $Timings)))
  if (-not $run) {
    if ($kept) { $results[$c.id] = $kept; $script:resultsById[$c.id] = $kept }
    continue
  }
  Say ("== {0}: {1}" -f $c.id, $c.title)
  $t0 = Get-Date
  try {
    $r = @(& $c.run) | Where-Object { $_ -is [hashtable] -and $_.ContainsKey("status") } | Select-Object -Last 1
    if (-not $r) { $r = Failed "the check returned no result" }
  } catch {
    $r = Failed ("error: " + $_.Exception.Message + " (" + ($_.InvocationInfo.PositionMessage -split "`n")[0] + ")")
  }
  $res = [ordered]@{ id = $c.id; status = $r.status; detail = $r.detail; numbers = $r.numbers; step = $r.step
                     commit = $script:head; dirty = $dirty; measured = (Get-Date).ToString("s"); seconds = [math]::Round(((Get-Date) - $t0).TotalSeconds) }
  $results[$c.id] = [pscustomobject]$res
  $script:resultsById[$c.id] = [pscustomobject]$res
  if (-not $dirty) { Write-Json (Join-Path $resultDir "$($c.id).json") $res }
  $color = switch ($r.status) { "passed" { "Green" } "waiting" { "Yellow" } default { "Red" } }
  Write-Host ("   {0}: {1}" -f $r.status, $r.detail) -ForegroundColor $color
}

# ---------------------------------------------------------------------------------------------------
# The report
# ---------------------------------------------------------------------------------------------------
$rowsOut = @()
foreach ($rk in $rowTitles.Keys) {
  $cs = @()
  foreach ($c in @($all | Where-Object row -eq $rk)) {
    $r = $results[$c.id]
    $status = if ($r) { $r.status } else { "not run" }
    $from = if ($r -and $r.commit -ne $script:head) { $r.commit } else { $null }
    $cs += [ordered]@{ id = $c.id; title = $c.title; pass = $c.pass; kind = $c.kind; status = $status
                       detail = $(if ($r) { $r.detail } else { "no result on this commit" }); numbers = $(if ($r) { $r.numbers } else { $null })
                       step = $(if ($r) { $r.step } else { $null }); measuredOn = $(if ($r) { $r.commit } else { $null }); carriedFrom = $from
                       measured = $(if ($r) { $r.measured } else { $null }) }
  }
  $st = @($cs | ForEach-Object { $_.status })
  $rs = if ($st | Where-Object { $_ -in "failed", "not run" }) { "failed" } elseif ($st -contains "waiting") { "waiting" } else { "passed" }
  $rowsOut += [ordered]@{ id = $rk; title = $rowTitles[$rk]; status = $rs; checks = $cs }
}
$exeInfo = $null
if (Test-Path (Get-Exe)) { $e = Get-Item (Get-Exe); $exeInfo = [ordered]@{ bytes = $e.Length; sha256 = (Get-FileHash $e.FullName -Algorithm SHA256).Hash } }
$failedRows = @($rowsOut | Where-Object status -eq "failed")
$gate = [ordered]@{
  version = $Version; commit = $script:head; branch = $branch; dirty = $dirty; generated = (Get-Date).ToString("s"); exe = $exeInfo
  publishable = (-not $dirty -and -not $failedRows.Count -and -not $CiCommit); ciCommit = $script:ciHead; rows = $rowsOut
}
$count = { param($s) @($rowsOut | ForEach-Object { $_.checks } | Where-Object { $_.status -eq $s }).Count }
$md = @("# Gate $Version", "",
  "Commit ``$($script:head)`` ($branch)$(if ($dirty) { ', **with uncommitted changes: counts for nothing**' })$(if ($CiCommit) { ", CI rows read ``$($script:ciHead)`` (-CiCommit: a trial, counts for nothing)" }), $((Get-Date).ToString('yyyy-MM-dd HH:mm')).",
  $(if ($exeInfo) { "Featherweight.exe ($Build): $('{0:N0}' -f $exeInfo.bytes) bytes, sha256 $($exeInfo.sha256)." } else { "No shipped build in $Build." }), "",
  "**$(if ($gate.publishable) { 'Publishable' } else { 'Not publishable' })**: rows $(@($rowsOut | Where-Object status -eq 'passed').Count) passed, $($failedRows.Count) failed, $(@($rowsOut | Where-Object status -eq 'waiting').Count) waiting; checks $(& $count 'passed') passed, $(& $count 'failed') failed, $(& $count 'not run') not run, $(& $count 'waiting') waiting for Roy's steps.", "",
  "| Row | Status | Checks |", "|---|---|---|")
foreach ($r in $rowsOut) {
  $md += "| $($r.title) | $($r.status) | $(@($r.checks | ForEach-Object { "$($_.id.Split('.')[1]): $($_.status)" }) -join ', ') |"
}
foreach ($r in $rowsOut) {
  $md += @("", "## $($r.title): $($r.status)", "")
  foreach ($c in $r.checks) {
    $md += "- **$($c.status)** ``$($c.id)``: $($c.title). Pass: $($c.pass)."
    $md += "  - $($c.detail)$(if ($c.carriedFrom) { " (measured on $($c.carriedFrom.Substring(0, 12)); only docs changed since)" })"
    if ($c.numbers) {
      $pairs = @()
      $nums = $c.numbers
      $keys = if ($nums -is [Collections.IDictionary]) { @($nums.Keys) } else { @($nums.PSObject.Properties | ForEach-Object Name) }
      foreach ($k in $keys) {
        if ($k -eq "tests") { continue }
        $v = if ($nums -is [Collections.IDictionary]) { $nums[$k] } else { $nums.$k }
        $pairs += "$k $v"
      }
      if ($pairs.Count) { $md += "  - " + ($pairs -join "; ") }
    }
  }
}
$waitingChecks = @($rowsOut | ForEach-Object { $_.checks } | Where-Object { $_.status -eq "waiting" })
if ($waitingChecks.Count) {
  $md += @("", "## Waiting for Roy's steps (for the release notes)", "")
  foreach ($w in $waitingChecks) { $md += "- Step $($w.step): $($w.title)" }
}
$jsonPath = Join-Path $script:outDir "gate-$Version.json"
$mdPath = Join-Path $script:outDir "gate-$Version.md"
Write-Json $jsonPath $gate
[IO.File]::WriteAllText($mdPath, ($md -join "`n") + "`n", $utf8)
if ($Report) {
  New-Item -ItemType Directory -Force $Report | Out-Null
  Copy-Item $jsonPath (Join-Path $Report "gate-$Version.json") -Force
  Copy-Item $mdPath (Join-Path $Report "gate-$Version.md") -Force
}
Say ""
Say ($md[5])
Say "Report: $mdPath$(if ($Report) { " (and $Report)" })"
if ($gate.publishable) { exit 0 } else { exit 1 }
