# The Windows App Certification Kit on the x64 MSIX (f1-ship), as the Store runs it. CI only: it installs the
# package (with a test-signed package, after trusting the test certificate on the runner).
param([Parameter(Mandatory)][string]$Dist)
$ErrorActionPreference = "Stop"
if ($env:GITHUB_ACTIONS -ne "true") { throw "msix-wack.ps1 installs a package: it runs on CI runners only." }
$Dist = (Resolve-Path $Dist).Path
$msix = Get-ChildItem $Dist -Filter "Featherweight-*.msix" | Where-Object { $_.Name -notmatch "arm64" } | Select-Object -First 1
if (-not $msix) { throw "No x64 MSIX in $Dist" }
if (Test-Path "$Dist\test-publisher.cer") {
  foreach ($store in "Root", "TrustedPeople") {
    Import-Certificate -FilePath "$Dist\test-publisher.cer" -CertStoreLocation "Cert:\LocalMachine\$store" | Out-Null
  }
}
$appcert = @("${env:ProgramFiles(x86)}\Windows Kits\10\App Certification Kit\appcert.exe") | Where-Object { Test-Path $_ } |
  Select-Object -First 1
if (-not $appcert) { throw "The Windows App Certification Kit (appcert.exe) isn't installed on this runner" }
$report = Join-Path $env:RUNNER_TEMP "wack-report.xml"
& $appcert reset | Out-Null
& $appcert test -appxpackagepath $msix.FullName -reportoutputpath $report | Out-Host
if (-not (Test-Path $report)) { throw "The App Certification Kit wrote no report" }
[xml]$x = Get-Content $report
$overall = $x.REPORT.OVERALL_RESULT
Write-Host "App Certification Kit: $overall"
foreach ($t in $x.SelectNodes("//TEST")) {
  if ($t.RESULT.'#cdata-section' -ne "PASS") { Write-Host ("  {0}: {1}" -f $t.NAME, $t.RESULT.'#cdata-section') }
}
Copy-Item $report (Join-Path $Dist "..\wack-report.xml") -ErrorAction SilentlyContinue
if ($overall -eq "FAIL") { throw "The App Certification Kit failed the MSIX" }
