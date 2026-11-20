<#
.SYNOPSIS
  Slides against PowerPoint's (P1's display criterion, docs\p1-plan.md): for every deck in a
  reference manifest (scripts\reference-renders.ps1) under the given folders, Featherweight draws
  every slide as the screen does, hidden ones too, at 100 dpi (the app's pagepngs:DIR|100 script op,
  page-N.png), and fwtool pdf-compare --pngs measures each against the page of PowerPoint's PDF
  (SSIM at 100 dpi, page origins aligned; a slide passes at >= 0.95). The criterion: >= 90 % of the
  slides pass, on the real-world corpus; the public set (tests\office\presentations) is measured the
  same way and reported separately.

  Featherweight: a --snapshot run of its own per deck (hidden, its own settings), never a running
  instance; ended if it doesn't finish in time. A .ppsx opens in its tab here, not in the show.
  The summary goes to <out>\summary.md and summary.json: per folder (the corpus's pptx\<kind>\,
  or presentations\), the slides and how many pass; the decks with the most slides failing (the
  worst 20 are reviewed at every milestone, plan section 10); and per deck, each slide's SSIM.
  --out keeps each failing slide as PowerPoint's, ours and the difference side by side.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\slide-pages.ps1 -Reference tests\office\reference -Folders presentations -Out build\slide-pages-public
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\slide-pages.ps1 -Reference $env:FW_CORPUS\reference -Folders pptx -Out build\slide-pages-corpus
#>
param(
  [Parameter(Mandatory = $true)][string]$Reference,
  # The manifest's top folders to measure (its sources' first part: presentations, generated, pptx).
  [string[]]$Folders = @("presentations"),
  [string]$Featherweight = "build\Release\Featherweight.exe",
  [string]$Fwtool = "build\tools\fwtool\Release\fwtool.exe",
  [string]$Out = "build\slide-pages",
  [int]$TimeoutSeconds = 300,
  [double]$MinSlides = 90,
  # Keep the decks already measured in -Out (their PNGs and comparison); draw only the others.
  [switch]$Reuse
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
foreach ($v in "Featherweight", "Fwtool", "Out") {
  $p = Get-Variable $v -ValueOnly
  if (-not [IO.Path]::IsPathRooted($p)) { Set-Variable $v (Join-Path $root $p) }
}
foreach ($p in $Featherweight, $Fwtool) { if (-not (Test-Path $p)) { throw "$p not found. Build it, or pass its path." } }
$Folders = @($Folders | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$refRoot = (Resolve-Path $Reference).Path
$manifest = Get-Content (Join-Path $refRoot "manifest.json") -Raw -Encoding UTF8 | ConvertFrom-Json
# The public manifest's sources are under tests\office; the corpus's under the corpus folder.
$corpus = if ($manifest.corpus) { $manifest.corpus } else { Split-Path -Parent $refRoot }
New-Item -ItemType Directory -Force $Out | Out-Null
$decks = Join-Path $Out "decks"
New-Item -ItemType Directory -Force $decks | Out-Null
$diffs = Join-Path $Out "failing"

$deckExt = '(?i)\.(pptx|pptm|ppsx|ppsm|potx|potm)$'
$entries = @($manifest.files | Where-Object { $_.source -match $deckExt -and $Folders -contains ($_.source -split '/')[0] -and $_.pdf -and -not $_.error } | Sort-Object source)
if (-not $entries.Count) { throw "The manifest in $refRoot has no rendered decks under $($Folders -join ', ')." }
$rows = @()
$n = 0
foreach ($e in $entries) {
  $n++
  $source = Join-Path $corpus ($e.source -replace '/', '\')
  $refPdf = Join-Path $refRoot ($e.pdf -replace '/', '\')
  $name = ($e.source -replace '[\\/:]', '_')
  $pngs = Join-Path $decks $name
  $json = Join-Path $decks "$name.compare.json"
  $folder = ($e.source -replace '/[^/]+$', '')
  $row = [ordered]@{ source = $e.source; folder = $folder; slides = [int]$e.pages; ourSlides = 0; passing = 0; inkPassing = 0; hidden = @($e.hidden); ssim = @(); inkSsim = @(); error = $null }
  $first = Join-Path $pngs "page-1.png"
  $kept = $Reuse -and (Test-Path $first) -and (Test-Path $json) -and (Get-Item $json).LastWriteTime -ge (Get-Item $first).LastWriteTime
  if (-not $kept) {
    if (Test-Path $pngs) { Remove-Item -Recurse -Force $pngs }
    if (Test-Path $json) { Remove-Item $json }
    New-Item -ItemType Directory -Force $pngs | Out-Null
    if (-not (Test-Path -LiteralPath $source)) {
      $row.error = "the deck isn't there ($source)"
    } else {
      $log = Join-Path $decks "$name.log"
      $p = Start-Process -FilePath $Featherweight -PassThru -WindowStyle Hidden -RedirectStandardError $log -ArgumentList @(
        "--snapshot", "`"$(Join-Path $decks "$name.png")`"", "--size", "1400x900", "--script", "`"pagepngs:$pngs|100`"", "`"$source`"")
      $null = $p.Handle  # so ExitCode is kept
      if (-not $p.WaitForExit($TimeoutSeconds * 1000)) {
        Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
        $row.error = "Featherweight didn't finish within $TimeoutSeconds s"
      } elseif (-not (Test-Path $first)) {
        $said = @(Get-Content $log -ErrorAction SilentlyContinue | Where-Object { $_ }) -join "; "
        $row.error = "no slides drawn (exit code $($p.ExitCode))" + $(if ($said) { ": $said" } else { "" })
      } else {
        & $Fwtool pdf-compare --pngs $pngs --json $json --out $diffs $refPdf *> $null
      }
    }
  }
  if (-not $row.error) {
    $c = if (Test-Path $json) { Get-Content $json -Raw | ConvertFrom-Json } else { $null }
    if ($c) {
      $row.ourSlides = [int]$c.oursPages
      $row.passing = [int]$c.passing
      if ($c.referencePages) { $row.slides = [int]$c.referencePages }
      $row.ssim = @($c.pages | ForEach-Object { [Math]::Round([double]$_.ssim, 4) })
      $row.inkSsim = @($c.pages | ForEach-Object { [Math]::Round([double]$_.inkSsim, 4) })
      $row.inkPassing = @($row.inkSsim | Where-Object { $_ -ge 0.95 }).Count
    } else {
      $row.error = "pdf-compare wrote nothing"
    }
  }
  $rows += [pscustomobject]$row
  "[{0}/{1}] {2}: {3}" -f $n, $entries.Count, $e.source, $(if ($row.error) { "FAILED: $($row.error)" } else { "$($row.passing) of $($row.slides) slides" })
}

function Get-Pct($pass, $all) { if ($all) { [Math]::Round(100.0 * $pass / $all, 2) } else { 0 } }
$md = @("# Slides against PowerPoint's", "", "Reference: $refRoot. Folders: $($Folders -join ', '). $(Get-Date -Format 'yyyy-MM-dd HH:mm').", "",
        "| Folder | Decks | Not drawn | PowerPoint's slides | Passing (SSIM >= 0.95) | % | Inside the ink >= 0.95 (information) |", "|---|---|---|---|---|---|---|")
$groups = @($rows | Group-Object folder | Sort-Object Name)
foreach ($g in $groups) {
  $slides = ($g.Group | Measure-Object slides -Sum).Sum
  $pass = ($g.Group | Measure-Object passing -Sum).Sum
  $ink = ($g.Group | Measure-Object inkPassing -Sum).Sum
  $md += "| $($g.Name) | $($g.Count) | $(@($g.Group | Where-Object error).Count) | $slides | $pass | $(Get-Pct $pass $slides) | $(Get-Pct $ink $slides) % |"
}
# A deck we couldn't draw counts as no slide passing.
$total = ($rows | Measure-Object slides -Sum).Sum
$passing = ($rows | Measure-Object passing -Sum).Sum
$pct = Get-Pct $passing $total
$inkAll = ($rows | Measure-Object inkPassing -Sum).Sum
if ($groups.Count -gt 1) { $md += "| **All** | $($rows.Count) | $(@($rows | Where-Object error).Count) | $total | $passing | **$pct** | $(Get-Pct $inkAll $total) % |" }
$md += "", "Criterion: >= $MinSlides % of the slides pass (P1, plan section 10): $(if ($pct -ge $MinSlides) { 'met' } else { 'not met' }) ($pct %).",
       "Inside the ink: SSIM over the box both slides' ink fills, for information. Whole-slide SSIM is lenient on sparse slides: a blank render of a slide with two lines of text scores 0.947 (embedded-fonts.pptx, 2026-11-20).", ""
$bad = @($rows | Where-Object { $_.error })
if ($bad.Count) { $md += "## Not drawn", ""; $md += @($bad | ForEach-Object { "- $($_.source): $($_.error)" }); $md += "" }
$worst = @($rows | Where-Object { -not $_.error -and $_.slides -gt 0 -and $_.passing -lt $_.slides } | Sort-Object { $_.passing / $_.slides }, source | Select-Object -First 20)
if ($worst.Count) {
  $md += "## The 20 decks with the most slides failing", ""
  foreach ($w in $worst) {
    $low = @(for ($i = 0; $i -lt $w.ssim.Count; $i++) { if ($w.ssim[$i] -lt 0.95) { "$($i + 1) ($($w.ssim[$i]))" } }) | Select-Object -First 8
    $md += "- $($w.source): $($w.passing) of $($w.slides) (ours: $($w.ourSlides)); failing: $($low -join ', ')"
  }
}
[IO.File]::WriteAllText((Join-Path $Out "summary.md"), ($md -join "`n") + "`n", [Text.UTF8Encoding]::new($false))
[IO.File]::WriteAllText((Join-Path $Out "summary.json"), ([ordered]@{ reference = $refRoot; folders = $Folders; minSsim = 0.95
  slides = $total; passing = $passing; percent = $pct; files = $rows } | ConvertTo-Json -Depth 5), [Text.UTF8Encoding]::new($false))
""
$md | Where-Object { $_ -like "|*" }
"Summary: $(Join-Path $Out 'summary.md')"
if ($pct -lt $MinSlides) { exit 1 }
