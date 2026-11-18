<#
.SYNOPSIS
  Shrinks a workbook torture run's edits (a saved copy's <name>.edits.txt, or a reproducer) to the
  fewest that still make Excel repair or refuse the saved copy: each try replays a subset with
  `fwtool torture --replay` and opens the result with scripts\excel-open-check.ps1 (delta
  debugging, at most -MaxTries tries). Writes <out>\<name>.min.txt (a reproducer), the saved copy
  it makes, and Excel's repair lines. Needs the Office reference PC.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\sheet-torture-minimize.ps1 build\sheet-torture\saved\x.edits.txt
#>
param(
  [Parameter(Mandatory = $true, Position = 0, ValueFromRemainingArguments = $true)][string[]]$Edits,
  [string]$Out = "build\sheet-torture\minimized",
  [string]$Fwtool = "build\tools\fwtool\Release\fwtool.exe",
  [int]$MaxTries = 60
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if (-not [IO.Path]::IsPathRooted($Out)) { $Out = Join-Path $root $Out }
if (-not [IO.Path]::IsPathRooted($Fwtool)) { $Fwtool = Join-Path $root $Fwtool }
New-Item -ItemType Directory -Force $Out | Out-Null
$check = Join-Path $PSScriptRoot "excel-open-check.ps1"
$work = Join-Path $Out "work"
New-Item -ItemType Directory -Force $work | Out-Null

# Replays `$lines` (edit lines) after `$header`; returns Excel's result for the saved copy, or $null
# when the replay saved nothing or failed by itself.
function Test-Edits([string[]]$header, [string[]]$lines, [string]$name) {
  $repro = Join-Path $work "$name.txt"
  [IO.File]::WriteAllLines($repro, [string[]]($header + $lines), [Text.UTF8Encoding]::new($false))
  $saved = Join-Path $work "$name-replay.xlsx"
  if (Test-Path $saved) { Remove-Item $saved }
  & $Fwtool torture --replay $repro --out $work | Out-Null
  if (-not (Test-Path $saved)) { return $null }
  $json = Join-Path $work "$name.json"
  & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $check $saved -Json $json | Out-Null
  $r = (Get-Content $json -Raw -Encoding UTF8 | ConvertFrom-Json).files[0]
  if ($r.status -in "repaired", "refused") { $r } else { $null }
}

foreach ($file in $Edits) {
  $all = @(Get-Content -LiteralPath $file -Encoding UTF8)
  $header = @($all | Where-Object { $_ -match '^(#|file: |sha256: |seed: |round: |failure: |edits: )' -or -not $_ })
  $lines = @($all | Where-Object { $_ -and $_ -notmatch '^(#|file: |sha256: |seed: |round: |failure: |edits: )' })
  $name = [IO.Path]::GetFileNameWithoutExtension($file) -replace '\.edits$', ''
  $first = Test-Edits $header $lines $name
  if (-not $first) { "[ ok ] $name`: Excel opens the replayed copy cleanly; nothing to shrink"; continue }
  $tries = 1
  $chunks = 2
  while ($lines.Count -gt 1 -and $tries -lt $MaxTries) {
    $size = [Math]::Ceiling($lines.Count / $chunks)
    $removed = $false
    for ($start = 0; $start -lt $lines.Count -and $tries -lt $MaxTries; $start += $size) {
      $rest = @(); if ($start -gt 0) { $rest += $lines[0..($start - 1)] }
      if ($start + $size -lt $lines.Count) { $rest += $lines[($start + $size)..($lines.Count - 1)] }
      $tries++
      if ($rest.Count -and ($r = Test-Edits $header $rest $name)) { $lines = $rest; $first = $r; $chunks = [Math]::Max($chunks - 1, 2); $removed = $true; break }
    }
    if (-not $removed) { if ($size -le 1) { break }; $chunks = [Math]::Min($chunks * 2, $lines.Count) }
  }
  # The saved copy and repair lines of the shrunk edits themselves (the last try may have been another).
  if ($r = Test-Edits $header $lines $name) { $first = $r }
  $min = Join-Path $Out "$name.min.txt"
  $header2 = @($header | ForEach-Object { if ($_ -match '^failure: ') { "failure: excel $($first.status): $(@($first.repairs) -join ' || ')" } elseif ($_ -match '^edits: ') { "edits: $($lines.Count)" } else { $_ } })
  [IO.File]::WriteAllLines($min, [string[]]($header2 + $lines), [Text.UTF8Encoding]::new($false))
  Copy-Item (Join-Path $work "$name-replay.xlsx") (Join-Path $Out "$name-replay.xlsx") -Force -ErrorAction SilentlyContinue
  "[FAIL] $name`: Excel $($first.status) with $($lines.Count) edits ($tries tries): $(@($first.repairs) -join ' || ')"
  $lines | ForEach-Object { "       $_" }
}
