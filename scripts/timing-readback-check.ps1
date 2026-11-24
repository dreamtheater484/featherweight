<#
.SYNOPSIS
  P3's read-back check for transitions and animations on decks Featherweight saved (docs/p3-plan.md,
  "Transitions and animations"; agreed with p3-anim). For each deck in -Decks:
  1. PowerPoint 2021 (scripts\powerpoint-readback.ps1 -SaveCopy) reads it through its object model
     and saves it again as .pptx: <Out>\om\<name>.readback.json and <Out>\resaved\<name>.pptx (a
     .pptx keeps its name; another kind keeps its extension too: links.ppsx.pptx).
  2. `fwtool timing` on Featherweight's file and on PowerPoint's save must print the same lines:
     PowerPoint understood the timing Featherweight wrote, and wrote back the same.
  3. The object model's numbers must match what `fwtool timing` reads from Featherweight's file: per
     slide the transition's duration (when there is one), advance on click and after a time; per main
     sequence effect, in order, the shape, paragraph, exit or not, trigger, duration (one play) and
     delay (1 ms tolerance: PowerPoint reads Appear as 0.001 s), and the repeat count.
  The summary goes to <Out>\summary.md and summary.json; exit 1 when a deck differs, 3 or 4 when
  PowerPoint couldn't run (as powerpoint-readback.ps1: run it again, or with -Reuse once it has).

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\timing-readback-check.ps1 -Decks build\torture-p3\public-1\saved
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\timing-readback-check.ps1 -Decks build\torture-p3\public-1\saved -Reuse
#>
param(
  [Parameter(Mandatory = $true)][string]$Decks,
  [string]$Out = "build\timing-readback",
  [string]$Fwtool = "build\tools\fwtool\Release\fwtool.exe",
  # Keep PowerPoint's read-backs and saves already in -Out: only steps 2 and 3 run.
  [switch]$Reuse
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$Out = if ([IO.Path]::IsPathRooted($Out)) { $Out } else { [IO.Path]::GetFullPath((Join-Path $root $Out)) }
$Decks = (Resolve-Path $Decks).Path.TrimEnd('\')
$Fwtool = if ([IO.Path]::IsPathRooted($Fwtool)) { $Fwtool } else { Join-Path $root $Fwtool }
$om = Join-Path $Out "om"; $resaved = Join-Path $Out "resaved"
New-Item -ItemType Directory -Force $Out, $om, $resaved | Out-Null

if (-not $Reuse) {
  & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "powerpoint-readback.ps1") `
    -Decks $Decks -Out $om -SaveCopy $resaved
  if ($LASTEXITCODE -eq 3 -or $LASTEXITCODE -eq 4) { "PowerPoint couldn't run (exit $LASTEXITCODE): run this again."; exit $LASTEXITCODE }
}

function Get-TimingLines([string]$path) {
  # fwtool prints the deck's path first; the rest is the timing.
  $lines = @(& $Fwtool timing $path 2>&1 | ForEach-Object { "$_" })
  if ($lines.Count) { $lines = @($lines | Select-Object -Skip 1) }
  $lines
}
function Read-Timing([string[]]$lines) {
  $slides = @{}; $cur = $null
  foreach ($l in $lines) {
    if ($l -match '^\s*slide (\d+): transition (.+?) (\d+) ms, click (\d), after (-?\d+) ms') {
      $cur = @{ transition = $Matches[2]; duration = [int]$Matches[3]; click = $Matches[4] -eq "1"; after = [int]$Matches[5]; effects = @() }
      $slides[[int]$Matches[1]] = $cur
    } elseif ($cur -and $l -match '^\s*\d+\. shape (\d+)(?: paragraph (\d+))? (\w+) .*?, (click|with|after), (\d+) ms, delay (\d+) ms(?:, repeat (\d+|until the end of the slide))?') {
      # Duration is one play, as PowerPoint's Timing shows it; repeat is printed only when it isn't 1.
      $rep = $Matches[7]  # before another -match replaces $Matches
      $cur.effects += @{ shape = [int]$Matches[1]; paragraph = $(if ($Matches[2]) { [int]$Matches[2] + 1 } else { 0 })
        class = $Matches[3]; trigger = $Matches[4]; duration = [int]$Matches[5]; delay = [int]$Matches[6]
        repeat = $(if (-not $rep) { 1 } elseif ($rep -match '^\d+$') { [int]$rep } else { $null }) }
    }
  }
  $slides
}
$triggers = @{ msoAnimTriggerOnPageClick = "click"; msoAnimTriggerWithPrevious = "with"; msoAnimTriggerAfterPrevious = "after" }

# The comments as the file holds them, per slide index (presentation order): each thread's author and
# text, and its replies'. Modern comments (p188, ppt/comments/modernComment_*.xml with ppt/authors.xml)
# and legacy ones (p:cm, ppt/comments/commentN.xml with ppt/commentAuthors.xml; a p15:threadingInfo
# parentCm makes one a reply).
Add-Type -AssemblyName System.IO.Compression.FileSystem
function Get-Norm([string]$s) { (($s -replace '[\s\u00A0]+', ' ').Trim()) }
function Read-FileComments([string]$deck) {
  $out = @{}
  $z = [IO.Compression.ZipFile]::OpenRead($deck)
  try {
    $read = { param($n) $e = $z.GetEntry($n.TrimStart('/')); if (-not $e) { return $null }; $r = New-Object IO.StreamReader($e.Open()); $t = $r.ReadToEnd(); $r.Close(); [xml]$t }
    $ns = @{ p = "http://schemas.openxmlformats.org/presentationml/2006/main"; a = "http://schemas.openxmlformats.org/drawingml/2006/main"
             p188 = "http://schemas.microsoft.com/office/powerpoint/2018/8/main"; p15 = "http://schemas.microsoft.com/office/powerpoint/2012/main"
             r = "http://schemas.openxmlformats.org/officeDocument/2006/relationships"; rel = "http://schemas.openxmlformats.org/package/2006/relationships" }
    $text = { param($node) Get-Norm ((@(Select-Xml -Xml $node -XPath ".//a:t" -Namespace $ns | ForEach-Object { $_.Node.InnerText }) -join " ")) }
    $authors = @{}
    $ax = & $read "ppt/authors.xml"
    if ($ax) { foreach ($a in (Select-Xml -Xml $ax -XPath "//p188:author" -Namespace $ns)) { $authors[$a.Node.id] = $a.Node.name } }
    $lx = & $read "ppt/commentAuthors.xml"
    if ($lx) { foreach ($a in (Select-Xml -Xml $lx -XPath "//p:cmAuthor" -Namespace $ns)) { $authors["legacy:" + $a.Node.id] = $a.Node.name } }
    $pres = & $read "ppt/presentation.xml"; $prels = & $read "ppt/_rels/presentation.xml.rels"
    $targets = @{}; foreach ($x in $prels.Relationships.Relationship) { $targets[$x.Id] = $x.Target }
    $i = 0
    foreach ($sid in (Select-Xml -Xml $pres -XPath "//p:sldIdLst/p:sldId" -Namespace $ns)) {
      $i++
      $part = "ppt/" + ($targets[$sid.Node.GetAttribute("id", $ns.r)] -replace '^/?ppt/', '')
      $relsPart = ($part -replace '/([^/]+)$', '/_rels/$1') + ".rels"
      $srels = & $read $relsPart
      $threads = @()
      if ($srels) {
        foreach ($x in $srels.Relationships.Relationship) {
          # Only comment parts: a hyperlink's target is a URL, which isn't a path (TargetMode="External").
          if ($x.Type -notlike "*/relationships/comments" -or $x.TargetMode -eq "External") { continue }
          $cpart = [IO.Path]::GetFullPath([IO.Path]::Combine("C:\x\" + ($part -replace '/[^/]+$', '').Replace('/', '\'), $x.Target.Replace('/', '\'))).Substring(5).Replace('\', '/')
          if ($x.Target.StartsWith('/')) { $cpart = $x.Target.TrimStart('/') }
          if ($x.Type -like "*/2018/10/relationships/comments") {
            $cx = & $read $cpart
            if (-not $cx) { continue }
            foreach ($cm in (Select-Xml -Xml $cx -XPath "//p188:cm" -Namespace $ns)) {
              $body = Select-Xml -Xml $cm.Node -XPath "./p188:txBody" -Namespace $ns | Select-Object -First 1
              $t = @{ author = [string]$authors[$cm.Node.authorId]; text = $(if ($body) { & $text $body.Node } else { "" }); replies = @() }
              foreach ($rp in (Select-Xml -Xml $cm.Node -XPath "./p188:replyLst/p188:reply" -Namespace $ns)) {
                $rb2 = Select-Xml -Xml $rp.Node -XPath "./p188:txBody" -Namespace $ns | Select-Object -First 1
                $t.replies += @{ author = [string]$authors[$rp.Node.authorId]; text = $(if ($rb2) { & $text $rb2.Node } else { "" }) }
              }
              $threads += $t
            }
          } elseif ($x.Type -like "*/relationships/comments") {
            $cx = & $read $cpart
            if (-not $cx) { continue }
            $byIdx = @{}; $legacy = @()
            foreach ($cm in (Select-Xml -Xml $cx -XPath "//p:cm" -Namespace $ns)) {
              $parent = Select-Xml -Xml $cm.Node -XPath ".//p15:parentCm" -Namespace $ns | Select-Object -First 1
              $c = @{ author = [string]$authors["legacy:" + $cm.Node.authorId]; text = (Get-Norm $cm.Node.text); replies = @()
                      key = "$($cm.Node.authorId)/$($cm.Node.idx)"; parent = $(if ($parent) { "$($parent.Node.authorId)/$($parent.Node.idx)" } else { $null }) }
              $byIdx[$c.key] = $c; $legacy += $c
            }
            foreach ($c in $legacy) { if ($c.parent -and $byIdx[$c.parent]) { $byIdx[$c.parent].replies += $c } else { $threads += $c } }
          }
        }
      }
      $out[$i] = $threads
    }
  } finally { $z.Dispose() }
  $out
}
# A thread as one line, for comparing as sets per slide: the author and text, then its replies'.
function Get-ThreadKey($t) { "$($t.author): $(Get-Norm $t.text)" + (@($t.replies | ForEach-Object { " | $($_.author): $(Get-Norm $_.text)" }) -join "") }

$results = @()
$files = Get-ChildItem -LiteralPath $Decks -Recurse -File | Where-Object { $_.Extension -in ".pptx", ".ppsx", ".potx", ".pptm" } | Sort-Object FullName
# What the object model reports that Featherweight rightly reads otherwise: Cut has no duration
# (PowerPoint greys the box out), a media play step lasts as long as its clip, and "until the end of
# the slide" effects are indefinite.
$noDuration = '^ppEffectCut'
$mediaEffects = '^msoAnimEffectMedia'
foreach ($f in $files) {
  $key = $f.FullName.Substring($Decks.Length + 1) -replace '\\', '__'
  $r = [ordered]@{ deck = $f.FullName.Substring($Decks.Length + 1); status = "ok"; resave = @(); om = @(); comments = @(); commentThreads = 0 }
  if ($key -match '(^|__)protected__') { $r.status = "encrypted: not checked (fwtool timing takes no password)"; $results += $r; continue }
  # As powerpoint-readback.ps1 -SaveCopy names it: a .pptx by its name, another kind with its extension too.
  $copy = Join-Path $resaved $(if ($f.Extension -ieq ".pptx") { $f.Name } else { $f.Name + ".pptx" })
  $json = Join-Path $om "$key.readback.json"
  if (-not (Test-Path $copy) -or -not (Test-Path $json)) { $r.status = "not read back"; $results += $r; continue }
  $ours = @(Get-TimingLines $f.FullName)
  $theirs = @(Get-TimingLines $copy)
  # Step 2: line for line.
  $n = [Math]::Max($ours.Count, $theirs.Count)
  for ($i = 0; $i -lt $n; $i++) {
    $a = if ($i -lt $ours.Count) { $ours[$i] } else { "(none)" }
    $b = if ($i -lt $theirs.Count) { $theirs[$i] } else { "(none)" }
    if ($a -ne $b) { $r.resave += "ours: $($a.Trim()) | PowerPoint's save: $($b.Trim())" }
  }
  # Step 3: the object model's numbers.
  $t = Read-Timing $ours
  $rb = Get-Content -LiteralPath $json -Raw -Encoding UTF8 | ConvertFrom-Json  # UTF-8 without a BOM: not ANSI
  if ($rb.error) { $r.status = "PowerPoint: $($rb.error)"; $results += $r; continue }
  foreach ($s in $rb.slides) {
    $x = $t[[int]$s.index]
    # fwtool prints no slide lines for a deck with no transition and no timing at all.
    if (-not $x) { $x = @{ transition = "None"; duration = 0; click = $true; after = -1; effects = @() } }
    $tr = $s.transition
    if ($tr.effect -ne "ppEffectNone" -and $tr.effect -notmatch $noDuration -and [Math]::Abs([Math]::Round($tr.duration * 1000) - $x.duration) -gt 1) {
      $r.om += "slide $($s.index): transition $($tr.duration) s in PowerPoint, $($x.duration) ms in Featherweight" }
    if ([bool]$tr.advanceOnClick -ne $x.click) { $r.om += "slide $($s.index): advance on click $($tr.advanceOnClick) in PowerPoint" }
    $after = if ($tr.advanceOnTime) { [int][Math]::Round($tr.advanceTime * 1000) } else { -1 }
    if ([Math]::Abs($after - $x.after) -gt 1) { $r.om += "slide $($s.index): advance after $after ms in PowerPoint, $($x.after) ms in Featherweight" }
    $main = @($s.main)
    if ($main.Count -ne $x.effects.Count) { $r.om += "slide $($s.index): $($main.Count) effects in PowerPoint, $($x.effects.Count) in Featherweight"; continue }
    for ($i = 0; $i -lt $main.Count; $i++) {
      $e = $main[$i]; $y = $x.effects[$i]; $why = @()
      if ([int]$e.shapeId -ne $y.shape) { $why += "shape $($e.shapeId) vs $($y.shape)" }
      if ([int]$e.paragraph -ne $y.paragraph) { $why += "paragraph $($e.paragraph) vs $($y.paragraph)" }
      if ([bool]$e.exit -ne ($y.class -eq "exit")) { $why += "exit $($e.exit) vs $($y.class)" }
      if ($triggers[$e.trigger] -ne $y.trigger) { $why += "trigger $($e.trigger) vs $($y.trigger)" }
      $d = [double]$e.duration
      if (-not [double]::IsInfinity($d) -and $e.effect -notmatch $mediaEffects -and [Math]::Abs([Math]::Round($d * 1000) - $y.duration) -gt 1) {
        $why += "duration $($e.duration) s (repeat $($e.repeat)) vs $($y.duration) ms" }
      if ([Math]::Abs([Math]::Round($e.delay * 1000) - $y.delay) -gt 1) { $why += "delay $($e.delay) s vs $($y.delay) ms" }
      # Until the end of the slide: not compared (the object model's count for it isn't a number of plays).
      if ($null -ne $y.repeat -and $null -ne $e.repeat -and [Math]::Abs([double]$e.repeat - $y.repeat) -gt 0.001) { $why += "repeat $($e.repeat) vs $($y.repeat)" }
      if ($why) { $r.om += "slide $($s.index) effect $($i + 1): " + ($why -join ", ") }
    }
  }
  # Step 4: the comments PowerPoint reads are the ones the file holds (threads, replies, authors, text).
  try {
    $fc = Read-FileComments $f.FullName
    foreach ($s in $rb.slides) {
      $mine = @($fc[[int]$s.index] | ForEach-Object { Get-ThreadKey $_ } | Sort-Object)
      $theirs = @($s.comments | ForEach-Object { Get-ThreadKey $_ } | Sort-Object)
      $r.commentThreads += $mine.Count
      if (($mine -join "`n") -ne ($theirs -join "`n")) {
        $r.comments += "slide $($s.index): the file has $($mine.Count) threads, PowerPoint reads $($theirs.Count)" +
          $(if ($mine.Count -eq $theirs.Count) { "; first difference: " + (@(Compare-Object $mine $theirs | Select-Object -First 1 | ForEach-Object { "$($_.SideIndicator) $($_.InputObject)" }) -join "") } else { "" })
      }
    }
  } catch { $r.comments += "comments not read: $($_.Exception.Message)" }
  if ($r.resave.Count -or $r.om.Count -or $r.comments.Count) { $r.status = "differs" }
  $results += $r
}

$bad = @($results | Where-Object { $_.status -ne "ok" })
$withComments = @($results | Where-Object { $_.commentThreads -gt 0 })
$commentsSame = @($withComments | Where-Object { -not @($_.comments).Count }).Count
$md = @("# Timing read-back check", "", "Decks: $Decks ($($results.Count)); $($results.Count - $bad.Count) read back the same.",
        "Comments: $commentsSame of $($withComments.Count) decks with comments ($((@($withComments | ForEach-Object { $_.commentThreads }) | Measure-Object -Sum).Sum) threads) read back the same.", "")
foreach ($r in $bad) {
  $md += "## $($r.deck): $($r.status)"
  $md += @($r.resave | Select-Object -First 20 | ForEach-Object { "- resave: $_" })
  $md += @($r.om | Select-Object -First 20 | ForEach-Object { "- object model: $_" })
  $md += @($r.comments | Select-Object -First 10 | ForEach-Object { "- comments: $_" })
  $md += ""
}
[IO.File]::WriteAllText((Join-Path $Out "summary.md"), ($md -join "`r`n"), [Text.UTF8Encoding]::new($false))
[IO.File]::WriteAllText((Join-Path $Out "summary.json"), ($results | ConvertTo-Json -Depth 5), [Text.UTF8Encoding]::new($false))
"$($results.Count - $bad.Count) of $($results.Count) decks read back the same in PowerPoint. Summary: $(Join-Path $Out 'summary.md')"
if ($bad.Count) { exit 1 }
exit 0
