<#
.SYNOPSIS
  P3's .ppt and .odp corpora (docs/p3-plan.md, "Reference PC"): PowerPoint saves each screened deck
  in the private corpus's pptx folder as PowerPoint 97-2003 (.ppt, .pps from a .ppsx, .pot from a
  .potx) and as OpenDocument (.odp), into <corpus>\ppt and <corpus>\odp, by category, the same
  names. Each folder gets a manifest.json: every copy, its source ("copyOf"), both files' SHA-256,
  its size, and what went wrong if PowerPoint couldn't make it. Then scripts\reference-renders.ps1
  -Corpus <corpus> -CorpusFolders ppt,odp makes PowerPoint's PDFs of the copies, the references for
  fwtool compare.

  -From adds another folder of decks PowerPoint made (scripts\make-deck-edge-cases.ps1's made\):
  their copies go in ppt\<folder> and odp\<folder>.

  Only decks the safety screen passed (their SHA-256 in tests\office\real-world.json) or that
  PowerPoint made itself (-From) are opened. PowerPoint runs as P1 and P2 run it (docs/p2-plan.md,
  "Reference PC"):
  - only from Roy's active session (the console or Remote Desktop), never a disconnected or locked
    one: exit 3;
  - never while a PowerPoint this script didn't start runs (PowerPoint is single-instance, and that
    one is someone's), nor while another job holds FeatherweightNotes\quiet-pc.lock (a job that
    holds it itself sets FW_HOLDS_QUIET_PC_LOCK=1): exit 4;
  - a new PowerPoint per deck, hidden, macros forced off (AutomationSecurity 3), alerts off; only
    that one is ever quit or ended, and it always is, also when a deck hangs it;
  - one Office automation at a time (the global mutex reference-renders.ps1 and the open checks
    share), taken per deck. Sources open read-only and are never saved over.
  A copy whose source hasn't changed since it was made is kept (-Force makes them all again), so a
  run that stops part way goes on where it was.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\make-deck-copies.ps1 -Corpus D:\Documents\Codex\FeatherweightCorpus
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\make-deck-copies.ps1 -Corpus D:\corpus -Only shows -Formats ppt
#>
param(
  [string]$Corpus = $(if ($env:FW_CORPUS) { $env:FW_CORPUS } else { "D:\Documents\Codex\FeatherweightCorpus" }),
  [string[]]$Formats = @("ppt", "odp"),
  # Only sources whose path under the source folder contains one of these (a category, a file name).
  [string[]]$Only,
  # Folders under the corpus with decks PowerPoint made (not downloads), copied as well.
  [string[]]$From = @(),
  [switch]$Force,
  [int]$TimeoutSeconds = 300,
  # Internal: one deck in this process (the parent runs one child per deck).
  [string]$Child, [string]$Source, [string]$Targets, [string]$ResultFile
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
# PpSaveAsFileType: ppSaveAsPresentation, ppSaveAsShow, ppSaveAsTemplate, ppSaveAsOpenDocumentPresentation.
$saveFormats = @{ ".ppt" = 1; ".pps" = 7; ".pot" = 5; ".odp" = 35 }
$quietLock = "D:\Documents\Codex\FeatherweightNotes\quiet-pc.lock"

function Write-Json($value, [string]$path) {
  [IO.File]::WriteAllText($path, ($value | ConvertTo-Json -Depth 6), [Text.UTF8Encoding]::new($false))
}

# ---- Child: open the source once, save a copy per target.
if ($Child) {
  $result = [ordered]@{ pid = @(); powerpoint = $null; targets = @() }
  $pp = $null; $deck = $null; $mine = $true
  try {
    if (@(Get-Process POWERPNT -ErrorAction SilentlyContinue).Count) { throw "a PowerPoint this script didn't start is running" }
    $since = Get-Date
    $pp = New-Object -ComObject PowerPoint.Application
    $new = @(Get-Process POWERPNT -ErrorAction SilentlyContinue | Where-Object { $_.StartTime -ge $since.AddSeconds(-5) } | ForEach-Object Id)
    if (-not $new.Count) {
      [void][Runtime.InteropServices.Marshal]::ReleaseComObject($pp); $pp = $null
      throw "COM attached to a PowerPoint this script didn't start; nothing was done to it"
    }
    $result.pid = $new
    Write-Json $result $ResultFile  # the parent can end this PowerPoint from here on
    $pp.AutomationSecurity = 3  # msoAutomationSecurityForceDisable: macros never run
    $pp.DisplayAlerts = 1       # ppAlertsNone
    $result.powerpoint = "PowerPoint $($pp.Version) (build $($pp.Build))"
    # ReadOnly, not untitled, no window. A .ppsx opens for editing, not into the show; a .potx as itself.
    try { $deck = $pp.Presentations.Open($Source, -1, 0, 0) }
    catch { throw "PowerPoint didn't open it: $($_.Exception.Message)" }
    $result.slides = $deck.Slides.Count
    foreach ($t in $Targets -split '\|') {
      $dest = $t
      $entry = [ordered]@{ path = $dest; error = $null }
      try {
        New-Item -ItemType Directory -Force (Split-Path -Parent $dest) | Out-Null
        if (Test-Path -LiteralPath $dest) { Remove-Item -LiteralPath $dest }
        $deck.SaveCopyAs($dest, $saveFormats[[IO.Path]::GetExtension($dest).ToLower()])
        if (-not (Test-Path -LiteralPath $dest)) { throw "PowerPoint saved nothing" }
      } catch {
        $entry.error = $_.Exception.Message
      }
      $result.targets += [pscustomobject]$entry
    }
  } catch {
    $result.error = $_.Exception.Message
  } finally {
    if ($pp) {
      try { if ($deck) { $deck.Close() } } catch {}
      # PowerPoint runs once per user: a deck someone opened meanwhile (a double-click) opened in this
      # PowerPoint. Then it's theirs: neither quit nor ended.
      $others = 0
      try { $others = [int]$pp.Presentations.Count } catch {}
      if ($others -gt 0) {
        $mine = $false
        $result.error = "someone opened a presentation in the PowerPoint this script started; it's left running"
        $result.pid = @()
      } else {
        try { $pp.Quit() } catch {}
      }
      foreach ($o in $deck, $pp) { if ($o) { try { [void][Runtime.InteropServices.Marshal]::ReleaseComObject($o) } catch {} } }
      $deck = $null; $pp = $null
      [GC]::Collect(); [GC]::WaitForPendingFinalizers()
    }
  }
  Write-Json $result $ResultFile
  exit 0
}

# ---- Parent
Set-Location $root
$corpusRoot = (Resolve-Path $Corpus).Path.TrimEnd('\')
if ($corpusRoot.StartsWith($root + '\', [StringComparison]::OrdinalIgnoreCase)) { throw "$corpusRoot is inside the repository; the private corpus stays outside it." }
$Formats = @($Formats | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
foreach ($f in $Formats) { if (@("ppt", "odp") -notcontains $f) { throw "Unknown format '$f' (ppt, odp)" } }
$Only = @($Only | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$From = @($From | ForEach-Object { $_ -split ',' } | Where-Object { $_ })

# The decks: the screened ones (by hash) in pptx\, and the ones PowerPoint made in the -From folders.
$screened = @{}
foreach ($m in @((Get-Content (Join-Path $root "tests\office\real-world.json") -Raw -Encoding UTF8 | ConvertFrom-Json).files)) {
  if ($m.sha256 -and $m.file -like "pptx/*") { $screened[[string]$m.sha256] = $true }
}
function Get-Sha([string]$path) { (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLower() }
$sources = @()
foreach ($folder in @("pptx") + $From) {
  $dir = Join-Path $corpusRoot $folder
  if (-not (Test-Path $dir)) { throw "$corpusRoot has no $folder folder." }
  foreach ($f in Get-ChildItem $dir -Recurse -File | Where-Object { @(".pptx", ".ppsx", ".potx") -contains $_.Extension.ToLower() } | Sort-Object FullName) {
    $rel = $f.FullName.Substring($dir.Length + 1) -replace '\\', '/'
    # Password-protected decks (made\protected\) would wait for their password; their encrypted
    # .ppt copies are scripts\make-deck-edge-cases.ps1's.
    if ($rel -match '(^|/)protected/') { continue }
    if ($Only -and -not @($Only | Where-Object { $rel -like "*$_*" }).Count) { continue }
    $sha = Get-Sha $f.FullName
    if ($folder -eq "pptx" -and -not $screened[$sha]) { "Not copied: pptx/$rel isn't a deck the screen passed (its hash isn't in tests\office\real-world.json)."; continue }
    # Copies of a -From folder's decks go under ppt\<folder>\ and odp\<folder>\.
    $sources += @{ File = $f; Rel = $(if ($folder -eq "pptx") { $rel } else { "$folder/$rel" }); Of = "$folder/$rel"; Sha = $sha }
  }
}

# The manifests: each copy by its path under the corpus ("ppt/shows/a.pps").
$manifests = @{}
foreach ($fmt in $Formats) {
  $path = Join-Path $corpusRoot "$fmt\manifest.json"
  $entries = [ordered]@{}
  $app = $null
  if ((Test-Path $path) -and -not $Force) {
    $old = Get-Content $path -Raw -Encoding UTF8 | ConvertFrom-Json
    foreach ($e in $old.files) { $entries[$e.copy] = $e }
    $app = $old.powerpoint
  }
  $manifests[$fmt] = @{ Path = $path; Entries = $entries; App = $app }
}
function Save-Manifests {
  foreach ($fmt in $Formats) {
    $m = $manifests[$fmt]
    New-Item -ItemType Directory -Force (Split-Path -Parent $m.Path) | Out-Null
    Write-Json ([ordered]@{
      made = (Get-Date).ToString("yyyy-MM-dd HH:mm")
      powerpoint = $m.App
      format = $fmt
      note = $(if ($fmt -eq "ppt") { "PowerPoint's SaveCopyAs of each deck in pptx\ as PowerPoint 97-2003: .ppt (ppSaveAsPresentation), .pps from a .ppsx (ppSaveAsShow), .pot from a .potx (ppSaveAsTemplate) (scripts\make-deck-copies.ps1)" }
               else { "PowerPoint's SaveCopyAs of each deck in pptx\ as OpenDocument (ppSaveAsOpenDocumentPresentation) (scripts\make-deck-copies.ps1)" })
      files = @($m.Entries.Values | Sort-Object copy)
    }) $m.Path
  }
}
function Get-CopyName([string]$rel, [string]$fmt) {
  if ($fmt -eq "odp") { return "odp/" + ($rel -replace '\.[^./]+$', '.odp') }
  $ext = @{ ".pptx" = ".ppt"; ".ppsx" = ".pps"; ".potx" = ".pot" }[[IO.Path]::GetExtension($rel).ToLower()]
  "ppt/" + ($rel -replace '\.[^./]+$', $ext)
}
# Why PowerPoint can't be used now: exit 3 (no active session) or 4 (someone's PowerPoint, the lock).
function Get-Stop {
  $me = @(qwinsta 2>$null | Where-Object { $_ -match '^>' }) | Select-Object -First 1
  if (-not $me -or $me -notmatch '^>(console|rdp-tcp#\d+)\s' -or $me -notmatch '\sActive\s*$') {
    return @{ code = 3; why = "no active session: " + (($me -replace '^>', '') -replace '\s+', ' ').Trim() }
  }
  $sid = [Diagnostics.Process]::GetCurrentProcess().SessionId
  if (@(Get-Process LogonUI -ErrorAction SilentlyContinue | Where-Object { $_.SessionId -eq $sid }).Count) { return @{ code = 3; why = "the session is locked" } }
  if (@(Get-Process POWERPNT -ErrorAction SilentlyContinue).Count) { return @{ code = 4; why = "a PowerPoint this script didn't start is running" } }
  if ((Test-Path $quietLock) -and -not $env:FW_HOLDS_QUIET_PC_LOCK) {
    return @{ code = 4; why = "another job holds FeatherweightNotes\quiet-pc.lock: " + ((Get-Content $quietLock -Raw -ErrorAction SilentlyContinue) -replace '\s+', ' ').Trim() }
  }
  $null
}

$tmp = Join-Path ([IO.Path]::GetTempPath()) "fw-deck-copies-$PID"
New-Item -ItemType Directory -Force $tmp | Out-Null
$resultPath = Join-Path $tmp "result.json"
$made = 0; $kept = 0; $failed = @(); $n = 0; $stopped = $null
foreach ($src in $sources) {
  $n++
  $todo = @()
  foreach ($fmt in $Formats) {
    $copy = Get-CopyName $src.Rel $fmt
    $dest = Join-Path $corpusRoot ($copy -replace '/', '\')
    $e = $manifests[$fmt].Entries[$copy]
    if ($e -and $e.sourceSha256 -eq $src.Sha -and -not $e.error -and (Test-Path -LiteralPath $dest)) { $kept++; continue }
    $todo += @{ Format = $fmt; Copy = $copy; Dest = $dest }
  }
  if (-not $todo.Count) { continue }
  $mutex = New-Object Threading.Mutex($false, "Global\FeatherweightOfficeAutomation")
  try { [void]$mutex.WaitOne() } catch [Threading.AbandonedMutexException] {}
  $r = $null
  try {
    # Checked with the mutex held: another automation may have just finished with its PowerPoint.
    $stopped = Get-Stop
    if ($stopped) { break }
    if (Test-Path $resultPath) { Remove-Item $resultPath }
    $targets = ($todo | ForEach-Object { $_.Dest }) -join '|'
    $args_ = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$PSCommandPath`"", "-Child", "1",
               "-Source", "`"$($src.File.FullName)`"", "-Targets", "`"$targets`"", "-ResultFile", "`"$resultPath`"")
    $p = Start-Process -FilePath "powershell.exe" -PassThru -WindowStyle Hidden -ArgumentList $args_
    $finished = $p.WaitForExit($TimeoutSeconds * 1000)
    if (-not $finished) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
    $r = if (Test-Path $resultPath) { Get-Content $resultPath -Raw -Encoding UTF8 | ConvertFrom-Json } else { $null }
    # Only the PowerPoint this deck's child started is ever ended.
    foreach ($id in @(if ($r) { $r.pid })) {
      $x = Get-Process -Id $id -ErrorAction SilentlyContinue
      if ($x -and $x.ProcessName -eq "POWERPNT" -and -not $x.WaitForExit($(if ($finished) { 15000 } else { 0 }))) { Stop-Process -Id $id -Force -ErrorAction SilentlyContinue }
    }
    if (-not $finished) { $r = [pscustomobject]@{ error = "timeout after $TimeoutSeconds s"; targets = @() } }
  } finally {
    $mutex.ReleaseMutex()
  }
  foreach ($t in $todo) {
    $res = if ($r) { @($r.targets) | Where-Object { $_.path -eq $t.Dest } | Select-Object -First 1 } else { $null }
    $err = if (-not $r) { "no result from PowerPoint" } elseif ($r.error) { $r.error } elseif ($res) { $res.error } else { "not made" }
    $entry = [ordered]@{ copy = $t.Copy; copyOf = $src.Of; sourceSha256 = $src.Sha }
    if (-not $err -and (Test-Path -LiteralPath $t.Dest)) {
      $entry.sha256 = Get-Sha $t.Dest
      $entry.bytes = (Get-Item -LiteralPath $t.Dest).Length
      if ($r.slides) { $entry.slides = [int]$r.slides }
      $made++
    } else {
      $entry.error = $(if ($err) { $err } else { "PowerPoint saved nothing" })
      $failed += "$($t.Copy): $($entry.error)"
    }
    $manifests[$t.Format].Entries[$t.Copy] = [pscustomobject]$entry
    if ($r -and $r.powerpoint) { $manifests[$t.Format].App = $r.powerpoint }
  }
  Save-Manifests  # after every deck: a stopped run keeps what it made
  $status = @($todo | ForEach-Object { $e = $manifests[$_.Format].Entries[$_.Copy]; if ($e.error) { "$($_.Format) FAILED ($($e.error))" } else { "$($_.Format) $($e.bytes) bytes" } }) -join ", "
  "[{0}/{1}] {2}: {3}" -f $n, $sources.Count, $src.Of, $status
}
Save-Manifests
Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
if ($stopped) { "STOPPED before $($src.Of): $($stopped.why). $made made, $kept kept; run it again to go on."; exit $stopped.code }
"$made made, $kept kept (unchanged), $($failed.Count) failed; manifests in $corpusRoot\<format>\manifest.json"
if ($failed.Count) { $failed | ForEach-Object { "FAILED $_" }; exit 1 }
