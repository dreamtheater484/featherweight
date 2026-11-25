<#
.SYNOPSIS
  Writes the package managers' manifests for a release (f1-ship): winget (packaging\winget\<version>\), Chocolatey
  (packaging\chocolatey\) and Scoop (packaging\scoop\featherweight.json), with the release's download URLs and
  SHA-256 hashes. scripts\release.ps1 -Publish runs it once the release is out; the hashes come from the release's
  SHA256SUMS.txt (-Sums) or from GitHub's asset digests (-Tag), so nothing is downloaded.

  winget is submitted by release.ps1 -Winget. Chocolatey and Scoop wait for Roy's step 3 (an API key; an OK for the
  pull request to the Extras bucket): packaging\chocolatey\README.md and packaging\scoop\README.md say how.
#>
param(
  [Parameter(Mandatory)][string]$Version,
  [string]$Sums,  # a SHA256SUMS.txt
  [string]$Tag,   # or read the digests of this release on GitHub
  [string]$Out = (Split-Path -Parent $PSCommandPath),
  [string]$Repo = "dreamtheater484/featherweight",
  [string]$ReleaseDate = (Get-Date -Format "yyyy-MM-dd")
)
$ErrorActionPreference = "Stop"
$hash = @{}
if ($Sums) {
  foreach ($line in Get-Content $Sums) {
    if ($line -match '^([0-9a-fA-F]{64})\s+\*?(.+)$') { $hash[$Matches[2].Trim()] = $Matches[1].ToLowerInvariant() }
  }
} elseif ($Tag) {
  $release = gh api "repos/$Repo/releases/tags/$Tag" | ConvertFrom-Json
  foreach ($a in $release.assets) {
    if ($a.digest -match '^sha256:([0-9a-f]{64})$') { $hash[$a.name] = $Matches[1] }
  }
} else { throw "Give -Sums or -Tag" }

$base = "https://github.com/$Repo/releases/download/v$Version"
$files = @{
  setup = "Featherweight-$Version-Setup.exe"; setupArm = "Featherweight-$Version-Setup-arm64.exe"
  zip = "Featherweight-$Version-Portable.zip"; zipArm = "Featherweight-$Version-Portable-arm64.zip"
}
foreach ($k in "setup", "zip") {
  if (-not $hash[$files[$k]]) { throw "No SHA-256 for $($files[$k])" }
}
$hasArm = [bool]$hash[$files.setupArm]
function Write-Utf8([string]$path, [string]$text) {
  New-Item -ItemType Directory -Force (Split-Path -Parent $path) | Out-Null
  [IO.File]::WriteAllText($path, $text.Replace("`r`n", "`n"), (New-Object Text.UTF8Encoding($false)))
}
$description = "A light, fast office suite for Windows: PDFs, documents, spreadsheets and presentations. No account, no subscription, no telemetry."

# --- winget (manifest schema 1.12) ---
$id = "dreamtheater484.Featherweight"
$wg = Join-Path $Out "winget\$Version"
$installers = "- Architecture: x64`n  InstallerUrl: $base/$($files.setup)`n  InstallerSha256: $($hash[$files.setup].ToUpperInvariant())`n"
if ($hasArm) {
  $installers += "- Architecture: arm64`n  InstallerUrl: $base/$($files.setupArm)`n  InstallerSha256: $($hash[$files.setupArm].ToUpperInvariant())`n"
}
Write-Utf8 "$wg\$id.yaml" @"
# yaml-language-server: `$schema=https://aka.ms/winget-manifest.version.1.12.0.schema.json

PackageIdentifier: $id
PackageVersion: $Version
DefaultLocale: en-US
ManifestType: version
ManifestVersion: 1.12.0
"@
Write-Utf8 "$wg\$id.installer.yaml" @"
# yaml-language-server: `$schema=https://aka.ms/winget-manifest.installer.1.12.0.schema.json

PackageIdentifier: $id
PackageVersion: $Version
Platform:
- Windows.Desktop
MinimumOSVersion: 10.0.18362.0
InstallerType: inno
Scope: user
InstallModes:
- interactive
- silent
- silentWithProgress
UpgradeBehavior: install
FileExtensions:
- pdf
- docx
- doc
- odt
- rtf
- xlsx
- xls
- ods
- csv
- pptx
- ppt
- odp
ProductCode: Featherweight_is1
ReleaseDate: $ReleaseDate
AppsAndFeaturesEntries:
- DisplayName: Featherweight
  Publisher: Featherweight
  ProductCode: Featherweight_is1
Installers:
$installers
ManifestType: installer
ManifestVersion: 1.12.0
"@
Write-Utf8 "$wg\$id.locale.en-US.yaml" @"
# yaml-language-server: `$schema=https://aka.ms/winget-manifest.defaultLocale.1.12.0.schema.json

