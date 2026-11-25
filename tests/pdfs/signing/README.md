# Test signing certificate (F1, f1-pdf)

`test-signer.pfx` (password `featherweight`) and its certificate `test-signer.crt` are a
self-signed RSA-2048 certificate made for the tests only: "Featherweight Test Signer, Featherweight
tests (not a real identity)", valid 2026-2046, key usage digital signature and non-repudiation,
extended key usage document signing.

The tests load the PFX in memory (`PFXImportCertStore` with `PKCS12_NO_PERSIST_KEY`): it is never
added to a Windows certificate store, and its key is never written anywhere. Validators that need
a trust anchor (pyHanko on CI) are given `test-signer.crt`. It signs nothing outside the tests.

Made with Git for Windows' OpenSSL:

    openssl req -x509 -newkey rsa:2048 -keyout key.pem -out test-signer.crt -days 7300 -nodes \
      -subj "/CN=Featherweight Test Signer/O=Featherweight tests (not a real identity)" \
      -addext "keyUsage=critical,digitalSignature,nonRepudiation" \
      -addext "extendedKeyUsage=1.3.6.1.4.1.311.10.3.12"
    openssl pkcs12 -export -out test-signer.pfx -inkey key.pem -in test-signer.crt \
      -passout pass:featherweight -certpbe AES-256-CBC -keypbe AES-256-CBC -macalg sha256
