<#
.SYNOPSIS
  The editing torture test over the corpus (plan section 10; D2's exit gate: every corpus file,
  after scripted random edits and a save, has no new validator errors, passes fwtool check, and
  opens in Word without a repair prompt).
  1. fwtool torture: seeded random edits through the editor, a save, fwtool check, and the saved
     file read back into the model. Failures are shrunk to reproducers (<out>\repro). Kept
     reproducers (tests\torture) are replayed too, and their saved copies checked with the rest.
  2. The Open XML SDK validator (tools\validate): each saved copy against its original.
  3. Word (scripts\word-open-check.ps1): each saved copy must open without a repair prompt or an
     error. When one doesn't, its original is opened too: if Word complains about the original as
     well, that isn't the editor's doing, and the summary says so.
  The summary goes to <out>\summary.md and summary.json. The exit code is 1 when anything failed.

  D5: the files are opened as the app opens them. A .doc or an .odt is imported, a template is
  made a new document, and a password-protected file is decrypted with its test password. Saving
  writes a .docx; a protected file is saved encrypted with its password. The validator and Word
  compare the saved copy with the .docx the app opened (<out>\opened). Word also opens each
  protected copy as the app writes it (<out>\saved-protected\<password>) with that password.
  Chart parts must be kept byte for byte, and equations as they were. Typing includes
  right-to-left and East Asian text, and edits aim at charts and equations too.

  -Ci runs the CI subset: the public corpus only, fewer edits, no Word. Without -Paths the full run
  takes tests\office's Word documents (generated, edge, layout, and torture: a table of contents and
  other fields made by tests\office\make_torture_docs.py) and, when FW_CORPUS is set, the private corpus.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\torture.ps1
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\torture.ps1 -Ci
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\torture.ps1 -Paths $env:FW_CORPUS\docx\legal -Seed 7 -Rounds 10
#>
param(
  [string[]]$Paths,
  [uint32]$Seed = 1,
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
  $Paths = @("tests\office\generated", "tests\office\edge", "tests\office\layout", "tests\office\torture",
             "tests\office\script", "tests\office\doc", "tests\office\odt", "tests\office\dotx", "tests\office\protected")
  if (-not $Ci -and $env:FW_CORPUS) {
    foreach ($sub in "docx", "doc", "odt", "dotx", "protected") {
      $dir = Join-Path $env:FW_CORPUS $sub
      if (Test-Path $dir) { $Paths += $dir }
    }
  }
}
if (-not [IO.Path]::IsPathRooted($Out)) { $Out = Join-Path $root $Out }
if (-not [IO.Path]::IsPathRooted($Fwtool)) { $Fwtool = Join-Path $root $Fwtool }
if (-not (Test-Path $Fwtool)) { throw "fwtool.exe not found at $Fwtool. Build it, or pass -Fwtool." }
# This run's output only: earlier saved copies and reproducers would mix with this run's.
foreach ($old in "saved", "repro", "opened", "saved-protected") { if (Test-Path (Join-Path $Out $old)) { Remove-Item -Recurse -Force (Join-Path $Out $old) } }
New-Item -ItemType Directory -Force $Out | Out-Null

function Read-Json([string]$path) { if (Test-Path $path) { Get-Content $path -Raw -Encoding UTF8 | ConvertFrom-Json } }

# 1. Edits, save, check, read back.
"== fwtool torture: seed $Seed, $Rounds rounds of $Edits edits"
& $Fwtool torture --seed $Seed --edits $Edits --rounds $Rounds --out $Out --json (Join-Path $Out "torture.json") @Paths |
  Tee-Object -FilePath (Join-Path $Out "torture.log")
$torture = Read-Json (Join-Path $Out "torture.json")
if (-not $torture) { throw "fwtool torture wrote no results." }

# 1b. Kept reproducers (tests\torture\*.txt): edits once suspected, replayed in every run; their saved
#     copies go to the validator and Word with the rest. Each names its file relative to the private
#     corpus (FW_CORPUS) or to the checkout; one whose file isn't on this PC is left out.
$pairsFile = Join-Path $Out "pairs.tsv"
foreach ($repro in @(Get-ChildItem (Join-Path $root "tests\torture") -Filter *.txt -ErrorAction SilentlyContinue)) {
  $recorded = @(Get-Content $repro.FullName -Encoding UTF8 | Where-Object { $_ -match '^file: ' } | ForEach-Object { $_.Substring(6) })[0]
  $base = @($env:FW_CORPUS, $root) | Where-Object { $_ -and $recorded -and (Test-Path (Join-Path $_ $recorded)) } | Select-Object -First 1
  if (-not $base) { "== Kept reproducer $($repro.Name): its file isn't on this PC, left out"; continue }
  "== Kept reproducer $($repro.Name)"
  $original = [IO.Path]::GetFullPath((Join-Path $base $recorded))
  Push-Location $base  # the replay finds the relative path from here
  $log = @(& $Fwtool torture --replay $repro.FullName --out (Join-Path $Out "saved"))
  $code = $LASTEXITCODE
  Pop-Location
  $log
  $saved = Join-Path $Out "saved\$($repro.BaseName)-replay.docx"
  $counts = [regex]::Match(($log -join "`n"), '\((\d+) done, (\d+) refused\)')
  $failures = @(if ($code) {
    [pscustomobject]@{ round = 0; category = "kept reproducer"; message = (@($log | Where-Object { $_ -match '^\[FAIL\]' }) -join " ")
                       edits = $repro.Name; reproducer = $repro.FullName }
  })
  # The validator compares the saved copy with the .docx the app opened.
  $opened = $original
  if ([IO.Path]::GetExtension($original) -ieq ".doc") {
    New-Item -ItemType Directory -Force (Join-Path $Out "opened") | Out-Null
    $opened = Join-Path $Out "opened\$($repro.BaseName).docx"
    & $Fwtool convert $original $opened
  }
  if ((Test-Path $saved) -and [IO.Path]::GetExtension($opened) -in ".docx", ".docm") {
    [IO.File]::AppendAllText($pairsFile, "$opened`t$saved`n", [Text.UTF8Encoding]::new($false))
  }
  $torture.files = @($torture.files) + [pscustomobject]@{
    file = $original; failures = $failures; skipped = $null
    done = if ($counts.Success) { [int]$counts.Groups[1].Value } else { 0 }
    refused = if ($counts.Success) { [int]$counts.Groups[2].Value } else { 0 }
    saved = if (Test-Path $saved) { $saved } else { $null }
  }
}

