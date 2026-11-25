# The packaging tools on a CI runner (f1-ship), pinned by version and hash. CI only: the reference PC gets no
# downloads (docs/f1-plan.md, "Reference PC").
#   Inno Setup 6: the runner image's, else Chocolatey's package (which checks the installer's hash).
#   WiX 5: the .NET tool, its package checked against the SHA-512 below.
param(
  [string]$InnoVersion = "6.4.3",
  [string]$WixVersion = "5.0.2",
  [string]$WixSha512 = ""  # of wix.$WixVersion.nupkg; empty prints it (to pin it here) instead of checking
)
$ErrorActionPreference = "Stop"
if ($env:GITHUB_ACTIONS -ne "true") { throw "ci-tools.ps1 installs tools: it runs on CI runners only." }

$iscc = @("${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe", "$env:ProgramFiles\Inno Setup 6\ISCC.exe") |
  Where-Object { Test-Path $_ } | Select-Object -First 1
if ($iscc) {
  Write-Host "Inno Setup: the runner's ($iscc)"
} else {
  choco install innosetup --version $InnoVersion --no-progress -y | Out-Host
  if ($LASTEXITCODE) { throw "Installing Inno Setup $InnoVersion failed" }
}

dotnet tool install --global wix --version $WixVersion | Out-Host
if ($LASTEXITCODE) { throw "Installing WiX $WixVersion failed" }
$nupkg = Join-Path $env:USERPROFILE ".nuget\packages\wix\$WixVersion\wix.$WixVersion.nupkg"
if (Test-Path $nupkg) {
  $hash = (Get-FileHash $nupkg -Algorithm SHA512).Hash.ToLowerInvariant()
  if (-not $WixSha512) { Write-Host "::warning::WiX $WixVersion isn't pinned yet: its SHA-512 is $hash" }
  elseif ($hash -ne $WixSha512) { throw "WiX $WixVersion's package has SHA-512 $hash, not the pinned $WixSha512" }
} else {
  Write-Host "::warning::WiX's package isn't in the NuGet cache ($nupkg): not checked"
}
"$env:USERPROFILE\.dotnet\tools" | Out-File -Append -Encoding utf8 $env:GITHUB_PATH
& "$env:USERPROFILE\.dotnet\tools\wix.exe" --version
