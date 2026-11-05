<#
.SYNOPSIS
  The editing torture test over the corpus (plan section 10; D2's exit gate: every corpus file,
  after scripted random edits and a save, has no new validator errors, passes fwtool check, and
  opens in Word without a repair prompt).
  1. fwtool torture: seeded random edits through the editor, a save, fwtool check, and the saved
     file read back into the model. Failures are shrunk to reproducers (<out>\repro).
  2. The Open XML SDK validator (tools\validate): each saved copy against its original.
  3. Word (scripts\word-open-check.ps1): each saved copy must open without a repair prompt or an
     error. When one doesn't, its original is opened too: if Word complains about the original as
     well, that isn't the editor's doing, and the summary says so.
  The summary goes to <out>\summary.md and summary.json. The exit code is 1 when anything failed.

  -Ci runs the CI subset: the public corpus only, fewer edits, no Word. Without -Paths the full run
  takes tests\office's Word documents and, when FW_CORPUS is set, the private corpus.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\torture.ps1
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\torture.ps1 -Ci
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\torture.ps1 -Paths $env:FW_CORPUS\docx\legal -Seed 7 -Rounds 10
#>
param(
  [string[]]$Paths,
  [uint64]$Seed = 1,
  [int]$Edits = 40,
  [int]$Rounds = 3,
  [string]$Out = "build\torture",
  [string]$Fwtool = "build\tools\fwtool\Release\fwtool.exe",
  [switch]$Ci,
  [switch]$NoWord
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
if ($Ci) {
  $NoWord = $true
  if (-not $PSBoundParameters.ContainsKey("Edits")) { $Edits = 30 }
  if (-not $PSBoundParameters.ContainsKey("Rounds")) { $Rounds = 2 }
}
# powershell -File passes "-Paths a,b" as one string.
$Paths = @($Paths | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
if (-not $Paths) {
  $Paths = @("tests\office\generated", "tests\office\edge", "tests\office\layout")
  if (-not $Ci -and $env:FW_CORPUS) { $Paths += Join-Path $env:FW_CORPUS "docx" }
}
if (-not [IO.Path]::IsPathRooted($Out)) { $Out = Join-Path $root $Out }
if (-not [IO.Path]::IsPathRooted($Fwtool)) { $Fwtool = Join-Path $root $Fwtool }
if (-not (Test-Path $Fwtool)) { throw "fwtool.exe not found at $Fwtool. Build it, or pass -Fwtool." }
# This run's output only: earlier saved copies and reproducers would mix with this run's.
foreach ($old in "saved", "repro") { if (Test-Path (Join-Path $Out $old)) { Remove-Item -Recurse -Force (Join-Path $Out $old) } }
New-Item -ItemType Directory -Force $Out | Out-Null

function Read-Json([string]$path) { if (Test-Path $path) { Get-Content $path -Raw -Encoding UTF8 | ConvertFrom-Json } }

# 1. Edits, save, check, read back.
"== fwtool torture: seed $Seed, $Rounds rounds of $Edits edits"
& $Fwtool torture --seed $Seed --edits $Edits --rounds $Rounds --out $Out --json (Join-Path $Out "torture.json") @Paths |
  Tee-Object -FilePath (Join-Path $Out "torture.log")
$torture = Read-Json (Join-Path $Out "torture.json")
if (-not $torture) { throw "fwtool torture wrote no results." }

# 2. The validator: no new errors in any saved copy.
"== Validator (Open XML SDK): saved copies against their originals"
$validator = Join-Path $root "build\validate\fwvalidate.exe"
& dotnet build (Join-Path $root "tools\validate\Validate.csproj") -c Release -o (Join-Path $root "build\validate") --nologo -v q | Out-Null
if ($LASTEXITCODE) { throw "building tools\validate failed" }
$pairsFile = Join-Path $Out "pairs.tsv"
$newErrors = @{}  # saved copy -> new validator errors
if ((Test-Path $pairsFile) -and (Get-Item $pairsFile).Length) {
  & $validator --pairs $pairsFile --json (Join-Path $Out "validate.json") | Where-Object { $_ -match '^\[FAIL\]|^\s{4}|files,' }
  foreach ($f in (Read-Json (Join-Path $Out "validate.json")).files) { $newErrors[[IO.Path]::GetFullPath($f.saved)] = @($f.newErrors | Where-Object { $_ }) }
}

# 3. Word: each saved copy opens without a prompt; the originals of those that don't, too.
$word = @{}      # saved copy -> Word's result
$baseline = @{}  # original -> Word's result
$pairs = @{}     # saved copy -> original
foreach ($line in @(Get-Content $pairsFile -ErrorAction SilentlyContinue)) {
  $cols = $line -split "`t"
  if ($cols.Count -eq 2) { $pairs[[IO.Path]::GetFullPath($cols[1])] = [IO.Path]::GetFullPath($cols[0]) }
}
if (-not $NoWord -and $pairs.Count) {
  "== Word: does each saved copy open without a repair prompt?"
  $check = Join-Path $PSScriptRoot "word-open-check.ps1"
  & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $check (Join-Path $Out "saved") -Json (Join-Path $Out "word.json")
  foreach ($f in (Read-Json (Join-Path $Out "word.json")).files) { $word[[IO.Path]::GetFullPath($f.file)] = $f }
  $originals = @($word.Values | Where-Object { $_.status -notin "ok", "notice" } | ForEach-Object { $pairs[[IO.Path]::GetFullPath($_.file)] } | Where-Object { $_ })
  if ($originals.Count) {
    "== Word: the originals of the copies it complained about"
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $check @originals -Json (Join-Path $Out "word-originals.json")
    foreach ($f in (Read-Json (Join-Path $Out "word-originals.json")).files) { $baseline[[IO.Path]::GetFullPath($f.file)] = $f }
  }
}

# 4. The summary.
$rows = @()
foreach ($f in $torture.files) {
  $saved = if ($f.saved) { [IO.Path]::GetFullPath($f.saved) } else { $null }
  $original = [IO.Path]::GetFullPath($f.file)
  $w = if ($saved) { $word[$saved] } else { $null }
  $b = $baseline[$original]
  $wordStatus = if (-not $w) { $null } elseif ($w.status -in "ok", "notice") { $w.status } elseif ($b -and $b.status -notin "ok", "notice") { "original too" } else { $w.status }
  $rows += [pscustomobject][ordered]@{
    file = $f.file
    skipped = $f.skipped
    done = $f.done
    refused = $f.refused
    failures = @($f.failures | Where-Object { $_ })
    newValidatorErrors = @(if ($saved -and $newErrors.ContainsKey($saved)) { $newErrors[$saved] })  # @(): one error stays a list
    word = $wordStatus
    wordDetail = if ($w -and $w.status -ne "ok") { (@($w.dialogs | ForEach-Object { $_.text }) + @($w.error) | Where-Object { $_ }) -join " / " } else { $null }
  }
}
$wordPasses = "ok", "notice", "original too"
$failed = @($rows | Where-Object { $_.failures.Count -or $_.newValidatorErrors.Count -or ($_.word -and $_.word -notin $wordPasses) })
$md = @("# Editing torture test", "",
  "Seed $Seed, $Rounds rounds of $Edits edits per document. $(Get-Date -Format 'yyyy-MM-dd HH:mm').", "",
  "| | Documents |", "|---|---|",
  "| Checked | $($rows.Count) |",
  "| Not read by the model (skipped) | $(@($rows | Where-Object skipped).Count) |",
  "| Edits done / refused | $(($rows | Measure-Object done -Sum).Sum) / $(($rows | Measure-Object refused -Sum).Sum) |",
  "| fwtool torture failures (save, check, read back) | $(@($rows | Where-Object { $_.failures.Count }).Count) |",
  "| New validator errors | $(@($rows | Where-Object { $_.newValidatorErrors.Count }).Count) |",
  "| Word: repair prompt or error | $(if ($NoWord) { 'not run' } else { @($rows | Where-Object { $_.word -and $_.word -notin $wordPasses }).Count }) |",
  "| Word: the original complains too | $(if ($NoWord) { 'not run' } else { @($rows | Where-Object { $_.word -eq 'original too' }).Count }) |",
  "| Word: only a notice about this PC's Office | $(if ($NoWord) { 'not run' } else { @($rows | Where-Object { $_.word -eq 'notice' }).Count }) |", "")
if ($failed.Count) {
  $md += "## Failures", ""
  foreach ($r in $failed) {
    $md += "### $($r.file)", ""
    foreach ($x in $r.failures) { $md += "- round $($x.round): $($x.category): $($x.message) ($($x.edits) edits: ``$($x.reproducer)``)" }
    foreach ($x in $r.newValidatorErrors | Select-Object -First 5) { $md += "- validator: $($x.Part) $($x.Path): $($x.Description) ($($x.Id))" }
    if ($r.word -and $r.word -notin $wordPasses) { $md += "- Word: $($r.word): $($r.wordDetail)" }
    $md += ""
  }
}
[IO.File]::WriteAllText((Join-Path $Out "summary.md"), ($md -join "`n") + "`n", [Text.UTF8Encoding]::new($false))
[IO.File]::WriteAllText((Join-Path $Out "summary.json"), ([ordered]@{ seed = $Seed; edits = $Edits; rounds = $Rounds; word = -not $NoWord; files = $rows } | ConvertTo-Json -Depth 8), [Text.UTF8Encoding]::new($false))
""
$md[4..13] -join "`n"
"Summary: $(Join-Path $Out 'summary.md')"
exit $(if ($failed.Count) { 1 } else { 0 })
