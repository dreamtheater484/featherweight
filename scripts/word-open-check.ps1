<#
.SYNOPSIS
  Opens each Word document in Word and reports whether Word shows a repair prompt or an error
  (plan section 3, "never damage a file": edited files must open in Office without a repair
  prompt). It reports that and nothing else. Run it on the Office reference PC (decision 19).

  Word is driven by automation, as in scripts\reference-renders.ps1:
  - Word runs hidden in safe mode (/safe): no add-ins, and the session's settings aren't saved.
  - Files are opened read-only, with macros forced off, links not updated, no conversion
    dialog, and nothing added to Word's recent files. Nothing is ever saved.
  - Each file gets its own Word process, which always ends before the next file starts. The check
    makes sure COM attached to that Word (by a caption only it knows) before opening anything, and
    never ends another Word: one check runs at a time on the PC (a global mutex).
  - Word's alerts stay on, so that a repair prompt is shown instead of answered silently. While
    the file opens, every dialog the Word process shows is read (its title and text) and
    cancelled. A dialog that offers to recover the contents is a repair prompt.

  Results, per file: ok (opened, no dialog); notice (opened, and the only dialogs were about this
  PC's Office, such as a proofing feature that isn't installed: nothing about the file); repair
  (Word offered to repair it, or refused it as unreadable); error (Word showed another dialog or
  refused the file otherwise); timeout; unchecked (Word's automation failed three times: Word didn't
  start or answer, which says nothing about the file). The exit code is 1 when any file isn't ok or
  notice.

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
  # Also read what Word sees once the file is open: its paragraphs, tracked changes (type, author,
  # text) and comments (author, text, resolved, the comment each one replies to), into the JSON's
  # "review" (scripts\contract-review-check.ps1).
  [switch]$Review,
  # The password of protected files (D5: Featherweight's encrypted saves). Without it, a protected
  # file fails to open instead of prompting.
  [string]$Password,
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
  // Whether a window of this process, shown or hidden, has `text` in its title.
  public static bool HasTitle(int pid, string text) {
    bool found = false;
    EnumWindows((h, l) => {
      uint owner; GetWindowThreadProcessId(h, out owner);
      if ((int)owner == pid && TextOf(h).Contains(text)) found = true;
      return !found;
    }, IntPtr.Zero);
    return found;
  }
  public static void Close(IntPtr h) { PostMessage(h, 0x0010 /* WM_CLOSE */, IntPtr.Zero, IntPtr.Zero); }
  public static void Cancel(IntPtr h) { PostMessage(h, 0x0111 /* WM_COMMAND */, (IntPtr)2 /* IDCANCEL */, IntPtr.Zero); }
}
"@

