# Makes the updater tests' code-signing certificates (f1-ship). TEST-ONLY KEYS: they're in the repository on
# purpose, nothing they sign is trusted anywhere, and they're never added to a Windows certificate store; the
# unit tests load them in memory (PFXImportCertStore) and sign copies of a file with them.
#   publisher-a.pfx   CN=Featherweight Test Publisher   the "running" publisher
#   publisher-b.pfx   CN=Other Test Publisher           another publisher: refused
#   impostor-a.pfx    CN=Featherweight Test Publisher   the same name with its own key and root: refused
# Password: test. Run again only to replace them (the unit tests read each one's thumbprint from the file).
param([string]$OpenSsl = "C:\Program Files\Git\mingw64\bin\openssl.exe")
$ErrorActionPreference = "Continue"  # openssl writes progress to stderr; failures are caught by exit code
$here = Split-Path -Parent $PSCommandPath
$tmp = Join-Path ([IO.Path]::GetTempPath()) ("fw-test-certs-" + [Guid]::NewGuid())
New-Item -ItemType Directory $tmp | Out-Null
try {
  foreach ($c in @(@{ n = "publisher-a"; cn = "Featherweight Test Publisher" },
                   @{ n = "publisher-b"; cn = "Other Test Publisher" },
                   @{ n = "impostor-a"; cn = "Featherweight Test Publisher" })) {
    $key = Join-Path $tmp "$($c.n).key"; $crt = Join-Path $tmp "$($c.n).crt"
    & $OpenSsl req -x509 -newkey rsa:2048 -nodes -sha256 -days 7300 -keyout $key -out $crt `
      -subj "/CN=$($c.cn)/O=Featherweight Tests" `
      -addext "basicConstraints=critical,CA:FALSE" -addext "keyUsage=critical,digitalSignature" `
      -addext "extendedKeyUsage=codeSigning" 2>&1 | Out-Null
    if ($LASTEXITCODE) { throw "openssl req failed for $($c.n)" }
    & $OpenSsl pkcs12 -export -inkey $key -in $crt -out (Join-Path $here "$($c.n).pfx") -passout pass:test `
      -keypbe PBE-SHA1-3DES -certpbe PBE-SHA1-3DES -macalg sha1 -name $c.n 2>&1 | Out-Null
    if ($LASTEXITCODE) { throw "openssl pkcs12 failed for $($c.n)" }
  }
} finally {
  Remove-Item -Recurse -Force $tmp
}
Get-ChildItem $here -Filter *.pfx | Select-Object Name, Length
