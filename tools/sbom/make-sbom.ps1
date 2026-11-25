<#
.SYNOPSIS
  Writes the release's CycloneDX SBOM to -Out and checks that THIRD_PARTY_NOTICES.md names exactly its
  components (tools\sbom\make_sbom.py; F1 f1-trust). Exits 1 when they differ. The 4.1 gate's
  legal.notices row runs it (gate-contract.md).

.EXAMPLE
  tools\sbom\make-sbom.ps1 -Out build\sbom\Featherweight.cdx.json
  tools\sbom\make-sbom.ps1 -Out sbom.cdx.json -Pdfium third_party\pdfium\8086-v8, third_party\pdfium\8086-standard
#>
param(
  [Parameter(Mandatory = $true)][string]$Out,
  [string[]]$Pdfium = @()
)
$ErrorActionPreference = "Stop"
$dir = Split-Path -Parent ([IO.Path]::GetFullPath($Out))
if ($dir) { New-Item -ItemType Directory -Force $dir | Out-Null }
$pyArgs = @((Join-Path $PSScriptRoot "make_sbom.py"), "--check-notices", "--out", $Out)
foreach ($p in $Pdfium) { $pyArgs += "--pdfium"; $pyArgs += $p }
& python @pyArgs
exit $LASTEXITCODE
