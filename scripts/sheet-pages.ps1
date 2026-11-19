<#
.SYNOPSIS
  Printed pages against Excel's (S1's display criterion, used again in S4 for the .xls and .ods
  corpora): for every workbook in a reference manifest (scripts\reference-renders.ps1) under the
  given folders, Featherweight exports every sheet to PDF as Excel's reference PDFs have them
  (the app's workbookpdf: script op), and fwtool pdf-compare measures each page against Excel's
  PDF (SSIM at 100 dpi; a page passes at >= 0.95). The criterion: >= 90 % of the pages pass.

  Featherweight: a --snapshot run of its own per workbook (hidden, its own settings), never a
  running instance; ended if it doesn't finish in time. The summary goes to <out>\summary.md and
  summary.json (per workbook: pages, passing, or why there's none).

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\sheet-pages.ps1 -Reference $env:FW_CORPUS\reference -Folders xls,ods
#>
param(
  [Parameter(Mandatory = $true)][string]$Reference,
  # The manifest's top folders to measure (its sources' first part: xlsx, xls, ods).
  [string[]]$Folders = @("xls", "ods"),
  [string]$Featherweight = "build\Release\Featherweight.exe",
  [string]$Fwtool = "build\tools\fwtool\Release\fwtool.exe",
  [string]$Out = "build\sheet-pages",
  [int]$TimeoutSeconds = 300,
  [double]$MinPages = 90
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
  $excelPdf = Join-Path $refRoot ($e.pdf -replace '/', '\')
  $name = ($e.source -replace '[\\/:]', '_')
  $ours = Join-Path $pdfs "$name.pdf"
  $row = [ordered]@{ source = $e.source; copyOf = $e.copyOf; pages = [int]$e.pages; ourPages = 0; passing = 0; error = $null }
  if (Test-Path $ours) { Remove-Item $ours }
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
    $json = Join-Path $pdfs "$name.compare.json"
    & $Fwtool pdf-compare --json $json $excelPdf $ours *> $null
    $c = if (Test-Path $json) { Get-Content $json -Raw | ConvertFrom-Json } else { $null }
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
foreach ($f in $Folders) {
  $set = @($rows | Where-Object { ($_.source -split '/')[0] -eq $f })
  if (-not $set.Count) { continue }
  $pages = ($set | Measure-Object pages -Sum).Sum
  $pass = ($set | Measure-Object passing -Sum).Sum
  $pct = if ($pages) { [Math]::Round(100.0 * $pass / $pages, 2) } else { 0 }
  if ($pct -lt $MinPages) { $ok = $false }
  $md += "| $f | $($set.Count) | $(@($set | Where-Object error).Count) | $pages | $pass | $pct |"
}
$md += "", "Criterion: >= $MinPages % of the pages pass (S1, plan section 10).", ""
$bad = @($rows | Where-Object { $_.error })
if ($bad.Count) { $md += "## Not exported", ""; $md += @($bad | ForEach-Object { "- $($_.source): $($_.error)" }); $md += "" }
$worst = @($rows | Where-Object { -not $_.error -and $_.pages -gt 0 -and $_.passing -lt $_.pages } | Sort-Object { $_.passing / $_.pages }, source | Select-Object -First 30)
if ($worst.Count) { $md += "## Most pages failing", ""; $md += @($worst | ForEach-Object { "- $($_.source): $($_.passing) of $($_.pages) (ours: $($_.ourPages))" }) }
[IO.File]::WriteAllText((Join-Path $Out "summary.md"), ($md -join "`n") + "`n", [Text.UTF8Encoding]::new($false))
[IO.File]::WriteAllText((Join-Path $Out "summary.json"), ([ordered]@{ reference = $refRoot; folders = $Folders; files = $rows } | ConvertTo-Json -Depth 5), [Text.UTF8Encoding]::new($false))
""
$md | Where-Object { $_ -like "|*" }
"Summary: $(Join-Path $Out 'summary.md')"
if (-not $ok) { exit 1 }
