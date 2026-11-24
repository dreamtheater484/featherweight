<#
.SYNOPSIS
  P3's table of PowerPoint's object-model names for transitions and animations against what PowerPoint
  writes in the file (tests\office\powerpoint-effects.json), for the read-back check and the tools
  (never the app: p3-anim's TimingOps maps the file's terms to the gallery's itself).

  1. PowerPoint 2021 builds a deck through its object model (needs PowerPoint; -Parse skips it): a
     slide per PpEntryEffect (its title the name), and shapes named after each MsoAnimEffect added
     on click, its exit variant (Exit set), and the common effects with each MsoAnimDirection. Each
     setting PowerPoint refuses is listed. The deck is saved as <Out>\effects-map.pptx, never over
     anything else.
  2. The deck's XML is read (no PowerPoint): per name, the transition element PowerPoint writes (in
     its mc:Choice when it has one, with the Fallback), the duration it gives, and per effect its
     presetClass, presetID and presetSubtype.

  PowerPoint runs as in scripts\make-deck-copies.ps1: only from Roy's active, unlocked session
  (exit 3), never with a PowerPoint this script didn't start or another job's quiet-pc.lock (exit 4),
  its own instance, hidden, macros forced off, alerts off, always quit or ended.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\powerpoint-effects-map.ps1
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\powerpoint-effects-map.ps1 -Parse
#>
param(
  [string]$Out = "D:\Documents\Codex\FeatherweightCorpus\made\map",
  [string]$Json = "tests\office\powerpoint-effects.json",
  [switch]$Parse,  # only step 2, from the deck step 1 made
  [int]$TimeoutSeconds = 900,
  [string]$Child, [string]$Deck, [string]$ResultFile
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$quietLock = "D:\Documents\Codex\FeatherweightNotes\quiet-pc.lock"
function Write-Json($value, [string]$path) { [IO.File]::WriteAllText($path, ($value | ConvertTo-Json -Depth 6), [Text.UTF8Encoding]::new($false)) }
[void][Reflection.Assembly]::LoadWithPartialName("Microsoft.Office.Interop.PowerPoint")
function Get-EnumNames([string]$type) { [Enum]::GetNames([type]"Microsoft.Office.Interop.PowerPoint.$type") }
function Get-EnumValue([string]$type, [string]$name) { [int][Enum]::Parse([type]"Microsoft.Office.Interop.PowerPoint.$type", $name) }
# The effects whose directions are listed: the gallery's common ones (plan §8.2) and their old twins.
$directional = "msoAnimEffectFly", "msoAnimEffectWipe", "msoAnimEffectSplit", "msoAnimEffectFadedZoom", "msoAnimEffectZoom",
               "msoAnimEffectAscend", "msoAnimEffectFloat", "msoAnimEffectSpin", "msoAnimEffectGrowShrink", "msoAnimEffectWheel"

if ($Child) {
  $result = [ordered]@{ pid = @(); powerpoint = $null; refused = @() }
  $pp = $null; $d = $null
  try {
    if (@(Get-Process POWERPNT -ErrorAction SilentlyContinue).Count) { throw "a PowerPoint this script didn't start is running" }
    $since = Get-Date
    $pp = New-Object -ComObject PowerPoint.Application
    $new = @(Get-Process POWERPNT -ErrorAction SilentlyContinue | Where-Object { $_.StartTime -ge $since.AddSeconds(-5) } | ForEach-Object Id)
    if (-not $new.Count) { [void][Runtime.InteropServices.Marshal]::ReleaseComObject($pp); $pp = $null; throw "COM attached to a PowerPoint this script didn't start" }
    $result.pid = $new
    Write-Json $result $ResultFile
    $pp.AutomationSecurity = 3; $pp.DisplayAlerts = 1
    $result.powerpoint = "PowerPoint $($pp.Version) (build $($pp.Build))"
    $d = $pp.Presentations.Add(0)
    $titleOnly = Get-EnumValue PpSlideLayout ppLayoutTitleOnly
    foreach ($t in Get-EnumNames PpEntryEffect) {
      if ($t -eq "ppEffectMixed") { continue }
      $s = $d.Slides.Add($d.Slides.Count + 1, $titleOnly)
      $s.Shapes.Title.TextFrame.TextRange.Text = "tr:$t"
      try { $s.SlideShowTransition.EntryEffect = (Get-EnumValue PpEntryEffect $t) }
      catch { $result.refused += "transition $t"; $s.Delete() }
    }
    # Effects: 24 shapes a slide, named after what they get.
    $slide = $null; $n = 0
    function Add-Named([string]$name) {
      if (-not $script:slide -or $script:n -ge 24) {
        $script:slide = $d.Slides.Add($d.Slides.Count + 1, $titleOnly)
        $script:slide.Shapes.Title.TextFrame.TextRange.Text = "effects"
        $script:n = 0
      }
      $b = $script:slide.Shapes.AddShape(1, 20 + ($script:n % 6) * 150, 110 + [Math]::Floor($script:n / 6) * 100, 130, 80)
      $b.Name = $name
      $script:n++
      $b
    }
    $click = Get-EnumValue MsoAnimTriggerType msoAnimTriggerOnPageClick
    foreach ($a in Get-EnumNames MsoAnimEffect) {
      if ($a -match 'Mixed|Custom$') { continue }
      $value = Get-EnumValue MsoAnimEffect $a
      foreach ($exit in $false, $true) {
        $b = Add-Named $(if ($exit) { "exit:$a" } else { "fx:$a" })
        try {
          $e = $b.Parent.TimeLine.MainSequence.AddEffect($b, $value, 0, $click)
          if ($exit) { $e.Exit = -1 }
        } catch { $result.refused += "$(if ($exit) { 'exit' } else { 'effect' }) $a"; $b.Delete(); $script:n-- }
      }
    }
    foreach ($a in $directional) {
      foreach ($dir in Get-EnumNames MsoAnimDirection) {
        if ($dir -match 'Mixed|None|OrdinalMask') { continue }
        $b = Add-Named "dir:${a}:$dir"
        try {
          $e = $b.Parent.TimeLine.MainSequence.AddEffect($b, (Get-EnumValue MsoAnimEffect $a), 0, $click)
          $e.EffectParameters.Direction = (Get-EnumValue MsoAnimDirection $dir)
        } catch { $b.Delete(); $script:n-- }  # most directions don't apply to most effects: not listed
      }
    }
    if (Test-Path -LiteralPath $Deck) { Remove-Item -LiteralPath $Deck }
    $d.SaveAs($Deck, 24)  # ppSaveAsOpenXMLPresentation
  } catch {
    $result.error = $_.Exception.Message + " (line " + $_.InvocationInfo.ScriptLineNumber + ")"
  } finally {
    if ($pp) {
      try { if ($d) { $d.Close() } } catch {}
      $others = 0
      try { $others = [int]$pp.Presentations.Count } catch {}
      if ($others -gt 0) { $result.error = "someone opened a presentation in the PowerPoint this script started; it's left running"; $result.pid = @() }
      else { try { $pp.Quit() } catch {} }
      try { [void][Runtime.InteropServices.Marshal]::ReleaseComObject($pp) } catch {}
      [GC]::Collect(); [GC]::WaitForPendingFinalizers()
    }
  }
  Write-Json $result $ResultFile
  exit 0
}

Set-Location $root
New-Item -ItemType Directory -Force $Out | Out-Null
$deckPath = Join-Path $Out "effects-map.pptx"
$refusedPath = Join-Path $Out "refused.json"
if (-not $Parse) {
  $me = @(qwinsta 2>$null | Where-Object { $_ -match '^>' }) | Select-Object -First 1
  if (-not $me -or $me -notmatch '^>(console|rdp-tcp#\d+)\s' -or $me -notmatch '\sActive\s*$') { "STOPPED: no active session"; exit 3 }
  $sid = [Diagnostics.Process]::GetCurrentProcess().SessionId
  if (@(Get-Process LogonUI -ErrorAction SilentlyContinue | Where-Object { $_.SessionId -eq $sid }).Count) { "STOPPED: the session is locked"; exit 3 }
  $resultPath = Join-Path ([IO.Path]::GetTempPath()) "fw-effects-map-$PID.json"
  $mutex = New-Object Threading.Mutex($false, "Global\FeatherweightOfficeAutomation")
  try { [void]$mutex.WaitOne() } catch [Threading.AbandonedMutexException] {}
  try {
    if (@(Get-Process POWERPNT -ErrorAction SilentlyContinue).Count) { "STOPPED: a PowerPoint this script didn't start is running"; exit 4 }
    if ((Test-Path $quietLock) -and -not $env:FW_HOLDS_QUIET_PC_LOCK) { "STOPPED: another job holds FeatherweightNotes\quiet-pc.lock"; exit 4 }
    $p = Start-Process -FilePath "powershell.exe" -PassThru -WindowStyle Hidden -ArgumentList @("-NoProfile", "-ExecutionPolicy", "Bypass",
      "-File", "`"$PSCommandPath`"", "-Child", "1", "-Deck", "`"$deckPath`"", "-ResultFile", "`"$resultPath`"")
    $finished = $p.WaitForExit($TimeoutSeconds * 1000)
    if (-not $finished) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
    $r = if (Test-Path $resultPath) { Get-Content $resultPath -Raw -Encoding UTF8 | ConvertFrom-Json } else { $null }
    foreach ($id in @(if ($r) { $r.pid })) {
      $x = Get-Process -Id $id -ErrorAction SilentlyContinue
      if ($x -and $x.ProcessName -eq "POWERPNT" -and -not $x.WaitForExit($(if ($finished) { 15000 } else { 0 }))) { Stop-Process -Id $id -Force -ErrorAction SilentlyContinue }
    }
  } finally { $mutex.ReleaseMutex() }
  if (-not $finished) { "FAILED: PowerPoint didn't finish within $TimeoutSeconds s"; exit 1 }
  if (-not $r -or $r.error) { "FAILED: $(if ($r) { $r.error } else { 'no result' })"; exit 1 }
  Write-Json ([ordered]@{ powerpoint = $r.powerpoint; refused = @($r.refused) }) $refusedPath
}

# ---- 2. What PowerPoint wrote for each name.
Add-Type -AssemblyName System.IO.Compression.FileSystem
$made = Get-Content $refusedPath -Raw -Encoding UTF8 | ConvertFrom-Json
$zip = [IO.Compression.ZipFile]::OpenRead($deckPath)
function Get-Xml([string]$name) {
  $e = $zip.GetEntry($name); if (-not $e) { return $null }
  $r = New-Object IO.StreamReader($e.Open()); $x = [xml]$r.ReadToEnd(); $r.Close(); $x
}
function Format-Element($el) {
  if (-not $el) { return $null }
  $attrs = @($el.Attributes | Where-Object { $_.Name -notmatch '^xmlns' } | ForEach-Object { "$($_.Name)=$($_.Value)" })
  "<" + (@($el.Name) + $attrs -join " ") + ">"
}
$transitions = @(); $effects = @(); $directions = @()
$slides = @($zip.Entries | Where-Object { $_.FullName -match '^ppt/slides/slide(\d+)\.xml$' } | Sort-Object { [int]($_.FullName -replace '\D', '') })
foreach ($entry in $slides) {
  $x = Get-Xml $entry.FullName
  $ns = New-Object Xml.XmlNamespaceManager($x.NameTable)
  $ns.AddNamespace("p", "http://schemas.openxmlformats.org/presentationml/2006/main")
  $ns.AddNamespace("a", "http://schemas.openxmlformats.org/drawingml/2006/main")
  $ns.AddNamespace("mc", "http://schemas.openxmlformats.org/markup-compatibility/2006")
  $title = [string]($x.SelectSingleNode("//p:sp[.//p:ph[@type='title']]//a:t", $ns).InnerText)
  if ($title -like "tr:*") {
    $choice = $x.SelectSingleNode("/p:sld/mc:AlternateContent/mc:Choice", $ns)
    $fallback = $x.SelectSingleNode("/p:sld/mc:AlternateContent/mc:Fallback/p:transition", $ns)
    $tr = if ($choice) { $choice.SelectSingleNode("p:transition", $ns) } else { $x.SelectSingleNode("/p:sld/p:transition", $ns) }
    $inner = if ($tr) { @($tr.ChildNodes | Where-Object { $_.NodeType -eq 'Element' -and $_.LocalName -ne 'sndAc' -and $_.LocalName -ne 'extLst' }) | Select-Object -First 1 } else { $null }
    $name = $title.Substring(3)
    $transitions += [ordered]@{
      ppEntryEffect = $name; value = Get-EnumValue PpEntryEffect $name
      requires = $(if ($choice) { $choice.GetAttribute("Requires") } else { $null })
      element = Format-Element $inner
      transition = Format-Element $tr
      fallback = $(if ($fallback) { Format-Element (@($fallback.ChildNodes | Where-Object { $_.NodeType -eq 'Element' }) | Select-Object -First 1) } else { $null })
    }
    continue
  }
  # Shapes by id, and each effect's preset and target.
  $names = @{}
  foreach ($c in $x.SelectNodes("//p:cNvPr", $ns)) { $names[$c.GetAttribute("id")] = $c.GetAttribute("name") }
  foreach ($ctn in $x.SelectNodes("//p:timing//p:cTn[@presetClass]", $ns)) {
    $tgt = $ctn.SelectSingleNode(".//p:spTgt", $ns)
    $shape = if ($tgt) { $names[$tgt.GetAttribute("spid")] } else { $null }
    if (-not $shape) { continue }
    $row = [ordered]@{ presetClass = $ctn.GetAttribute("presetClass"); presetID = [int]$ctn.GetAttribute("presetID"); presetSubtype = [int]$ctn.GetAttribute("presetSubtype") }
    if ($shape -match '^(fx|exit):(.+)$') {
      $effects += [ordered]@{ msoAnimEffect = $matches[2]; value = Get-EnumValue MsoAnimEffect $matches[2]; exit = ($matches[1] -eq "exit") } + $row
    } elseif ($shape -match '^dir:([^:]+):(.+)$') {
      $directions += [ordered]@{ msoAnimEffect = $matches[1]; msoAnimDirection = $matches[2]; value = Get-EnumValue MsoAnimDirection $matches[2] } + $row
    }
  }
}
$zip.Dispose()
$table = [ordered]@{
  about = "PowerPoint's object-model names (PpEntryEffect, MsoAnimEffect, MsoAnimDirection) against what it writes in the file, from a deck PowerPoint built through its object model (scripts\powerpoint-effects-map.ps1). For the read-back check and the tools; the app doesn't use it."
  powerpoint = $made.powerpoint
  made = (Get-Date).ToString("yyyy-MM-dd")
  refused = @($made.refused)
  transitions = $transitions
  effects = $effects
  directions = $directions
}
Write-Json $table (Join-Path $root $Json)
"{0} transitions, {1} effects, {2} directions, {3} refused -> {4}" -f $transitions.Count, $effects.Count, $directions.Count, @($made.refused).Count, $Json
