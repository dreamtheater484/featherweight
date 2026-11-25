#pragma once

// Certificate signatures (F1, f1-pdf): PAdES baseline B-B (ETSI.CAdES.detached) with a
// certificate from the Windows store, its key used through CNG. The signature is an incremental
// update after the file as it is: an invisible signature field on the first page whose value is a
// signature dictionary, and the CMS SignedData over every byte but its own. Verifying reads what
// PDFium found (fpdf_signature.h) against the file's bytes. Nothing here uses the network: chains
// are built from what Windows has cached.

#include <windows.h>

#include <wincrypt.h>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pdfsign {

struct Details {
  std::wstring name;      // the signer, as the certificate names them
  std::wstring reason;    // optional
  std::wstring location;  // optional
  SYSTEMTIME utc{};       // when (zero: now)
};

enum class Error {
  None,
  Unreadable,  // the file's structure couldn't be read for an update
  Encrypted,   // a protected PDF: its new strings would need the file's key
  NoPage,
  NoKey,       // the certificate's private key isn't usable
  Failed,      // Windows couldn't make the signature (a cancelled PIN, a smart card removed)
  TooLarge,    // the signature didn't fit in the space kept for it
};

// Signs `file` (the whole PDF as it is) with `cert`: `out` is the file and the signature's update.
Error SignPdf(std::string_view file, const CERT_CONTEXT* cert, const Details& details, std::string& out);

// The parts of the job, for tests and the fuzzer.
struct Prepared {
  std::string bytes;      // the file and the update, the signature's contents still zeros
  size_t contentsAt = 0;  // the '<' of /Contents
  size_t contentsHex = 0; // hex digits kept for the signature
  std::array<size_t, 4> range{};
};
Error Prepare(std::string_view file, const Details& details, size_t reserveBytes, Prepared& out);
bool Fill(Prepared& p, const std::vector<uint8_t>& cms);
Error MakeCms(const CERT_CONTEXT* cert, std::string_view first, std::string_view second, std::vector<uint8_t>& cms);

// ---- Checking a signature
struct Verdict {
  enum Kind {
    Valid,      // the signed bytes are as signed, and Windows trusts the certificate
    Untrusted,  // as signed, but Windows doesn't trust the certificate (self-signed, unknown issuer, expired)
    Changed,    // as signed, but the file was changed after it was signed
    Invalid,    // the signed bytes or the signature don't match
    Unreadable  // not a signature Featherweight can check
  };
  Kind kind = Unreadable;
  std::wstring signer;  // the certificate's name
};
// `range` is the signature's /ByteRange, `contents` its /Contents, `file` the whole file.
Verdict Verify(std::string_view file, const std::vector<int>& range, const std::vector<uint8_t>& contents);

// ---- Certificates
// The current user's certificates that can sign (a private key, valid now, usable for signatures).
// Free each with CertFreeCertificateContext.
std::vector<const CERT_CONTEXT*> SigningCertificates();
std::wstring CertName(const CERT_CONTEXT* cert);
std::wstring CertIssuer(const CERT_CONTEXT* cert);
// A PFX file's certificate with its key, in memory only: never added to a store (the tests).
const CERT_CONTEXT* LoadPfx(std::string_view pfx, const std::wstring& password);

}  // namespace pdfsign
