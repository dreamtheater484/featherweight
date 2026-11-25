// The signer's signature algorithm (ECDSA keys) is a CMS field of CMSG_SIGNER_ENCODE_INFO.
#define CMSG_SIGNER_ENCODE_INFO_HAS_CMS_FIELDS
#define CMSG_SIGNED_ENCODE_INFO_HAS_CMS_FIELDS
#include "pdf/sign/Sign.h"

#include <bcrypt.h>

#include <algorithm>
#include <cstdio>
#include <set>
#include <tuple>

#include "pdf/sign/PdfObjects.h"

namespace pdfsign {

namespace {

using pdfobj::Obj;

constexpr size_t kReserve = 12 * 1024;  // bytes kept for the CMS: a 4096-bit key and a chain of three
constexpr const char* kOidSigningCertificateV2 = "1.2.840.113549.1.9.16.2.47";
constexpr DWORD kEncoding = X509_ASN_ENCODING | PKCS_7_ASN_ENCODING;
// Chains from what Windows has: no fetching of issuers, revocation lists or root updates.
constexpr DWORD kChainFlags = CERT_CHAIN_CACHE_ONLY_URL_RETRIEVAL | CERT_CHAIN_DISABLE_AUTH_ROOT_AUTO_UPDATE;

std::string PdfDate(const SYSTEMTIME& t) {
  char buf[40];
  snprintf(buf, sizeof buf, "D:%04u%02u%02u%02u%02u%02u+00'00'", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
           t.wSecond);
  return buf;
}

bool Sha256(const void* data, size_t size, uint8_t out[32]) {
  return BCRYPT_SUCCESS(BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, static_cast<PUCHAR>(const_cast<void*>(data)),
                                   static_cast<ULONG>(size), out, 32));
}

// The length of a DER value from its header, so the zeros after it in /Contents are left out.
size_t DerLength(const std::vector<uint8_t>& d) {
  if (d.size() < 2 || d[0] != 0x30) return 0;
  if (d[1] < 0x80) return 2u + d[1];
  const size_t n = d[1] & 0x7F;
  if (n == 0 || n > 4 || d.size() < 2 + n) return 0;
  size_t len = 0;
  for (size_t i = 0; i < n; ++i) len = len << 8 | d[2 + i];
  return 2 + n + len <= d.size() ? 2 + n + len : 0;
}

std::wstring NameOf(const CERT_CONTEXT* cert, DWORD flags) {
  wchar_t buf[256] = {};
  CertGetNameStringW(cert, CERT_NAME_SIMPLE_DISPLAY_TYPE, flags, nullptr, buf, 256);
  return buf;
}

// The certificate's chain, as Windows has it cached (the certificate first). Empty when Windows
// can't build one; `trusted` says whether it ends at a root Windows trusts and is valid now.
std::vector<const CERT_CONTEXT*> Chain(const CERT_CONTEXT* cert, HCERTSTORE extra, bool* trusted) {
  std::vector<const CERT_CONTEXT*> out;
  CERT_CHAIN_PARA para{};
  para.cbSize = sizeof para;
  PCCERT_CHAIN_CONTEXT chain = nullptr;
  if (trusted) *trusted = false;
  if (!CertGetCertificateChain(nullptr, cert, nullptr, extra, &para, kChainFlags, nullptr, &chain) || !chain) return out;
  if (chain->cChain > 0)
    for (DWORD i = 0; i < chain->rgpChain[0]->cElement; ++i)
      out.push_back(CertDuplicateCertificateContext(chain->rgpChain[0]->rgpElement[i]->pCertContext));
  if (trusted) {
    CERT_CHAIN_POLICY_PARA policy{};
    policy.cbSize = sizeof policy;
    CERT_CHAIN_POLICY_STATUS status{};
    status.cbSize = sizeof status;
    *trusted = CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_BASE, chain, &policy, &status) && status.dwError == 0;
  }
  CertFreeCertificateChain(chain);
  return out;
}

}  // namespace

