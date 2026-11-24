# Waits for a quiet PC, then runs the slide show's frame-rate bench and prints its numbers.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tests\office\show\quiet-bench.ps1 -Report <report.json>
#
# Quiet means all of these at once:
#   - no compiler, linker or MSBuild running, and the whole CPU under 10 % busy, for a minute
#     (12 samples 5 s apart);
#   - no Featherweight running (someone's own, or another bench);
#   - at the PC's own screen: this session on the console, not over Remote Desktop, and not locked.
#     Over RDP the show presents to RDP's virtual display, composed and sent at RDP's rate, never to
#     the panel: its frame count says nothing about the display's refresh rate;
#   - the user away but the display still on: last input between 2 and 4.5 minutes ago (this PC
#     turns the display off after 5 minutes; the show keeps it on once it runs);
#   - the quiet-PC lock free.
#
# The lock (shared with the quiet-PC timing jobs, the same way): the file below, created with
# CreateNew, holding who measures ("<session> <time> (pid N): <what>"), deleted when done. Anyone
# else waits while it exists. A stale one (a crash) is never deleted here: it is reported and waited on.
#
# The bench takes the whole screen. The mouse, clicks and keys change nothing in it (the cursor may
# show); only Esc, Alt+F4 or another program taking the focus stop it early (exit code 3, "aborted"
# in the report) and the desktop comes back; this script then waits for the next quiet spell and
# tries again, up to -Tries times.
#
# -AbortTest: no CPU wait; starts the bench, moves the mouse by one pixel 2 s later (the bench must
# go on), posts Esc to the show's window a second later and checks that the show ends within a
# second with exit code 3.
param(
  [string]$Exe = '',   # default: build\Release\Featherweight.exe of this checkout
  [string]$Deck = '',  # default: bench50.pptx next to this script
  [Parameter(Mandatory = $true)] [string]$Report,
  [string]$Lock = 'D:\Documents\Codex\FeatherweightNotes\quiet-pc.lock',
  [string]$Session = 'p1-show',
  [string]$Log = '',   # also append the status lines here
  [int]$Tries = 3,
  [double]$MaxHours = 12,
  [switch]$AbortTest
)
$ErrorActionPreference = 'Stop'
if (-not $Exe) { $Exe = Join-Path $PSScriptRoot '..\..\..\build\Release\Featherweight.exe' }
if (-not $Deck) { $Deck = Join-Path $PSScriptRoot 'bench50.pptx' }
$Exe = [IO.Path]::GetFullPath($Exe)
$Deck = [IO.Path]::GetFullPath($Deck)
$Report = [IO.Path]::GetFullPath($Report)
if (-not (Test-Path $Exe)) { throw "No ${Exe}: build it first." }
if (-not (Test-Path $Deck)) { throw "No $Deck." }

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class QuietPc {
  [StructLayout(LayoutKind.Sequential)] struct LASTINPUTINFO { public uint cbSize; public uint dwTime; }
  [DllImport("user32.dll")] static extern bool GetLastInputInfo(ref LASTINPUTINFO info);
  [DllImport("kernel32.dll")] static extern bool GetSystemTimes(out long idle, out long kernel, out long user);
  [StructLayout(LayoutKind.Sequential)] struct MOUSEINPUT { public int dx, dy; public uint data, flags, time; public IntPtr extra; }
  [StructLayout(LayoutKind.Sequential)] struct INPUT { public uint type; public MOUSEINPUT mi; }  // the union's largest member
  [DllImport("user32.dll")] static extern uint SendInput(uint count, INPUT[] inputs, int size);
  [DllImport("user32.dll")] static extern int GetSystemMetrics(int index);
  [DllImport("kernel32.dll")] static extern uint WTSGetActiveConsoleSessionId();
  [DllImport("kernel32.dll")] static extern bool ProcessIdToSessionId(uint pid, out uint session);
  [DllImport("kernel32.dll")] static extern uint GetCurrentProcessId();

  public static double IdleSeconds() {
    var li = new LASTINPUTINFO();
    li.cbSize = (uint)Marshal.SizeOf(li);
    if (!GetLastInputInfo(ref li)) return -1;
    return unchecked((uint)Environment.TickCount - li.dwTime) / 1000.0;
  }
  static long lastIdle, lastTotal;
  // Percent of the CPU busy since the last call (the first call returns -1).
  public static double Busy() {
    long idle, kernel, user;
    if (!GetSystemTimes(out idle, out kernel, out user)) return -1;
    long total = kernel + user;  // kernel time includes idle time
    double busy = -1;
    if (lastTotal != 0 && total > lastTotal) busy = 100.0 * (1.0 - (double)(idle - lastIdle) / (total - lastTotal));
    lastIdle = idle;
    lastTotal = total;
    return busy;
  }
  // This session is over Remote Desktop (SM_REMOTESESSION).
  public static bool Remote() { return GetSystemMetrics(0x1000) != 0; }
  // This session is the one on the PC's own screen (not a disconnected or another user's session).
  public static bool OnConsole() {
    uint mine;
    return ProcessIdToSessionId(GetCurrentProcessId(), out mine) && mine == WTSGetActiveConsoleSessionId();
  }
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern IntPtr FindWindowW(string cls, string title);
  [DllImport("user32.dll")] static extern bool PostMessageW(IntPtr hwnd, uint msg, IntPtr w, IntPtr l);
  // Esc to the show's window only (WM_KEYDOWN VK_ESCAPE), never to whatever has the focus.
  public static bool PostEscToShow() {
    IntPtr h = FindWindowW("FeatherweightSlideShow", null);
    return h != IntPtr.Zero && PostMessageW(h, 0x0100, (IntPtr)0x1B, IntPtr.Zero);
  }
  public static uint NudgeMouse() {  // the number of inputs sent: 2
    var inputs = new INPUT[2];
    inputs[0].type = 0; inputs[0].mi.dx = 1; inputs[0].mi.flags = 1;   // MOUSEEVENTF_MOVE
    inputs[1].type = 0; inputs[1].mi.dx = -1; inputs[1].mi.flags = 1;
    return SendInput(2, inputs, Marshal.SizeOf(typeof(INPUT)));
  }
}
'@

