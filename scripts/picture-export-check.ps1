<#
.SYNOPSIS
  P3's picture export check (docs/p3-plan.md, "Templates and pictures"; TEST_CHECKLIST's P3 gate):
  every slide Featherweight exports as a picture (File > Export > All slides as PNG) looks as the
  slide does in the app's own PDF (File > Export PDF) at the same size. For each deck, one --snapshot
  run of the app (hidden, its own settings, never a running instance) plays
  `slidepngs:<dir>\s.png|<width>;exportpdf:<dir>\deck.pdf`, with the width the slide has at 100 dpi;
  then `fwtool pdf-compare --pngs` renders the PDF at 100 dpi and compares each slide's picture with
  its page (SSIM; a slide passes at >= -MinSsim, 0.99 by default). Hidden slides are exported as
  pictures but not in the PDF, so a deck with hidden slides is compared on the shown ones only.
  The criterion, as the coordinator restated it (p3-plan, 2026-11-24): a picture equals the view's own
  drawing, SSIM >= -MinViewSsim (0.999) against P1's page renderer (the same run's
  `pagepngs:<dir>\v|100`, every slide, hidden ones too) at the same size (scripts\png-ssim.py, fwtool's
  SSIM). Against the PDF it's >= 0.99, or >= -PdfFloor (0.95) on slides with dense small text, where
  pdfium rasterizes glyphs differently from DirectWrite.
  The summary goes to <Out>\summary.md and summary.json; exit 1 when a picture misses the page
  renderer's drawing or the PDF's floor.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\picture-export-check.ps1
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\picture-export-check.ps1 -Paths D:\corpus\pptx\charts -Out build\picture-export-charts
#>
param(
  [string[]]$Paths = @("tests\office\presentations"),
  [string]$Featherweight = "build\Release\Featherweight.exe",
  [string]$Fwtool = "build\tools\fwtool\Release\fwtool.exe",
  [string]$Out = "build\picture-export",
  [double]$MinSsim = 0.99,
  [double]$MinViewSsim = 0.999,
  [double]$PdfFloor = 0.95,
  # Python 3 with Pillow and numpy, for scripts\png-ssim.py.
  [string]$Python = "python",
  [int]$TimeoutSeconds = 300
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
Add-Type -AssemblyName System.IO.Compression.FileSystem
foreach ($v in "Featherweight", "Fwtool") {
  $p = Get-Variable $v -ValueOnly
  if (-not [IO.Path]::IsPathRooted($p)) { Set-Variable $v ([IO.Path]::GetFullPath((Join-Path $root $p))) }
  if (-not (Test-Path (Get-Variable $v -ValueOnly))) { throw "$v not found at $(Get-Variable $v -ValueOnly): build it, or pass -$v." }
}
if (-not [IO.Path]::IsPathRooted($Out)) { $Out = [IO.Path]::GetFullPath((Join-Path $root $Out)) }
New-Item -ItemType Directory -Force $Out | Out-Null

# The slide's width in pixels at 100 dpi, and which slides are hidden (in presentation order).
function Get-DeckInfo([string]$deck) {
  $z = [IO.Compression.ZipFile]::OpenRead($deck)
  try {
    $read = { param($n) $e = $z.GetEntry($n); if (-not $e) { return "" }; $r = New-Object IO.StreamReader($e.Open()); $t = $r.ReadToEnd(); $r.Close(); $t }
    $p = & $read "ppt/presentation.xml"; $rels = & $read "ppt/_rels/presentation.xml.rels"
    $cx = [double]([regex]::Match($p, '<p:sldSz[^>]*\bcx="(\d+)"').Groups[1].Value)
    $hidden = @()
    $i = 0
    foreach ($m in [regex]::Matches($p, '<p:sldId [^>]*r:id="([^"]+)"')) {
      $i++
      $rid = $m.Groups[1].Value
      $target = [regex]::Match($rels, "<Relationship [^>]*Id=`"$rid`"[^>]*>").Value
      $target = [regex]::Match($target, 'Target="([^"]+)"').Groups[1].Value -replace '^/?ppt/', ''
      $x = & $read ("ppt/" + $target)
      if ($x -match '<p:sld [^>]*\bshow="(0|false)"') { $hidden += $i }
    }
    [pscustomobject]@{ width = [int][Math]::Round($cx / 914400 * 100); slides = $i; hidden = $hidden }
  } finally { $z.Dispose() }
}

$decks = @(foreach ($p in $Paths) {
  if (Test-Path -LiteralPath $p -PathType Container) { Get-ChildItem -LiteralPath $p -Recurse -File | Where-Object { $_.Extension -in ".pptx", ".pptm", ".ppsx", ".potx" } }
  else { Get-Item -LiteralPath $p }
}) | Sort-Object FullName
$results = @()
foreach ($d in $decks) {
  $name = [IO.Path]::GetFileNameWithoutExtension($d.Name)
  $dir = Join-Path $Out $name
  if (Test-Path $dir) { Remove-Item -Recurse -Force $dir }
  New-Item -ItemType Directory -Force $dir | Out-Null
  $row = [ordered]@{ deck = $d.FullName; slides = 0; compared = 0; passing = 0; worst = $null
                     viewCompared = 0; viewPassing = 0; viewWorst = $null; error = $null }
  try {
    $info = Get-DeckInfo $d.FullName
    $row.slides = $info.slides
    $log = Join-Path $dir "app.log"
    $script = "slidepngs:$dir\s.png|$($info.width);exportpdf:$dir\deck.pdf;pagepngs:$dir\v|100"
    $proc = Start-Process -FilePath $Featherweight -PassThru -WindowStyle Hidden -RedirectStandardError $log -ArgumentList @(
      "--snapshot", "`"$dir\window.png`"", "--size", "1400x900", "--script", "`"$script`"", "`"$($d.FullName)`"")
    $null = $proc.Handle
    if (-not $proc.WaitForExit($TimeoutSeconds * 1000)) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue; throw "Featherweight didn't finish within $TimeoutSeconds s" }
    $pngDir = Join-Path $dir "s"
    if (-not (Test-Path "$dir\deck.pdf")) { throw "no PDF: " + ((Get-Content $log -ErrorAction SilentlyContinue) -join "; ") }
    $pngs = @(Get-ChildItem $pngDir -Filter *.png -ErrorAction SilentlyContinue)
    if (-not $pngs.Count) { throw "no pictures: " + ((Get-Content $log -ErrorAction SilentlyContinue) -join "; ") }
    # The PDF has the shown slides only: number the shown slides' pictures as its pages.
    $cmp = Join-Path $dir "pages"
    New-Item -ItemType Directory -Force $cmp | Out-Null
    $page = 0
    for ($i = 1; $i -le $info.slides; $i++) {
      if ($info.hidden -contains $i) { continue }
      $page++
      $src = Join-Path $pngDir "Slide$i.png"
      if (Test-Path $src) { Copy-Item $src (Join-Path $cmp "page-$page.png") }
    }
    $json = Join-Path $dir "compare.json"
    & $Fwtool pdf-compare --pngs $cmp --min-ssim $MinSsim --json $json --out (Join-Path $dir "diffs") "$dir\deck.pdf" | Out-Null
    $c = Get-Content $json -Raw | ConvertFrom-Json
    $ssims = @($c.pages | ForEach-Object { [double]$_.ssim })
    $row.compared = $ssims.Count
    $row.passing = @($ssims | Where-Object { $_ -ge $MinSsim }).Count
    if ($ssims.Count) { $row.worst = [Math]::Round(($ssims | Measure-Object -Minimum).Minimum, 4) }
    if ([int]$c.oursPages -ne [int]$c.referencePages) { $row.error = "$($c.oursPages) pictures against $($c.referencePages) PDF pages" }
    # The criterion: each slide's picture against the page renderer's drawing of it (page-N.png is slide N).
    $pairs = @(for ($i = 1; $i -le $info.slides; $i++) { Join-Path $pngDir "Slide$i.png"; Join-Path $dir "v\page-$i.png" })
    $vjson = Join-Path $dir "view.json"
    & $Python -I (Join-Path $root "scripts\png-ssim.py") --json $vjson --pairs @pairs
    if (-not (Test-Path $vjson)) { throw "png-ssim.py wrote nothing (Python with Pillow and numpy: -Python)" }
    $v = @((Get-Content $vjson -Raw | ConvertFrom-Json).pairs)
    $vs = @($v | Where-Object { $null -ne $_.ssim } | ForEach-Object { [double]$_.ssim })
    $row.viewCompared = $v.Count
    $row.viewPassing = @($vs | Where-Object { $_ -ge $MinViewSsim }).Count
    if ($vs.Count) { $row.viewWorst = [Math]::Round(($vs | Measure-Object -Minimum).Minimum, 6) }
    $verr = @($v | Where-Object { $_.error } | ForEach-Object { "$([IO.Path]::GetFileName($_.a)): $($_.error)" })
    if ($verr.Count) { $row.error = (@($row.error) + $verr | Where-Object { $_ }) -join "; " }
  } catch { $row.error = "$($_.Exception.Message)" }
  "{0,-60} view {1,3}/{2,-3} worst {3}  PDF {4,3}/{5,-3} worst {6} {7}" -f $d.Name, $row.viewPassing, $row.viewCompared, $row.viewWorst,
    $row.passing, $row.compared, $row.worst, $row.error
  $results += [pscustomobject]$row
}
$total = ($results | Measure-Object compared -Sum).Sum; $pass = ($results | Measure-Object passing -Sum).Sum
$vtotal = ($results | Measure-Object viewCompared -Sum).Sum; $vpass = ($results | Measure-Object viewPassing -Sum).Sum
$floor = @($results | Where-Object { $null -ne $_.worst -and $_.worst -lt $PdfFloor })
$bad = @($results | Where-Object { $_.error -or $_.viewPassing -lt $_.viewCompared })
$md = @("# Picture export", "", "Against the page renderer's drawing at the same size (the criterion): $vpass of $vtotal slides at SSIM >= $MinViewSsim, in $($results.Count) decks.",
        "Against the app's PDF: $pass of $total slides at SSIM >= $MinSsim; $(@($floor).Count) decks with a slide under $PdfFloor.", "")
foreach ($r in $bad) { $md += "- $([IO.Path]::GetFileName($r.deck)): page renderer $($r.viewPassing) of $($r.viewCompared), worst $($r.viewWorst)$(if ($r.error) { "; $($r.error)" })" }
foreach ($r in @($results | Where-Object { $_.passing -lt $_.compared })) { $md += "- $([IO.Path]::GetFileName($r.deck)): PDF $($r.passing) of $($r.compared), worst $($r.worst)" }
[IO.File]::WriteAllText((Join-Path $Out "summary.md"), ($md -join "`r`n"), [Text.UTF8Encoding]::new($false))
[IO.File]::WriteAllText((Join-Path $Out "summary.json"), ($results | ConvertTo-Json -Depth 4), [Text.UTF8Encoding]::new($false))
"Page renderer: $vpass of $vtotal slides at SSIM >= $MinViewSsim. PDF: $pass of $total at >= $MinSsim. Summary: $(Join-Path $Out 'summary.md')"
if ($bad.Count -or $floor.Count) { exit 1 }
exit 0