Error Prepare(std::string_view file, const Details& details, size_t reserveBytes, Prepared& out) {
  pdfobj::File f;
  if (!f.Open(file)) return Error::Unreadable;
  if (f.Trailer().Get("Encrypt")) return Error::Encrypted;
  const Obj rootRef = *f.Trailer().Get("Root");
  Obj catalog;
  int catalogGen = 0;
  if (!f.Get(rootRef.num, catalog, &catalogGen) || catalog.type != Obj::Dict) return Error::Unreadable;
  const int pageNum = f.FirstPage();
  Obj page;
  int pageGen = 0;
  if (pageNum < 0 || !f.Get(pageNum, page, &pageGen) || page.type != Obj::Dict) return Error::NoPage;

  int next = f.Size();
  const int sigNum = next++, fieldNum = next++;
  std::vector<std::tuple<int, int, std::string>> objects;  // number, generation, the object
  bool catalogChanged = false, pageChanged = false;

  // The form: the field joins /Fields, and /SigFlags says the file has signatures and is
  // changed only by appending.
  Obj form;
  int formNum = -1, formGen = 0;
  if (const Obj* af = catalog.Get("AcroForm"); af && af->type == Obj::Ref) {
    formNum = af->num;
    if (!f.Get(formNum, form, &formGen) || form.type != Obj::Dict) return Error::Unreadable;
  } else if (af && af->type == Obj::Dict) {
    form = *af;
  } else {
    form.type = Obj::Dict;
  }
  Obj fields;
  int fieldsNum = -1, fieldsGen = 0;
  if (const Obj* fl = form.Get("Fields"); fl && fl->type == Obj::Ref) {
    fieldsNum = fl->num;
    if (!f.Get(fieldsNum, fields, &fieldsGen) || fields.type != Obj::Array) return Error::Unreadable;
  } else if (fl && fl->type == Obj::Array) {
    fields = *fl;
  } else {
    fields.type = Obj::Array;
  }
  std::set<std::string> names;
  for (const Obj& item : fields.items) {
    Obj field;
    if (f.Resolve(item, field) && field.type == Obj::Dict && field.Get("T")) names.insert(field.Get("T")->text);
  }
  std::string fieldName;
  for (int k = 1; k < 10000; ++k) {
    fieldName = "Signature" + std::to_string(k);
    if (!names.count(fieldName)) break;
  }
  fields.items.push_back(pdfobj::MakeRef(fieldNum));
  if (fieldsNum >= 0) objects.emplace_back(fieldsNum, fieldsGen, pdfobj::Write(fields));
  else form.Set("Fields", fields);
  form.Set("SigFlags", pdfobj::MakeInt(3));
  if (formNum >= 0) {
    objects.emplace_back(formNum, formGen, pdfobj::Write(form));
  } else {
    catalog.Set("AcroForm", form);
    catalogChanged = true;
  }

  // The page lists the field's widget among its annotations.
  if (const Obj* annots = page.Get("Annots"); annots && annots->type == Obj::Ref) {
    Obj list;
    int listGen = 0;
    if (!f.Get(annots->num, list, &listGen) || list.type != Obj::Array) return Error::Unreadable;
    list.items.push_back(pdfobj::MakeRef(fieldNum));
    objects.emplace_back(annots->num, listGen, pdfobj::Write(list));
  } else {
    Obj list;
    if (annots && annots->type == Obj::Array) list = *annots;
    else list.type = Obj::Array;
    list.items.push_back(pdfobj::MakeRef(fieldNum));
    page.Set("Annots", list);
    pageChanged = true;
  }
  if (catalogChanged) objects.emplace_back(rootRef.num, catalogGen, pdfobj::Write(catalog));
  if (pageChanged) objects.emplace_back(pageNum, pageGen, pdfobj::Write(page));

  // The field and its widget in one, invisible (no area), printed and locked.
  objects.emplace_back(fieldNum, 0,
                       "<</Type /Annot /Subtype /Widget /FT /Sig /T (" + fieldName + ") /V " + std::to_string(sigNum) +
                           " 0 R /F 132 /Rect [0 0 0 0] /P " + std::to_string(pageNum) + " " + std::to_string(pageGen) +
                           " R>>");

  SYSTEMTIME when = details.utc;
  if (when.wYear == 0) GetSystemTime(&when);
  std::string sig = "<</Type /Sig /Filter /Adobe.PPKLite /SubFilter /ETSI.CAdES.detached /M " +
                    pdfobj::Write(pdfobj::MakeString(PdfDate(when)));
  if (!details.name.empty()) sig += " /Name " + pdfobj::Write(pdfobj::MakeText(details.name));
  if (!details.reason.empty()) sig += " /Reason " + pdfobj::Write(pdfobj::MakeText(details.reason));
  if (!details.location.empty()) sig += " /Location " + pdfobj::Write(pdfobj::MakeText(details.location));
  const std::string rangeHolder = "/ByteRange [0 0 0 0]" + std::string(40, ' ');
  sig += " " + rangeHolder + " /Contents <" + std::string(reserveBytes * 2, '0') + ">>>";

  // The update after the file.
  std::string inc;
  if (!file.empty() && file.back() != '\n' && file.back() != '\r') inc += '\n';
  const size_t base = file.size();
  std::vector<std::pair<int, std::pair<size_t, int>>> written;  // number, (offset, generation)
  size_t rangeAt = 0;
  for (const auto& [num, gen, text] : objects) {
    written.push_back({num, {base + inc.size(), gen}});
    inc += std::to_string(num) + " " + std::to_string(gen) + " obj\n" + text + "\nendobj\n";
  }
  written.push_back({sigNum, {base + inc.size(), 0}});
  inc += std::to_string(sigNum) + " 0 obj\n";
  rangeAt = base + inc.size() + sig.find("/ByteRange");
  out.contentsAt = base + inc.size() + sig.find("/Contents <") + 10;
  inc += sig + "\nendobj\n";

  std::string trailerKeys = " /Root " + pdfobj::Write(rootRef);
  for (const char* key : {"Info", "ID"})
    if (const Obj* v = f.Trailer().Get(key)) trailerKeys += std::string(" /") + key + " " + pdfobj::Write(*v);
  trailerKeys += " /Prev " + std::to_string(f.LastXref());
  const size_t xrefAt = base + inc.size();
  std::sort(written.begin(), written.end());
  if (!f.LastXrefIsStream()) {
    inc += "xref\n";
    for (size_t i = 0; i < written.size();) {
      size_t j = i;
      while (j + 1 < written.size() && written[j + 1].first == written[j].first + 1) ++j;
      inc += std::to_string(written[i].first) + " " + std::to_string(j - i + 1) + "\n";
      for (size_t k = i; k <= j; ++k) {
        char e[24];
        snprintf(e, sizeof e, "%010llu %05d n\r\n", static_cast<unsigned long long>(written[k].second.first),
                 written[k].second.second);
        inc += e;
      }
      i = j + 1;
    }
    inc += "trailer\n<</Size " + std::to_string(next) + trailerKeys + ">>\n";
  } else {
    // The file's last cross-reference is a stream: the update's is one too (not compressed).
    const int xrefNum = next++;
    written.push_back({xrefNum, {xrefAt, 0}});
    std::string rows, index;
    for (size_t i = 0; i < written.size();) {
      size_t j = i;
      while (j + 1 < written.size() && written[j + 1].first == written[j].first + 1) ++j;
      index += (index.empty() ? "" : " ") + std::to_string(written[i].first) + " " + std::to_string(j - i + 1);
      for (size_t k = i; k <= j; ++k) {
        const uint64_t off = written[k].second.first;
        const auto gen = static_cast<uint16_t>(written[k].second.second);
        rows += '\x01';
        for (int shift : {24, 16, 8, 0}) rows += static_cast<char>((off >> shift) & 0xFF);
        rows += static_cast<char>(gen >> 8);
        rows += static_cast<char>(gen & 0xFF);
      }
      i = j + 1;
    }
    if (xrefAt > 0xFFFFFFFFull) return Error::Unreadable;
    inc += std::to_string(xrefNum) + " 0 obj\n<</Type /XRef /Size " + std::to_string(next) + " /W [1 4 2] /Index [" +
           index + "]" + trailerKeys + " /Length " + std::to_string(rows.size()) + ">>\nstream\n" + rows +
           "\nendstream\nendobj\n";
  }
  inc += "startxref\n" + std::to_string(xrefAt) + "\n%%EOF\n";

  out.bytes.reserve(file.size() + inc.size());
  out.bytes.assign(file);
  out.bytes += inc;
  out.contentsHex = reserveBytes * 2;
  const size_t after = out.contentsAt + out.contentsHex + 2;
  out.range = {0, out.contentsAt, after, out.bytes.size() - after};
  const std::string range = "/ByteRange [0 " + std::to_string(out.range[1]) + " " + std::to_string(out.range[2]) + " " +
                            std::to_string(out.range[3]) + "]";
  if (range.size() > rangeHolder.size()) return Error::Unreadable;
  out.bytes.replace(rangeAt, range.size(), range);
  return Error::None;
}