# Compilers and linkers exist only while they work; MSBuild's worker nodes stay for a while after a
# build, so for MSBuild only the CPU it used counts (more than 250 ms in a sample).
$buildTools = 'cl', 'link', 'lib', 'ninja', 'ctest', 'rc', 'mt'
$msbuildMs = @{}
function Get-Busy {
  $tools = @(Get-Process -Name $buildTools -ErrorAction SilentlyContinue).Count
  $used = 0.0
  $seen = @{}
  foreach ($p in @(Get-Process -Name MSBuild -ErrorAction SilentlyContinue)) {
    try { $ms = $p.TotalProcessorTime.TotalMilliseconds } catch { continue }
    $seen[$p.Id] = $ms
    if ($msbuildMs.ContainsKey($p.Id)) { $used += $ms - $msbuildMs[$p.Id] } else { $used += $ms }
  }
  $first = $msbuildMs.Count -eq 0 -and $script:sampled -ne $true
  $script:msbuildMs = $seen
  $script:sampled = $true
  if (-not $first -and $used -gt 250) { $tools++ }
  [pscustomobject]@{ Cpu = [QuietPc]::Busy(); Tools = $tools }
}
# The lock as the timing jobs take it: CreateNew fails when someone else has it. True when taken.
function Enter-Lock {
  try {
    $fs = [IO.File]::Open($Lock, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write)
    $who = [Text.Encoding]::UTF8.GetBytes(("{0} {1:yyyy-MM-dd HH:mm:ss} (pid {2}): quiet-bench.ps1, the slide show's frame-rate bench`r`n" -f $Session, (Get-Date), $PID))
    $fs.Write($who, 0, $who.Length)
    $fs.Close()
    return $true
  } catch { return $false }
}
function Write-Status([string]$text) {
  $line = "$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') $text"
  if ($Log) { Add-Content -Path $Log -Value $line -Encoding utf8 }
  Write-Output $line
}

