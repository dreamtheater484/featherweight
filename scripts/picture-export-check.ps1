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
  The summary goes to <Out>\summary.md and summary.json; exit 1 when a slide fails.

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
  $row = [ordered]@{ deck = $d.FullName; slides = 0; compared = 0; passing = 0; worst = $null; error = $null }
  try {
    $info = Get-DeckInfo $d.FullName
    $row.slides = $info.slides
    $log = Join-Path $dir "app.log"
    $script = "slidepngs:$dir\s.png|$($info.width);exportpdf:$dir\deck.pdf"
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
  } catch { $row.error = "$($_.Exception.Message)" }
  "{0,-60} {1,3}/{2,-3} worst {3} {4}" -f $d.Name, $row.passing, $row.compared, $row.worst, $row.error
  $results += [pscustomobject]$row
}
$total = ($results | Measure-Object compared -Sum).Sum; $pass = ($results | Measure-Object passing -Sum).Sum
$bad = @($results | Where-Object { $_.error -or $_.passing -lt $_.compared })
$md = @("# Picture export against the app's PDF", "", "$pass of $total slides at SSIM >= $MinSsim, in $($results.Count) decks.", "")
foreach ($r in $bad) { $md += "- $([IO.Path]::GetFileName($r.deck)): $($r.passing) of $($r.compared), worst $($r.worst)$(if ($r.error) { "; $($r.error)" })" }
[IO.File]::WriteAllText((Join-Path $Out "summary.md"), ($md -join "`r`n"), [Text.UTF8Encoding]::new($false))
[IO.File]::WriteAllText((Join-Path $Out "summary.json"), ($results | ConvertTo-Json -Depth 4), [Text.UTF8Encoding]::new($false))
"$pass of $total slides at SSIM >= $MinSsim in $($results.Count) decks. Summary: $(Join-Path $Out 'summary.md')"
if ($bad.Count) { exit 1 }
exit 0