PackageIdentifier: $id
PackageVersion: $Version
PackageLocale: en-US
Publisher: Featherweight
PublisherUrl: https://github.com/dreamtheater484
PublisherSupportUrl: https://github.com/$Repo/issues
PrivacyUrl: https://dreamtheater484.github.io/featherweight/privacy/
Author: Roy Clerx
PackageName: Featherweight
PackageUrl: https://github.com/$Repo
License: MIT
LicenseUrl: https://github.com/$Repo/blob/main/LICENSE
Copyright: Copyright (c) 2026 Roy Clerx
ShortDescription: $description
Description: |-
  Featherweight opens PDFs, documents, spreadsheets and presentations instantly, and edits them: fill in and
  sign forms, write and format documents, calculate in workbooks, build slide shows. It's small, starts fast and
  uses little memory.
  No account, no subscription, no telemetry; the only network access is the update check, which can be turned off.
Moniker: featherweight
Tags:
- pdf
- pdf-reader
- office
- documents
- spreadsheets
- presentations
ReleaseNotesUrl: https://github.com/$Repo/releases/tag/v$Version
ManifestType: defaultLocale
ManifestVersion: 1.12.0
"@

# --- Chocolatey: installs per machine with the installer, silently ---
$choco = Join-Path $Out "chocolatey"
Write-Utf8 "$choco\featherweight.nuspec" @"
<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://schemas.microsoft.com/packaging/2015/06/nuspec.xsd">
  <metadata>
    <id>featherweight</id>
    <version>$Version</version>
    <title>Featherweight</title>
    <authors>Roy Clerx</authors>
    <owners>dreamtheater484</owners>
    <projectUrl>https://github.com/$Repo</projectUrl>
    <licenseUrl>https://github.com/$Repo/blob/main/LICENSE</licenseUrl>
    <requireLicenseAcceptance>false</requireLicenseAcceptance>
    <projectSourceUrl>https://github.com/$Repo</projectSourceUrl>
    <bugTrackerUrl>https://github.com/$Repo/issues</bugTrackerUrl>
    <releaseNotes>https://github.com/$Repo/releases/tag/v$Version</releaseNotes>
    <tags>pdf office documents spreadsheets presentations foss</tags>
    <summary>$description</summary>
    <description>$description The only network access is the update check, which can be turned off.</description>
  </metadata>
  <files>
    <file src="tools\**" target="tools" />
  </files>
</package>
"@
Write-Utf8 "$choco\tools\chocolateyinstall.ps1" @"
`$ErrorActionPreference = 'Stop'
# Featherweight $Version, per machine (Program Files): it doesn't update itself there; choco upgrade does.
`$packageArgs = @{
  packageName    = `$env:ChocolateyPackageName
  fileType       = 'exe'
  url64bit       = '$base/$($files.setup)'
  checksum64     = '$($hash[$files.setup])'
  checksumType64 = 'sha256'
  silentArgs     = '/VERYSILENT /ALLUSERS /SUPPRESSMSGBOXES /NORESTART /SP-'
  validExitCodes = @(0)
}
Install-ChocolateyPackage @packageArgs
"@

# --- Scoop: the portable zip; settings kept across updates; Scoop updates it, so the app's own updater is off ---
$scoop = [ordered]@{
  version     = $Version
  description = $description
  homepage    = "https://github.com/$Repo"
  license     = "MIT"
  architecture = [ordered]@{ "64bit" = [ordered]@{ url = "$base/$($files.zip)"; hash = $hash[$files.zip] } }
  pre_install = @('if (!(Test-Path "$persist_dir\Featherweight.ini")) { Set-Content "$dir\Featherweight.ini" "[General]`r`nUpdateAutomatically=false" -Encoding Ascii }')
  bin         = "Featherweight.exe"
  shortcuts   = @(, @("Featherweight.exe", "Featherweight"))
  persist     = @("Featherweight.ini", "Backups", "Recovery")
  checkver    = "github"
  autoupdate  = [ordered]@{ architecture = [ordered]@{ "64bit" = [ordered]@{ url = "https://github.com/$Repo/releases/download/v`$version/Featherweight-`$version-Portable.zip" } } }
}
if ($hash[$files.zipArm]) {
  $scoop.architecture["arm64"] = [ordered]@{ url = "$base/$($files.zipArm)"; hash = $hash[$files.zipArm] }
  $scoop.autoupdate.architecture["arm64"] = [ordered]@{ url = "https://github.com/$Repo/releases/download/v`$version/Featherweight-`$version-Portable-arm64.zip" }
}
Write-Utf8 (Join-Path $Out "scoop\featherweight.json") (($scoop | ConvertTo-Json -Depth 6) + "`n")
Write-Host "Manifests for $Version in $Out (winget\$Version, chocolatey, scoop)"
