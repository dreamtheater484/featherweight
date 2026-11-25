<#
.SYNOPSIS
  PDFium's update routine (f1-ship; docs/f1-plan.md, "a PDFium and V8 update routine"). Featherweight uses the prebuilt
  PDFium from bblanchon/pdfium-binaries, in two builds (standard, and v8 with V8 and XFA) for x64 and ARM64, pinned
  in cmake/FetchPdfium.cmake by build number and SHA-256.

    update-pdfium.ps1 -Check            says whether a newer build than the pinned one is out (the weekly workflow
                                        .github/workflows/pdfium-update.yml opens an issue when there is)
    update-pdfium.ps1 [-Build 8123]     pins that build (default: the newest), with GitHub's digest of each archive

  Nothing is downloaded here: the hashes come from GitHub's release metadata, and the next CMake configure downloads
  the archives and checks them against those hashes. Then build, run ctest and the PDF checks (fwtool pdf-compare on
  tests\pdfs, the PDF torture run), and commit cmake/FetchPdfium.cmake with the release's notes in the message.
#>
param([switch]$Check, [string]$Build = "", [string]$Json = "")
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$cmakeFile = Join-Path $root "cmake\FetchPdfium.cmake"
$text = [IO.File]::ReadAllText($cmakeFile)
if ($text -notmatch 'set\(FW_PDFIUM_BUILD "(\d+)"\)') { throw "No FW_PDFIUM_BUILD in $cmakeFile" }
$pinned = [int]$Matches[1]
$repo = "bblanchon/pdfium-binaries"

function Get-Release([string]$tag) {
  $path = if ($tag) { "repos/$repo/releases/tags/" + [Uri]::EscapeDataString($tag) } else { "repos/$repo/releases/latest" }
  $r = gh api $path | ConvertFrom-Json
  if (-not $r -or $r.tag_name -notmatch '^chromium/(\d+)$') { throw "No PDFium release for '$tag'" }
  $r
}

$release = Get-Release $(if ($Build) { "chromium/$Build" } else { "" })
$latest = [int]($release.tag_name -replace '^chromium/', '')

if ($Check) {
  $result = [ordered]@{ pinned = $pinned; latest = $latest; newer = $latest -gt $pinned; url = $release.html_url;
                        published = $release.published_at }
  if ($Json) { $result | ConvertTo-Json | Set-Content -Encoding utf8 $Json }
  if ($result.newer) { Write-Host "PDFium $latest is out ($($release.published_at)); pinned: $pinned. $($release.html_url)" }
  else { Write-Host "PDFium $pinned is the newest build." }
  exit 0
}

$wanted = [ordered]@{
  standard_x64 = "pdfium-win-x64.tgz"; v8_x64 = "pdfium-v8-win-x64.tgz"
  standard_arm64 = "pdfium-win-arm64.tgz"; v8_arm64 = "pdfium-v8-win-arm64.tgz"
}
foreach ($k in $wanted.Keys) {
  $asset = $release.assets | Where-Object { $_.name -eq $wanted[$k] }
  if (-not $asset -or $asset.digest -notmatch '^sha256:([0-9a-f]{64})$') { throw "PDFium $latest has no digest for $($wanted[$k])" }
  $hash = $Matches[1]
  $pattern = "set\(_fw_pdfium_sha256_$k `"[0-9a-f]{64}`"\)"
  if ($text -notmatch $pattern) { throw "No _fw_pdfium_sha256_$k in $cmakeFile" }
  $text = [regex]::Replace($text, $pattern, "set(_fw_pdfium_sha256_$k `"$hash`")")
  Write-Host ("{0,-16} {1}" -f $k, $hash)
}
$text = [regex]::Replace($text, 'set\(FW_PDFIUM_BUILD "\d+"\)', "set(FW_PDFIUM_BUILD `"$latest`")")
[IO.File]::WriteAllText($cmakeFile, $text, (New-Object Text.UTF8Encoding($false)))
Write-Host "Pinned PDFium $latest (was $pinned) in cmake/FetchPdfium.cmake. Next: configure (it downloads and checks the"
Write-Host "archives), build, ctest, the PDF checks; then commit with the notes from $($release.html_url)."
