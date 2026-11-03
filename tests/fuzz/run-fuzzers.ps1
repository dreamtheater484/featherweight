# Runs each fuzz target for a fixed time, seeded from tests/fuzz/seeds and the office corpus
# (plan §10). CI runs it on every push (.github/workflows/build.yml); locally it runs the same way:
#
#   powershell -File tests/fuzz/run-fuzzers.ps1 -BuildDir build-fuzz -Seconds 60
#
# A crash, an AddressSanitizer report, a timeout or a broken invariant fails the run. The input
# that caused it is in -ArtifactDir\<target>, next to the target's log. libFuzzer adds the inputs
# it finds to -WorkDir\<target>, so a later run starts from there.
param(
  [string]$BuildDir = 'build-fuzz',
  [string]$Config = 'RelWithDebInfo',
  [int]$Seconds = 60,
  [string]$WorkDir = 'fuzz-work',
  [string]$ArtifactDir = 'fuzz-artifacts',
  [string[]]$Targets = @(),
  [switch]$BelowNormal,  # long local runs: leave the PC responsive
  [switch]$KeepAwake     # long local runs: no idle sleep while this script runs (nothing is changed after)
)

$ErrorActionPreference = 'Stop'
if ($KeepAwake) {
  Add-Type -Namespace Fw -Name Power -MemberDefinition '[DllImport("kernel32.dll")] public static extern uint SetThreadExecutionState(uint flags);'
  # ES_CONTINUOUS | ES_SYSTEM_REQUIRED (0x80000001), until the script ends. Written in decimal:
  # Windows PowerShell reads 0x80000001 as a negative Int32.
  $null = [Fw.Power]::SetThreadExecutionState([uint32]2147483649)
}
$office = Join-Path $PSScriptRoot '..\office'
$seeds = Join-Path $PSScriptRoot 'seeds'
$regressions = Join-Path $PSScriptRoot 'regressions'

# Each target, its seed folders (regressions included) and the longest input worth trying.
$all = [ordered]@{
  fuzz_zip     = @{ Seeds = @("$office\generated", "$office\edge", "$office\malicious", "$office\broken", "$regressions\zip"); MaxLen = 200000 }
  fuzz_xml     = @{ Seeds = @("$seeds\xml", "$regressions\xml"); MaxLen = 65536 }
  fuzz_opc     = @{ Seeds = @("$seeds\opc", "$regressions\opc"); MaxLen = 16384 }
  fuzz_check   = @{ Seeds = @("$seeds\package", "$regressions\check"); MaxLen = 65536 }
  fuzz_formula = @{ Seeds = @("$seeds\formula", "$regressions\formula"); MaxLen = 1024 }
  fuzz_docx    = @{ Seeds = @("$seeds\docx", "$seeds\package", "$regressions\docx"); MaxLen = 65536 }
}
if ($Targets.Count -eq 0) { $Targets = @($all.Keys) }

function Quote([string]$arg) { if ($arg -match '\s') { '"' + $arg + '"' } else { $arg } }

$failed = @()
foreach ($name in $Targets) {
  $target = $all[$name]
  if (-not $target) { throw "Unknown fuzz target $name" }
  $exe = Join-Path $BuildDir "tests\fuzz\$Config\$name.exe"
  if (-not (Test-Path $exe)) { throw "$exe is missing: build the fuzz targets first (see tests/fuzz/README.md)" }
  $work = Join-Path $WorkDir $name
  $artifacts = Join-Path $ArtifactDir $name
  New-Item -ItemType Directory -Force $work, $artifacts | Out-Null
  $log = Join-Path $artifacts 'log.txt'
  $arguments = @("-max_total_time=$Seconds", "-max_len=$($target.MaxLen)", '-timeout=25', '-rss_limit_mb=2560',
    '-print_final_stats=1', "-artifact_prefix=$((Resolve-Path $artifacts).Path)\", (Resolve-Path $work).Path) +
    ($target.Seeds | ForEach-Object { (Resolve-Path $_).Path })

  Write-Host "== $name for $Seconds s"
  $p = Start-Process -FilePath $exe -ArgumentList ($arguments | ForEach-Object { Quote $_ }) -NoNewWindow -PassThru `
    -RedirectStandardError $log -RedirectStandardOutput (Join-Path $artifacts 'stdout.txt')
  $null = $p.Handle  # keeps the handle, so ExitCode is there after the process ends
  if ($BelowNormal) { try { $p.PriorityClass = 'BelowNormal' } catch {} }
  $p.WaitForExit()
  Get-Content $log | Select-String -Pattern '^(#\d+\s+DONE|stat::|==\d+==ERROR|SUMMARY|Test unit written|artifact_prefix|INFO: Seed)' |
    ForEach-Object { Write-Host "   $($_.Line)" }
  if ($p.ExitCode -ne 0) {
    Write-Host "   FAILED with exit code $($p.ExitCode); the end of the log:"
    Get-Content $log -Tail 40 | ForEach-Object { Write-Host "   | $_" }
    $failed += $name
  }
}

$found = Get-ChildItem $ArtifactDir -Recurse -File -Include 'crash-*', 'timeout-*', 'oom-*', 'leak-*' -ErrorAction SilentlyContinue
if ($found) {
  Write-Host "Inputs that failed (add each to tests/fuzz/regressions once it is fixed):"
  $found | ForEach-Object { Write-Host "   $($_.FullName)" }
}
if ($failed.Count -gt 0) {
  Write-Host "Failed: $($failed -join ', ')"
  exit 1
}
Write-Host "All fuzz targets ran without findings."