# 2. The validator: no new errors in any saved copy.
"== Validator (Open XML SDK): saved copies against their originals"
$validator = Join-Path $root "build\validate\fwvalidate.exe"
& dotnet build (Join-Path $root "tools\validate\Validate.csproj") -c Release -o (Join-Path $root "build\validate") --nologo -v q | Out-Null
if ($LASTEXITCODE) { throw "building tools\validate failed" }
$newErrors = @{}  # saved copy -> new validator errors
if ((Test-Path $pairsFile) -and (Get-Item $pairsFile).Length) {
  & $validator --pairs $pairsFile --json (Join-Path $Out "validate.json") | Where-Object { $_ -match '^\[FAIL\]|^\s{4}|files,' }
  foreach ($f in (Read-Json (Join-Path $Out "validate.json")).files) { $newErrors[[IO.Path]::GetFullPath($f.saved)] = @($f.newErrors | Where-Object { $_ }) }
}

# 3. Word: each saved copy opens without a prompt; the originals of those that don't, too.
$word = @{}      # saved copy -> Word's result
$baseline = @{}  # original -> Word's result
$protectedWord = @{}  # D5: a protected copy's file name (as its saved copy's) -> Word's result
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
  $originals = @($word.Values | Where-Object { $_.status -notin "ok", "notice", "unchecked" } | ForEach-Object { $pairs[[IO.Path]::GetFullPath($_.file)] } | Where-Object { $_ })
  if ($originals.Count) {
    "== Word: the originals of the copies it complained about"
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $check @originals -Json (Join-Path $Out "word-originals.json")
    foreach ($f in (Read-Json (Join-Path $Out "word-originals.json")).files) { $baseline[[IO.Path]::GetFullPath($f.file)] = $f }
  }
  # D5: the protected copies as the app writes them, each folder opened with its password.
  $protectedDir = Join-Path $Out "saved-protected"
  if (Test-Path $protectedDir) {
    foreach ($dir in Get-ChildItem $protectedDir -Directory) {
      "== Word: the protected copies, with their password"
      $json = Join-Path $Out "word-protected-$($dir.Name).json"
      & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $check $dir.FullName -Password $dir.Name -Json $json
      foreach ($f in (Read-Json $json).files) { $protectedWord[[IO.Path]::GetFileName($f.file)] = $f }
    }
  }
}

# 4. The summary.
$rows = @()
foreach ($f in $torture.files) {
  $saved = if ($f.saved) { [IO.Path]::GetFullPath($f.saved) } else { $null }
  $original = if ($saved -and $pairs[$saved]) { $pairs[$saved] } else { [IO.Path]::GetFullPath($f.file) }  # D5: the .docx the app opened
  $w = if ($saved) { $word[$saved] } else { $null }
  $b = $baseline[$original]
  # A protected copy that Word doesn't open with its password fails the file.
  $p = if ($saved) { $protectedWord[[IO.Path]::GetFileName($saved)] } else { $null }
  if ($p -and $p.status -notin "ok", "notice", "unchecked") { $w = $p; $b = $null }
  $wordStatus = if (-not $w) { $null } elseif ($w.status -in "ok", "notice", "unchecked") { $w.status } elseif ($b -and $b.status -notin "ok", "notice") { "original too" } else { $w.status }
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
$wordPasses = "ok", "notice", "original too", "unchecked"  # unchecked has its own row: Word's automation, not the file
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
  "| Word: only a notice about this PC's Office | $(if ($NoWord) { 'not run' } else { @($rows | Where-Object { $_.word -eq 'notice' }).Count }) |",
  "| Word: couldn't check (its automation failed three times; run again) | $(if ($NoWord) { 'not run' } else { @($rows | Where-Object { $_.word -eq 'unchecked' }).Count }) |", "")
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
$md[4..14] -join "`n"
"Summary: $(Join-Path $Out 'summary.md')"
$unchecked = @($rows | Where-Object { $_.word -eq "unchecked" })
if ($unchecked.Count) { "Word couldn't check $($unchecked.Count) saved copies: run again." }
exit $(if ($failed.Count -or $unchecked.Count) { 1 } else { 0 })