bool Fill(Prepared& p, const std::vector<uint8_t>& cms) {
  if (cms.size() * 2 > p.contentsHex) return false;
  static const char* hex = "0123456789ABCDEF";
  for (size_t i = 0; i < cms.size(); ++i) {
    p.bytes[p.contentsAt + 1 + 2 * i] = hex[cms[i] >> 4];
    p.bytes[p.contentsAt + 2 + 2 * i] = hex[cms[i] & 15];
  }
  return true;
}

Error MakeCms(const CERT_CONTEXT* cert, std::string_view first, std::string_view second, std::vector<uint8_t>& cms) {
  HCRYPTPROV_OR_NCRYPT_KEY_HANDLE key = 0;
  DWORD spec = 0;
  BOOL mustFree = FALSE;
  // CNG first; the key handle stays cached with the certificate, so nothing here frees it.
  if (!CryptAcquireCertificatePrivateKey(cert, CRYPT_ACQUIRE_PREFER_NCRYPT_KEY_FLAG | CRYPT_ACQUIRE_CACHE_FLAG, nullptr,
                                         &key, &spec, &mustFree))
    return Error::NoKey;

  // ESS signing-certificate-v2 (RFC 5035), which PAdES requires: the certificate's SHA-256.
  uint8_t certHash[32];
  if (!Sha256(cert->pbCertEncoded, cert->cbCertEncoded, certHash)) return Error::Failed;
  std::vector<uint8_t> ess = {0x30, 0x26, 0x30, 0x24, 0x30, 0x22, 0x04, 0x20};
  ess.insert(ess.end(), certHash, certHash + 32);
  CRYPT_ATTR_BLOB essBlob{static_cast<DWORD>(ess.size()), ess.data()};
  CRYPT_ATTRIBUTE attr{const_cast<char*>(kOidSigningCertificateV2), 1, &essBlob};

  CMSG_SIGNER_ENCODE_INFO signer{};
  signer.cbSize = sizeof signer;
  signer.pCertInfo = cert->pCertInfo;
  signer.hNCryptKey = key;
  signer.dwKeySpec = spec;
  signer.HashAlgorithm.pszObjId = const_cast<char*>(szOID_NIST_sha256);
  if (strcmp(cert->pCertInfo->SubjectPublicKeyInfo.Algorithm.pszObjId, szOID_ECC_PUBLIC_KEY) == 0)
    signer.HashEncryptionAlgorithm.pszObjId = const_cast<char*>(szOID_ECDSA_SHA256);
  signer.cAuthAttr = 1;
  signer.rgAuthAttr = &attr;

  // The certificate and its chain, so a validator can find the issuers.
  std::vector<const CERT_CONTEXT*> chain = Chain(cert, cert->hCertStore, nullptr);
  if (chain.empty()) chain.push_back(CertDuplicateCertificateContext(cert));
  std::vector<CERT_BLOB> blobs;
  for (const CERT_CONTEXT* c : chain) blobs.push_back({c->cbCertEncoded, c->pbCertEncoded});
  CMSG_SIGNED_ENCODE_INFO info{};
  info.cbSize = sizeof info;
  info.cSigners = 1;
  info.rgSigners = &signer;
  info.cCertEncoded = static_cast<DWORD>(blobs.size());
  info.rgCertEncoded = blobs.data();

  Error result = Error::Failed;
  HCRYPTMSG msg = CryptMsgOpenToEncode(kEncoding, CMSG_DETACHED_FLAG, CMSG_SIGNED, &info, nullptr, nullptr);
  if (msg) {
    DWORD size = 0;
    if (CryptMsgUpdate(msg, reinterpret_cast<const BYTE*>(first.data()), static_cast<DWORD>(first.size()), FALSE) &&
        CryptMsgUpdate(msg, reinterpret_cast<const BYTE*>(second.data()), static_cast<DWORD>(second.size()), TRUE) &&
        CryptMsgGetParam(msg, CMSG_CONTENT_PARAM, 0, nullptr, &size) && size > 0) {
      cms.resize(size);
      if (CryptMsgGetParam(msg, CMSG_CONTENT_PARAM, 0, cms.data(), &size)) {
        cms.resize(size);
        result = Error::None;
      }
    }
    CryptMsgClose(msg);
  }
  for (const CERT_CONTEXT* c : chain) CertFreeCertificateContext(c);
  return result;
}

