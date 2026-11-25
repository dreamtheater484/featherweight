# Silent installs and uninstalls (f1-ship): the MSI per machine, the installer per machine (/ALLUSERS) and per user,
# each started once (an off-screen snapshot of a PDF) and removed again. CI only: it installs software.
param([Parameter(Mandatory)][string]$Dist)
$ErrorActionPreference = "Stop"
if ($env:GITHUB_ACTIONS -ne "true") { throw "install-test.ps1 installs software: it runs on CI runners only." }
$Dist = (Resolve-Path $Dist).Path
$root = Split-Path -Parent $PSScriptRoot
$pdf = Join-Path $root "tests\pdfs\text.pdf"
$msi = Get-ChildItem $Dist -Filter "Featherweight-*.msi" | Where-Object { $_.Name -notmatch "arm64" } | Select-Object -First 1
$setup = Get-ChildItem $Dist -Filter "Featherweight-*-Setup.exe" | Select-Object -First 1
if (-not $msi -or -not $setup) { throw "The x64 MSI or installer isn't in $Dist" }

function Smoke([string]$exe) {
  if (-not (Test-Path $exe)) { throw "$exe wasn't installed" }
  $png = Join-Path $env:RUNNER_TEMP "smoke.png"
  Remove-Item $png -ErrorAction SilentlyContinue
  $p = Start-Process -FilePath $exe -ArgumentList "--snapshot", $png, $pdf -PassThru -Wait
  if ($p.ExitCode -ne 0 -or -not (Test-Path $png)) { throw "$exe didn't start ($($p.ExitCode))" }
  Write-Host "  started: $exe"
}
function WaitGone([string]$path) {
  for ($i = 0; $i -lt 120 -and (Test-Path $path); $i++) { Start-Sleep -Milliseconds 500 }
  if (Test-Path $path) { throw "$path is still there after uninstalling" }
}

Write-Host "== MSI, per machine"
$p = Start-Process msiexec -ArgumentList "/i", "`"$($msi.FullName)`"", "/qn", "/norestart", "/l*v", "`"$env:RUNNER_TEMP\msi-install.log`"" -PassThru -Wait
if ($p.ExitCode -ne 0) { Get-Content "$env:RUNNER_TEMP\msi-install.log" -Tail 40 | Out-Host; throw "msiexec /i failed ($($p.ExitCode))" }
$exe = Join-Path $env:ProgramFiles "Featherweight\Featherweight.exe"
Smoke $exe
if (-not (Test-Path "Registry::HKEY_LOCAL_MACHINE\Software\Classes\Featherweight.PDF\shell\open\command")) { throw "The MSI registered no file types" }
$p = Start-Process msiexec -ArgumentList "/x", "`"$($msi.FullName)`"", "/qn", "/norestart" -PassThru -Wait
if ($p.ExitCode -ne 0) { throw "msiexec /x failed ($($p.ExitCode))" }
WaitGone $exe

Write-Host "== Installer, per machine (/ALLUSERS)"
$p = Start-Process $setup.FullName -ArgumentList "/VERYSILENT", "/ALLUSERS", "/SUPPRESSMSGBOXES", "/NORESTART", "/LOG=`"$env:RUNNER_TEMP\setup-machine.log`"" -PassThru -Wait
if ($p.ExitCode -ne 0) { throw "Setup /ALLUSERS failed ($($p.ExitCode))" }
Smoke $exe
$p = Start-Process (Join-Path $env:ProgramFiles "Featherweight\unins000.exe") -ArgumentList "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART" -PassThru -Wait
WaitGone $exe

Write-Host "== Installer, per user"
$p = Start-Process $setup.FullName -ArgumentList "/VERYSILENT", "/CURRENTUSER", "/SUPPRESSMSGBOXES", "/NORESTART", "/LOG=`"$env:RUNNER_TEMP\setup-user.log`"" -PassThru -Wait
if ($p.ExitCode -ne 0) { throw "Setup /CURRENTUSER failed ($($p.ExitCode))" }
$userExe = Join-Path $env:LOCALAPPDATA "Programs\Featherweight\Featherweight.exe"
Smoke $userExe
$p = Start-Process (Join-Path $env:LOCALAPPDATA "Programs\Featherweight\unins000.exe") -ArgumentList "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART" -PassThru -Wait
WaitGone $userExe
Write-Host "All installs and uninstalls passed."
