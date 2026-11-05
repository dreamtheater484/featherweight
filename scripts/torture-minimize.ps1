<#
.SYNOPSIS
  Shrinks a torture test's edits to the fewest that still make Word or the Open XML SDK validator
  complain, for failures fwtool torture can't see itself (it shrinks its own failures as it finds
  them). Each try replays a subset with fwtool torture --replay, then asks the judge:
  - Word: scripts\word-open-check.ps1 on the replayed file, which fails unless it's ok or notice.
    One Word at a time, about 10 seconds a try: give it a budget.
  - Validator: tools\validate's fwvalidate --compare against the original.
  The input is any list of edits in reproducer form: a saved copy's <name>.edits.txt from
  fwtool torture's <out>\saved, or a reproducer. The result is <name>-<judge>.txt in -Out, with
  the file it makes beside it.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\torture-minimize.ps1 build\torture\saved\x.edits.txt -Judge Word
#>
param(
  [Parameter(Mandatory = $true, Position = 0)][string]$Edits,
  [ValidateSet("Word", "Validator")][string]$Judge = "Word",
  [string]$Out = "build\torture\minimized",
  [string]$Fwtool = "build\tools\fwtool\Release\fwtool.exe",
  [int]$BudgetMinutes = 30
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
if (-not [IO.Path]::IsPathRooted($Out)) { $Out = Join-Path $root $Out }
if (-not [IO.Path]::IsPathRooted($Fwtool)) { $Fwtool = Join-Path $root $Fwtool }
$validator = Join-Path $root "build\validate\fwvalidate.exe"
if ($Judge -eq "Validator" -and -not (Test-Path $validator)) {
  & dotnet build (Join-Path $root "tools\validate\Validate.csproj") -c Release -o (Join-Path $root "build\validate") --nologo -v q | Out-Null
}
New-Item -ItemType Directory -Force $Out | Out-Null
$work = Join-Path $Out "work"
New-Item -ItemType Directory -Force $work | Out-Null

# The header (file, sha256, seed...) and the edits.
$lines = [IO.File]::ReadAllLines((Resolve-Path $Edits), [Text.UTF8Encoding]::new($false))
$header = @($lines | Where-Object { $_ -match '^(file|sha256|seed|round): ' })
$all = @($lines | Where-Object { $_ -and $_ -notmatch '^#' -and $_ -notmatch '^(file|sha256|seed|round|failure|edits): ' })
$original = (($header | Where-Object { $_ -like "file: *" }) -replace '^file: ', '')
if (-not (Test-Path $original)) {
  # From another checkout: the tail of the path, from here (as fwtool torture --replay does).
  $parts = $original -split '[\\/]'
  for ($i = 1; $i -lt $parts.Count; $i++) { $tail = $parts[$i..($parts.Count - 1)] -join '\'; if (Test-Path $tail) { $original = $tail; break } }
}
$name = [IO.Path]::GetFileNameWithoutExtension($Edits) -replace '\.edits$', ''
$utf8 = [Text.UTF8Encoding]::new($false)
$deadline = (Get-Date).AddMinutes($BudgetMinutes)
$tries = 0

# Replays the edits and asks the judge. True when it still complains; $script:what says how.
function Test-Fails([string[]]$subset) {
  $script:tries++
  $txt = Join-Path $work "try.txt"
  [IO.File]::WriteAllLines($txt, [string[]](@("# torture-minimize try") + $header + $subset), $utf8)
  $made = Join-Path $work "try-replay.docx"
  if (Test-Path $made) { Remove-Item $made }
  & $Fwtool torture --replay $txt 2>&1 | Out-Null
  if (-not (Test-Path $made)) { return $false }  # nothing saved: not this failure
  if ($Judge -eq "Validator") {
    $o = & $validator --compare $original $made 2>&1
    $script:what = "$($o | Where-Object { "$_" -match '^\s{4}' } | Select-Object -First 1)".Trim()
    return $LASTEXITCODE -ne 0
  }
  $json = Join-Path $work "word.json"
  & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "word-open-check.ps1") $made -Json $json | Out-Null
  $r = (Get-Content $json -Raw -Encoding UTF8 | ConvertFrom-Json).files | Select-Object -First 1
  $script:what = "$($r.status): $(@($r.dialogs | ForEach-Object { $_.text }) + @($r.error) | Where-Object { $_ } | Select-Object -First 1)"
  return $r.status -notin "ok", "notice"
}

"== $name, $($all.Count) edits, judged by $Judge"
if (-not (Test-Fails $all)) { "It doesn't fail with all its edits: nothing to shrink."; exit 0 }
$first = $what
"   fails: $first"

# ddmin over the edits: drop chunks while it still fails.
$cur = $all
$n = 2
while ($cur.Count -ge 2 -and (Get-Date) -lt $deadline) {
  $size = [Math]::Ceiling($cur.Count / $n)
  $reduced = $false
  for ($i = 0; $i -lt $n -and (Get-Date) -lt $deadline; $i++) {
    $from = $i * $size
    if ($from -ge $cur.Count) { break }
    $to = [Math]::Min($from + $size, $cur.Count) - 1
    $rest = @(for ($k = 0; $k -lt $cur.Count; $k++) { if ($k -lt $from -or $k -gt $to) { $cur[$k] } })
    if ($rest.Count -and (Test-Fails $rest)) {
      $cur = $rest
      $n = [Math]::Max($n - 1, 2)
      $reduced = $true
      "   $($cur.Count) edits still fail"
      break
    }
  }
  if (-not $reduced) {
    if ($n -ge $cur.Count) { break }
    $n = [Math]::Min($n * 2, $cur.Count)
  }
}

# Keep it: the edits, and the file they make.
[void](Test-Fails $cur)
$result = Join-Path $Out "$name-$($Judge.ToLower()).txt"
[IO.File]::WriteAllLines($result, [string[]](@("# fwtool torture reproducer: fwtool torture --replay $result") + $header +
  @("failure: $($Judge.ToLower()): $what", "edits: $($cur.Count) of $($all.Count)") + $cur), $utf8)
Copy-Item (Join-Path $work "try-replay.docx") ([IO.Path]::ChangeExtension($result, ".docx")) -Force
$timedOut = if ((Get-Date) -ge $deadline) { " (the budget ran out: it may shrink further)" } else { "" }
"   $($cur.Count) of $($all.Count) edits, $tries tries$timedOut"
"   $result"
