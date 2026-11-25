<#
.SYNOPSIS
  Featherweight's releases. Since F1 (4.1) releases are built, signed and drafted in CI by
  .github\workflows\release.yml; this script is the local side:
    release.ps1            builds the shipped configuration here and packages it, for testing (unsigned)
    release.ps1 -Publish   checks the 4.1 gate's report, tags the version, waits for release.yml on the tag (its
                           second build must match byte for byte), checks that no file in the draft it made carries a
                           signature Windows doesn't trust, publishes it, and writes the winget, Chocolatey and Scoop
                           manifests from the release's own hashes
    -Winget                with -Publish: also submits the version to winget

.NOTES
  The version comes from project(... VERSION x.y.z) in CMakeLists.txt.
  Local packages need Visual Studio Build Tools (C++), CMake and Inno Setup 6 (the MSI needs WiX 5 and the MSIX the
  Windows SDK; each is skipped when missing). -Publish needs the GitHub CLI logged in to the repository owner's
  account; -Winget also needs wingetcreate (winget install Microsoft.WingetCreate).
#>
param(
  [switch]$Publish,
  [switch]$Winget,
  [switch]$SkipTests,
  # A pre-release (a beta): GitHub never shows it as the latest release, so the stable channel doesn't offer it;
  # the beta channel does. -Winget isn't allowed with it.
  [switch]$Prerelease,
  [string]$Title = "",  # the release's title; by default "Featherweight x.y.z"
  [int]$WaitMinutes = 120
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
function Step($text) { Write-Host "==> $text" -ForegroundColor Cyan }

$cmake = Get-Content CMakeLists.txt -Raw
if ($cmake -notmatch 'project\(Featherweight VERSION (\d+\.\d+\.\d+)') { throw "Version not found in CMakeLists.txt" }
$version = $Matches[1]
$tag = "v$version"
$repo = "dreamtheater484/featherweight"
Step "Featherweight $version"
if ($Prerelease -and $Winget) { throw "-Winget can't be used with -Prerelease: winget gets stable releases only." }
if (-not $Title) { $Title = "Featherweight $version" }

if (-not $Publish) {
  # A local build of the shipped configuration, for testing. The packages carry the PDFium build this PC has
  # (CI's carry the standard one); nothing is signed.
  $build = Join-Path $root "build-release"
  Step "Configuring"
  cmake -S . -B $build -DFW_BUILD_TESTS=ON -DFW_TEST_SCREENS=OFF | Out-Host
  if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }
  Step "Building"
  cmake --build $build --config Release -- /nologo /verbosity:minimal | Out-Host
  if ($LASTEXITCODE -ne 0) { throw "Build failed" }
  if (-not $SkipTests) {
    Step "Running unit tests"
    ctest --test-dir $build -C Release --output-on-failure | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "Unit tests failed" }
  }
  $dist = Join-Path $build "dist"
  if (Test-Path $dist) { Remove-Item -Recurse -Force $dist }
  & (Join-Path $root "packaging\package.ps1") -Stage "$build\Release" -Arch x64 -Version $version -Out $dist
  & (Join-Path $root "scripts\release-hashes.ps1") -Dir $dist -Out (Join-Path $dist "SHA256SUMS.txt") -Sums
  Write-Host "Packages are in $dist (unsigned, for testing). Releases are built in CI: release.ps1 -Publish." -ForegroundColor Green
  exit 0
}

# ---- Publish ----
if (git status --porcelain) { throw "The working tree has uncommitted changes. Commit them first." }
git rev-parse -q --verify "refs/tags/$tag" | Out-Null
if ($LASTEXITCODE -eq 0) { throw "Tag $tag already exists. Bump the version in CMakeLists.txt." }
gh auth status 2>&1 | Out-Null
if ($LASTEXITCODE -ne 0) { throw "The GitHub CLI is not logged in (run: gh auth login)." }
$commit = (git rev-parse HEAD).Trim()

# The gate (f1-gate's check, agreed with f1-ship): from 4.1 on, scripts\f1-gate.ps1 must have passed on this commit:
# its report is for HEAD, made on a clean tree, with no failed row (rows waiting for Roy's steps don't block).
if ([version]$version -ge [version]"4.1.0") {
  $minor = ($version -split '\.')[0..1] -join '.'
  $gateReport = if ($env:FW_GATE_REPORT) { $env:FW_GATE_REPORT } else { "D:\Documents\Codex\FeatherweightNotes\f1\gate-$minor.json" }
  $gate = Join-Path $root "scripts\f1-gate.ps1"
  if (-not (Test-Path $gate)) { throw "scripts\f1-gate.ps1 isn't here: from 4.1 on a release needs the gate's report." }
  Step "Checking the gate's report ($gateReport)"
  & powershell -NoProfile -ExecutionPolicy Bypass -File $gate -Verify $gateReport | Out-Host
  if ($LASTEXITCODE -ne 0) { throw "The $minor gate hasn't passed on $commit ($gateReport): run scripts\f1-gate.ps1 on it first." }
}

Step "Tagging $tag at $commit"
git tag -a $tag -m "Featherweight $version"
git push origin $tag
if ($LASTEXITCODE -ne 0) { throw "Pushing $tag failed" }

