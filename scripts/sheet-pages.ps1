<#
.SYNOPSIS
  Printed pages against Excel's (S1's display criterion, used again in S4 for the .xls and .ods
  corpora): for every workbook in a reference manifest (scripts\reference-renders.ps1) under the
  given folders, Featherweight exports every sheet to PDF as Excel's reference PDFs have them
  (the app's workbookpdf: script op), and fwtool pdf-compare measures each page against Excel's
  PDF (SSIM at 100 dpi; a page passes at >= 0.95). The criterion: >= 90 % of the pages pass.

  With -Originals (the summary.json of a run over the xlsx folder), the .xls and .ods copies are
  measured against their own .xlsx originals instead (S4's criterion, docs\s4-plan.md): on the
  workbooks both have a reference for, the copies' pages pass at least at the originals' rate.

  Featherweight: a --snapshot run of its own per workbook (hidden, its own settings), never a
  running instance; ended if it doesn't finish in time. The summary goes to <out>\summary.md and
  summary.json (per workbook: pages, passing, or why there's none).

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\sheet-pages.ps1 -Reference $env:FW_CORPUS\reference -Folders xlsx -Out build\sheet-pages-xlsx
  powershell -ExecutionPolicy Bypass -File scripts\sheet-pages.ps1 -Reference $env:FW_CORPUS\reference -Folders xls,ods -Originals build\sheet-pages-xlsx\summary.json
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\sheet-pages.ps1 -Reference tests\office\reference -Folders excel -Out build\sheet-pages-public
#>
param(
  [Parameter(Mandatory = $true)][string]$Reference,
  # The manifest's top folders to measure (its sources' first part: xlsx, xls, ods).
  [string[]]$Folders = @("xls", "ods"),
  [string]$Featherweight = "build\Release\Featherweight.exe",
  [string]$Fwtool = "build\tools\fwtool\Release\fwtool.exe",
  [string]$Out = "build\sheet-pages",
  [int]$TimeoutSeconds = 300,
  [double]$MinPages = 90,
  # The summary.json of a run over the xlsx folder: the copies are then measured against their originals.
  # Without it, a run whose -Folders include xlsx measures its copies against its own xlsx rows.
  [string]$Originals,
  # Keep the workbooks already measured in -Out (their PDF and comparison); export only the others.
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
$corpus = if ($manifest.corpus) { $manifest.corpus } else { Split-Path -Parent $refRoot }
New-Item -ItemType Directory -Force $Out | Out-Null
$pdfs = Join-Path $Out "pdf"
New-Item -ItemType Directory -Force $pdfs | Out-Null

$entries = @($manifest.files | Where-Object { $Folders -contains ($_.source -split '/')[0] -and $_.pdf -and -not $_.error } | Sort-Object source)
$rows = @()
$n = 0
foreach ($e in $entries) {
  $n++
  $source = Join-Path $corpus ($e.source -replace '/', '\')
  # Excel's PDF export, as S1 measured (its printed PDFs, printPdf, lay columns out a little wider).
  $excelPdf = Join-Path $refRoot ($e.pdf -replace '/', '\')
  $name = ($e.source -replace '[\\/:]', '_')
  $ours = Join-Path $pdfs "$name.pdf"
  $row = [ordered]@{ source = $e.source; copyOf = $e.copyOf; pages = [int]$e.pages; ourPages = 0; passing = 0; error = $null }
  $json = Join-Path $pdfs "$name.compare.json"
  # -Reuse: a workbook already measured in -Out (its PDF, and the comparison made after it) is kept.
  $kept = $Reuse -and (Test-Path $ours) -and (Test-Path $json) -and (Get-Item $json).LastWriteTime -ge (Get-Item $ours).LastWriteTime
  if (-not $kept) {
    foreach ($old in $ours, $json) { if (Test-Path $old) { Remove-Item $old } }
    $log = Join-Path $pdfs "$name.log"
    $p = Start-Process -FilePath $Featherweight -PassThru -WindowStyle Hidden -RedirectStandardError $log -ArgumentList @(
      "--snapshot", "`"$(Join-Path $pdfs "$name.png")`"", "--size", "1400x900", "--script", "`"workbookpdf:$ours`"", "`"$source`"")
    $null = $p.Handle  # so ExitCode is kept
    if (-not $p.WaitForExit($TimeoutSeconds * 1000)) {
      Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
      $row.error = "Featherweight didn't finish within $TimeoutSeconds s"
    } elseif (-not (Test-Path $ours)) {
      $said = @(Get-Content $log -ErrorAction SilentlyContinue | Where-Object { $_ }) -join "; "
      $row.error = "no PDF (exit code $($p.ExitCode))" + $(if ($said) { ": $said" } else { "" })
    } else {
      & $Fwtool pdf-compare --json $json $excelPdf $ours *> $null
    }
  }
  if (-not $row.error) {
    $c = if (Test-Path $json) { Get-Content $json -Raw -Encoding UTF8 | ConvertFrom-Json } else { $null }
    if ($c) {
      $row.ourPages = [int]$c.oursPages
      $row.passing = [int]$c.passing
      if ($c.referencePages) { $row.pages = [int]$c.referencePages }
    } else {
      $row.error = "pdf-compare wrote nothing"
    }
  }
  $rows += [pscustomobject]$row
  "[{0}/{1}] {2}: {3}" -f $n, $entries.Count, $e.source, $(if ($row.error) { "FAILED: $($row.error)" } else { "$($row.passing) of $($row.pages) pages" })
}

$md = @("# Printed pages against Excel's", "", "Reference: $refRoot. Folders: $($Folders -join ', '). $(Get-Date -Format 'yyyy-MM-dd HH:mm').", "",
        "| Folder | Workbooks | Not exported | Excel's pages | Passing (SSIM >= 0.95) | % |", "|---|---|---|---|---|---|")
$ok = $true
function Get-Pct($pass, $pages) { if ($pages) { [Math]::Round(100.0 * $pass / $pages, 2) } else { 0 } }
# The originals' rows (a run over the xlsx folder), by source.
$orig = @{}
if ($Originals) {
  foreach ($r in (Get-Content $Originals -Raw -Encoding UTF8 | ConvertFrom-Json).files) { $orig[$r.source] = $r }
} elseif ($Folders -contains "xlsx") {
  foreach ($r in $rows | Where-Object { ($_.source -split '/')[0] -eq "xlsx" }) { $orig[$r.source] = $r }
}
$compareCopies = $orig.Count -gt 0
$relative = @()  # per folder measured against its originals
foreach ($f in $Folders) {
  $set = @($rows | Where-Object { ($_.source -split '/')[0] -eq $f })
  if (-not $set.Count) { continue }
  $pages = ($set | Measure-Object pages -Sum).Sum
  $pass = ($set | Measure-Object passing -Sum).Sum
  $pct = Get-Pct $pass $pages
  $md += "| $f | $($set.Count) | $(@($set | Where-Object error).Count) | $pages | $pass | $pct |"
  # Copies with an original measured too: the same workbooks on both sides (a copy we couldn't
  # export counts as no page passing; so does an original).
  $pairs = @($set | Where-Object { $_.copyOf -and $orig.ContainsKey($_.copyOf) } | ForEach-Object { [pscustomobject]@{ copy = $_; original = $orig[$_.copyOf] } })
  if ($compareCopies -and $f -eq "xlsx") {
    # The real-world .xlsx rate is a baseline here (S4): the copies are measured against it.
  } elseif ($compareCopies -and $pairs.Count) {
    $cp = ($pairs.copy | Measure-Object pages -Sum).Sum; $cs = ($pairs.copy | Measure-Object passing -Sum).Sum
    $op = ($pairs.original | Measure-Object pages -Sum).Sum; $os = ($pairs.original | Measure-Object passing -Sum).Sum
    $relative += [pscustomobject]@{ folder = $f; workbooks = $pairs.Count; pages = $cp; passing = $cs; pct = (Get-Pct $cs $cp)
                                    originalPages = $op; originalPassing = $os; originalPct = (Get-Pct $os $op); pairs = $pairs }
    if ((Get-Pct $cs $cp) -lt (Get-Pct $os $op)) { $ok = $false }
  } elseif ($pct -lt $MinPages) { $ok = $false }
}
$md += "", "Criterion: >= $MinPages % of the pages pass (S1, plan section 10)$(if ($relative.Count) { "; for $(($relative.folder) -join ' and '): at least their .xlsx originals' rate on the same workbooks (S4)" })$(if ($compareCopies -and $Folders -contains 'xlsx') { '; xlsx: recorded as the baseline' })."
if ($relative.Count) {
  $md += "", "## Against their .xlsx originals (the same workbooks)", "",
         "| Folder | Workbooks | Pages | Passing | % | Originals' pages | Passing | % |", "|---|---|---|---|---|---|---|---|"
  foreach ($r in $relative) { $md += "| $($r.folder) | $($r.workbooks) | $($r.pages) | $($r.passing) | $($r.pct) | $($r.originalPages) | $($r.originalPassing) | $($r.originalPct) |" }
  # Where a copy falls furthest behind its original (by the share of pages passing).
  $behind = @($relative | ForEach-Object { $_.pairs } | ForEach-Object {
      $c = if ($_.copy.pages) { $_.copy.passing / $_.copy.pages } else { 0 }
      $o = if ($_.original.pages) { $_.original.passing / $_.original.pages } else { 0 }
      [pscustomobject]@{ source = $_.copy.source; drop = $o - $c; text = "$($_.copy.passing) of $($_.copy.pages) (the original: $($_.original.passing) of $($_.original.pages))" } } |
    Where-Object { $_.drop -gt 0 } | Sort-Object drop -Descending | Select-Object -First 30)
  if ($behind.Count) { $md += "", "### Furthest behind their originals", ""; $md += @($behind | ForEach-Object { "- $($_.source): $($_.text)" }) }
}
$md += ""
$bad = @($rows | Where-Object { $_.error })
if ($bad.Count) { $md += "## Not exported", ""; $md += @($bad | ForEach-Object { "- $($_.source): $($_.error)" }); $md += "" }
$worst = @($rows | Where-Object { -not $_.error -and $_.pages -gt 0 -and $_.passing -lt $_.pages } | Sort-Object { $_.passing / $_.pages }, source | Select-Object -First 30)
if ($worst.Count) { $md += "## Most pages failing", ""; $md += @($worst | ForEach-Object { "- $($_.source): $($_.passing) of $($_.pages) (ours: $($_.ourPages))" }) }
[IO.File]::WriteAllText((Join-Path $Out "summary.md"), ($md -join "`n") + "`n", [Text.UTF8Encoding]::new($false))
[IO.File]::WriteAllText((Join-Path $Out "summary.json"), ([ordered]@{ reference = $refRoot; folders = $Folders; files = $rows
  againstOriginals = @($relative | Select-Object folder, workbooks, pages, passing, pct, originalPages, originalPassing, originalPct) } | ConvertTo-Json -Depth 5), [Text.UTF8Encoding]::new($false))
""
$md | Where-Object { $_ -like "|*" }
"Summary: $(Join-Path $Out 'summary.md')"
if (-not $ok) { exit 1 }
