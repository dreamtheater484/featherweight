<#
.SYNOPSIS
  Packages built (and, in CI, signed) program files for one architecture (f1-ship): the portable zip, the
  installer (Inno Setup), the per-machine MSI (WiX), the MSIX package, and the updater's assets.
  scripts\release.ps1 calls it locally; .github\workflows\release.yml calls it in CI.

.PARAMETER Stage
  A folder holding Featherweight.exe, pdfium.dll (the standard PDFium build, which the packages carry) and,
  optionally, pdfium-v8.dll (the form script engine, published as a separate asset only).

.EXAMPLE
  packaging\package.ps1 -Stage build-release\Release -Arch x64 -Version 4.1.0 -Out build-release\dist
#>
param(
  [Parameter(Mandatory)][string]$Stage,
  [ValidateSet("x64", "arm64")][string]$Arch = "x64",
  [Parameter(Mandatory)][string]$Version,
  [Parameter(Mandatory)][string]$Out,
  [ValidateSet("Zip", "Setup", "Msi", "Msix")][string[]]$Kinds = @("Zip", "Setup", "Msi", "Msix"),
  # The MSIX identity. Sideloaded packages are signed by the release's publisher, whose certificate subject this
  # must equal; the Store re-signs with the identity Partner Center reserves (Roy's step 2, packaging/msix/README.md).
  [string]$MsixName = "Featherweight.Featherweight",
  [string]$MsixPublisher = "CN=Featherweight Test Publisher, O=Featherweight Tests",
  [string]$MsixPublisherDisplayName = "Featherweight",
  [switch]$Strict  # fail when a tool for a requested kind is missing (CI); locally such kinds are skipped
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$sfx = if ($Arch -eq "arm64") { "-arm64" } else { "" }
function Step($text) { Write-Host "==> $text" -ForegroundColor Cyan }
function Missing($what) {
  if ($Strict) { throw "$what is missing" }
  Write-Warning "$what is missing: skipped"
}
function SdkTool($name) {
  $kits = "${env:ProgramFiles(x86)}\Windows Kits\10\bin"
  $hostArch = if ($env:PROCESSOR_ARCHITECTURE -eq "ARM64") { "arm64" } else { "x64" }
  Get-ChildItem "$kits\10.*\$hostArch\$name", "$kits\10.*\x64\$name", "$kits\$hostArch\$name" -ErrorAction SilentlyContinue |
    Sort-Object FullName | Select-Object -Last 1 -ExpandProperty FullName
}

$Stage = (Resolve-Path $Stage).Path
foreach ($f in "Featherweight.exe", "pdfium.dll") {
  if (-not (Test-Path (Join-Path $Stage $f))) { throw "$f isn't in $Stage" }
}
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path $Out).Path
$work = Join-Path $Out "work$sfx"
if (Test-Path $work) { Remove-Item -Recurse -Force $work }
$files = Join-Path $work "Featherweight"  # what every package installs
New-Item -ItemType Directory -Force $files | Out-Null
Copy-Item (Join-Path $Stage "Featherweight.exe"), (Join-Path $Stage "pdfium.dll") $files
Copy-Item (Join-Path $root "LICENSE") (Join-Path $files "LICENSE.txt")
Copy-Item (Join-Path $root "THIRD_PARTY_NOTICES.md") $files

Step "Updater assets ($Arch)"
# The updater downloads these one by one; the PDFium builds by name, so an install keeps the build it has.
Copy-Item (Join-Path $Stage "Featherweight.exe") (Join-Path $Out "Featherweight$sfx.exe") -Force
Copy-Item (Join-Path $Stage "pdfium.dll") (Join-Path $Out "pdfium-standard$sfx.dll") -Force
if (Test-Path (Join-Path $Stage "pdfium-v8.dll")) {
  Copy-Item (Join-Path $Stage "pdfium-v8.dll") (Join-Path $Out "pdfium-v8$sfx.dll") -Force
}

if ($Kinds -contains "Zip") {
  Step "Portable zip ($Arch)"
  $zip = Join-Path $Out "Featherweight-$Version-Portable$sfx.zip"
  Remove-Item $zip -ErrorAction SilentlyContinue
  Compress-Archive -Path "$files\*" -DestinationPath $zip
}