$deadline = (Get-Date).AddHours($MaxHours)
$window = New-Object System.Collections.Generic.List[object]
[void][QuietPc]::Busy()
[void](Get-Busy)
$lastWhy = ''
$try = 0
Write-Status "armed: $Exe, $Deck, report $Report, up to $Tries tries, $MaxHours h"
while ((Get-Date) -lt $deadline) {
  Start-Sleep -Seconds 5
  $sample = Get-Busy
  $window.Add($sample)
  while ($window.Count -gt 12) { $window.RemoveAt(0) }
  $idle = [QuietPc]::IdleSeconds()
  $fw = @(Get-Process -Name Featherweight -ErrorAction SilentlyContinue).Count
  $cpuQuiet = $window.Count -ge 12 -and -not ($window | Where-Object { $_.Cpu -lt 0 -or $_.Cpu -ge 10 -or $_.Tools -gt 0 })
  $why = @()
  if ([QuietPc]::Remote()) { $why += 'over Remote Desktop' }
  elseif (-not [QuietPc]::OnConsole()) { $why += 'not on the console (disconnected)' }
  elseif (Get-Process LogonUI -ErrorAction SilentlyContinue) { $why += 'locked' }
  if (-not $AbortTest -and -not $cpuQuiet) { $why += 'CPU or a build busy' }
  if ($fw -gt 0) { $why += 'a Featherweight runs' }
  if ($idle -lt 120) { $why += 'someone at the PC' }
  if ($idle -gt 270) { $why += 'idle too long (display may go off)' }
  if ($why.Count -eq 0 -and (Test-Path $Lock)) {
    $holder = ((Get-Content $Lock -Raw -ErrorAction SilentlyContinue) -replace '\s+', ' ').Trim()
    $why += "lock held: $holder"
  }
  if ($why.Count -gt 0) {
    $now = $why -join ', '
    if ($now -ne $lastWhy) { Write-Status "waiting: $now (CPU $([math]::Round($sample.Cpu)) %, idle $([math]::Round($idle)) s)"; $lastWhy = $now }
    continue
  }
  if (-not (Enter-Lock)) {
    Write-Status 'lock taken by someone else just now; waiting again'
    $lastWhy = ''
    continue
  }
  $lastWhy = ''
  try {
    $try++
    if (Test-Path $Report) { Remove-Item $Report }
    Write-Status "try ${try}: bench starts (idle $([math]::Round($idle)) s, console, not remote, not locked)"
    $showArgs = @('--show', "`"$Deck`"", '--show-bench', "`"$Report`"")
    $p = Start-Process -FilePath $Exe -ArgumentList $showArgs -PassThru
    $null = $p.Handle  # keeps the exit code readable after the process ends
    if ($AbortTest) {
      Start-Sleep -Seconds 2
      $sent = [QuietPc]::NudgeMouse()
      if ($p.WaitForExit(1000)) { Write-Status "abort test FAILED: a mouse move ended the bench (exit code $($p.ExitCode))"; exit 1 }
      $escAt = Get-Date
      $posted = [QuietPc]::PostEscToShow()
      $ended = $p.WaitForExit(1000)
      $ms = [math]::Round(((Get-Date) - $escAt).TotalMilliseconds)
      if (-not $ended) { $p.Kill(); $p.WaitForExit(); Write-Status "abort test FAILED: the show still ran 1 s after Esc (posted: $posted; killed)"; exit 1 }
      $r = Get-Content $Report -Raw | ConvertFrom-Json
      Write-Status "abort test: a mouse move ($sent inputs) went on; Esc ended it after $ms ms, exit code $($p.ExitCode), report aborted: $($r.aborted) ($($r.abortReason))"
      exit $(if ($p.ExitCode -eq 3 -and $r.abortReason -eq 'Esc') { 0 } else { 1 })
    }
    if (-not $p.WaitForExit(15 * 60 * 1000)) { $p.Kill(); $p.WaitForExit(); Write-Status 'the bench ran 15 minutes: killed'; exit 1 }
    $code = $p.ExitCode
  } finally {
    Remove-Item $Lock -ErrorAction SilentlyContinue
  }
  if ($code -eq 3) {
    Write-Status "try $try stopped early (Esc, closed or the focus taken); waiting for the next quiet spell"
    $window.Clear()
    if ($try -ge $Tries) { Write-Status "gave up after $try tries"; exit 3 }
    continue
  }
  if ($code -eq 1 -or -not (Test-Path $Report)) { Write-Status "the bench failed to run (exit code $code)"; exit 1 }
  $r = Get-Content $Report -Raw | ConvertFrom-Json
  Write-Status ("done, exit code {0}: {1} transitions, {2} frames at {3:N1} Hz ({4}x{5}), missed refreshes {6}, worst interval {7:N1} ms, CPU per frame median {8:N2} / p99 {9:N2} / max {10:N2} ms, pass {11}" -f `
      $code, $r.transitions, $r.transitionFrames, $r.refreshHz, $r.width, $r.height, $r.missedRefreshes, $r.worstIntervalMs, `
      $r.cpuMsMedian, $r.cpuMs99, $r.cpuMsMax, $r.pass)
  $bad = @($r.perTransition | Where-Object { $_.missed -gt 0 } | ForEach-Object { "s$($_.slide): $($_.missed)" })
  if ($bad.Count) { Write-Status ("missed by slide: " + ($bad -join ', ')) }
  exit $code
}
Write-Status "no quiet spell in $MaxHours hours"
exit 4
