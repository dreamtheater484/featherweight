<#
.SYNOPSIS
  The deck torture test (P2's "never damage a file" gate, docs\p2-plan.md): every corpus deck, after
  seeded random edits and a save, has no new validator errors, passes fwtool check, and opens in
  PowerPoint without a repair prompt; and every deck saved without edits opens in PowerPoint and
  looks as the original does, slide for slide.
  1. fwtool torture --decks: each deck saved with no edits (no part written, every part as it was),
     then seeded random edits through the edit API, a save, fwtool check and the model's checks with no
     new errors, the saved file read back as the edited deck, every part the save didn't write kept
     byte for byte, undo and redo of everything. Failures are shrunk to reproducers (<out>\repro).
  2. The Open XML SDK validator (tools\validate): each edited copy against its original.
  3. PowerPoint (scripts\powerpoint-open-check.ps1): each edited copy must open cleanly. When one
     doesn't, its original is checked too: a copy no worse than its original (PowerPoint repairs or
     refuses the original the same way) isn't the editor's doing, and the summary says so.
  4. Saved without edits: PowerPoint opens each unchanged copy and its original and saves both as PDF
     in the same run (every slide, hidden ones too); fwtool pdf-compare --min-ssim 0.99: every slide
     of the copy must be at SSIM >= 0.99 against the original's.
  The summary goes to <out>\summary.md and summary.json. The exit code is 1 when anything failed, 3
  when PowerPoint couldn't be used (no active session, or someone's PowerPoint running): run it again
  with -Reuse then, and only PowerPoint's part runs.

  Corpus decks reach PowerPoint only when their SHA-256 is in tests\office\real-world.json (the
  screened ones, scripts\fetch-corpus.ps1); the public decks are the repository's.

  Without -Paths: the public decks (tests\office\presentations, generated, edge, show) and, with
  -Corpus (or FW_CORPUS), the private corpus's pptx folder. -Ci: the public decks only, fewer edits,
  no PowerPoint.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\deck-torture.ps1 -Corpus D:\Documents\Codex\FeatherweightCorpus
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\deck-torture.ps1 -Ci
#>
param(
  [string[]]$Paths,
  [string]$Corpus = $env:FW_CORPUS,
  [uint32]$Seed = 1,
  [int]$Edits = 40,
  [int]$Rounds = 3,
  [string]$Out = "build\deck-torture",
  [string]$Fwtool = "build\tools\fwtool\Release\fwtool.exe",
  [switch]$Ci,
  [switch]$NoPowerPoint,
  # Skip the save-without-edits PDFs (step 4): PowerPoint opens only the edited copies.
  [switch]$NoPdf,
  # Keep the torture results already in -Out and run only the validator, PowerPoint and the summary
  # again (after PowerPoint couldn't run, or one of them changed).
  [switch]$Reuse
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
if ($Ci) {
  $NoPowerPoint = $true
  if (-not $PSBoundParameters.ContainsKey("Edits")) { $Edits = 30 }
  if (-not $PSBoundParameters.ContainsKey("Rounds")) { $Rounds = 2 }
}
$Paths = @($Paths | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
if (-not $Paths) {
  $Paths = @("tests\office\presentations", "tests\office\generated", "tests\office\edge", "tests\office\show")
  if (-not $Ci -and $Corpus -and (Test-Path (Join-Path $Corpus "pptx"))) { $Paths += Join-Path $Corpus "pptx" }
}
if (-not [IO.Path]::IsPathRooted($Out)) { $Out = Join-Path $root $Out }
if (-not [IO.Path]::IsPathRooted($Fwtool)) { $Fwtool = Join-Path $root $Fwtool }
if (-not (Test-Path $Fwtool)) { throw "fwtool.exe not found at $Fwtool. Build it, or pass -Fwtool." }
if (-not $Reuse) { foreach ($old in "saved", "unchanged", "repro", "pdf") { if (Test-Path (Join-Path $Out $old)) { Remove-Item -Recurse -Force (Join-Path $Out $old) } } }
New-Item -ItemType Directory -Force $Out | Out-Null

function Read-Json([string]$path) { if (Test-Path $path) { Get-Content $path -Raw -Encoding UTF8 | ConvertFrom-Json } }
function Write-Lines([string]$path, $lines) { [IO.File]::WriteAllLines($path, [string[]]@($lines), [Text.UTF8Encoding]::new($false)) }

# 1. Saved without edits, edits, save, check, read back.
"== fwtool torture --decks: seed $Seed, $Rounds rounds of $Edits edits"
if (-not $Reuse) {
  & $Fwtool torture --decks --seed $Seed --edits $Edits --rounds $Rounds --out $Out --json (Join-Path $Out "torture.json") @Paths |
    Tee-Object -FilePath (Join-Path $Out "torture.log")
}
$torture = Read-Json (Join-Path $Out "torture.json")
if (-not $torture) { throw "fwtool torture wrote no results." }
$pairs = @{}  # edited copy -> original
foreach ($f in $torture.files) { if ($f.saved) { $pairs[[IO.Path]::GetFullPath($f.saved)] = [IO.Path]::GetFullPath($f.file) } }

# Which originals may reach PowerPoint: the repository's decks, and corpus decks screened by fetch-corpus.
$screened = @{}
foreach ($m in (Read-Json (Join-Path $root "tests\office\real-world.json")).files) { if ($m.sha256) { $screened[$m.sha256.ToLowerInvariant()] = $true } }
$office = [IO.Path]::GetFullPath((Join-Path $root "tests\office")) + "\"
function Test-Screened([string]$original) {
  if ($original.StartsWith($office, [StringComparison]::OrdinalIgnoreCase)) { return $true }
  $screened[(Get-FileHash -LiteralPath $original -Algorithm SHA256).Hash.ToLowerInvariant()] -eq $true
}

# 2. The validator: no new errors in any edited copy.
"== Validator (Open XML SDK): edited copies against their originals"
$validator = Join-Path $root "build\validate\fwvalidate.exe"
& dotnet build (Join-Path $root "tools\validate\Validate.csproj") -c Release -o (Join-Path $root "build\validate") --nologo -v q | Out-Null
if ($LASTEXITCODE) { throw "building tools\validate failed" }
$newErrors = @{}
if ($pairs.Count) {
  & $validator --pairs (Join-Path $Out "pairs.tsv") --json (Join-Path $Out "validate.json") | Where-Object { $_ -match '^\[FAIL\]|^\s{4}|files,' }
  foreach ($f in (Read-Json (Join-Path $Out "validate.json")).files) { $newErrors[[IO.Path]::GetFullPath($f.saved)] = @($f.newErrors | Where-Object { $_ }) }
}

# 3 and 4. PowerPoint.
$check = Join-Path $PSScriptRoot "powerpoint-open-check.ps1"
$ppStopped = $false
# Runs the open check on these decks (a list file: more than a command line holds); returns its results by full path.
function Invoke-PowerPoint([string]$name, $decks, [string]$pdfDir) {
  $results = @{}
  $decks = @($decks | Where-Object { $_ })
  if (-not $decks.Count -or $script:ppStopped) { return $results }
  $list = Join-Path $Out "$name.txt"
  Write-Lines $list $decks
  $json = Join-Path $Out "$name.json"
  $ppArgs = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $check, "-ListFile", $list, "-Json", $json)
  if ($pdfDir) { $ppArgs += @("-Pdf", $pdfDir) }
  & powershell.exe @ppArgs | Out-Host
  if ($LASTEXITCODE -eq 3 -or $LASTEXITCODE -eq 4) { $script:ppStopped = $true }
  foreach ($f in (Read-Json $json).files) { $results[[IO.Path]::GetFullPath($f.file)] = $f }
  $results
}
$pp = @{}; $baseline = @{}; $unchangedPp = @{}; $originalPp = @{}; $ssim = @{}
$notScreened = @($pairs.Values | Where-Object { -not (Test-Screened $_) } | Sort-Object -Unique)
if (-not $NoPowerPoint -and $pairs.Count) {
  "== PowerPoint: does each edited copy open without a repair?"
  $pp = Invoke-PowerPoint "powerpoint" @($pairs.Keys | Where-Object { $notScreened -notcontains $pairs[$_] } | Sort-Object) $null
  $originals = @($pp.Values | Where-Object { $_.status -notin "ok", "unchecked" } | ForEach-Object { $pairs[[IO.Path]::GetFullPath($_.file)] } | Where-Object { $_ })
  if ($originals.Count) {
    "== PowerPoint: the originals of the copies it didn't open cleanly"
    $baseline = Invoke-PowerPoint "powerpoint-originals" $originals $null
  }
}
$unchanged = @{}  # unchanged copy -> original
foreach ($f in $torture.files) { if ($f.unchanged) { $unchanged[[IO.Path]::GetFullPath($f.unchanged)] = [IO.Path]::GetFullPath($f.file) } }
if (-not $NoPowerPoint -and -not $NoPdf -and $unchanged.Count) {
  "== PowerPoint: each deck saved without edits, and its original, as PDF"
  $ok = @($unchanged.Keys | Where-Object { Test-Screened $unchanged[$_] } | Sort-Object)
  $unchangedPp = Invoke-PowerPoint "powerpoint-unchanged" $ok (Join-Path $Out "pdf\unchanged")
  $originalPp = Invoke-PowerPoint "powerpoint-unchanged-originals" @($ok | ForEach-Object { $unchanged[$_] }) (Join-Path $Out "pdf\original")
  "== Saved without edits: every slide at SSIM >= 0.99 against the original's"
  foreach ($u in $ok) {
    $a = $originalPp[$unchanged[$u]]; $b = $unchangedPp[$u]
    if (-not $a -or -not $b -or -not $a.pdf -or -not $b.pdf) { continue }
    $json = Join-Path $Out ("pdf\" + [IO.Path]::GetFileName($u) + ".compare.json")
    & $Fwtool pdf-compare --min-ssim 0.99 --json $json $a.pdf $b.pdf | Select-Object -Last 1 | Out-Null
    $c = Read-Json $json
    if ($c) {
      $worst = ($c.pages | Measure-Object ssim -Minimum).Minimum
      $ssim[$u] = [ordered]@{ pages = [int]$c.referencePages; copyPages = [int]$c.oursPages; passing = [int]$c.passing; worst = $worst
                              failing = @($c.pages | Where-Object { -not $_.ok } | ForEach-Object { "slide $($_.page): $('{0:N4}' -f $_.ssim)" }) }
    }
  }
}

# The summary.
$rows = @()
foreach ($f in $torture.files) {
  $saved = if ($f.saved) { [IO.Path]::GetFullPath($f.saved) } else { $null }
  $original = [IO.Path]::GetFullPath($f.file)
  $u = if ($f.unchanged) { [IO.Path]::GetFullPath($f.unchanged) } else { $null }
  $x = if ($saved) { $pp[$saved] } else { $null }
  $b = $baseline[$original]
  $same = $x -and $b -and $b.status -eq $x.status
  $ppStatus = if (-not $x) { $null } elseif ($x.status -in "ok", "unchecked") { $x.status } elseif ($same) { "original too" } else { $x.status }
  $uo = if ($u) { $unchangedPp[$u] } else { $null }
  $oo = $originalPp[$original]
  # Saved without edits: PowerPoint must open the copy as it opens the original.
  $uStatus = if (-not $uo) { $null } elseif ($uo.status -eq "ok" -or $uo.status -eq "unchecked") { $uo.status }
             elseif ($oo -and $oo.status -eq $uo.status) { "original too" } else { $uo.status }
  $s = if ($u) { $ssim[$u] } else { $null }
  $rows += [pscustomobject][ordered]@{
    file = $f.file
    skipped = $f.skipped
    identical = $f.identical
    done = $f.done
    refused = $f.refused
    failures = @($f.failures | Where-Object { $_ })
    newValidatorErrors = @(if ($saved -and $newErrors.ContainsKey($saved)) { $newErrors[$saved] })
    powerpoint = $ppStatus
    powerpointDetail = if ($x -and $x.status -ne "ok") { (@($x.dialogs | ForEach-Object { "$($_.title): $($_.text)" }) + @($x.error) | Where-Object { $_ }) -join " / " } else { $null }
    unchangedPowerPoint = $uStatus
    unchangedSlides = if ($s) { "$($s.passing) of $([Math]::Max($s.pages, $s.copyPages))" } else { $null }
    unchangedWorstSsim = if ($s) { $s.worst } else { $null }
    unchangedFailing = if ($s) { @($s.failing) } else { @() }
    screened = -not ($notScreened -contains $original)
  }
}
$ppPasses = "ok", "original too", "unchecked"
$failed = @($rows | Where-Object { $_.failures.Count -or $_.newValidatorErrors.Count -or ($_.powerpoint -and $_.powerpoint -notin $ppPasses) -or
                                   ($_.unchangedPowerPoint -and $_.unchangedPowerPoint -notin $ppPasses) -or $_.unchangedFailing.Count })
$unchecked = @($rows | Where-Object { $_.powerpoint -eq "unchecked" -or $_.unchangedPowerPoint -eq "unchecked" })
$through = Select-String -Path (Join-Path $Out "torture.log") -Pattern 'edits through: (.*)\)$' | Select-Object -Last 1
$through = if ($through) { $through.Matches[0].Groups[1].Value } else { "?" }
$noPp = if ($NoPowerPoint) { "not run" } else { $null }
$slidesAll = ($ssim.Values | ForEach-Object { [Math]::Max($_.pages, $_.copyPages) } | Measure-Object -Sum).Sum
$slidesOk = ($ssim.Values | Measure-Object passing -Sum).Sum
$md = @("# Deck torture test", "",
  "Seed $Seed, $Rounds rounds of $Edits edits per deck. $(Get-Date -Format 'yyyy-MM-dd HH:mm'). Edits through: $through.", "",
  "| | Decks |", "|---|---|",
  "| Checked | $($rows.Count) |",
  "| Not read by the model (skipped) | $(@($rows | Where-Object skipped).Count) |",
  "| Saved without edits: the same bytes | $(@($rows | Where-Object identical).Count) |",
  "| Edits done / refused | $(($rows | Measure-Object done -Sum).Sum) / $(($rows | Measure-Object refused -Sum).Sum) |",
  "| fwtool torture failures (save, check, read back, untouched parts, undo, saved without edits) | $(@($rows | Where-Object { $_.failures.Count }).Count) |",
  "| New validator errors | $(@($rows | Where-Object { $_.newValidatorErrors.Count }).Count) |",
  "| PowerPoint: edited copy repaired or refused | $(if ($noPp) { $noPp } else { @($rows | Where-Object { $_.powerpoint -and $_.powerpoint -notin $ppPasses }).Count }) |",
  "| PowerPoint: the original is repaired or refused the same way | $(if ($noPp) { $noPp } else { @($rows | Where-Object { $_.powerpoint -eq 'original too' }).Count }) |",
  "| PowerPoint: saved without edits, repaired or refused | $(if ($noPp -or $NoPdf) { 'not run' } else { @($rows | Where-Object { $_.unchangedPowerPoint -and $_.unchangedPowerPoint -notin $ppPasses }).Count }) |",
  "| Saved without edits: slides at SSIM >= 0.99 against the original's PDF | $(if ($noPp -or $NoPdf) { 'not run' } else { "$([int]$slidesOk) of $([int]$slidesAll)" }) |",
  "| PowerPoint couldn't check (no active session, someone's PowerPoint, or its automation failed): run again with -Reuse | $(if ($noPp) { $noPp } else { $unchecked.Count }) |",
  "| Corpus decks not screened (kept away from PowerPoint) | $($notScreened.Count) |", "")
if ($failed.Count) {
  $md += "## Failures", ""
  foreach ($r in $failed) {
    $md += "### $($r.file)", ""
    foreach ($x in $r.failures) { $md += "- round $($x.round) (seed $($x.seed)): $($x.category): $($x.message) ($($x.edits) edits: ``$($x.reproducer)``)" }
    foreach ($x in $r.newValidatorErrors | Select-Object -First 5) { $md += "- validator: $($x.Part) $($x.Path): $($x.Description) ($($x.Id))" }
    if ($r.powerpoint -and $r.powerpoint -notin $ppPasses) { $md += "- PowerPoint: $($r.powerpoint): $($r.powerpointDetail)" }
    if ($r.unchangedPowerPoint -and $r.unchangedPowerPoint -notin $ppPasses) { $md += "- saved without edits: PowerPoint: $($r.unchangedPowerPoint)" }
    if ($r.unchangedFailing.Count) { $md += "- saved without edits: below SSIM 0.99: $($r.unchangedFailing -join ', ')" }
    $md += ""
  }
}
[IO.File]::WriteAllText((Join-Path $Out "summary.md"), ($md -join "`n") + "`n", [Text.UTF8Encoding]::new($false))
[IO.File]::WriteAllText((Join-Path $Out "summary.json"), ([ordered]@{ seed = $Seed; edits = $Edits; rounds = $Rounds; powerpoint = -not $NoPowerPoint; files = $rows } | ConvertTo-Json -Depth 8), [Text.UTF8Encoding]::new($false))
""
$md[4..17] -join "`n"
"Summary: $(Join-Path $Out 'summary.md')"
if ($ppStopped -or $unchecked.Count) { "PowerPoint couldn't check everything: run this again with -Reuse while Roy is signed in (an active session) and no PowerPoint of his is open." }
exit $(if ($failed.Count) { 1 } elseif ($ppStopped -or $unchecked.Count) { 3 } else { 0 })
