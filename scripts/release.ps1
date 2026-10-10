<#
.SYNOPSIS
  Builds Featherweight locally and produces the installer, the portable zip and checksums.
  With -Publish it also tags the version and creates the GitHub Release (uses the gh CLI).

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\release.ps1             # build packages only
  powershell -ExecutionPolicy Bypass -File scripts\release.ps1 -Publish    # build, tag and publish

.NOTES
  The version comes from project(... VERSION x.y.z) in CMakeLists.txt.
  Requires: Visual Studio Build Tools (C++), CMake, Inno Setup 6 (winget install JRSoftware.InnoSetup),
  and for -Publish the GitHub CLI logged in to the repository owner's account.
#>
param(
  [switch]$Publish,
  [switch]$SkipTests,
  [string]$Notes = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

function Step($text) { Write-Host "==> $text" -ForegroundColor Cyan }

$cmake = Get-Content CMakeLists.txt -Raw
if ($cmake -notmatch 'project\(Featherweight VERSION (\d+\.\d+\.\d+)') { throw "Version not found in CMakeLists.txt" }
$version = $Matches[1]
$tag = "v$version"
Step "Featherweight $version"

$iscc = @(
  "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe",
  "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
  "$env:ProgramFiles\Inno Setup 6\ISCC.exe"
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $iscc) { throw "Inno Setup 6 was not found. Install it with: winget install JRSoftware.InnoSetup" }

if ($Publish) {
  if (git status --porcelain) { throw "The working tree has uncommitted changes. Commit them first." }
  git rev-parse -q --verify "refs/tags/$tag" | Out-Null
  if ($LASTEXITCODE -eq 0) { throw "Tag $tag already exists. Bump the version in CMakeLists.txt." }
  gh auth status | Out-Null
  if ($LASTEXITCODE -ne 0) { throw "The GitHub CLI is not logged in (run: gh auth login)." }
}

$build = Join-Path $root "build-release"
Step "Configuring"
cmake -S . -B $build -DFW_BUILD_TESTS=ON | Out-Host
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }
Step "Building"
cmake --build $build --config Release -- /nologo /verbosity:minimal | Out-Host
if ($LASTEXITCODE -ne 0) { throw "Build failed" }
if (-not $SkipTests) {
  Step "Running unit tests"
  ctest --test-dir $build -C Release --output-on-failure | Out-Host
  if ($LASTEXITCODE -ne 0) { throw "Unit tests failed" }
}

Step "Staging files"
$dist = Join-Path $build "dist"
if (Test-Path $dist) { Remove-Item -Recurse -Force $dist }
$stage = Join-Path $dist "Featherweight"
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item "$build\Release\Featherweight.exe", "$build\Release\pdfium.dll" $stage
Copy-Item LICENSE (Join-Path $stage "LICENSE.txt")
Copy-Item THIRD_PARTY_NOTICES.md $stage

Step "Portable zip"
$zipName = "Featherweight-$version-Portable.zip"
Compress-Archive -Path "$stage\*" -DestinationPath (Join-Path $dist $zipName)

Step "Installer"
& $iscc /Q "/DAppVersion=$version" "/DSourceDir=$stage" "/DOutputDir=$dist" installer\Featherweight.iss | Out-Host
if ($LASTEXITCODE -ne 0) { throw "Inno Setup failed" }
$setupName = "Featherweight-$version-Setup.exe"

Step "Checksums"
# The updater downloads Featherweight.exe and pdfium.dll individually and checks them against SHA256SUMS.txt.
Copy-Item "$stage\Featherweight.exe", "$stage\pdfium.dll" $dist
$assets = @("Featherweight.exe", "pdfium.dll", $zipName, $setupName)
$sums = foreach ($a in $assets) {
  "{0}  {1}" -f (Get-FileHash (Join-Path $dist $a) -Algorithm SHA256).Hash.ToLowerInvariant(), $a
}
Set-Content -Path (Join-Path $dist "SHA256SUMS.txt") -Value $sums -Encoding ascii
$assets += "SHA256SUMS.txt"

Get-ChildItem $dist -File | Format-Table Name, @{ n = "Size (MB)"; e = { "{0:N1}" -f ($_.Length / 1MB) } } | Out-Host

if (-not $Publish) {
  Write-Host "Packages are in $dist. Run with -Publish to release them on GitHub." -ForegroundColor Green
  exit 0
}

Step "Publishing $tag"
if (-not $Notes) {
  $Notes = @"
**Download:** ``$setupName`` (installer, no admin rights needed) or ``$zipName`` (portable).

Existing installations update themselves automatically.
"@
}
git tag -a $tag -m "Featherweight $version"
git push origin $tag
$paths = $assets | ForEach-Object { Join-Path $dist $_ }
gh release create $tag @paths --title "Featherweight $version" --notes $Notes
if ($LASTEXITCODE -ne 0) { throw "Creating the GitHub release failed" }
Write-Host "Released $tag" -ForegroundColor Green