if ($Kinds -contains "Setup") {
  $iscc = @("$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe", "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
            "$env:ProgramFiles\Inno Setup 6\ISCC.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
  if (-not $iscc) { Missing "Inno Setup 6 (ISCC.exe)" } else {
    Step "Installer ($Arch)"
    & $iscc /Q "/DAppVersion=$Version" "/DSourceDir=$files" "/DOutputDir=$Out" "/DArch=$Arch" `
      (Join-Path $root "installer\Featherweight.iss") | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "Inno Setup failed" }
  }
}

if ($Kinds -contains "Msi") {
  $wix = Get-Command wix -ErrorAction SilentlyContinue
  if (-not $wix) { Missing "WiX Toolset (wix)" } else {
    Step "MSI ($Arch)"
    $msi = Join-Path $Out "Featherweight-$Version$sfx.msi"
    & $wix.Source build (Join-Path $root "packaging\wix\Featherweight.wxs") -arch $Arch -d "Version=$Version" `
      -d "SourceDir=$files" -d "IconFile=$(Join-Path $root 'src\Featherweight.ico')" -o $msi | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "WiX failed" }
    Remove-Item (Join-Path $Out "Featherweight-$Version$sfx.wixpdb") -ErrorAction SilentlyContinue
  }
}

if ($Kinds -contains "Msix") {
  $makeappx = SdkTool "makeappx.exe"
  $makepri = SdkTool "makepri.exe"
  if (-not $makeappx -or -not $makepri) { Missing "makeappx.exe and makepri.exe (Windows SDK)" } else {
    Step "MSIX ($Arch)"
    $pkg = Join-Path $work "msix"
    Copy-Item -Recurse $files $pkg
    $assets = Join-Path $pkg "Assets"
    New-Item -ItemType Directory -Force $assets | Out-Null
    # The tiles and the Store logo, drawn from the app's icon.
    Add-Type -AssemblyName System.Drawing
    # The icon's largest frame (256 px, stored as PNG in the .ico).
    $ico = [IO.File]::ReadAllBytes((Join-Path $root "src\Featherweight.ico"))
    $best = $null
    for ($i = 0; $i -lt [BitConverter]::ToUInt16($ico, 4); $i++) {
      $e = 6 + 16 * $i
      $w = if ($ico[$e] -eq 0) { 256 } else { $ico[$e] }
      if (-not $best -or $w -gt $best.w) {
        $best = @{ w = $w; size = [BitConverter]::ToInt32($ico, $e + 8); at = [BitConverter]::ToInt32($ico, $e + 12) }
      }
    }
    $frame = New-Object byte[] $best.size
    [Array]::Copy($ico, $best.at, $frame, 0, $best.size)
    if ($frame[0] -eq 0x89 -and $frame[1] -eq 0x50) {
      $big = [System.Drawing.Image]::FromStream((New-Object IO.MemoryStream(, $frame)))
    } else {
      $big = (New-Object System.Drawing.Icon((Join-Path $root "src\Featherweight.ico"), $best.w, $best.w)).ToBitmap()
    }
    foreach ($a in @(@{ n = "Square44x44Logo.png"; w = 44; h = 44 }, @{ n = "Square150x150Logo.png"; w = 150; h = 150 },
                     @{ n = "Wide310x150Logo.png"; w = 310; h = 150 }, @{ n = "StoreLogo.png"; w = 50; h = 50 })) {
      $bmp = New-Object System.Drawing.Bitmap($a.w, $a.h)
      $g = [System.Drawing.Graphics]::FromImage($bmp)
      $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
      $side = [Math]::Min($a.w, $a.h)
      $g.DrawImage($big, [int](($a.w - $side) / 2), [int](($a.h - $side) / 2), $side, $side)
      $bmp.Save((Join-Path $assets $a.n), [System.Drawing.Imaging.ImageFormat]::Png)
      $g.Dispose(); $bmp.Dispose()
    }
    $big.Dispose()
    $manifest = Get-Content (Join-Path $root "packaging\msix\AppxManifest.xml") -Raw
    $esc = [Security.SecurityElement]
    $manifest = $manifest.Replace("@NAME@", $esc::Escape($MsixName)).Replace("@PUBLISHER@", $esc::Escape($MsixPublisher)).
      Replace("@PUBLISHER_DISPLAY_NAME@", $esc::Escape($MsixPublisherDisplayName)).Replace("@VERSION@", "$Version.0").
      Replace("@ARCH@", $Arch)
    Set-Content -Path (Join-Path $pkg "AppxManifest.xml") -Value $manifest -Encoding UTF8
    Push-Location $pkg
    try {
      & $makepri createconfig /cf priconfig.xml /dq en-US /o | Out-Null
      & $makepri new /pr . /cf priconfig.xml /of resources.pri /o | Out-Null
      if ($LASTEXITCODE -ne 0) { throw "makepri failed" }
      Remove-Item priconfig.xml
    } finally { Pop-Location }
    $msix = Join-Path $Out "Featherweight-$Version$sfx.msix"
    & $makeappx pack /d $pkg /p $msix /o | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "makeappx failed" }
  }
}

Remove-Item -Recurse -Force $work
Get-ChildItem $Out -File | Where-Object { $_.Name -like "*$sfx*" -or -not $sfx } |
  Format-Table Name, @{ n = "Size (MB)"; e = { "{0:N1}" -f ($_.Length / 1MB) } } | Out-Host
