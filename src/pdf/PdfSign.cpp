// Signing in the PDF tab (F1, f1-pdf): certificate signatures (pdf/sign: PAdES B-B through CNG,
// with a certificate from the Windows store), the Sign ▾ menu, and the check of a signed PDF's
// signatures when it opens. crypt32 is delay-loaded: it loads here, on a signed PDF or a signing.
#include <windows.h>

#include <fpdf_signature.h>

#include <algorithm>

#include "Commands.h"
#include "Util.h"
#include "core/Strings.h"
#include "pdf/PdfText.h"
#include "pdf/PdfView.h"
#include "pdf/sign/Sign.h"
#include "ui/Menu.h"
#include "ui/Theme.h"

namespace {

std::wstring BaseOf(const std::wstring& path) {
  std::wstring name = fw::FileNameOf(path);
  if (const size_t dot = name.rfind(L'.'); dot != std::wstring::npos) name.resize(dot);
  return name;
}

std::wstring Joined(const std::vector<std::wstring>& names) {
  std::wstring out;
  for (size_t i = 0; i < names.size(); ++i) {
    if (i > 0) out += i + 1 == names.size() ? T(L" and ") : L", ";
    out += names[i];
  }
  return out;
}

}  // namespace

void PdfView::CheckSignatures(bool always) {
  if (!Ready()) return;
  const int count = doc_->SignatureCount();
  if (count == 0) {
    if (always) host_.ViewMessage(BannerKind::Info, T(L"This PDF has no signatures."));
    return;
  }
  std::string bytes;
  if (!doc_->CurrentBytes(bytes)) return;
  std::vector<std::wstring> valid, untrusted, changed, invalid;
  for (int i = 0; i < count; ++i) {
    FPDF_SIGNATURE sig = FPDF_GetSignatureObject(doc_->Handle(), i);
    if (!sig) continue;
    std::vector<uint8_t> contents(FPDFSignatureObj_GetContents(sig, nullptr, 0));
    if (!contents.empty()) FPDFSignatureObj_GetContents(sig, contents.data(), static_cast<unsigned long>(contents.size()));
    std::vector<int> range(FPDFSignatureObj_GetByteRange(sig, nullptr, 0));
    if (!range.empty()) FPDFSignatureObj_GetByteRange(sig, range.data(), static_cast<unsigned long>(range.size()));
    const pdfsign::Verdict v = pdfsign::Verify(bytes, range, contents);
    const std::wstring who = v.signer.empty() ? std::wstring(T(L"someone")) : v.signer;
    switch (v.kind) {
      case pdfsign::Verdict::Valid: valid.push_back(who); break;
      case pdfsign::Verdict::Untrusted: untrusted.push_back(who); break;
      case pdfsign::Verdict::Changed: changed.push_back(who); break;
      case pdfsign::Verdict::Invalid:
      case pdfsign::Verdict::Unreadable: invalid.push_back(who); break;
    }
  }
  signatureWarned_ = false;
  if (!invalid.empty()) {
    host_.ViewMessage(BannerKind::Error,
                      pdftext::Fill(T(L"The signature by {0} isn't valid: what was signed has been changed, or the "
                                      L"signature can't be checked."),
                                    {Joined(invalid)}));
  } else if (!changed.empty()) {
    host_.ViewMessage(BannerKind::Warning,
                      pdftext::Fill(T(L"Signed by {0}, but the PDF was changed after it was signed."), {Joined(changed)}));
  } else if (!untrusted.empty()) {
    std::vector<std::wstring> all = valid;
    all.insert(all.end(), untrusted.begin(), untrusted.end());
    host_.ViewMessage(BannerKind::Info,
                      valid.empty() ? pdftext::Fill(T(L"Signed by {0}. The PDF is as it was signed, but Windows doesn't "
                                                      L"trust the certificate it was signed with."),
                                                    {Joined(all)})
                                    : pdftext::Fill(T(L"Signed by {0}. The PDF is as it was signed, but Windows doesn't "
                                                      L"trust the certificate of {1}."),
                                                    {Joined(all), Joined(untrusted)}));
  } else {
    host_.ViewMessage(BannerKind::Success,
                      pdftext::Fill(T(L"Signed by {0}. The signature is valid: the PDF is as it was signed."),
                                    {Joined(valid)}));
  }
}

std::wstring PdfView::SigningBlocked() const {
  if (!Ready()) return T(L"Signing works once the PDF is open.");
  if (doc_->IsXfa()) return T(L"This PDF is an XFA form, which Featherweight can't sign.");
  if (doc_->Encrypted()) return T(L"This PDF is protected, so Featherweight can't sign it.");
  return {};
}