# ---- Child: open one file in a new hidden Word, close it unsaved, quit.
if ($Child) {
  $started = New-Object Collections.Generic.List[int]  # the Word this child started: the only one it may end
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
      # Is it the Word this child started? Its main window takes a caption only this child knows. (Another
      # program's Word waiting for its own automation could have answered instead.)
      $mark = "fw-word-open-check $PID $wait"
      $caption = $word.Caption
      $word.Caption = $mark
      $ours = $false
      for ($i = 0; $i -lt 30 -and -not $ours; $i++) {
        $ours = [FwWindows]::HasTitle($safe.Id, $mark)
        if (-not $ours) { Start-Sleep -Milliseconds 100 }
      }
      if ($ours) { break }
      # Not ours. A Word this call started is closed through COM; another program's is left as it was.
      # Neither is ended by force: only the Word this child started is.
      if ($new.Count) { try { $word.Quit([ref]0) } catch {} } else { try { $word.Caption = $caption } catch {} }
      [void][Runtime.InteropServices.Marshal]::ReleaseComObject($word)
      $word = $null
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
    $pw = if ($Password) { $Password } else { "-" }
    $doc = $word.Documents.Open([ref]$Child, [ref]$false, [ref]$true, [ref]$false, [ref]$pw, [ref]"-", [ref]$false,
                                [ref]"-", [ref]"-", [ref]0, [ref]$missing, [ref]$false, [ref]$false, [ref]$missing, [ref]$true)
    $result.opened = $true
    # While a dialog is up (the parent cancels it), Word rejects calls: try again for a while.
    if ($Review) {
      for ($try = 0; ; $try++) {
        try {
          $paras = @(); foreach ($p in $doc.Paragraphs) { $paras += $p.Range.Text.TrimEnd([char]13) }
          $revs = @(); foreach ($v in $doc.Revisions) { $revs += [ordered]@{ type = $v.Type; author = $v.Author; text = $v.Range.Text } }
          $notes = @()
          foreach ($c in $doc.Comments) {
            $parent = $null
            try { if ($c.Ancestor) { $parent = $c.Ancestor.Index } } catch {}
            $notes += [ordered]@{ index = $c.Index; author = $c.Author; text = $c.Range.Text; done = [bool]$c.Done; parent = $parent }
          }
          $result.review = [ordered]@{ paragraphs = $paras; revisions = $revs; comments = $notes }
          break
        } catch {
          $ex = $_.Exception
          while ($ex.InnerException -and $ex -isnot [Runtime.InteropServices.COMException]) { $ex = $ex.InnerException }
          if ($try -ge 40 -or $ex.HResult -notin 0x80010001, 0x8001010A) { throw }
          Start-Sleep -Milliseconds 500
        }
      }
    }
    for ($try = 0; ; $try++) {
      try { $doc.Close([ref]0); break }  # wdDoNotSaveChanges
      catch {
        $ex = $_.Exception
        while ($ex.InnerException -and $ex -isnot [Runtime.InteropServices.COMException]) { $ex = $ex.InnerException }
        if ($try -ge 40 -or $ex.HResult -notin 0x80010001, 0x8001010A) { throw }  # RPC_E_CALL_REJECTED, RPC_E_SERVERCALL_RETRYLATER
        Start-Sleep -Milliseconds 500
      }
    }
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

# A dialog that is about this PC's Office, not the file (a proofing tool that isn't installed).
$noticeText = '(?i)feature is not (available|installed)|Microsoft Office Setup program|install this feature'
# A dialog or message that means Word wants to repair the file, or found it unreadable.
$repairText = '(?i)unreadable content|recover the contents|repair|is corrupt|appears to be corrupted|found a problem|problem with (its|the) contents|cannot be opened because there are problems'

$files = foreach ($p in $Paths) {
  # Filtered by hand: with -LiteralPath, Windows PowerShell's -Include lets every file through.
  if (Test-Path -LiteralPath $p -PathType Container) { Get-ChildItem -LiteralPath $p -Recurse -File | Where-Object { $_.Extension -in ".docx", ".docm", ".dotx", ".dotm" } | Sort-Object FullName }
  elseif (Test-Path -LiteralPath $p) { Get-Item -LiteralPath $p }
  else { Write-Warning "not found: $p" }
}
if (-not $files) { throw "Give the documents or folders to check." }
# One Office automation at a time on this PC (reference-renders.ps1 takes the same mutex): a second
# one's COM could attach to the first one's Word.
$mutex = New-Object Threading.Mutex($false, "Global\FeatherweightOfficeAutomation")
try { [void]$mutex.WaitOne() } catch [Threading.AbandonedMutexException] {}  # a check that died still let go
$tmp = Join-Path ([IO.Path]::GetTempPath()) "fw-word-open-check-$PID"
New-Item -ItemType Directory -Force $tmp | Out-Null
$resultPath = Join-Path $tmp "result.json"
# Word's automation failing (it didn't start or answer) says nothing about the file: try again.
$infraText = "(?i)didn't connect to the safe-mode instance|remote procedure call|RPC server|0x800706|0x80010108|0x80080005|disconnected from its clients|server execution failed"
$entries = @()
$n = 0
foreach ($f in @($files)) {
  $n++
  for ($attempt = 1; ; $attempt++) {
  if (Test-Path $resultPath) { Remove-Item $resultPath }
  $entry = [ordered]@{ file = $f.FullName; status = $null; opened = $false; dialogs = @(); error = $null }
  $childArgs = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$PSCommandPath`"",
                 "-Child", "`"$($f.FullName)`"", "-ResultFile", "`"$resultPath`"")
  if ($Review) { $childArgs += "-Review" }
  if ($Password) { $childArgs += @("-Password", "`"$Password`"") }
  $p = Start-Process -FilePath "powershell.exe" -PassThru -WindowStyle Hidden -ArgumentList $childArgs
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
  $entry.opened = [bool]($r -and $r.opened)
  if ($Review -and $r -and $r.review) { $entry.review = $r.review }
  $all = (@($entry.dialogs | ForEach-Object { "$($_.title) $($_.text)" }) + @($entry.error)) -join " "
  $entry.status =
    if (-not $finished) { "timeout" }
    elseif ($all -match $repairText) { "repair" }
    elseif ($entry.opened -and -not $entry.dialogs.Count -and -not $entry.error) { "ok" }
    elseif ($entry.opened -and -not $entry.error -and -not @($entry.dialogs | Where-Object { "$($_.title) $($_.text)" -notmatch $noticeText }).Count) { "notice" }
    else { "error" }
  if ($entry.status -ne "error" -or $entry.dialogs.Count -or "$($entry.error)" -notmatch $infraText) { break }
  if ($attempt -ge 3) {
    $entry.status = "unchecked"  # Word's automation failed three times: not a verdict on the file
    break
  }
  Start-Sleep -Seconds 5
  }
  $what = if ($entry.dialogs.Count) { ($entry.dialogs | ForEach-Object { $_.text }) -join " || " } elseif ($entry.error) { $entry.error } else { "" }
  if ($what.Length -gt 220) { $what = $what.Substring(0, 220) + "..." }
  "[{0}/{1}] {2,-7} {3}  {4}" -f $n, @($files).Count, $entry.status, $f.Name, $what
  $entries += [pscustomobject]$entry
}

$bad = @($entries | Where-Object { $_.status -notin "ok", "notice" })
if ($Json) {
  Write-Json ([ordered]@{
    made = (Get-Date).ToString("yyyy-MM-dd HH:mm")
    computer = "Office reference PC (decision 19)"
    files = $entries
  }) ([IO.Path]::GetFullPath($Json))
}
$mutex.ReleaseMutex()
Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
"$($entries.Count - $bad.Count) of $($entries.Count) opened in Word without a prompt or error."
exit $(if ($bad.Count) { 1 } else { 0 })