Error SignPdf(std::string_view file, const CERT_CONTEXT* cert, const Details& details, std::string& out) {
  Prepared p;
  if (const Error e = Prepare(file, details, kReserve, p); e != Error::None) return e;
  const std::string_view all = p.bytes;
  std::vector<uint8_t> cms;
  if (const Error e = MakeCms(cert, all.substr(p.range[0], p.range[1]), all.substr(p.range[2], p.range[3]), cms);
      e != Error::None)
    return e;
  if (!Fill(p, cms)) return Error::TooLarge;
  out = std::move(p.bytes);
  return Error::None;
}

Verdict Verify(std::string_view file, const std::vector<int>& range, const std::vector<uint8_t>& contents) {
  Verdict v;
  if (range.size() != 4 || range[0] != 0 || range[1] <= 0 || range[2] < range[1] || range[3] < 0 ||
      static_cast<size_t>(range[2]) + static_cast<size_t>(range[3]) > file.size())
    return v;
  const size_t der = DerLength(contents);
  if (der == 0) return v;
  HCRYPTMSG msg = CryptMsgOpenToDecode(kEncoding, CMSG_DETACHED_FLAG, 0, 0, nullptr, nullptr);
  if (!msg) return v;
  bool decoded = CryptMsgUpdate(msg, contents.data(), static_cast<DWORD>(der), TRUE) &&
                 CryptMsgUpdate(msg, reinterpret_cast<const BYTE*>(file.data()), static_cast<DWORD>(range[1]), FALSE) &&
                 CryptMsgUpdate(msg, reinterpret_cast<const BYTE*>(file.data() + range[2]), static_cast<DWORD>(range[3]),
                                TRUE);
  const CERT_CONTEXT* signerCert = nullptr;
  HCERTSTORE store = nullptr;
  if (decoded) {
    DWORD size = 0;
    std::vector<uint8_t> info;
    if (CryptMsgGetParam(msg, CMSG_SIGNER_CERT_INFO_PARAM, 0, nullptr, &size) && size > 0) {
      info.resize(size);
      if (CryptMsgGetParam(msg, CMSG_SIGNER_CERT_INFO_PARAM, 0, info.data(), &size)) {
        store = CertOpenStore(CERT_STORE_PROV_MSG, kEncoding, 0, 0, msg);
        if (store)
          signerCert = CertGetSubjectCertificateFromStore(store, kEncoding, reinterpret_cast<CERT_INFO*>(info.data()));
      }
    }
  }
  if (signerCert) {
    v.signer = NameOf(signerCert, 0);
    // The signature over the signed attributes, and their digest against the bytes signed.
    if (!CryptMsgControl(msg, 0, CMSG_CTRL_VERIFY_SIGNATURE, signerCert->pCertInfo)) {
      v.kind = Verdict::Invalid;
    } else if (static_cast<size_t>(range[2]) + static_cast<size_t>(range[3]) < file.size()) {
      v.kind = Verdict::Changed;
    } else {
      bool trusted = false;
      for (const CERT_CONTEXT* c : Chain(signerCert, store, &trusted)) CertFreeCertificateContext(c);
      v.kind = trusted ? Verdict::Valid : Verdict::Untrusted;
    }
    CertFreeCertificateContext(signerCert);
  } else if (decoded) {
    v.kind = Verdict::Invalid;
  }
  if (store) CertCloseStore(store, 0);
  CryptMsgClose(msg);
  return v;
}