bool PdfView::SignWith(const CERT_CONTEXT* cert, const std::wstring& path) {
  if (const std::wstring why = SigningBlocked(); !why.empty()) {
    host_.ViewMessage(BannerKind::Info, why);
    return false;
  }
  std::string bytes, signedBytes;
  if (!doc_->CurrentBytes(bytes)) return false;
  pdfsign::Details details;
  details.name = pdfsign::CertName(cert);
  const pdfsign::Error e = pdfsign::SignPdf(bytes, cert, details, signedBytes);
  if (e != pdfsign::Error::None) {
    std::wstring why;
    switch (e) {
      case pdfsign::Error::NoKey: why = T(L"The certificate's private key can't be used on this PC."); break;
      case pdfsign::Error::Failed: why = T(L"Windows couldn't make the signature (was the PIN cancelled?)."); break;
      case pdfsign::Error::Encrypted: why = T(L"This PDF is protected, so Featherweight can't sign it."); break;
      default: why = T(L"This PDF's structure couldn't be read well enough to sign it."); break;
    }
    host_.ViewMessage(BannerKind::Error, why);
    return false;
  }
  ForgetPages();
  std::wstring error;
  const bool ok = doc_->WriteAndReload(path, std::move(signedBytes), error);
  path_ = doc_->Path();
  pageUndo_.clear();
  pageRedo_.clear();
  pageBefore_.clear();
  EndPageChange(false, CurrentPage());
  if (!ok) {
    host_.ViewMessage(BannerKind::Error, error);
    return false;
  }
  CheckSignatures(true);
  return true;
}

bool PdfView::SignWithCertificate() {
  if (const std::wstring why = SigningBlocked(); !why.empty()) {
    host_.ViewMessage(BannerKind::Info, why);
    return false;
  }
  std::vector<const CERT_CONTEXT*> certs = pdfsign::SigningCertificates();
  if (certs.empty()) {
    host_.ViewMessage(BannerKind::Info, T(L"There's no certificate for signing on this PC. A drawn signature can be "
                                          L"placed instead (Sign, Draw signature)."));
    return false;
  }
  // The certificates as a menu: the name, and who issued it.
  HMENU m = CreatePopupMenu();
  for (size_t i = 0; i < certs.size() && i < 50; ++i)
    menu::AppendItem(m, static_cast<UINT>(i + 1),
                     pdftext::Fill(T(L"{0}, issued by {1}"), {pdfsign::CertName(certs[i]), pdfsign::CertIssuer(certs[i])}));
  POINT pt{};
  GetCursorPos(&pt);
  theme::ApplyToMenus();
  const int chosen = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_LEFTBUTTON, pt.x, pt.y, host_.ViewHwnd(), nullptr);
  DestroyMenu(m);
  bool ok = false;
  std::wstring path;
  if (chosen > 0 && ChoosePdfToSave(pdftext::Fill(T(L"{0} (signed)"), {BaseOf(Path())}), T(L"Save the signed PDF"),
                                    path, true))
    ok = SignWith(certs[static_cast<size_t>(chosen - 1)], path);
  for (const CERT_CONTEXT* c : certs) CertFreeCertificateContext(c);
  return ok;
}

bool PdfView::SignWithPfxForTesting(const std::wstring& pfxPath, const std::wstring& password, const std::wstring& out) {
  std::string pfx;
  if (!util::ReadFileBytes(pfxPath, pfx, 1u << 20)) return false;
  const CERT_CONTEXT* cert = pdfsign::LoadPfx(pfx, password);
  if (!cert) return false;
  const bool ok = SignWith(cert, out);
  CertFreeCertificateContext(cert);
  return ok;
}

void PdfView::FillMenu(int which, HMENU m) {
  if (which != CmdPdfSignMenu) {
    PagedView::FillMenu(which, m);
    return;
  }
  const bool canSign = SigningBlocked().empty();
  menu::AppendItem(m, CmdPdfDrawSignature, T(L"Draw signature…"), Ready() && PageToolsBlocked().empty());
  menu::AppendItem(m, CmdPdfSignCertificate, T(L"Sign with a certificate…"), canSign);
  menu::AppendSeparator(m);
  const int n = Ready() ? doc_->SignatureCount() : 0;
  menu::AppendItem(m, CmdPdfSignatures,
                   n == 0 ? std::wstring(T(L"No signatures in this PDF"))
                          : pdftext::Fill(T(L"Check signatures ({0})"), {std::to_wstring(n)}),
                   n > 0);
}
