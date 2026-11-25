<#
.SYNOPSIS
  Opens each deck in PowerPoint and reports whether PowerPoint opens it cleanly, has to repair it,
  or refuses it (plan section 3, "never damage a file": edited decks must open in PowerPoint without
  a repair prompt; P2's gate). With -Pdf it also saves each deck PowerPoint opened as a PDF, every
  slide, hidden ones too, for the SSIM comparisons (fwtool pdf-compare). Run it on the Office
  reference PC (decision 19).

  How a repair is detected without relying on PowerPoint's prompt being shown
  (scripts\powerpoint-open-check-proof.ps1 makes broken decks and shows each kind):
  - PowerPoint's alerts are off (ppAlertsNone), so a deck PowerPoint would offer to repair fails to
    open normally instead of prompting.
  - It is then opened with Open2007's OpenAndRepair. If that opens it, the deck is "repaired";
    if not, "refused".
  - A deck that opens normally is also checked for the signs of a silent repair: fewer or more slides
    than its presentation part lists, or "Repaired" in the presentation's name.
  - Every dialog the PowerPoint process still shows is read (title and text) and cancelled: "error".

  PowerPoint is driven by automation, with the rules of scripts\reference-renders.ps1 and P2's plan:
  - PowerPoint runs once per user. A PowerPoint already running is someone's (Roy's): it is never
    used, automated or closed. When one is running the check stops (exit 4) and the decks not yet
    checked are "unchecked": run it again later.
  - Only in an active session, at the console or over Remote Desktop (they render alike, p1-corpus
    2026-11-22); never in a disconnected one (exit 3, the rest "unchecked").
  - Not while another session holds D:\Documents\Codex\FeatherweightNotes\quiet-pc.lock (its renders
    or timings): exit 4, the rest "unchecked". A job that holds the lock itself sets
    FW_HOLDS_QUIET_PC_LOCK=1 for the checks it runs.
  - Each deck gets its own new PowerPoint process, which ends before the next deck starts. A deck
    someone opens in it meanwhile (a double-click) makes it theirs: it's left running.
  - Macros forced off (AutomationSecurity 3) before anything opens, alerts off, read-only, no window,
    nothing saved but the PDF copy. One Office automation runs at a time on the PC (a global mutex).

  Results, per deck: ok, repaired, refused, error (opened, but PowerPoint showed a dialog), timeout,
  unchecked (PowerPoint's automation failed three times, or the check had to stop). The exit code is
  1 when any deck isn't ok, 3 or 4 when the check stopped early (see above).

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\powerpoint-open-check.ps1 build\deck-torture\saved -Json build\deck-torture\powerpoint.json
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\powerpoint-open-check.ps1 a.pptx b.pptx -Pdf build\pdfs
#>
param(
  [Parameter(Position = 0, ValueFromRemainingArguments = $true)][string[]]$Paths,
  # A text file naming decks or folders, one per line (for more than a command line holds).
  [string]$ListFile,
  [string]$Json,
  # Also save each deck PowerPoint opened as <Pdf>\<file name>.pdf (every slide, hidden ones too;
  # the results' pdf names it: a second deck of the same name gets <file name>-2.pdf).
  [string]$Pdf,
  [int]$TimeoutSeconds = 180,
  [switch]$ShowWindows,  # print every window PowerPoint shows while a deck opens (for diagnosing)
  # Internal: open one deck in this process (the parent runs one child per deck).
  [string]$Child, [string]$PdfFile, [string]$ResultFile
)

$ErrorActionPreference = "Stop"

function Write-Json($value, [string]$path) {
  [IO.File]::WriteAllText($path, ($value | ConvertTo-Json -Depth 6), [Text.UTF8Encoding]::new($false))
}

# Ends the PowerPoint processes a child started, once they've had $waitMs to quit on their own. Only
# POWERPNT processes are touched, so a reused process id is left alone.
function Stop-Started($ids, [int]$waitMs = 15000) {
  foreach ($id in @($ids)) {
    $proc = Get-Process -Id $id -ErrorAction SilentlyContinue
    if (-not $proc -or $proc.ProcessName -ne "POWERPNT") { continue }
    if (-not $proc.WaitForExit($waitMs)) { Stop-Process -Id $id -Force -ErrorAction SilentlyContinue }
  }
}

# ---- Child: open one deck in a new PowerPoint, close it unsaved, quit.
if ($Child) {
  $started = New-Object Collections.Generic.List[int]  # the PowerPoint this child started: the only one it may end
  $result = [ordered]@{ pid = $started }
  $pp = $null; $doc = $null
  try {
    # PowerPoint runs once per user: COM would hand back one that's already running (someone's).
    if (Get-Process POWERPNT -ErrorAction SilentlyContinue) { $result.busy = $true; throw "a PowerPoint is already running (someone's): not used" }
    $before = @(Get-Process POWERPNT -ErrorAction SilentlyContinue | ForEach-Object Id)
    $pp = New-Object -ComObject PowerPoint.Application
    $new = @(Get-Process POWERPNT -ErrorAction SilentlyContinue | Where-Object { $before -notcontains $_.Id } | ForEach-Object Id)
    if (-not $new.Count) {
      [void][Runtime.InteropServices.Marshal]::ReleaseComObject($pp)
      $pp = $null
      $result.busy = $true
      throw "COM attached to a PowerPoint this check didn't start"
    }
    foreach ($id in $new) { $started.Add($id) }
    Write-Json $result $ResultFile
    $result.app = "PowerPoint $($pp.Version) (build $($pp.Build))"
    $pp.AutomationSecurity = 3  # msoAutomationSecurityForceDisable: macros never run
    $pp.DisplayAlerts = 1       # ppAlertsNone: a deck that needs repair fails instead of prompting
    $result.opening = $true
    Write-Json $result $ResultFile  # the parent watches for dialogs from here on
    $errors = [ordered]@{}
    foreach ($load in "normal", "repair") {
      try {
        # Read-only, not untitled, no window. A .ppsx opens for editing, not into the show; a .potx as itself.
        $doc = if ($load -eq "normal") { $pp.Presentations.Open($Child, -1, 0, 0) }
               else { $pp.Presentations.Open2007($Child, -1, 0, 0, -1) }  # OpenAndRepair
      } catch {
        $ex = $_.Exception
        while ($ex.InnerException) { $ex = $ex.InnerException }
        $errors[$load] = $ex.Message
        $doc = $null
      }
      if ($doc) {
        $result.opened = $true
        $result.load = $load
        $result.slides = [int]$doc.Slides.Count
        $result.name = [string]$doc.Name
        if ($PdfFile) {
          # PowerPoint's PDF leaves hidden slides out, so they're shown in the copy PowerPoint has
          # open (read-only, closed without saving) first, as reference-renders.ps1 does.
          for ($i = 1; $i -le $doc.Slides.Count; $i++) {
            $t = $doc.Slides.Item($i).SlideShowTransition
            if ($t.Hidden -eq -1) { $t.Hidden = 0 }
          }
          $doc.SaveCopyAs($PdfFile, 32)  # ppSaveAsPDF
          $result.pdf = $PdfFile
        }
        $doc.Close()
        [void][Runtime.InteropServices.Marshal]::ReleaseComObject($doc)
        $doc = $null
        break
      }
    }
    if ($errors.Count) { $result.openErrors = $errors }
  } catch {
    $result.error = $_.Exception.Message
  } finally {
    # A deck someone opened in this PowerPoint while it ran makes it theirs: neither quit nor ended.
    if ($pp) {
      $others = 0
      try { if ($doc) { $doc.Close() } } catch {}
      try { $others = [int]$pp.Presentations.Count } catch {}
      if ($others -gt 0) {
        $result.error = "someone opened a presentation in the PowerPoint this check started; it's left running"
        $result.busy = $true
        $started.Clear()
        foreach ($o in $doc, $pp) { if ($o) { try { [void][Runtime.InteropServices.Marshal]::ReleaseComObject($o) } catch {} } }
        $doc = $null; $pp = $null
      }
    }
    if ($pp) {
      try { $pp.Quit() } catch {}
      foreach ($o in $doc, $pp) { if ($o) { [void][Runtime.InteropServices.Marshal]::ReleaseComObject($o) } }
      $doc = $null; $pp = $null
      [GC]::Collect(); [GC]::WaitForPendingFinalizers()
    }
    Stop-Started $started
  }
  Write-Json $result $ResultFile
  exit 0
}

# ---- Parent
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes, System.IO.Compression, System.IO.Compression.FileSystem
Add-Type @"
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
public static class FwPpWindows {
  delegate bool EnumProc(IntPtr hwnd, IntPtr lParam);
  [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc proc, IntPtr lParam);
  [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr parent, EnumProc proc, IntPtr lParam);
  [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassName(IntPtr hwnd, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetWindowText(IntPtr hwnd, StringBuilder s, int n);
  [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr hwnd);
  [DllImport("user32.dll")] static extern bool PostMessage(IntPtr hwnd, uint msg, IntPtr w, IntPtr l);
  public static string ClassOf(IntPtr h) { var s = new StringBuilder(256); GetClassName(h, s, 256); return s.ToString(); }
  public static string TextOf(IntPtr h) { var s = new StringBuilder(2048); GetWindowText(h, s, 2048); return s.ToString(); }
  public static List<IntPtr> TopLevel(int[] pids) {
    var found = new List<IntPtr>();
    EnumWindows((h, l) => {
      uint pid; GetWindowThreadProcessId(h, out pid);
      if (Array.IndexOf(pids, (int)pid) >= 0 && IsWindowVisible(h)) found.Add(h);
      return true;
    }, IntPtr.Zero);
    return found;
  }
  public static List<string> ChildTexts(IntPtr parent) {
    var texts = new List<string>();
    EnumChildWindows(parent, (h, l) => { string t = TextOf(h); if (t.Length > 0) texts.Add(t); return true; }, IntPtr.Zero);
    return texts;
  }
  public static void Close(IntPtr h) { PostMessage(h, 0x0010 /* WM_CLOSE */, IntPtr.Zero, IntPtr.Zero); }
  public static void Cancel(IntPtr h) { PostMessage(h, 0x0111 /* WM_COMMAND */, (IntPtr)2 /* IDCANCEL */, IntPtr.Zero); }
}
"@

function Get-DialogText([IntPtr]$hwnd) {
  $title = [FwPpWindows]::TextOf($hwnd)
  $texts = @([FwPpWindows]::ChildTexts($hwnd))
  try {
    $el = [Windows.Automation.AutomationElement]::FromHandle($hwnd)
    $all = $el.FindAll([Windows.Automation.TreeScope]::Descendants, [Windows.Automation.Condition]::TrueCondition)
    foreach ($e in $all) { if ($e.Current.ControlType -ne [Windows.Automation.ControlType]::Button -and $e.Current.Name) { $texts += $e.Current.Name } }
  } catch {}
  $noise = '^(.*Icon|Show Help >>|Hide Help <<|Was this information helpful\?)$'
  $texts = @($texts | ForEach-Object { ($_ -replace '\s+', ' ').Trim() } | Where-Object { $_ -and $_ -ne $title -and $_ -notmatch $noise })
  ($texts | Select-Object -Unique) -join " "
}

# The slides the deck's presentation part lists (p:sldIdLst), or -1 when it can't be read.
function Get-ListedSlides([string]$path) {
  try {
    $zip = [IO.Compression.ZipFile]::OpenRead($path)
    try {
      $rels = $zip.GetEntry("_rels/.rels")
      $main = "ppt/presentation.xml"
      if ($rels) {
        $r = New-Object IO.StreamReader($rels.Open()); $t = $r.ReadToEnd(); $r.Close()
        $m = [regex]::Match($t, 'Type="[^"]*/officeDocument"[^>]*Target="/?([^"]+)"|Target="/?([^"]+)"[^>]*Type="[^"]*/officeDocument"')
        if ($m.Success) { $main = if ($m.Groups[1].Value) { $m.Groups[1].Value } else { $m.Groups[2].Value } }
      }
      $e = $zip.GetEntry($main)
      if (-not $e) { return -1 }
      $r = New-Object IO.StreamReader($e.Open()); $t = $r.ReadToEnd(); $r.Close()
      $list = [regex]::Match($t, '<(\w+:)?sldIdLst\b[^>]*>(.*?)</(\w+:)?sldIdLst>', 'Singleline')
      if (-not $list.Success) { return 0 }
      return [regex]::Matches($list.Groups[2].Value, '<(\w+:)?sldId\b').Count
    } finally { $zip.Dispose() }
  } catch { return -1 }
}

# An active session, at the console or over Remote Desktop; never a disconnected one.
function Test-ActiveSession {
  $me = @(qwinsta 2>$null | Where-Object { $_ -match '^>' }) | Select-Object -First 1
  $me -and $me -match '^>(console|rdp-tcp#\d+)\s' -and $me -match '\sActive\s*$'
}

$deckExt = ".pptx", ".pptm", ".potx", ".potm", ".ppsx", ".ppsm"
if ($ListFile) { $Paths = @($Paths) + @([IO.File]::ReadAllLines([IO.Path]::GetFullPath($ListFile)) | Where-Object { $_.Trim() }) }
$Paths = @($Paths | Where-Object { $_ })
$files = foreach ($p in $Paths) {
  if (Test-Path -LiteralPath $p -PathType Container) { Get-ChildItem -LiteralPath $p -Recurse -File | Where-Object { $_.Extension -in $deckExt } | Sort-Object FullName }
  elseif (Test-Path -LiteralPath $p) { Get-Item -LiteralPath $p }
  else { Write-Warning "not found: $p" }
}
if (-not $files) { throw "Give the decks or folders to check." }
if ($Pdf) { $Pdf = [IO.Path]::GetFullPath($Pdf); New-Item -ItemType Directory -Force $Pdf | Out-Null }
# Shared with the other sessions' renders and timings: while someone else holds it, PowerPoint is theirs.
$quietLock = "D:\Documents\Codex\FeatherweightNotes\quiet-pc.lock"
$mutex = New-Object Threading.Mutex($false, "Global\FeatherweightOfficeAutomation")
$tmp = Join-Path ([IO.Path]::GetTempPath()) "fw-powerpoint-open-check-$PID"
New-Item -ItemType Directory -Force $tmp | Out-Null
$resultPath = Join-Path $tmp "result.json"
$infraText = "(?i)remote procedure call|RPC server|0x800706|0x80010108|0x80080005|disconnected from its clients|server execution failed"
$entries = @()
$stopped = $null  # why the check stopped early, and its exit code
$pdfNames = @{}   # PDF names taken: decks of the same name in other folders get -2, -3...
$n = 0
foreach ($f in @($files)) {
  $n++
  $pdfName = $f.Name + ".pdf"
  for ($k = 2; $pdfNames.ContainsKey($pdfName.ToLowerInvariant()); $k++) { $pdfName = "$($f.Name)-$k.pdf" }
  $pdfNames[$pdfName.ToLowerInvariant()] = $true
  $entry = [ordered]@{ file = $f.FullName; status = $null; load = $null; slides = $null; listed = (Get-ListedSlides $f.FullName); dialogs = @(); error = $null; pdf = $null }
  if ($stopped) { $entry.status = "unchecked"; $entry.error = $stopped.why; $entries += [pscustomobject]$entry; continue }
  try { [void]$mutex.WaitOne() } catch [Threading.AbandonedMutexException] {}
  # Checked with the mutex held: another automation may have just finished with its PowerPoint.
  if (-not (Test-ActiveSession)) { $stopped = @{ why = "no active session (disconnected or locked out): PowerPoint isn't automated then"; code = 3 } }
  elseif (Get-Process POWERPNT -ErrorAction SilentlyContinue) { $stopped = @{ why = "a PowerPoint is running (someone's): never used or closed"; code = 4 } }
  elseif ((Test-Path $quietLock) -and -not $env:FW_HOLDS_QUIET_PC_LOCK) {
    $stopped = @{ why = "another session holds FeatherweightNotes\quiet-pc.lock: " + ((Get-Content $quietLock -Raw -ErrorAction SilentlyContinue) -replace '\s+', ' ').Trim(); code = 4 }
  }
  if ($stopped) {
    $mutex.ReleaseMutex()
    $entry.status = "unchecked"; $entry.error = $stopped.why; $entries += [pscustomobject]$entry
    "[{0}/{1}] stopped: {2}" -f $n, @($files).Count, $stopped.why
    continue
  }
  for ($attempt = 1; ; $attempt++) {
    if (Test-Path $resultPath) { Remove-Item $resultPath }
    $entry.dialogs = @(); $entry.error = $null
    $pdfFile = if ($Pdf) { Join-Path $Pdf $pdfName } else { $null }
    if ($pdfFile -and (Test-Path -LiteralPath $pdfFile)) { Remove-Item -LiteralPath $pdfFile }
    $childArgs = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$PSCommandPath`"",
                   "-Child", "`"$($f.FullName)`"", "-ResultFile", "`"$resultPath`"")
    if ($pdfFile) { $childArgs += @("-PdfFile", "`"$pdfFile`"") }
    $p = Start-Process -FilePath "powershell.exe" -PassThru -WindowStyle Hidden -ArgumentList $childArgs
    $seen = @{}
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    $r = $null
    while (-not $p.WaitForExit(250)) {
      if ((Get-Date) -gt $deadline) { break }
      try { $r = Get-Content $resultPath -Raw -Encoding UTF8 -ErrorAction Stop | ConvertFrom-Json } catch { continue }
      if (-not $r.opening) { continue }
      foreach ($h in [FwPpWindows]::TopLevel([int[]]@($r.pid))) {
        $class = [FwPpWindows]::ClassOf($h)
        if ($ShowWindows) { "    window {0} '{1}'" -f $class, [FwPpWindows]::TextOf($h) }
        if ($class -eq "PPTFrameClass") { continue }  # PowerPoint's own main window
        $key = [string]$h
        if (-not $seen.ContainsKey($key)) {
          $seen[$key] = 0
          $entry.dialogs += [ordered]@{ title = [FwPpWindows]::TextOf($h); class = $class; text = (Get-DialogText $h) }
        }
        if ($seen[$key] -eq 0) { [FwPpWindows]::Cancel($h) } else { [FwPpWindows]::Close($h) }  # never answer yes
        $seen[$key]++
      }
    }
    $finished = $p.HasExited
    if (-not $finished) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
    try { $r = Get-Content $resultPath -Raw -Encoding UTF8 -ErrorAction Stop | ConvertFrom-Json } catch {}
    if ($r) { Stop-Started @($r.pid) $(if ($finished) { 15000 } else { 0 }) }
    if ($r) {
      $entry.load = $r.load; $entry.error = $r.error; $entry.slides = $r.slides; $entry.name = $r.name; $entry.app = $r.app
      if ($r.openErrors) { $entry.openErrors = $r.openErrors }
      if ($r.pdf -and (Test-Path -LiteralPath $r.pdf)) { $entry.pdf = $r.pdf }
    }
    # A silent repair: PowerPoint shows other slides than the deck lists, or names it repaired.
    $silent = $r -and $r.load -eq "normal" -and (($entry.listed -ge 0 -and $r.slides -ne $entry.listed) -or "$($r.name)" -match 'Repaired')
    $entry.status =
      if (-not $finished) { "timeout" }
      elseif ($r -and $r.busy) { "unchecked" }
      elseif (-not $r -or $r.error) { "error" }
      elseif ($r.load -eq "repair" -or $silent) { "repaired" }
      elseif (-not $r.opened) { "refused" }
      elseif ($entry.dialogs.Count) { "error" }
      else { "ok" }
    if ($r -and $r.busy) { $stopped = @{ why = "$($r.error)"; code = 4 }; break }
    $infra = "$($r.error)" -match $infraText
    if ($entry.status -ne "error" -or $entry.dialogs.Count -or -not $infra) { break }
    if ($attempt -ge 3) { $entry.status = "unchecked"; break }
    Start-Sleep -Seconds 5
  }
  $mutex.ReleaseMutex()
  $what = if ($entry.dialogs.Count) { ($entry.dialogs | ForEach-Object { "$($_.title): $($_.text)" }) -join " || " }
          elseif ($entry.error) { $entry.error }
          elseif ($entry.status -eq "repaired" -and $entry.load -eq "normal") { "PowerPoint shows $($entry.slides) slides; the deck lists $($entry.listed) ($($entry.name))" }
          elseif ($entry.openErrors) { @($entry.openErrors.PSObject.Properties | ForEach-Object { "$($_.Name): $($_.Value)" }) -join " | " }
          else { "" }
  if ($what.Length -gt 240) { $what = $what.Substring(0, 240) + "..." }
  "[{0}/{1}] {2,-9} {3}  {4}" -f $n, @($files).Count, $entry.status, $f.Name, $what
  $entries += [pscustomobject]$entry
}

$bad = @($entries | Where-Object { $_.status -ne "ok" })
if ($Json) {
  Write-Json ([ordered]@{
    made = (Get-Date).ToString("yyyy-MM-dd HH:mm")
    computer = "Office reference PC (decision 19)"
    files = $entries
  }) ([IO.Path]::GetFullPath($Json))
}
Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
"$($entries.Count - $bad.Count) of $($entries.Count) opened in PowerPoint without a repair or error."
if ($stopped) { "Stopped early: $($stopped.why). Run it again then; the decks not checked are 'unchecked'."; exit $stopped.code }
exit $(if ($bad.Count) { 1 } else { 0 })