std::vector<const CERT_CONTEXT*> SigningCertificates() {
  std::vector<const CERT_CONTEXT*> out;
  HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0,
                                   CERT_SYSTEM_STORE_CURRENT_USER | CERT_STORE_READONLY_FLAG | CERT_STORE_OPEN_EXISTING_FLAG,
                                   L"MY");
  if (!store) return out;
  for (const CERT_CONTEXT* c = nullptr; (c = CertEnumCertificatesInStore(store, c)) != nullptr;) {
    DWORD size = 0;
    if (!CertGetCertificateContextProperty(c, CERT_KEY_PROV_INFO_PROP_ID, nullptr, &size)) continue;  // no private key
    if (CertVerifyTimeValidity(nullptr, c->pCertInfo) != 0) continue;
    BYTE usage[2] = {};
    if (CertGetIntendedKeyUsage(kEncoding, c->pCertInfo, usage, sizeof usage) &&
        !(usage[0] & (CERT_DIGITAL_SIGNATURE_KEY_USAGE | CERT_NON_REPUDIATION_KEY_USAGE)))
      continue;
    out.push_back(CertDuplicateCertificateContext(c));
  }
  CertCloseStore(store, 0);
  return out;
}

std::wstring CertName(const CERT_CONTEXT* cert) { return NameOf(cert, 0); }
std::wstring CertIssuer(const CERT_CONTEXT* cert) { return NameOf(cert, CERT_NAME_ISSUER_FLAG); }

const CERT_CONTEXT* LoadPfx(std::string_view pfx, const std::wstring& password) {
  CRYPT_DATA_BLOB blob{static_cast<DWORD>(pfx.size()), reinterpret_cast<BYTE*>(const_cast<char*>(pfx.data()))};
  HCERTSTORE store = PFXImportCertStore(&blob, password.c_str(), PKCS12_NO_PERSIST_KEY | PKCS12_ALWAYS_CNG_KSP);
  if (!store) return nullptr;
  const CERT_CONTEXT* found = nullptr;
  for (const CERT_CONTEXT* c = nullptr; (c = CertEnumCertificatesInStore(store, c)) != nullptr;) {
    DWORD size = 0;
    if (CertGetCertificateContextProperty(c, CERT_KEY_PROV_INFO_PROP_ID, nullptr, &size) ||
        CertGetCertificateContextProperty(c, CERT_NCRYPT_KEY_HANDLE_PROP_ID, nullptr, &size)) {
      found = CertDuplicateCertificateContext(c);
      CertFreeCertificateContext(c);
      break;
    }
  }
  CertCloseStore(store, 0);
  return found;
}

}  // namespace pdfsign
