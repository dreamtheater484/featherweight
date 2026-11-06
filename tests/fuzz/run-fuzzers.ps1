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
$clipboard = Join-Path $PSScriptRoot '..\clipboard'

# Each target, its seed folders (regressions included) and the longest input worth trying.
$all = [ordered]@{
  fuzz_zip     = @{ Seeds = @("$office\generated", "$office\edge", "$office\malicious", "$office\broken", "$regressions\zip"); MaxLen = 200000 }
  fuzz_xml     = @{ Seeds = @("$seeds\xml", "$regressions\xml"); MaxLen = 65536 }
  fuzz_opc     = @{ Seeds = @("$seeds\opc", "$regressions\opc"); MaxLen = 16384 }
  fuzz_check   = @{ Seeds = @("$seeds\package", "$regressions\check"); MaxLen = 65536 }
  fuzz_formula = @{ Seeds = @("$seeds\formula", "$regressions\formula"); MaxLen = 1024 }
  fuzz_docx    = @{ Seeds = @("$seeds\docx", "$seeds\package", "$regressions\docx"); MaxLen = 65536 }
  # Pictures are big allocations: with ASan's default 256 MB quarantine of freed memory, this
  # target's memory passes 2 GB within an hour. 32 MB keeps it well below while still catching
  # use after free.
  fuzz_drawing = @{ Seeds = @("$seeds\drawing", "$regressions\drawing"); MaxLen = 65536; Asan = 'quarantine_size_mb=32' }
  fuzz_layout  = @{ Seeds = @("$seeds\docx", "$seeds\package", "$regressions\layout"); MaxLen = 65536 }
  # Inserting a picture into a document, then changing it, saving and undoing (D3). Pictures again.
  fuzz_picture = @{ Seeds = @("$seeds\picture", "$regressions\picture"); MaxLen = 65536; Asan = 'quarantine_size_mb=32' }
  # Clipboard RTF and HTML (what Word, LibreOffice and browsers put there), and copying a document.
  fuzz_rtf     = @{ Seeds = @("$clipboard", "$regressions\rtf"); MaxLen = 65536 }
  fuzz_html    = @{ Seeds = @("$clipboard", "$regressions\html"); MaxLen = 65536 }
  fuzz_clip    = @{ Seeds = @("$seeds\docx", "$seeds\package", "$regressions\clip"); MaxLen = 65536 }
  # Shapes, text boxes and watermarks (D4): typing in text boxes, shape changes, saving and undoing.
  fuzz_shapes  = @{ Seeds = @("$seeds\shapes", "$seeds\docx", "$seeds\package", "$regressions\shapes"); MaxLen = 65536 }
  # Export PDF's font subsetter (D4): tiny fonts, and locally the small fonts in C:\Windows\Fonts
  # (copied to the work folder, never committed: their licences don't allow it).
  # Compound files (D5): .doc files and password-protected Office files.
  fuzz_cfb     = @{ Seeds = @("$seeds\cfb", "$regressions\cfb"); MaxLen = 65536 }
  fuzz_sfnt    = @{ Seeds = @("$seeds\font", "$regressions\sfnt"); MaxLen = 262144; SystemFonts = $true }
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
  if ($target.SystemFonts -and (Test-Path "$env:WINDIR\Fonts")) {
    Get-ChildItem "$env:WINDIR\Fonts" -File -Include *.ttf, *.otf, *.ttc -Recurse -ErrorAction SilentlyContinue |
      Where-Object { $_.Length -le $target.MaxLen } | Select-Object -First 40 |
      ForEach-Object { Copy-Item $_.FullName (Join-Path $work $_.Name) -ErrorAction SilentlyContinue }
  }
  $log = Join-Path $artifacts 'log.txt'
  $arguments = @("-max_total_time=$Seconds", "-max_len=$($target.MaxLen)", '-timeout=25', '-rss_limit_mb=2560',
    '-print_final_stats=1', "-artifact_prefix=$((Resolve-Path $artifacts).Path)\", (Resolve-Path $work).Path) +
    ($target.Seeds | ForEach-Object { (Resolve-Path $_).Path })

  Write-Host "== $name for $Seconds s"
  $asan = $env:ASAN_OPTIONS
  if ($target.Asan) { $env:ASAN_OPTIONS = (@($target.Asan, $asan) | Where-Object { $_ }) -join ':' }  # yours win
  $p = Start-Process -FilePath $exe -ArgumentList ($arguments | ForEach-Object { Quote $_ }) -NoNewWindow -PassThru `
    -RedirectStandardError $log -RedirectStandardOutput (Join-Path $artifacts 'stdout.txt')
  $env:ASAN_OPTIONS = $asan
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
