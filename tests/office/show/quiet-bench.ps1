# Waits for a quiet PC, then runs the slide show's frame-rate bench and prints its numbers.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tests\office\show\quiet-bench.ps1 -Report <report.json>
#
# Quiet means all of these at once:
#   - no compiler, linker or MSBuild running, and the whole CPU under 10 % busy, for a minute
#     (12 samples 5 s apart);
#   - no Featherweight running (someone's own, or another bench);
#   - the user away but the display still on: last input between 2 and 4.5 minutes ago (this PC
#     turns the display off after 5 minutes; the show keeps it on once it runs);
#   - the quiet-PC lock free.
#
# The lock (shared with other quiet-PC measurements): the file below, created with CreateNew and
# held open (unshared, DeleteOnClose) for exactly as long as the measurement runs. Anyone else waits
# while it exists. A crashed holder frees it (Windows closes the handle and deletes the file).
#
# The bench takes the whole screen. It stops at once on any input, anywhere, or another program
# taking the focus (exit code 3, "aborted" in the report) and the desktop comes back; this script
# then waits for the next quiet spell and tries again, up to -Tries times.
#
# -AbortTest: no CPU wait; starts the bench, moves the mouse by one pixel 3 s later and checks that
# the show ends within a second with exit code 3.
param(
  [string]$Exe = '',   # default: build\Release\Featherweight.exe of this checkout
  [string]$Deck = '',  # default: bench50.pptx next to this script
  [Parameter(Mandatory = $true)] [string]$Report,
  [string]$Lock = 'D:\Documents\Codex\FeatherweightNotes\quiet-pc.lock',
  [int]$Tries = 3,
  [double]$MaxHours = 12,
  [switch]$AbortTest
)
$ErrorActionPreference = 'Stop'
if (-not $Exe) { $Exe = Join-Path $PSScriptRoot '..\..\..\build\Release\Featherweight.exe' }
if (-not $Deck) { $Deck = Join-Path $PSScriptRoot 'bench50.pptx' }
$Exe =[IO.Path]::GetFullPath($Exe)
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
$buildTools = 'cl', 'link', 'lib', 'ninja', 'ctest'
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
function Enter-Lock {
  if (Test-Path $Lock) {
    # Left by a crash of the whole PC (a live holder keeps it open, so it can't be deleted): only
    # when it's hours old.
    $age = (Get-Date) - (Get-Item $Lock).LastWriteTime
    if ($age.TotalHours -lt 6) { return $null }
    try { Remove-Item $Lock -Force } catch { return $null }
  }
  try {
    $fs = New-Object IO.FileStream($Lock, [IO.FileMode]::CreateNew, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None, 4096, [IO.FileOptions]::DeleteOnClose)
    $who = [Text.Encoding]::UTF8.GetBytes("p1-show bench, pid $PID, $(Get-Date -Format s)`r`n")
    $fs.Write($who, 0, $who.Length)
    $fs.Flush()
    return $fs
  } catch { return $null }
}
function Write-Status([string]$text) { Write-Output "$(Get-Date -Format 'HH:mm:ss') $text" }

$deadline = (Get-Date).AddHours($MaxHours)
$window = New-Object System.Collections.Generic.List[object]
[void][QuietPc]::Busy()
[void](Get-Busy)
$lastWhy = ''
$try = 0
while ((Get-Date) -lt $deadline) {
  Start-Sleep -Seconds 5
  $sample = Get-Busy
  $window.Add($sample)
  while ($window.Count -gt 12) { $window.RemoveAt(0) }
  $idle = [QuietPc]::IdleSeconds()
  $fw = @(Get-Process -Name Featherweight -ErrorAction SilentlyContinue).Count
  $cpuQuiet = $window.Count -ge 12 -and -not ($window | Where-Object { $_.Cpu -lt 0 -or $_.Cpu -ge 10 -or $_.Tools -gt 0 })
  $why = @()
  if (-not $AbortTest -and -not $cpuQuiet) { $why += 'CPU or a build busy' }
  if ($fw -gt 0) { $why += 'a Featherweight runs' }
  if ($idle -lt 120) { $why += 'someone at the PC' }
  if ($idle -gt 270) { $why += 'idle too long (display may go off)' }
  if ($why.Count -gt 0) {
    $now = $why -join ', '
    if ($now -ne $lastWhy) { Write-Status "waiting: $now (CPU $([math]::Round($sample.Cpu)) %, idle $([math]::Round($idle)) s)"; $lastWhy = $now }
    continue
  }
  $held = Enter-Lock  # not $lock: PowerShell names ignore case, and $Lock is the path
  if (-not $held) {
    if ($lastWhy -ne 'lock') { Write-Status "waiting: the quiet-PC lock is held"; $lastWhy = 'lock' }
    continue
  }
  $lastWhy = ''
  try {
    $try++
    if (Test-Path $Report) { Remove-Item $Report }
    Write-Status "try ${try}: bench starts (idle $([math]::Round($idle)) s)"
    $showArgs = @('--show', "`"$Deck`"", '--show-bench', "`"$Report`"")
    $p = Start-Process -FilePath $Exe -ArgumentList $showArgs -PassThru
    $null = $p.Handle  # keeps the exit code readable after the process ends
    if ($AbortTest) {
      Start-Sleep -Seconds 3
      $nudged = Get-Date
      $sent = [QuietPc]::NudgeMouse()
      $ended = $p.WaitForExit(1000)
      $ms = [math]::Round(((Get-Date) - $nudged).TotalMilliseconds)
      if (-not $ended) { $p.Kill(); $p.WaitForExit(); Write-Status "abort test FAILED: the show still ran 1 s after the input (killed)"; exit 1 }
      Write-Status "abort test: $sent inputs sent; exit code $($p.ExitCode) $ms ms after the input; report aborted: $((Get-Content $Report -Raw | ConvertFrom-Json).aborted)"
      exit $(if ($p.ExitCode -eq 3) { 0 } else { 1 })
    }
    if (-not $p.WaitForExit(15 * 60 * 1000)) { $p.Kill(); $p.WaitForExit(); Write-Status 'the bench ran 15 minutes: killed'; exit 1 }
    $code = $p.ExitCode
  } finally {
    $held.Dispose()
  }
  if ($code -eq 3) {
    Write-Status "try $try stopped on input; waiting for the next quiet spell"
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
