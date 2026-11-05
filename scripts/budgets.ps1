<#
.SYNOPSIS
  Checks a build against the lightweight budgets (plan section 11) and the size ledger,
  tests\budgets\ledger.json, which records the reason for every accepted increase.

.DESCRIPTION
  Size, on every build (CI and local): Featherweight.exe, and the portable zip made the way
  scripts\release.ps1 makes it. The build must be the shipped configuration (-DFW_TEST_SCREENS=OFF,
  as release.ps1 builds it in build-release). It fails when either has grown more than 50 KB over
  the ledger's baseline, or is over its budget. Compilers make different sizes, so the baseline is
  the last entry made with this compiler, plus the increases accepted since with another one.

  Timings, with -Timings: fwtool bench-app starts a private copy of the build, alone and with each
  fixed file, and measures launch to window, to ready (the window drawn, or the first page shown),
  memory, and CPU and timers while idle (tests\budgets\README.md). The baseline is this computer's
  last entry in the ledger. It fails when a time is more than 10 % slower or memory 10 % more,
  when memory after launch or launch time is over its budget, or when anything runs while idle.
  -TrackTimings measures and reports without failing: CI's runners are too noisy to gate on.

  -Record "why" adds this build's sizes (and timings, with -Timings) to the ledger with the reason,
  making them the new baseline. Commit the ledger with the change that needed it.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\budgets.ps1
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\budgets.ps1 -Timings
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\budgets.ps1 -Timings -Record "D1: the docx layout engine"
#>
param(
  [string]$Build = "build-release",
  [switch]$Timings,
  [switch]$TrackTimings,
  [int]$Runs = 5,
  [string]$Record,
  [string]$Json,
  [string]$Fwtool,
  # With -Record: sizes measured elsewhere, such as on CI, whose log prints the command.
  [string]$Toolset, [long]$ExeBytes, [long]$DownloadBytes
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

# Plan section 11. KB and MB are 1024-based.
$sizeStep = 50KB           # an increase over this needs a reason in the ledger
# Featherweight.exe: the budget of the release being built (decision 22).
# $exeBudget = 4MB         # until 1.1 (history)
$exeBudget = 7MB           # 2.0, which D2 to D5 aim at
$downloadBudget = 25MB     # the installer or zip, with PDFium
$launchMemoryBudget = 40   # MB, the private working set right after launch
$launchBudget = 300        # ms, launch to the window drawn
$slower = 0.10             # more than this over the baseline fails...
$floorMs = 5; $floorMB = 2 # ...when it is also more than this: smaller changes are noise
$idleCpuLimit = 5          # ms over bench-app's idle window, and thread wakeups:
$idleWakeupLimit = 2       # a timer firing once a second wakes the app 3 times
# Opening fixed files of each kind. Add the office formats as they open.
$fixedFiles = @("tests\pdfs\text.pdf", "tests\pdfs\big-600-pages.pdf", "tests\pdfs\form.pdf", "tests\office\layout\report.docx")
# A 300-page Word document, made by tests\office\make_long_docs.py when Python is there: "ready"
# includes paginating it all in the background, which has a budget of its own.
$longDocBudget = 3000      # ms, launch to a 300-page document fully paginated
$longDoc = "long-300.docx"
# Typing in it (fwtool bench --typing): keystroke to the page painted again, the median of letters
# and Backspaces, and the worst of every key, Enter included.
$typingBudget = 16; $typingWorstBudget = 50  # ms
$typingFloorMs = 1         # a typing change smaller than this is noise

$ledgerPath = Join-Path $root "tests\budgets\ledger.json"
$ledger = Get-Content $ledgerPath -Raw | ConvertFrom-Json
$sizes = @($ledger.sizes | Where-Object { $_ })
$timingEntries = @($ledger.timings | Where-Object { $_ })
$typingEntries = @($ledger.typing | Where-Object { $_ })
$failures = New-Object Collections.Generic.List[string]

function Format-Bytes([long]$n) { "{0:N0}" -f $n }
function Format-Change([double]$now, [double]$then, [string]$unit) {
  $sign = if ($now -ge $then) { "+" } else { "" }
  $pct = if ($then) { " ({0}{1:N1} %)" -f $sign, (100 * ($now - $then) / $then) } else { "" }
  "{0}{1:N1} {2}{3}" -f $sign, ($now - $then), $unit, $pct
}

# The compiler, from the linker version in the exe's header: v142 is Visual Studio 2019, v143 2022.
function Get-Toolset([string]$exe) {
  $bytes = [IO.File]::ReadAllBytes($exe)
  $pe = [BitConverter]::ToInt32($bytes, 0x3C)
  $major = $bytes[$pe + 26]; $minor = $bytes[$pe + 27]
  $name = if ($major -eq 14 -and $minor -ge 20 -and $minor -le 29) { "v142" }
          elseif ($major -eq 14 -and $minor -ge 30 -and $minor -le 49) { "v143" }
          else { "link$major.$minor" }
  [pscustomobject]@{ name = $name; linker = "$major.$minor" }
}

# The portable zip, staged and compressed as scripts\release.ps1 does it (keep the two the same).
function Measure-Download([string]$release) {
  $tmp = Join-Path ([IO.Path]::GetTempPath()) "fw-budgets-$PID"
  $stage = Join-Path $tmp "Featherweight"
  New-Item -ItemType Directory -Force $stage | Out-Null
  try {
    Copy-Item "$release\Featherweight.exe", "$release\pdfium.dll" $stage
    Copy-Item LICENSE (Join-Path $stage "LICENSE.txt")
    Copy-Item THIRD_PARTY_NOTICES.md $stage
    $zip = Join-Path $tmp "Featherweight-Portable.zip"
    Compress-Archive -Path "$stage\*" -DestinationPath $zip
    (Get-Item $zip).Length
  } finally {
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
  }
}

# The last sizes made with this compiler, plus the increases accepted since with others.
function Get-SizeBaseline([string]$toolset) {
  $base = $null
  $previous = @{}
  foreach ($e in $sizes) {
    $before = $previous[$e.toolset]
    if ($e.toolset -eq $toolset) {
      $base = [pscustomobject]@{ exe = [long]$e.exe; download = [long]$e.download; entry = $e; since = @() }
    } elseif ($base -and $before) {
      $base.exe += [long]$e.exe - [long]$before.exe
      $base.download += [long]$e.download - [long]$before.download
      $base.since += $e
    }
    $previous[$e.toolset] = $e
  }
  $base
}

function Write-Ledger([string]$path) {
  # One entry per line, so a new entry is one line in the diff.
  $line = { param($o) (ConvertTo-Json $o -Compress -Depth 8) -replace '\\u0027', "'" -replace '\\u0026', '&' -replace '\\u003c', '<' -replace '\\u003e', '>' }
  $list = { param($items) if ($items.Count) { "[`n    " + (($items | ForEach-Object { & $line $_ }) -join ",`n    ") + "`n  ]" } else { "[]" } }
  $text = "{`n  `"format`": `"featherweight-budget-ledger`",`n  `"version`": 1,`n" +
          "  `"about`": " + (& $line $ledger.about) + ",`n" +
          "  `"sizes`": " + (& $list $sizes) + ",`n" +
          "  `"timings`": " + (& $list $timingEntries) + ",`n" +
          "  `"typing`": " + (& $list $typingEntries) + "`n}`n"
  [IO.File]::WriteAllText($path, $text, (New-Object Text.UTF8Encoding $false))
}

$today = (Get-Date).ToString("yyyy-MM-dd")

# ---------------------------------------------------------------- Size
$recordedElsewhere = $Record -and $Toolset
if ($recordedElsewhere) {
  if (-not $ExeBytes -or -not $DownloadBytes) { throw "-Toolset needs -ExeBytes and -DownloadBytes." }
  $exe = $null
  $tool = [pscustomobject]@{ name = $Toolset; linker = $null }
  $exeSize = $ExeBytes; $downloadSize = $DownloadBytes; $version = $null; $commit = $null
} else {
  if ($PSVersionTable.PSVersion.Major -ne 5) {
    throw "Run it in Windows PowerShell (powershell.exe), as release.ps1 runs, so the zip is compressed the same way."
  }
  $release = [IO.Path]::Combine($root, $Build, "Release")
  $exe = Join-Path $release "Featherweight.exe"
  if (-not (Test-Path $exe)) { throw "$exe doesn't exist. Build the shipped configuration first (tests\budgets\README.md)." }
  $cache = Join-Path $Build "CMakeCache.txt"
  if ((Test-Path $cache) -and (Select-String -Path $cache -Pattern '^FW_TEST_SCREENS:BOOL=(ON|1|TRUE)$' -Quiet)) {
    throw "$Build is a test build (FW_TEST_SCREENS=ON). Measure the shipped configuration: cmake -S . -B build-release -DFW_TEST_SCREENS=OFF"
  }
  $tool = Get-Toolset $exe
  $exeSize = (Get-Item $exe).Length
  $downloadSize = Measure-Download $release
  $version = (Get-Item $exe).VersionInfo.ProductVersion
  # The commit the build's source folder is at.
  $source = if (Test-Path $cache) { Select-String -Path $cache -Pattern '^CMAKE_HOME_DIRECTORY:INTERNAL=(.+)$' | ForEach-Object { $_.Matches[0].Groups[1].Value } }
  $commit = if ($source) { git -C $source rev-parse --short HEAD 2>$null } else { $null }
}
$report = [ordered]@{ format = "featherweight-budgets"; version = 1; date = $today; productVersion = $version; commit = $commit }
$report.size = [ordered]@{ toolset = $tool.name; linker = $tool.linker; exe = $exeSize; download = $downloadSize }

$base = Get-SizeBaseline $tool.name
""
"Size, {0} build{1}" -f $tool.name, $(if ($version) { " of $version ($commit)" } else { "" })
"{0,-20} {1,14} {2,14} {3,24} {4,10}" -f "", "now", "baseline", "change", "budget"
foreach ($item in @(@{ name = "Featherweight.exe"; now = $exeSize; key = "exe"; budget = $exeBudget },
                    @{ name = "Download (zip)"; now = $downloadSize; key = "download"; budget = $downloadBudget })) {
  $then = if ($base) { $base.($item.key) } else { $null }
  $change = if ($base) { Format-Change ($item.now / 1KB) ($then / 1KB) "KB" } else { "" }
  "{0,-20} {1,14} {2,14} {3,24} {4,10}" -f $item.name, (Format-Bytes $item.now), $(if ($base) { Format-Bytes $then } else { "-" }),
    $change, ("< {0} MB" -f ($item.budget / 1MB))
  if ($item.now -ge $item.budget) { $failures.Add("$($item.name) is over its budget of $($item.budget / 1MB) MB.") }
  if ($base -and $item.now - $then -gt $sizeStep) {
    $failures.Add((("{0} grew {1:N1} KB over the baseline, more than {2} KB, and the ledger doesn't say why. " +
                    "If it's worth it, record the reason: scripts\budgets.ps1 -Record `"why`"") -f $item.name, (($item.now - $then) / 1KB), ($sizeStep / 1KB)))
  }
}
if ($base) {
  "Baseline: {0} ({1}, {2}): {3}" -f $base.entry.version, $base.entry.date, $base.entry.commit, $base.entry.reason
  foreach ($e in $base.since) { "  plus, accepted with {0} on {1}: {2}" -f $e.toolset, $e.date, $e.reason }
} elseif (-not $Record) {
  "No baseline for {0} yet. To make this build's sizes the baseline:" -f $tool.name
  "  powershell -ExecutionPolicy Bypass -File scripts\budgets.ps1 -Record `"{0} baseline`" -Toolset {0} -ExeBytes {1} -DownloadBytes {2}" -f $tool.name, $exeSize, $downloadSize
}

# ---------------------------------------------------------------- Timings
$bench = $null
if (($Timings -or $TrackTimings) -and $exe) {
  if (-not $Fwtool) { $Fwtool = Join-Path $Build "tools\fwtool\Release\fwtool.exe" }
  if (-not (Test-Path $Fwtool)) { throw "$Fwtool doesn't exist (-Fwtool names another)." }
  $benchJson = Join-Path ([IO.Path]::GetTempPath()) "fw-budgets-$PID.json"
  $longDir = Join-Path ([IO.Path]::GetTempPath()) "fw-budgets-docs-$PID"
  $files = @($fixedFiles)
  if (Get-Command python -ErrorAction SilentlyContinue) {
    New-Item -ItemType Directory -Force $longDir | Out-Null
    & python tests\office\make_long_docs.py $longDir 300 | Out-Null
    if (Test-Path (Join-Path $longDir $longDoc)) { $files += (Join-Path $longDir $longDoc) }
  }
  ""
  & $Fwtool bench-app --runs $Runs --json $benchJson $exe @files | Out-Host
  $benchFailed = $LASTEXITCODE -ne 0
  # Typing in the 300-page document.
  $typing = $null; $typingTried = $false
  $longPath = Join-Path $longDir $longDoc
  if (Test-Path $longPath) {
    $typingTried = $true
    $typingJson = Join-Path ([IO.Path]::GetTempPath()) "fw-budgets-typing-$PID.json"
    ""
    & $Fwtool bench --typing --runs $Runs --json $typingJson $longPath | Out-Host
    $typingFile = if (Test-Path $typingJson) { (Get-Content $typingJson -Raw | ConvertFrom-Json).files | Select-Object -First 1 } else { $null }
    if ($LASTEXITCODE -eq 0 -and $typingFile) {
      $typing = [ordered]@{ document = $longDoc; pages = $typingFile.pages; typicalMs = $typingFile.typicalMs
                            worstMs = $typingFile.worstMs; places = @($typingFile.places) }
    }
    Remove-Item $typingJson -ErrorAction SilentlyContinue
  }
  Remove-Item $longDir -Recurse -ErrorAction SilentlyContinue
  $bench = if (Test-Path $benchJson) { Get-Content $benchJson -Raw | ConvertFrom-Json } else { $null }
  Remove-Item $benchJson -ErrorAction SilentlyContinue
  $timingFailures = New-Object Collections.Generic.List[string]
  if ($benchFailed -or -not $bench) { $timingFailures.Add("fwtool bench-app didn't measure every scenario.") }
  $machine = $env:COMPUTERNAME
  $then = $timingEntries | Where-Object { $_.machine -eq $machine } | Select-Object -Last 1
  ""
  if ($then) {
    "Timings on {0} against its baseline: {1} ({2}, {3}): {4}" -f $machine, $then.version, $then.date, $then.commit, $then.reason
  } elseif (-not $Record) {
    "No timing baseline for {0} yet: record one with -Timings -Record `"why`"." -f $machine
  }
  "{0,-20} {1,-8} {2,10} {3,10} {4,22}" -f "", "", "now", "baseline", "change"
  foreach ($s in @($bench.scenarios)) {
    if ($s.error) { continue }
    $old = if ($then) { @($then.scenarios) | Where-Object { $_.name -eq $s.name } | Select-Object -First 1 } else { $null }
    foreach ($m in @(@{ key = "windowMs"; label = "window"; unit = "ms"; floor = $floorMs },
                     @{ key = "readyMs"; label = "ready"; unit = "ms"; floor = $floorMs },
                     @{ key = "memoryMB"; label = "memory"; unit = "MB"; floor = $floorMB })) {
      $now = [double]$s.($m.key)
      $was = if ($old) { [double]$old.($m.key) } else { $null }
      "{0,-20} {1,-8} {2,10} {3,10} {4,22}" -f $s.name, $m.label, ("{0:N1}" -f $now), $(if ($old) { "{0:N1}" -f $was } else { "-" }),
        $(if ($old) { Format-Change $now $was $m.unit } else { "" })
      if ($old -and $now -gt $was * (1 + $slower) -and $now - $was -gt $m.floor) {
        $timingFailures.Add(("{0}: {1} went from {2:N1} to {3:N1} {4}, more than {5} % worse, and the ledger doesn't say why." -f
                             $s.name, $m.label, $was, $now, $m.unit, ($slower * 100)))
      }
    }
    if ($null -eq $s.idleCpuMs) {
      $timingFailures.Add("$($s.name): idle wasn't measured: the mouse or keyboard was used each time. Run it again without touching them.")
    } elseif ([double]$s.idleCpuMs -gt $idleCpuLimit -or [double]$s.idleWakeups -gt $idleWakeupLimit) {
      $timingFailures.Add(("{0}: not idle once settled: {1:N1} ms of CPU and {2} wakeups in {3} s." -f $s.name, [double]$s.idleCpuMs, $s.idleWakeups, $bench.idleSeconds))
    }
    if ($s.name -eq "launch") {
      if ([double]$s.memoryMB -ge $launchMemoryBudget) { $timingFailures.Add("Memory after launch is $($s.memoryMB) MB, over its budget of $launchMemoryBudget MB.") }
      if ([double]$s.readyMs -ge $launchBudget) { $timingFailures.Add("Launch took $($s.readyMs) ms, over its budget of $launchBudget ms.") }
    }
    if ($s.name -eq $longDoc -and [double]$s.readyMs -ge $longDocBudget) {
      $timingFailures.Add("Paginating a 300-page document took $($s.readyMs) ms, over its budget of $longDocBudget ms.")
    }
  }
  if ($typingTried) {
    if (-not $typing) {
      $timingFailures.Add("fwtool bench --typing didn't measure typing in the 300-page document.")
    } else {
      $typingThen = $typingEntries | Where-Object { $_.machine -eq $machine } | Select-Object -Last 1
      foreach ($m in @(@{ key = "typicalMs"; label = "typing"; budget = $typingBudget }, @{ key = "worstMs"; label = "worst key"; budget = $typingWorstBudget })) {
        $now = [double]$typing.($m.key)
        $was = if ($typingThen) { [double]$typingThen.($m.key) } else { $null }
        "{0,-20} {1,-8} {2,10} {3,10} {4,22}" -f $longDoc, $m.label, ("{0:N2}" -f $now), $(if ($typingThen) { "{0:N2}" -f $was } else { "-" }),
          $(if ($typingThen) { Format-Change $now $was "ms" } else { "" })
        if ($now -ge $m.budget) { $timingFailures.Add(("Typing in a 300-page document: the {0} is {1:N1} ms, over its budget of {2} ms." -f $m.label, $now, $m.budget)) }
        if ($typingThen -and $now -gt $was * (1 + $slower) -and $now - $was -gt $typingFloorMs) {
          $timingFailures.Add(("Typing in a 300-page document: the {0} went from {1:N2} to {2:N2} ms, more than {3} % worse, and the ledger doesn't say why." -f
                               $m.label, $was, $now, ($slower * 100)))
        }
      }
    }
  }
  $report.timings = [ordered]@{ machine = $machine; runs = $Runs; bench = $bench; typing = $typing; failures = @($timingFailures) }
  if ($TrackTimings) {
    foreach ($f in $timingFailures) { Write-Host "Tracked, not gated: $f" -ForegroundColor Yellow }
  } else {
    foreach ($f in $timingFailures) { $failures.Add($f) }
  }
}

# ---------------------------------------------------------------- Record
if ($Record) {
  $entry = [ordered]@{ date = $today; version = $version; commit = $commit; toolset = $tool.name; exe = $exeSize; download = $downloadSize; reason = $Record }
  if ($recordedElsewhere) { $entry.Remove("version"); $entry.Remove("commit") }
  $sizes += [pscustomobject]$entry
  if ($bench -and -not $benchFailed) {
    $timingEntries += [pscustomobject][ordered]@{
      date = $today; machine = $env:COMPUTERNAME; version = $version; commit = $commit; toolset = $tool.name
      runs = $Runs; scenarios = @($bench.scenarios); reason = $Record
    }
  }
  if ($typing) {
    $typingEntries += [pscustomobject][ordered]@{
      date = $today; machine = $env:COMPUTERNAME; version = $version; commit = $commit; toolset = $tool.name; runs = $Runs
      document = $typing.document; pages = $typing.pages; typicalMs = $typing.typicalMs; worstMs = $typing.worstMs
      places = $typing.places; reason = $Record
    }
  }
  Write-Ledger $ledgerPath
  ""
  "Recorded in tests\budgets\ledger.json: the new baseline. Commit it with the change."
  $failures.Clear()
  if ($bench -and $benchFailed) { $failures.Add("fwtool bench-app didn't measure every scenario, so no timings were recorded.") }
}

$report.failures = @($failures)
if ($Json) { [IO.File]::WriteAllText([IO.Path]::Combine($root, $Json), (ConvertTo-Json $report -Depth 10), (New-Object Text.UTF8Encoding $false)) }
""
if ($failures.Count) {
  foreach ($f in $failures) { Write-Host "FAIL: $f" -ForegroundColor Red }
  exit 1
}
Write-Host "Within the budgets and the ledger." -ForegroundColor Green
