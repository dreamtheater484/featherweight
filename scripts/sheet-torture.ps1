<#
.SYNOPSIS
  The workbook torture test (S2's "never damage a file" gate, docs\s2-plan.md): every corpus
  workbook, after seeded random edits and a save, has no new validator errors, passes fwtool check,
  and opens in Excel without a repair prompt.
  1. fwtool torture --workbooks: seeded random edits through the edit API, a save, fwtool check and
     the model's checks with no new errors, the saved file read back as the edited workbook, and
     every part the save didn't write kept byte for byte. Failures are shrunk to reproducers
     (<out>\repro).
  2. The Open XML SDK validator (tools\validate): each saved copy against its original.
  3. Excel (scripts\excel-open-check.ps1): each saved copy must open cleanly. When one doesn't, its
     original is checked too: a copy that is no worse than its original (Excel repairs or refuses
     the original the same way) isn't the editor's doing, and the summary says so.
  The summary goes to <out>\summary.md and summary.json. The exit code is 1 when anything failed.

  Without -Paths: tests\office's workbooks (sheet, excel, generated, edge) and, with -Corpus (or
  FW_CORPUS), the private corpus's xlsx folder. -Ci: the public workbooks only, fewer edits, no Excel.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\sheet-torture.ps1 -Corpus D:\Documents\Codex\FeatherweightCorpus
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\sheet-torture.ps1 -Ci
#>
param(
  [string[]]$Paths,
  [string]$Corpus = $env:FW_CORPUS,
  [uint32]$Seed = 1,
  [int]$Edits = 40,
  [int]$Rounds = 3,
  [string]$Out = "build\sheet-torture",
  [string]$Fwtool = "build\tools\fwtool\Release\fwtool.exe",
  [switch]$Ci,
  [switch]$NoExcel
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
if ($Ci) {
  $NoExcel = $true
  if (-not $PSBoundParameters.ContainsKey("Edits")) { $Edits = 30 }
  if (-not $PSBoundParameters.ContainsKey("Rounds")) { $Rounds = 2 }
}
$Paths = @($Paths | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
if (-not $Paths) {
  $Paths = @("tests\office\sheet", "tests\office\excel", "tests\office\generated", "tests\office\edge")
  if (-not $Ci -and $Corpus -and (Test-Path (Join-Path $Corpus "xlsx"))) { $Paths += Join-Path $Corpus "xlsx" }
}
if (-not [IO.Path]::IsPathRooted($Out)) { $Out = Join-Path $root $Out }
if (-not [IO.Path]::IsPathRooted($Fwtool)) { $Fwtool = Join-Path $root $Fwtool }
if (-not (Test-Path $Fwtool)) { throw "fwtool.exe not found at $Fwtool. Build it, or pass -Fwtool." }
# This run's output only.
foreach ($old in "saved", "repro") { if (Test-Path (Join-Path $Out $old)) { Remove-Item -Recurse -Force (Join-Path $Out $old) } }
New-Item -ItemType Directory -Force $Out | Out-Null

function Read-Json([string]$path) { if (Test-Path $path) { Get-Content $path -Raw -Encoding UTF8 | ConvertFrom-Json } }

# 1. Edits, save, check, read back.
"== fwtool torture --workbooks: seed $Seed, $Rounds rounds of $Edits edits"
& $Fwtool torture --workbooks --seed $Seed --edits $Edits --rounds $Rounds --out $Out --json (Join-Path $Out "torture.json") @Paths |
  Tee-Object -FilePath (Join-Path $Out "torture.log")
$torture = Read-Json (Join-Path $Out "torture.json")
if (-not $torture) { throw "fwtool torture wrote no results." }
$pairsFile = Join-Path $Out "pairs.tsv"
$pairs = @{}  # saved copy -> original
foreach ($line in @(Get-Content $pairsFile -Encoding UTF8 -ErrorAction SilentlyContinue)) {
  $cols = $line -split "`t"
  if ($cols.Count -eq 2) { $pairs[[IO.Path]::GetFullPath($cols[1])] = [IO.Path]::GetFullPath($cols[0]) }
}

# 2. The validator: no new errors in any saved copy.
"== Validator (Open XML SDK): saved copies against their originals"
$validator = Join-Path $root "build\validate\fwvalidate.exe"
& dotnet build (Join-Path $root "tools\validate\Validate.csproj") -c Release -o (Join-Path $root "build\validate") --nologo -v q | Out-Null
if ($LASTEXITCODE) { throw "building tools\validate failed" }
$newErrors = @{}  # saved copy -> new validator errors
if ($pairs.Count) {
  & $validator --pairs $pairsFile --json (Join-Path $Out "validate.json") | Where-Object { $_ -match '^\[FAIL\]|^\s{4}|files,' }
  foreach ($f in (Read-Json (Join-Path $Out "validate.json")).files) { $newErrors[[IO.Path]::GetFullPath($f.saved)] = @($f.newErrors | Where-Object { $_ }) }
}

# 3. Excel: each saved copy opens cleanly; the originals of those that don't, too.
$excel = @{}     # saved copy -> Excel's result
$baseline = @{}  # original -> Excel's result
if (-not $NoExcel -and $pairs.Count) {
  "== Excel: does each saved copy open without a repair?"
  $check = Join-Path $PSScriptRoot "excel-open-check.ps1"
  & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $check (Join-Path $Out "saved") -Json (Join-Path $Out "excel.json")
  foreach ($f in (Read-Json (Join-Path $Out "excel.json")).files) { $excel[[IO.Path]::GetFullPath($f.file)] = $f }
  $originals = @($excel.Values | Where-Object { $_.status -notin "ok", "unchecked" } | ForEach-Object { $pairs[[IO.Path]::GetFullPath($_.file)] } | Where-Object { $_ })
  if ($originals.Count) {
    "== Excel: the originals of the copies it didn't open cleanly"
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $check @originals -Json (Join-Path $Out "excel-originals.json")
    foreach ($f in (Read-Json (Join-Path $Out "excel-originals.json")).files) { $baseline[[IO.Path]::GetFullPath($f.file)] = $f }
  }
}

# 4. The summary.
$rows = @()
foreach ($f in $torture.files) {
  $saved = if ($f.saved) { [IO.Path]::GetFullPath($f.saved) } else { $null }
  $original = [IO.Path]::GetFullPath($f.file)
  $x = if ($saved) { $excel[$saved] } else { $null }
  $b = $baseline[$original]
  # No worse than the original: Excel treats the original the same way (both repaired, with the same
  # repairs, or both refused).
  $same = $x -and $b -and $b.status -eq $x.status -and (@($b.repairs) -join "|") -eq (@($x.repairs) -join "|")
  $excelStatus = if (-not $x) { $null } elseif ($x.status -in "ok", "unchecked") { $x.status } elseif ($same) { "original too" } else { $x.status }
  $rows += [pscustomobject][ordered]@{
    file = $f.file
    skipped = $f.skipped
    done = $f.done
    refused = $f.refused
    failures = @($f.failures | Where-Object { $_ })
    newValidatorErrors = @(if ($saved -and $newErrors.ContainsKey($saved)) { $newErrors[$saved] })
    excel = $excelStatus
    excelDetail = if ($x -and $x.status -ne "ok") {
      (@($x.repairs) + @($x.dialogs | ForEach-Object { $_.text }) + @($x.error) | Where-Object { $_ }) -join " / "
    } else { $null }
  }
}
$excelPasses = "ok", "original too", "unchecked"  # unchecked has its own row: Excel's automation, not the file
$failed = @($rows | Where-Object { $_.failures.Count -or $_.newValidatorErrors.Count -or ($_.excel -and $_.excel -notin $excelPasses) })
$through = Select-String -Path (Join-Path $Out "torture.log") -Pattern 'edits through: (.*)\)$' | Select-Object -Last 1
$through = if ($through) { $through.Matches[0].Groups[1].Value } else { "?" }
$md = @("# Workbook torture test", "",
  "Seed $Seed, $Rounds rounds of $Edits edits per workbook. $(Get-Date -Format 'yyyy-MM-dd HH:mm'). Edits through: $through.", "",
  "| | Workbooks |", "|---|---|",
  "| Checked | $($rows.Count) |",
  "| Not read by the model (skipped) | $(@($rows | Where-Object skipped).Count) |",
  "| Edits done / refused | $(($rows | Measure-Object done -Sum).Sum) / $(($rows | Measure-Object refused -Sum).Sum) |",
  "| fwtool torture failures (save, check, read back, untouched parts) | $(@($rows | Where-Object { $_.failures.Count }).Count) |",
  "| New validator errors | $(@($rows | Where-Object { $_.newValidatorErrors.Count }).Count) |",
  "| Excel: repaired or refused | $(if ($NoExcel) { 'not run' } else { @($rows | Where-Object { $_.excel -and $_.excel -notin $excelPasses }).Count }) |",
  "| Excel: the original is repaired or refused the same way | $(if ($NoExcel) { 'not run' } else { @($rows | Where-Object { $_.excel -eq 'original too' }).Count }) |",
  "| Excel: couldn't check (its automation failed three times; run again) | $(if ($NoExcel) { 'not run' } else { @($rows | Where-Object { $_.excel -eq 'unchecked' }).Count }) |", "")
if ($failed.Count) {
  $md += "## Failures", ""
  foreach ($r in $failed) {
    $md += "### $($r.file)", ""
    foreach ($x in $r.failures) { $md += "- round $($x.round): $($x.category): $($x.message) ($($x.edits) edits: ``$($x.reproducer)``)" }
    foreach ($x in $r.newValidatorErrors | Select-Object -First 5) { $md += "- validator: $($x.Part) $($x.Path): $($x.Description) ($($x.Id))" }
    if ($r.excel -and $r.excel -notin $excelPasses) { $md += "- Excel: $($r.excel): $($r.excelDetail)" }
    $md += ""
  }
}
[IO.File]::WriteAllText((Join-Path $Out "summary.md"), ($md -join "`n") + "`n", [Text.UTF8Encoding]::new($false))
[IO.File]::WriteAllText((Join-Path $Out "summary.json"), ([ordered]@{ seed = $Seed; edits = $Edits; rounds = $Rounds; excel = -not $NoExcel; files = $rows } | ConvertTo-Json -Depth 8), [Text.UTF8Encoding]::new($false))
""
$md[4..13] -join "`n"
"Summary: $(Join-Path $Out 'summary.md')"
$unchecked = @($rows | Where-Object { $_.excel -eq "unchecked" })
if ($unchecked.Count) { "Excel couldn't check $($unchecked.Count) saved copies: run again." }
exit $(if ($failed.Count -or $unchecked.Count) { 1 } else { 0 })
