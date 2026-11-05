<#
.SYNOPSIS
  Opens each Word document in Word and reports whether Word shows a repair prompt or an error
  (plan section 3, "never damage a file": edited files must open in Office without a repair
  prompt). It reports that and nothing else. Run it on the Office reference PC (decision 19).

  Word is driven by automation, as in scripts\reference-renders.ps1:
  - Word runs hidden in safe mode (/safe): no add-ins, and the session's settings aren't saved.
  - Files are opened read-only, with macros forced off, links not updated, no conversion
    dialog, and nothing added to Word's recent files. Nothing is ever saved.
  - Each file gets its own Word process, which always ends before the next file starts.
  - Word's alerts stay on, so that a repair prompt is shown instead of answered silently. While
    the file opens, every dialog the Word process shows is read (its title and text) and
    cancelled. A dialog that offers to recover the contents is a repair prompt.

  Results, per file: ok (opened, no dialog); repair (Word offered to repair it, or refused it as
  unreadable); error (Word showed another dialog or refused the file otherwise); timeout.
  The exit code is 1 when any file isn't ok.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\word-open-check.ps1 out\saved
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\word-open-check.ps1 a.docx b.docx -Json word-open.json
#>
param(
  [Parameter(Position = 0, ValueFromRemainingArguments = $true)][string[]]$Paths,
  [string]$Json,
  [int]$TimeoutSeconds = 90,
  [switch]$ShowWindows,  # print every window Word shows while a file opens (for diagnosing)
  # Internal: open one file in this process (the parent runs one child per file).
  [string]$Child, [string]$ResultFile
)

$ErrorActionPreference = "Stop"

function Write-Json($value, [string]$path) {
  [IO.File]::WriteAllText($path, ($value | ConvertTo-Json -Depth 6), [Text.UTF8Encoding]::new($false))
}

# Ends the Word processes a child started, once they've had $waitMs to quit on their own. Only
# WINWORD processes are touched, so a reused process id is left alone.
function Stop-Started($ids, [int]$waitMs = 15000) {
  foreach ($id in @($ids)) {
    $proc = Get-Process -Id $id -ErrorAction SilentlyContinue
    if (-not $proc -or $proc.ProcessName -ne "WINWORD") { continue }
    if (-not $proc.WaitForExit($waitMs)) { Stop-Process -Id $id -Force -ErrorAction SilentlyContinue }
  }
}