Step "Waiting for release.yml on $tag"
$run = $null
for ($i = 0; $i -lt 60 -and -not $run; $i++) {
  Start-Sleep -Seconds 10
  $run = gh run list --repo $repo --workflow release.yml --branch $tag --limit 1 --json databaseId,headSha | ConvertFrom-Json |
    Where-Object { $_.headSha -eq $commit } | Select-Object -First 1
}
if (-not $run) { throw "release.yml didn't start for $tag" }
gh run watch $run.databaseId --repo $repo --exit-status --interval 60
if ($LASTEXITCODE -ne 0) { throw "release.yml failed for ${tag}: https://github.com/$repo/actions/runs/$($run.databaseId)" }
# Its "reproducible (compare)" job built the commit a second time on fresh runners: the release can't be drafted
# unless the program files matched byte for byte.
$jobs = (gh run view $run.databaseId --repo $repo --json jobs | ConvertFrom-Json).jobs
$repro = $jobs | Where-Object { $_.name -like "reproducible (compare)*" }
if (-not $repro -or $repro.conclusion -ne "success") { throw "The second build didn't match the first." }
# The scan of the tag build's files (f1-trust's scan-release.yml, its "scan-report" artifact): a file a scanner
# flagged stops the release; "waiting" (no keys yet, Roy's step 5) doesn't.
$scanDir = Join-Path ([IO.Path]::GetTempPath()) "fw-scan-$($run.databaseId)"
Remove-Item -Recurse -Force $scanDir -ErrorAction SilentlyContinue
& {
  $ErrorActionPreference = "Continue"  # no artifact is an answer, not an error
  gh run download $run.databaseId --repo $repo -n scan-report -D $scanDir 2>&1 | Out-Null
}
$summaryFile = Get-ChildItem $scanDir -Recurse -Filter summary.json -ErrorAction SilentlyContinue | Select-Object -First 1
if ($summaryFile) {
  $scan = Get-Content $summaryFile.FullName -Raw | ConvertFrom-Json
  Write-Host "The scan: $($scan.verdict)"
  if (-not $scan.pass -and -not $scan.waiting) { throw "The scan flagged the tag build's files: $($scan.verdict)" }
} else {
  Write-Warning "The tag build has no scan report (scan-report): not checked."
}

$view = gh release view $tag --repo $repo --json isDraft,assets | ConvertFrom-Json
if (-not $view.isDraft) { throw "The release $tag isn't a draft: was it published already?" }
# A safety net: every program file in the draft (the assets, and the exe and DLLs in the portable zips) is either
# signed by a publisher Windows trusts (SignPath, after Roy's step 1) or not signed at all (4.1). A signature
# Windows doesn't trust, such as the CI's test certificate, is never published.
Step "Checking the draft's signatures"
$sigDir = Join-Path ([IO.Path]::GetTempPath()) "fw-draft-$($run.databaseId)"
Remove-Item -Recurse -Force $sigDir -ErrorAction SilentlyContinue
$code = & {
  $ErrorActionPreference = "Continue"  # gh's messages on stderr aren't errors; its exit code is the answer
  gh release download $tag --repo $repo -D $sigDir -p "*.exe" -p "*.dll" -p "*.msi" -p "*.msix" -p "*-Portable*.zip" 2>&1 | Out-Host
  $LASTEXITCODE
}
if ($code -ne 0) { throw "Downloading the draft's files failed" }
foreach ($z in Get-ChildItem $sigDir -Filter *.zip) { Expand-Archive $z.FullName (Join-Path $sigDir $z.BaseName) }
$checked = @(Get-ChildItem $sigDir -Recurse -File -Include *.exe, *.dll, *.msi, *.msix)
$untrusted = @($checked | ForEach-Object {
  $s = (Get-AuthenticodeSignature $_.FullName).Status
  if ("$s" -ne "Valid" -and "$s" -ne "NotSigned") { "$($_.FullName.Substring($sigDir.Length + 1)) ($s)" }
})
Remove-Item -Recurse -Force $sigDir
if (-not $checked.Count) { throw "The draft $tag has no program files to check." }
if ($untrusted.Count) { throw "The draft $tag has files with a signature Windows doesn't trust: $($untrusted -join ', ')" }
Write-Host "$($checked.Count) program files: each signed by a trusted publisher or not signed"

Step "Publishing the draft"
$pre = @()
if ($Prerelease) { $pre = @("--prerelease") }
gh release edit $tag --repo $repo --draft=false --title $Title @pre
if ($LASTEXITCODE -ne 0) { throw "Publishing the release failed" }
Write-Host "Released $tag" -ForegroundColor Green

Step "Manifests (winget, Chocolatey, Scoop)"
& (Join-Path $root "packaging\manifests.ps1") -Version $version -Tag $tag
git add packaging\winget packaging\chocolatey packaging\scoop
git commit -q -m "Manifests for Featherweight $version (winget, Chocolatey, Scoop), from the release's hashes"
git push

if ($Winget) {
  Step "Submitting $version to winget"
  # Opens a pull request on microsoft/winget-pkgs from your GitHub account; Microsoft reviews it automatically.
  $env:WINGET_CREATE_GITHUB_TOKEN = gh auth token  # read by wingetcreate; never printed
  $urls = @("https://github.com/$repo/releases/download/$tag/Featherweight-$version-Setup.exe")
  if ($view.assets.name -contains "Featherweight-$version-Setup-arm64.exe") {
    $urls += "https://github.com/$repo/releases/download/$tag/Featherweight-$version-Setup-arm64.exe"
  }
  wingetcreate update dreamtheater484.Featherweight --version $version --urls @urls --submit
  Remove-Item Env:WINGET_CREATE_GITHUB_TOKEN
  if ($LASTEXITCODE -ne 0) { throw "winget submission failed" }
}
