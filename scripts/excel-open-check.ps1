<#
.SYNOPSIS
  Opens each workbook in Excel and reports whether Excel opens it cleanly, has to repair it, or
  refuses it (plan section 3, "never damage a file": edited files must open in Excel without a
  repair prompt). Run it on the Office reference PC (decision 19).

  How a repair is detected without relying on Excel's prompt being shown:
  - Excel's alerts are off. A workbook Excel would offer to repair then fails to open normally
    (CorruptLoad xlNormalLoad) instead of prompting.
    Garbage fails differently: "the file format or file extension is not valid".
  - It is then opened again with CorruptLoad xlRepairFile, and then xlExtractData. If one of them
    opens it, the file is "repaired": Excel writes its repair log (the error*.xml recovery log in
    the user's TEMP folder), which the check reads and reports. If both fail, it is "refused".
    (Repair mode is never the first try: it writes a log even for a sound file.)
  - A workbook that opens normally is also checked for the signs of a silent repair: a new repair
    log naming it, or "[Repaired]" in its window's caption.
  scripts\excel-open-check-proof.ps1 makes deliberately broken workbooks and shows each kind.

  Excel is driven by automation, with the rules of scripts\reference-renders.ps1:
  - Each file gets its own new Excel process (Excel starts a new instance for every COM call), which
    always ends before the next file starts. Only the process behind that Excel's own window is ever
    ended: someone may be using Excel at the same time. One Office automation runs at a time on the
    PC (a global mutex).
  - Hidden, macros forced off, events off, links never updated, read-only, a dummy password (a
    protected file fails instead of prompting), nothing added to the recent files. Nothing is saved.
  - Every dialog the Excel process still shows is read (title and text) and cancelled.

  Results, per file: ok (opened normally, no sign of a repair, no dialog); repaired (Excel had to
  repair it; the repair log's lines are reported); refused (Excel couldn't open it, even repairing);
  error (opened, but Excel showed a dialog); timeout; unchecked (Excel's automation failed three
  times: Excel didn't start or answer, which says nothing about the file). The exit code is 1 when
  any file isn't ok.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\excel-open-check.ps1 out\saved
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\excel-open-check.ps1 a.xlsx b.xlsx -Json excel-open.json
#>
param(
  [Parameter(Position = 0, ValueFromRemainingArguments = $true)][string[]]$Paths,
  [string]$Json,
  [int]$TimeoutSeconds = 120,
  [switch]$ShowWindows,  # print every window Excel shows while a file opens (for diagnosing)
  [string]$Password,     # the password of protected files; without it they fail instead of prompting
  # Internal: open one file in this process (the parent runs one child per file).
  [string]$Child, [string]$ResultFile
)

$ErrorActionPreference = "Stop"

function Write-Json($value, [string]$path) {
  [IO.File]::WriteAllText($path, ($value | ConvertTo-Json -Depth 6), [Text.UTF8Encoding]::new($false))
}

# Ends the Excel processes a child started, once they've had $waitMs to quit on their own. Only
# EXCEL processes are touched, so a reused process id is left alone.
function Stop-Started($ids, [int]$waitMs = 15000) {
  foreach ($id in @($ids)) {
    $proc = Get-Process -Id $id -ErrorAction SilentlyContinue
    if (-not $proc -or $proc.ProcessName -ne "EXCEL") { continue }
    if (-not $proc.WaitForExit($waitMs)) { Stop-Process -Id $id -Force -ErrorAction SilentlyContinue }
  }
}

# The folders Excel writes its repair logs to: the user's TEMP (Excel started by COM gets the
# user's environment, which can differ from this process's).
function Get-LogFolders {
  $dirs = @($env:TEMP, [Environment]::ExpandEnvironmentVariables([string](Get-ItemProperty "HKCU:\Environment").TEMP),
            (Join-Path $env:LOCALAPPDATA "Temp"))
  @($dirs | Where-Object { $_ -and (Test-Path -LiteralPath $_) } | ForEach-Object { (Resolve-Path -LiteralPath $_).Path.TrimEnd('\') } | Select-Object -Unique)
}

function Get-RepairLogs($folders) {
  @(foreach ($d in $folders) { Get-ChildItem -LiteralPath $d -Filter "error*.xml" -File -ErrorAction SilentlyContinue | ForEach-Object FullName })
}

# The repair logs written since $before that name this file: their summary and their lines.
function Read-NewLogs($folders, $before, [string]$file) {
  $name = [IO.Path]::GetFileName($file)
  $logs = @()
  foreach ($path in Get-RepairLogs $folders) {
    if ($before -contains $path) { continue }
    $text = $null
    for ($i = 0; $i -lt 10 -and -not $text; $i++) {
      try { $text = [IO.File]::ReadAllText($path) } catch { Start-Sleep -Milliseconds 200 }
    }
    if (-not $text -or $text.IndexOf($name, [StringComparison]::OrdinalIgnoreCase) -lt 0) { continue }
    # What Excel repaired or removed; without such lines, its general note ("Excel completed file
    # level validation and repair...").
    $lines = @([regex]::Matches($text, '<(repairedRecord|removedRecord|repairedPart|removedPart|removedFeature|repairedFeature)\b[^>]*>([^<]*)<') |
               ForEach-Object { [Net.WebUtility]::HtmlDecode($_.Groups[2].Value).Trim() } | Where-Object { $_ })
    if (-not $lines.Count) {
      $lines = @([regex]::Matches($text, '<info\b[^>]*>([^<]*)<') | ForEach-Object { [Net.WebUtility]::HtmlDecode($_.Groups[1].Value).Trim() } | Where-Object { $_ })
    }
    $logs += [ordered]@{ log = $path; lines = $lines }
  }
  $logs
}

Add-Type @"
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
public static class FwXlWindows {
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
  public static int ProcessOf(int hwnd) { uint pid; GetWindowThreadProcessId((IntPtr)hwnd, out pid); return (int)pid; }
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

# ---- Child: open one file in a new hidden Excel, close it unsaved, quit.
if ($Child) {
  $started = New-Object Collections.Generic.List[int]  # the Excel this child started: the only one it may end
  $result = [ordered]@{ pid = $started }
  $xl = $null; $wb = $null
  $missing = [Type]::Missing
  try {
    $before = @(Get-Process EXCEL -ErrorAction SilentlyContinue | ForEach-Object Id)
    $xl = New-Object -ComObject Excel.Application
    # Excel starts a new instance for every COM call; only the process behind this Excel's own
    # window is this child's.
    $xlPid = [FwXlWindows]::ProcessOf([int]$xl.Hwnd)
    if ($before -contains $xlPid) {
      [void][Runtime.InteropServices.Marshal]::ReleaseComObject($xl)
      $xl = $null
      throw "COM attached to an Excel this check didn't start (process $xlPid)"
    }
    $started.Add($xlPid)
    Write-Json $result $ResultFile
    $result.app = "Excel $($xl.Version) (build $($xl.Build))"
    $xl.AutomationSecurity = 3  # msoAutomationSecurityForceDisable: macros never run
    $xl.Visible = $false
    $xl.DisplayAlerts = $false  # a workbook that needs repair fails to open instead of prompting
    $xl.AskToUpdateLinks = $false
    $xl.EnableEvents = $false
    $xl.ScreenUpdating = $false
    $folders = Get-LogFolders
    $result.opening = $true
    Write-Json $result $ResultFile  # the parent watches for dialogs from here on
    $pw = if ($Password) { $Password } else { "-" }
    # Named arguments: CorruptLoad given by position makes every Open fail. UpdateLinks never,
    # read-only, a dummy password (a protected file fails instead of prompting), no "file in use"
    # notification, not added to the recent files.
    $names = [string[]]@("Filename", "UpdateLinks", "ReadOnly", "Password", "WriteResPassword", "IgnoreReadOnlyRecommended",
                         "Notify", "AddToMru", "CorruptLoad")
    $workbooks = $xl.Workbooks
    $errors = [ordered]@{}
    foreach ($load in 0, 1, 2) {  # xlNormalLoad, xlRepairFile, xlExtractData
      $logsBefore = Get-RepairLogs $folders
      try {
        $values = [object[]]@($Child, 0, $true, $pw, "-", $true, $false, $false, $load)
        $wb = [System.__ComObject].InvokeMember("Open", [Reflection.BindingFlags]::InvokeMethod, $null, $workbooks, $values, $null, $null, $names)
      } catch {
        $ex = $_.Exception
        while ($ex.InnerException) { $ex = $ex.InnerException }
        $errors[@("normal", "repair", "extract")[$load]] = $ex.Message
        $wb = $null
      }
      $logs = @(Read-NewLogs $folders $logsBefore $Child)
      if ($logs.Count) { $result.logs = @(@($result.logs) + $logs | Where-Object { $_ }) }
      if ($wb) {
        $result.opened = $true
        $result.load = @("normal", "repair", "extract")[$load]
        try { $result.caption = [string]$wb.Windows.Item(1).Caption } catch {}
        $result.sheets = [int]$wb.Sheets.Count
        $wb.Close($false)
        [void][Runtime.InteropServices.Marshal]::ReleaseComObject($wb)
        $wb = $null
        break
      }
    }
    if ($errors.Count) { $result.openErrors = $errors }
    [void][Runtime.InteropServices.Marshal]::ReleaseComObject($workbooks)
  } catch {
    $result.error = $_.Exception.Message
  } finally {
    if ($xl) {
      try { $xl.Quit() } catch {}
      foreach ($o in $wb, $xl) { if ($o) { [void][Runtime.InteropServices.Marshal]::ReleaseComObject($o) } }
      $wb = $null; $xl = $null
      [GC]::Collect(); [GC]::WaitForPendingFinalizers()
    }
    # Whatever happened, the Excel this child started ends with it.
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
  $title = [FwXlWindows]::TextOf($hwnd)
  $texts = @([FwXlWindows]::ChildTexts($hwnd))
  try {
    $el = [Windows.Automation.AutomationElement]::FromHandle($hwnd)
    $all = $el.FindAll([Windows.Automation.TreeScope]::Descendants, [Windows.Automation.Condition]::TrueCondition)
    foreach ($e in $all) { if ($e.Current.ControlType -ne [Windows.Automation.ControlType]::Button -and $e.Current.Name) { $texts += $e.Current.Name } }
  } catch {}
  $noise = '^(.*Icon|Show Help >>|Hide Help <<|Was this information helpful\?)$'
  $texts = @($texts | ForEach-Object { ($_ -replace '\s+', ' ').Trim() } | Where-Object { $_ -and $_ -ne $title -and $_ -notmatch $noise })
  ($texts | Select-Object -Unique) -join " "
}

$files = foreach ($p in $Paths) {
  # Filtered by hand: with -LiteralPath, Windows PowerShell's -Include lets every file through.
  if (Test-Path -LiteralPath $p -PathType Container) { Get-ChildItem -LiteralPath $p -Recurse -File | Where-Object { $_.Extension -in ".xlsx", ".xlsm", ".xltx", ".xltm" } | Sort-Object FullName }
  elseif (Test-Path -LiteralPath $p) { Get-Item -LiteralPath $p }
  else { Write-Warning "not found: $p" }
}
if (-not $files) { throw "Give the workbooks or folders to check." }
# One Office automation at a time on this PC (reference-renders.ps1 and word-open-check.ps1 take
# the same mutex).
$mutex = New-Object Threading.Mutex($false, "Global\FeatherweightOfficeAutomation")
try { [void]$mutex.WaitOne() } catch [Threading.AbandonedMutexException] {}  # a check that died still let go
$tmp = Join-Path ([IO.Path]::GetTempPath()) "fw-excel-open-check-$PID"
New-Item -ItemType Directory -Force $tmp | Out-Null
$resultPath = Join-Path $tmp "result.json"
# Excel's automation failing (it didn't start or answer) says nothing about the file: try again.
$infraText = "(?i)didn't start|remote procedure call|RPC server|0x800706|0x80010108|0x80080005|disconnected from its clients|server execution failed|COM attached to an Excel"
$entries = @()
$n = 0
foreach ($f in @($files)) {
  $n++
  for ($attempt = 1; ; $attempt++) {
    if (Test-Path $resultPath) { Remove-Item $resultPath }
    $entry = [ordered]@{ file = $f.FullName; status = $null; load = $null; repairs = @(); dialogs = @(); error = $null }
    $childArgs = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$PSCommandPath`"",
                   "-Child", "`"$($f.FullName)`"", "-ResultFile", "`"$resultPath`"")
    if ($Password) { $childArgs += @("-Password", "`"$Password`"") }
    $p = Start-Process -FilePath "powershell.exe" -PassThru -WindowStyle Hidden -ArgumentList $childArgs
    # Watch the Excel process for dialogs until the child is done: read each one, then cancel it.
    $seen = @{}
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    $r = $null
    while (-not $p.WaitForExit(250)) {
      if ((Get-Date) -gt $deadline) { break }
      try { $r = Get-Content $resultPath -Raw -ErrorAction Stop | ConvertFrom-Json } catch { continue }
      if (-not $r.opening) { continue }
      foreach ($h in [FwXlWindows]::TopLevel([int[]]@($r.pid))) {
        $class = [FwXlWindows]::ClassOf($h)
        if ($ShowWindows) { "    window {0} '{1}'" -f $class, [FwXlWindows]::TextOf($h) }
        if ($class -eq "XLMAIN") { continue }  # Excel's own (hidden) main window
        $key = [string]$h
        if (-not $seen.ContainsKey($key)) {
          $seen[$key] = 0
          $entry.dialogs += [ordered]@{ title = [FwXlWindows]::TextOf($h); class = $class; text = (Get-DialogText $h) }
        }
        # Cancel, then close; never answer yes.
        if ($seen[$key] -eq 0) { [FwXlWindows]::Cancel($h) } else { [FwXlWindows]::Close($h) }
        $seen[$key]++
      }
    }
    $finished = $p.HasExited
    if (-not $finished) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
    try { $r = Get-Content $resultPath -Raw -ErrorAction Stop | ConvertFrom-Json } catch {}
    # The child ends the Excel it started; if it hung or died, that's done here.
    if ($r) { Stop-Started @($r.pid) $(if ($finished) { 15000 } else { 0 }) }
    if ($r) {
      $entry.load = $r.load
      $entry.error = $r.error
      # Why each way of opening failed: normal, repair, extract.
      if ($r.openErrors) { $entry.openErrors = $r.openErrors }
      $entry.repairs = @($r.logs | Where-Object { $_ } | ForEach-Object { $_.lines } | Where-Object { $_ })
      $entry.logs = @($r.logs | Where-Object { $_ } | ForEach-Object { $_.log })
      $entry.caption = $r.caption
    }
    $silent = $r -and $r.load -eq "normal" -and ($entry.logs.Count -or "$($r.caption)" -match '\[Repaired\]')
    $entry.status =
      if (-not $finished) { "timeout" }
      elseif (-not $r -or $r.error) { "error" }
      elseif ($r.load -in "repair", "extract" -or $silent) { "repaired" }
      elseif (-not $r.opened) { "refused" }
      elseif ($entry.dialogs.Count) { "error" }
      else { "ok" }
    $infra = "$($r.error)" -match $infraText
    if ($entry.status -ne "error" -or $entry.dialogs.Count -or -not $infra) { break }
    if ($attempt -ge 3) { $entry.status = "unchecked"; break }  # Excel's automation failed three times
    Start-Sleep -Seconds 5
  }
  $what = if ($entry.repairs.Count) { $entry.repairs -join " || " }
          elseif ($entry.dialogs.Count) { ($entry.dialogs | ForEach-Object { "$($_.title): $($_.text)" }) -join " || " }
          elseif ($entry.error) { $entry.error }
          elseif ($entry.openErrors) { @($entry.openErrors.PSObject.Properties | ForEach-Object { "$($_.Name): $($_.Value)" }) -join " | " }
          else { "" }
  if ($entry.status -eq "repaired" -and $entry.load -ne "normal") { $what = "($($entry.load)) $what" }
  if ($what.Length -gt 240) { $what = $what.Substring(0, 240) + "..." }
  "[{0}/{1}] {2,-8} {3}  {4}" -f $n, @($files).Count, $entry.status, $f.Name, $what
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
$mutex.ReleaseMutex()
Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
"$($entries.Count - $bad.Count) of $($entries.Count) opened in Excel without a repair or error."
exit $(if ($bad.Count) { 1 } else { 0 })