# ---- Child: open one file in a new hidden Word, close it unsaved, quit.
if ($Child) {
  $started = New-Object Collections.Generic.List[int]
  $result = [ordered]@{ pid = $started }
  $word = $null; $doc = $null
  try {
    # Word starts in safe mode, and COM attaches to that instance instead of starting another.
    $exe = (Get-ItemProperty "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\Winword.exe").'(default)'
    $safe = Start-Process -FilePath $exe -ArgumentList "/safe", "/automation", "-Embedding" -PassThru -WindowStyle Hidden
    $started.Add($safe.Id)
    Write-Json $result $ResultFile
    [void]$safe.WaitForInputIdle(30000)
    foreach ($wait in 2, 4, 8, 0) {
      Start-Sleep -Seconds $wait  # until it has registered for COM
      $before = @(Get-Process WINWORD -ErrorAction SilentlyContinue | ForEach-Object Id)
      $word = New-Object -ComObject Word.Application
      $new = @(Get-Process WINWORD -ErrorAction SilentlyContinue | Where-Object { $before -notcontains $_.Id } | ForEach-Object Id)
      foreach ($id in $new) { $started.Add($id) }
      Write-Json $result $ResultFile
      if (-not $new.Count) { break }
      # COM started a normal Word instead: close it and wait longer.
      try { $word.Quit([ref]0) } catch {}
      [void][Runtime.InteropServices.Marshal]::ReleaseComObject($word)
      $word = $null
      Stop-Started $new
    }
    if (-not $word) { throw "Word didn't connect to the safe-mode instance" }
    $result.app = "Word $($word.Version) (build $($word.Build)), safe mode"
    $word.AutomationSecurity = 3  # msoAutomationSecurityForceDisable: macros never run
    $word.Visible = $false
    $word.DisplayAlerts = -1  # wdAlertsAll: a repair prompt is shown (and cancelled by the parent), not answered
    $word.Options.UpdateLinksAtOpen = $false  # safe mode doesn't save it, so only this session
    $word.Options.ConfirmConversions = $false
    if (@($word.COMAddIns | Where-Object { $_.Connect }).Count) { throw "add-ins are loaded" }
    $result.opening = $true
    Write-Json $result $ResultFile  # the parent watches for dialogs from here on
    # FileName, ConfirmConversions, ReadOnly, AddToRecentFiles, PasswordDocument, PasswordTemplate, Revert,
    # WritePasswordDocument, WritePasswordTemplate, Format, Encoding, Visible, OpenAndRepair, DocumentDirection,
    # NoEncodingDialog, all by reference. A dummy password makes a protected file fail instead of prompting.
    $missing = [Type]::Missing
    $doc = $word.Documents.Open([ref]$Child, [ref]$false, [ref]$true, [ref]$false, [ref]"-", [ref]"-", [ref]$false,
                                [ref]"-", [ref]"-", [ref]0, [ref]$missing, [ref]$false, [ref]$false, [ref]$missing, [ref]$true)
    $result.opened = $true
    $doc.Close([ref]0)  # wdDoNotSaveChanges
  } catch {
    $result.error = $_.Exception.Message
  } finally {
    if ($word) {
      try { $word.Quit([ref]0) } catch {}
      foreach ($o in $doc, $word) { if ($o) { [void][Runtime.InteropServices.Marshal]::ReleaseComObject($o) } }
      $doc = $null; $word = $null
      [GC]::Collect(); [GC]::WaitForPendingFinalizers()
    }
    # Whatever happened, the Word this child started ends with it.
    Stop-Started $started
  }
  Write-Json $result $ResultFile
  exit 0
}

# ---- Parent
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
Add-Type @"
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
public static class FwWindows {
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
  public static bool Visible(IntPtr h) { return IsWindowVisible(h); }
  // The visible top-level windows of these processes.
  public static List<IntPtr> TopLevel(int[] pids) {
    var found = new List<IntPtr>();
    EnumWindows((h, l) => {
      uint pid; GetWindowThreadProcessId(h, out pid);
      if (Array.IndexOf(pids, (int)pid) >= 0 && IsWindowVisible(h)) found.Add(h);
      return true;
    }, IntPtr.Zero);
    return found;
  }
  // The text of a classic dialog's controls.
  public static List<string> ChildTexts(IntPtr parent) {
    var texts = new List<string>();
    EnumChildWindows(parent, (h, l) => { string t = TextOf(h); if (t.Length > 0) texts.Add(t); return true; }, IntPtr.Zero);
    return texts;
  }
  public static void Close(IntPtr h) { PostMessage(h, 0x0010 /* WM_CLOSE */, IntPtr.Zero, IntPtr.Zero); }
  public static void Cancel(IntPtr h) { PostMessage(h, 0x0111 /* WM_COMMAND */, (IntPtr)2 /* IDCANCEL */, IntPtr.Zero); }
}
"@

# The message a dialog shows: its controls' text, or for Office's own dialogs (NUIDialog), what UI
# Automation reads, without the title, icon and buttons.
function Get-DialogText([IntPtr]$hwnd) {
  $title = [FwWindows]::TextOf($hwnd)
  $texts = @([FwWindows]::ChildTexts($hwnd))
  try {
    $el = [Windows.Automation.AutomationElement]::FromHandle($hwnd)
    $all = $el.FindAll([Windows.Automation.TreeScope]::Descendants, [Windows.Automation.Condition]::TrueCondition)
    foreach ($e in $all) { if ($e.Current.ControlType -ne [Windows.Automation.ControlType]::Button -and $e.Current.Name) { $texts += $e.Current.Name } }
  } catch {}
  $noise = '^(.*Icon|Show Help >>|Hide Help <<|Was this information helpful\?)$'
  $texts = @($texts | ForEach-Object { ($_ -replace '\s+', ' ').Trim() } | Where-Object { $_ -and $_ -ne $title -and $_ -notmatch $noise })
  ($texts | Select-Object -Unique) -join " "
}

