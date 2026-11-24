<#
.SYNOPSIS
  P3's PowerPoint read-back check (docs/p3-plan.md, "Transitions and animations" and "Comments"):
  PowerPoint 2021 opens each deck and writes what its object model reads back, as JSON beside the
  deck's name in -Out (<name>.readback.json): per slide the transition (SlideShowTransition), the
  main sequence and the interactive sequences (TimeLine), and the comments with their replies.
  Names are PowerPoint's own enums (PpEntryEffect, MsoAnimEffect, MsoAnimTriggerType,
  MsoAnimDirection); shapes are named by their id (the file's cNvPr id, the timing's spid). `fwtool
  readback` writes the same JSON from Featherweight's model; comparing the two is the check:
  - a deck saved without edits reads back the same as its original;
  - an edited deck reads back what Featherweight says it wrote.
  -Compare A,B compares two read-back files (or folders of them) and lists every difference; exit 1
  when there is one.

  PowerPoint runs as in scripts\make-deck-copies.ps1 (docs/p3-plan.md, "Reference PC"): only from
  Roy's active, unlocked session (exit 3), never with a PowerPoint this script didn't start or another
  job's FeatherweightNotes\quiet-pc.lock (exit 4; a job holding it sets FW_HOLDS_QUIET_PC_LOCK=1), a
  new hidden PowerPoint per deck, macros forced off, alerts off, read-only, nothing saved, always quit
  or ended. A deck in a protected\ folder opens with the test password "featherweight" (-Password
  for another).

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\powerpoint-readback.ps1 -Decks D:\corpus\made\animations.pptx -Out build\readback
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\powerpoint-readback.ps1 -Compare build\readback\original,build\readback\saved
#>
param(
  [string[]]$Decks = @(),
  [string]$Out = "build\readback",
  [string]$Password,
  [string[]]$Compare = @(),
  [int]$TimeoutSeconds = 180,
  # Internal: one deck in this process.
  [string]$Child, [string]$Source, [string]$ResultFile
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$quietLock = "D:\Documents\Codex\FeatherweightNotes\quiet-pc.lock"

function Write-Json($value, [string]$path) {
  [IO.File]::WriteAllText($path, ($value | ConvertTo-Json -Depth 10), [Text.UTF8Encoding]::new($false))
}

if ($Child) {
  [void][Reflection.Assembly]::LoadWithPartialName("Microsoft.Office.Interop.PowerPoint")
  function N([string]$type, $value) {
    $name = [Enum]::GetName([type]"Microsoft.Office.Interop.PowerPoint.$type", [int]$value)
    if ($name) { $name } else { "unknown($([int]$value))" }
  }
  function R($v) { [Math]::Round([double]$v, 3) }
  function Read-Effect($e) {
    $x = [ordered]@{}
    try { $x.shapeId = [int]$e.Shape.Id; $x.shape = [string]$e.Shape.Name } catch { $x.shapeId = $null; $x.shape = $null }
    try { $x.paragraph = [int]$e.Paragraph } catch { $x.paragraph = 0 }
    $x.effect = N MsoAnimEffect $e.EffectType
    $x.effectId = [int]$e.EffectType
    $x.exit = ([int]$e.Exit -eq -1)
    $x.trigger = N MsoAnimTriggerType $e.Timing.TriggerType
    $x.duration = R $e.Timing.Duration
    $x.delay = R $e.Timing.TriggerDelayTime
    $x.direction = $null
    try { $dir = [int]$e.EffectParameters.Direction; if ($dir -ne 0 -and $dir -ne -2) { $x.direction = N MsoAnimDirection $dir } } catch {}
    try { $x.repeat = R $e.Timing.RepeatCount } catch { $x.repeat = $null }
    $x
  }
  function Read-Comment($c) {
    $x = [ordered]@{ author = [string]$c.Author; initials = [string]$c.AuthorInitials; text = [string]$c.Text; left = R $c.Left; top = R $c.Top }
    $x.replies = @()
    try { foreach ($r in $c.Replies) { $x.replies += [ordered]@{ author = [string]$r.Author; initials = [string]$r.AuthorInitials; text = [string]$r.Text } } } catch {}
    $x
  }
  $result = [ordered]@{ pid = @() }
  $pp = $null; $d = $null
  try {
    if (@(Get-Process POWERPNT -ErrorAction SilentlyContinue).Count) { throw "a PowerPoint this script didn't start is running" }
    $since = Get-Date
    $pp = New-Object -ComObject PowerPoint.Application
    $new = @(Get-Process POWERPNT -ErrorAction SilentlyContinue | Where-Object { $_.StartTime -ge $since.AddSeconds(-5) } | ForEach-Object Id)
    if (-not $new.Count) { [void][Runtime.InteropServices.Marshal]::ReleaseComObject($pp); $pp = $null; throw "COM attached to a PowerPoint this script didn't start; nothing was done to it" }
    $result.pid = $new
    Write-Json $result $ResultFile
    $pp.AutomationSecurity = 3
    $pp.DisplayAlerts = 1
    $deck = [ordered]@{ deck = $Source; powerpoint = "PowerPoint $($pp.Version) (build $($pp.Build))"; slides = @() }
    $open = if ($Password) { $Source + "::" + $Password + "::" } else { $Source }
    $d = $pp.Presentations.Open($open, -1, 0, 0)
    foreach ($s in $d.Slides) {
      $st = $s.SlideShowTransition
      $slide = [ordered]@{ index = [int]$s.SlideIndex; slideId = [int]$s.SlideID; hidden = ([int]$st.Hidden -eq -1) }
      $t = [ordered]@{ effect = N PpEntryEffect $st.EntryEffect; effectId = [int]$st.EntryEffect }
      try { $t.duration = R $st.Duration } catch { $t.duration = $null }
      $t.advanceOnClick = ([int]$st.AdvanceOnClick -eq -1)
      $t.advanceOnTime = ([int]$st.AdvanceOnTime -eq -1)
      $t.advanceTime = R $st.AdvanceTime
      $t.sound = $null
      try { if ([int]$st.SoundEffect.Type -ne 0 -and [int]$st.SoundEffect.Type -ne 1) { $t.sound = [string]$st.SoundEffect.Name } } catch {}
      $slide.transition = $t
      $slide.main = @(foreach ($e in $s.TimeLine.MainSequence) { Read-Effect $e })
      $slide.interactive = @(foreach ($q in $s.TimeLine.InteractiveSequences) {
        $effects = @(foreach ($e in $q) { Read-Effect $e })
        $trig = $null
        try { $trig = [int]$q.Item(1).Timing.TriggerShape.Id } catch {}
        [ordered]@{ triggerShapeId = $trig; effects = $effects }
      })
      $slide.comments = @(foreach ($c in $s.Comments) { Read-Comment $c })
      $deck.slides += $slide
    }
    $result.deck = $deck
  } catch {
    $result.error = $_.Exception.Message
  } finally {
    if ($pp) {
      try { if ($d) { $d.Close() } } catch {}
      $others = 0
      try { $others = [int]$pp.Presentations.Count } catch {}
      if ($others -gt 0) { $result.error = "someone opened a presentation in the PowerPoint this script started; it's left running"; $result.pid = @() }
      else { try { $pp.Quit() } catch {} }
      foreach ($o in $d, $pp) { if ($o) { try { [void][Runtime.InteropServices.Marshal]::ReleaseComObject($o) } catch {} } }
      [GC]::Collect(); [GC]::WaitForPendingFinalizers()
    }
  }
  Write-Json $result $ResultFile
  exit 0
}

Set-Location $root

# ---- Compare two read-backs (files, or folders of *.readback.json by name)
if ($Compare.Count) {
  $Compare = @($Compare | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
  if ($Compare.Count -ne 2) { throw "-Compare takes two read-back files or folders" }
  $pairs = @()
  if (Test-Path -LiteralPath $Compare[0] -PathType Container) {
    foreach ($a in Get-ChildItem $Compare[0] -Filter *.readback.json) { $pairs += , @($a.FullName, (Join-Path $Compare[1] $a.Name)) }
  } else { $pairs += , @($Compare[0], $Compare[1]) }
  $diffs = 0
  function Get-Flat($node, [string]$path, $into) {
    if ($null -eq $node) { $into[$path] = "null"; return }
    if ($node -is [Array]) { $into["$path.count"] = "$($node.Count)"; for ($i = 0; $i -lt $node.Count; $i++) { Get-Flat $node[$i] "$path[$i]" $into }; return }
    if ($node -is [Management.Automation.PSCustomObject]) { foreach ($p in $node.PSObject.Properties) { Get-Flat $p.Value "$path.$($p.Name)" $into }; return }
    $into[$path] = [string]$node
  }
  foreach ($p in $pairs) {
    if (-not (Test-Path -LiteralPath $p[1])) { "MISSING $($p[1])"; $diffs++; continue }
    $a = (Get-Content -LiteralPath $p[0] -Raw -Encoding UTF8 | ConvertFrom-Json).slides
    $b = (Get-Content -LiteralPath $p[1] -Raw -Encoding UTF8 | ConvertFrom-Json).slides
    $fa = @{}; $fb = @{}
    Get-Flat @($a) "slides" $fa; Get-Flat @($b) "slides" $fb
    $keys = @($fa.Keys + $fb.Keys | Sort-Object -Unique)
    $here = @($keys | Where-Object { $fa[$_] -ne $fb[$_] })
    if ($here.Count) {
      "$(Split-Path -Leaf $p[0]): $($here.Count) differences"
      $here | Select-Object -First 40 | ForEach-Object { "  $_ : $($fa[$_]) -> $($fb[$_])" }
      $diffs += $here.Count
    } else { "$(Split-Path -Leaf $p[0]): same" }
  }
  exit $(if ($diffs) { 1 } else { 0 })
}

# ---- Read back each deck
$Decks = @($Decks | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
if (-not $Decks.Count) { throw "Pass -Decks (decks or folders) or -Compare" }
# Each deck and the name of its read-back: a folder's decks by their path under it ("protected__a.pptx").
$files = @(foreach ($d in $Decks) {
  if (Test-Path -LiteralPath $d -PathType Container) {
    $base = (Resolve-Path -LiteralPath $d).Path.TrimEnd('\')
    Get-ChildItem $d -Recurse -File | Where-Object { @(".pptx", ".pptm", ".ppsx", ".potx", ".ppt", ".pps", ".pot", ".odp") -contains $_.Extension.ToLower() } |
      ForEach-Object { $_ | Add-Member -NotePropertyName Key -NotePropertyValue ($_.FullName.Substring($base.Length + 1) -replace '\\', '__') -PassThru }
  } else { Get-Item -LiteralPath $d | ForEach-Object { $_ | Add-Member -NotePropertyName Key -NotePropertyValue $_.Name -PassThru } }
})
$outDir = [IO.Path]::GetFullPath((Join-Path $root $Out))
if ([IO.Path]::IsPathRooted($Out)) { $outDir = $Out }
New-Item -ItemType Directory -Force $outDir | Out-Null
$tmp = Join-Path ([IO.Path]::GetTempPath()) "fw-readback-$PID"
New-Item -ItemType Directory -Force $tmp | Out-Null
$resultPath = Join-Path $tmp "result.json"
$n = 0; $failed = 0
foreach ($f in $files) {
  $n++
  $me = @(qwinsta 2>$null | Where-Object { $_ -match '^>' }) | Select-Object -First 1
  if (-not $me -or $me -notmatch '^>(console|rdp-tcp#\d+)\s' -or $me -notmatch '\sActive\s*$') { "STOPPED before $($f.Name): no active session"; exit 3 }
  $sid = [Diagnostics.Process]::GetCurrentProcess().SessionId
  if (@(Get-Process LogonUI -ErrorAction SilentlyContinue | Where-Object { $_.SessionId -eq $sid }).Count) { "STOPPED before $($f.Name): the session is locked"; exit 3 }
  $mutex = New-Object Threading.Mutex($false, "Global\FeatherweightOfficeAutomation")
  try { [void]$mutex.WaitOne() } catch [Threading.AbandonedMutexException] {}
  try {
    if (@(Get-Process POWERPNT -ErrorAction SilentlyContinue).Count) { "STOPPED before $($f.Name): a PowerPoint this script didn't start is running"; exit 4 }
    if ((Test-Path $quietLock) -and -not $env:FW_HOLDS_QUIET_PC_LOCK) { "STOPPED before $($f.Name): another job holds FeatherweightNotes\quiet-pc.lock"; exit 4 }
    if (Test-Path $resultPath) { Remove-Item $resultPath }
    $pw = if ($Password) { $Password } elseif ($f.FullName -match '[\\/]protected[\\/]') { "featherweight" } else { $null }
    $args_ = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$PSCommandPath`"", "-Child", "1", "-Source", "`"$($f.FullName)`"", "-ResultFile", "`"$resultPath`"")
    if ($pw) { $args_ += @("-Password", $pw) }
    $p = Start-Process -FilePath "powershell.exe" -PassThru -WindowStyle Hidden -ArgumentList $args_
    $finished = $p.WaitForExit($TimeoutSeconds * 1000)
    if (-not $finished) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
    $r = if (Test-Path $resultPath) { Get-Content $resultPath -Raw -Encoding UTF8 | ConvertFrom-Json } else { $null }
    foreach ($id in @(if ($r) { $r.pid })) {
      $x = Get-Process -Id $id -ErrorAction SilentlyContinue
      if ($x -and $x.ProcessName -eq "POWERPNT" -and -not $x.WaitForExit($(if ($finished) { 15000 } else { 0 }))) { Stop-Process -Id $id -Force -ErrorAction SilentlyContinue }
    }
  } finally {
    $mutex.ReleaseMutex()
  }
  $err = if (-not $finished) { "timeout after $TimeoutSeconds s" } elseif (-not $r) { "no result" } else { $r.error }
  if ($err -or -not $r.deck) { $failed++; "[{0}/{1}] {2}: FAILED ({3})" -f $n, $files.Count, $f.Name, $err; continue }
  Write-Json $r.deck (Join-Path $outDir "$($f.Key).readback.json")
  $sl = @($r.deck.slides)
  "[{0}/{1}] {2}: {3} slides, {4} transitions, {5} effects, {6} comments" -f $n, $files.Count, $f.Name, $sl.Count,
    @($sl | Where-Object { $_.transition.effectId -ne 0 }).Count, (@($sl | ForEach-Object { @($_.main).Count + @($_.interactive | ForEach-Object { @($_.effects).Count }).Count }) | Measure-Object -Sum).Sum,
    (@($sl | ForEach-Object { @($_.comments).Count }) | Measure-Object -Sum).Sum
}
Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
if ($failed) { "$failed of $($files.Count) failed"; exit 1 }
"$($files.Count) read back into $outDir"
