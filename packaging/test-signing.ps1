<#
  Signing with a throwaway test certificate, until SignPath signs (Roy's step 1), and checking signatures (f1-ship).
    -Make         makes the certificate (OpenSSL; CN=Featherweight Test Publisher) in $env:RUNNER_TEMP. CI only.
    -Sign files   signs them with it (SHA-256).
    -Trust        trusts it on this runner (LocalMachine Root and TrustedPeople), so signtool verify and the MSIX
                  install accept it. CI only: runners are thrown away; the reference PC never gets a test
                  certificate in a store (docs/f1-plan.md, "Reference PC").
    -Verify files signtool verify /pa /all on each; fails on the first that doesn't pass.
    -Unsigned files  each carries no signature at all (signtool verify /pa: "No signature found"; Windows:
                  NotSigned): the unsigned set a release drafts until SignPath.
    -Releasable dir  every program file and package in the folder, and the program files inside its portable
                  zips, is signed by a publisher Windows trusts (SignPath) or not signed at all; any other
                  signature fails, such as the test certificate's on a runner that doesn't trust it. The release
                  job runs it before drafting.
#>
param([switch]$Make, [string[]]$Sign, [switch]$Trust, [string[]]$Verify, [string[]]$Unsigned, [string]$Releasable)
$ErrorActionPreference = "Stop"
$dir = Join-Path ($(if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { [IO.Path]::GetTempPath() })) "fw-test-signing"
function SdkTool($name) {
  $kits = "${env:ProgramFiles(x86)}\Windows Kits\10\bin"
  $hostArch = if ($env:PROCESSOR_ARCHITECTURE -eq "ARM64") { "arm64" } else { "x64" }
  $t = Get-ChildItem "$kits\10.*\$hostArch\$name", "$kits\10.*\x64\$name" -ErrorAction SilentlyContinue | Sort-Object FullName |
    Select-Object -Last 1 -ExpandProperty FullName
  if (-not $t) { throw "$name (Windows SDK) not found" }
  $t
}
function CiOnly($what) { if ($env:GITHUB_ACTIONS -ne "true") { throw "$what runs on CI runners only." } }

if ($Make) {
  CiOnly "Making a test certificate"
  New-Item -ItemType Directory -Force $dir | Out-Null
  $openssl = @("$env:ProgramFiles\Git\mingw64\bin\openssl.exe", "$env:ProgramFiles\Git\usr\bin\openssl.exe") |
    Where-Object { Test-Path $_ } | Select-Object -First 1
  if (-not $openssl) { $openssl = (Get-Command openssl).Source }
  $password = [Guid]::NewGuid().ToString("N")
  $ErrorActionPreference = "Continue"  # openssl reports progress on stderr
  & $openssl req -x509 -newkey rsa:3072 -nodes -sha256 -days 7 -keyout "$dir\key.pem" -out "$dir\cert.pem" `
    -subj "/CN=Featherweight Test Publisher/O=Featherweight Tests" -addext "basicConstraints=critical,CA:FALSE" `
    -addext "keyUsage=critical,digitalSignature" -addext "extendedKeyUsage=codeSigning" 2>&1 | Out-Null
  if ($LASTEXITCODE) { throw "openssl req failed" }
  & $openssl pkcs12 -export -inkey "$dir\key.pem" -in "$dir\cert.pem" -out "$dir\test.pfx" -passout "pass:$password" `
    -keypbe PBE-SHA1-3DES -certpbe PBE-SHA1-3DES -macalg sha1 2>&1 | Out-Null
  if ($LASTEXITCODE) { throw "openssl pkcs12 failed" }
  & $openssl x509 -in "$dir\cert.pem" -outform der -out "$dir\test-publisher.cer" 2>&1 | Out-Null
  $ErrorActionPreference = "Stop"
  Remove-Item "$dir\key.pem"
  Set-Content "$dir\password.txt" $password -NoNewline
  Write-Host "Test certificate: CN=Featherweight Test Publisher, O=Featherweight Tests (valid 7 days)"
}

if ($Sign) {
  $signtool = SdkTool "signtool.exe"
  $password = Get-Content "$dir\password.txt" -Raw
  foreach ($f in $Sign) {
    & $signtool sign /q /fd sha256 /f "$dir\test.pfx" /p $password $f
    if ($LASTEXITCODE) { throw "signing $f failed" }
  }
  Write-Host "Test-signed $($Sign.Count) files"
}

if ($Trust) {
  CiOnly "Trusting a test certificate"
  foreach ($store in "Root", "TrustedPeople") {
    Import-Certificate -FilePath "$dir\test-publisher.cer" -CertStoreLocation "Cert:\LocalMachine\$store" | Out-Null
  }
}

if ($Verify) {
  $signtool = SdkTool "signtool.exe"
  $bad = 0
  foreach ($f in $Verify) {
    $out = & $signtool verify /pa /all $f 2>&1
    if ($LASTEXITCODE) { Write-Host "::error::not verified: $f"; $out | Out-Host; $bad++ }
    else { Write-Host "verified: $(Split-Path -Leaf $f)" }
  }
  if ($bad) { throw "$bad files didn't verify" }
}

if ($Unsigned) {
  $signtool = SdkTool "signtool.exe"
  $bad = 0
  foreach ($f in $Unsigned) {
    $ErrorActionPreference = "Continue"  # signtool reports the missing signature on stderr
    $out = (& $signtool verify /pa $f 2>&1 | Out-String)
    $code = $LASTEXITCODE
    $ErrorActionPreference = "Stop"
    $status = (Get-AuthenticodeSignature $f).Status
    if ($code -eq 0 -or $out -notmatch 'No signature found' -or "$status" -ne "NotSigned") {
      Write-Host "::error::signed: $f ($status)"; $out | Out-Host; $bad++
    } else { Write-Host "no signature: $(Split-Path -Leaf $f)" }
  }
  if ($bad) { throw "$bad files carry a signature" }
  Write-Host "$($Unsigned.Count) files, none signed"
}

if ($Releasable) {
  $Releasable = (Resolve-Path $Releasable).Path
  $inside = Join-Path ([IO.Path]::GetTempPath()) "fw-releasable-$([Guid]::NewGuid().ToString('N'))"
  foreach ($z in Get-ChildItem $Releasable -File -Filter "*-Portable*.zip") {
    Expand-Archive $z.FullName (Join-Path $inside $z.BaseName)
  }
  $files = @(Get-ChildItem "$Releasable\*" -File -Include *.exe, *.dll, *.msi, *.msix)
  if (Test-Path $inside) { $files += @(Get-ChildItem $inside -Recurse -File -Include *.exe, *.dll) }
  if (-not $files.Count) { throw "no program files or packages in $Releasable" }
  $bad = @()
  foreach ($f in $files) {
    $s = Get-AuthenticodeSignature $f.FullName
    $who = if ($s.SignerCertificate) { $s.SignerCertificate.Subject } else { "-" }
    Write-Host ("{0,-14} {1}  ({2})" -f $s.Status, $f.FullName.Substring($f.FullName.IndexOf($f.Directory.Name)), $who)
    if ("$($s.Status)" -ne "Valid" -and "$($s.Status)" -ne "NotSigned") { $bad += "$($f.Name) ($($s.Status))" }
  }
  Remove-Item -Recurse -Force $inside -ErrorAction SilentlyContinue
  if ($bad.Count) { throw "$($bad.Count) files carry a signature Windows doesn't trust: $($bad -join ', ')" }
  Write-Host "$($files.Count) files: each signed by a trusted publisher or not signed"
}