# A dialog or message that means Word wants to repair the file, or found it unreadable.
$repairText = '(?i)unreadable content|recover the contents|repair|is corrupt|appears to be corrupted|found a problem|problem with (its|the) contents|cannot be opened because there are problems'

$files = foreach ($p in $Paths) {
  if (Test-Path -LiteralPath $p -PathType Container) { Get-ChildItem -LiteralPath $p -Recurse -File -Include *.docx, *.docm, *.dotx, *.dotm | Sort-Object FullName }
  elseif (Test-Path -LiteralPath $p) { Get-Item -LiteralPath $p }
  else { Write-Warning "not found: $p" }
}
if (-not $files) { throw "Give the documents or folders to check." }
$tmp = Join-Path ([IO.Path]::GetTempPath()) "fw-word-open-check"
New-Item -ItemType Directory -Force $tmp | Out-Null
$resultPath = Join-Path $tmp "result.json"
$entries = @()
$n = 0
foreach ($f in @($files)) {
  $n++
  if (Test-Path $resultPath) { Remove-Item $resultPath }
  $entry = [ordered]@{ file = $f.FullName; status = $null; dialogs = @(); error = $null }
  $p = Start-Process -FilePath "powershell.exe" -PassThru -WindowStyle Hidden -ArgumentList @(
    "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$PSCommandPath`"",
    "-Child", "`"$($f.FullName)`"", "-ResultFile", "`"$resultPath`"")
  # Watch the Word process for dialogs until the child is done: read each one, then cancel it.
  $seen = @{}
  $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
  $r = $null
  while (-not $p.WaitForExit(250)) {
    if ((Get-Date) -gt $deadline) { break }
    try { $r = Get-Content $resultPath -Raw -ErrorAction Stop | ConvertFrom-Json } catch { continue }
    if (-not $r.opening) { continue }
    foreach ($h in [FwWindows]::TopLevel([int[]]@($r.pid))) {
      $class = [FwWindows]::ClassOf($h)
      if ($ShowWindows) { "    window {0} '{1}'" -f $class, [FwWindows]::TextOf($h) }
      if ($class -eq "OpusApp") { continue }  # Word's own (hidden) main window
      $key = [string]$h
      if (-not $seen.ContainsKey($key)) {
        $seen[$key] = 0
        $entry.dialogs += [ordered]@{ title = [FwWindows]::TextOf($h); class = $class; text = (Get-DialogText $h) }
      }
      # Cancel, then close; never answer yes.
      if ($seen[$key] -eq 0) { [FwWindows]::Cancel($h) } else { [FwWindows]::Close($h) }
      $seen[$key]++
    }
  }
  $finished = $p.HasExited
  if (-not $finished) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
  try { $r = Get-Content $resultPath -Raw -ErrorAction Stop | ConvertFrom-Json } catch {}
  # The child ends the Word it started; if it hung or died, that's done here.
  if ($r) { Stop-Started @($r.pid) $(if ($finished) { 15000 } else { 0 }) }
  if ($r -and $r.error) { $entry.error = $r.error }
  $all = (@($entry.dialogs | ForEach-Object { "$($_.title) $($_.text)" }) + @($entry.error)) -join " "
  $entry.status =
    if (-not $finished) { "timeout" }
    elseif ($all -match $repairText) { "repair" }
    elseif ($r -and $r.opened -and -not $entry.dialogs.Count) { "ok" }
    else { "error" }
  $what = if ($entry.dialogs.Count) { ($entry.dialogs | ForEach-Object { $_.text }) -join " || " } elseif ($entry.error) { $entry.error } else { "" }
  if ($what.Length -gt 220) { $what = $what.Substring(0, 220) + "..." }
  "[{0}/{1}] {2,-7} {3}  {4}" -f $n, @($files).Count, $entry.status, $f.Name, $what
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
"$($entries.Count - $bad.Count) of $($entries.Count) opened in Word without a prompt or error."
exit $(if ($bad.Count) { 1 } else { 0 })
