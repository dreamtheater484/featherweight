// f1-ship: the updater's trust rule, offline. Copies of this test program are signed here with the test-only
// certificates in tests/data/updater (loaded in memory with PFXImportCertStore, never added to a certificate
// store), then checked the way the updater checks a download.

#include <windows.h>
#include <wincrypt.h>

#include <map>
#include <string>
#include <vector>

#include "Policy.h"
#include "TestHarness.h"
#include "Updater.h"
#include "Util.h"
#include "Version.h"
#include "core/Json.h"
#include "core/Strings.h"

#ifndef FW_TEST_DATA_DIR
#error FW_TEST_DATA_DIR must point at tests/data
#endif

namespace {

// mssign32.dll's signing call (documented, but not in the SDK headers).
struct SignerFileInfo {
  DWORD cbSize;
  LPCWSTR pwszFileName;
  HANDLE hFile;
};
struct SignerSubjectInfo {
  DWORD cbSize;
  DWORD* pdwIndex;
  DWORD dwSubjectChoice;  // 1: a file
  SignerFileInfo* pSignerFileInfo;
};
struct SignerCertStoreInfo {
  DWORD cbSize;
  PCCERT_CONTEXT pSigningCert;
  DWORD dwCertPolicy;  // 2: include the chain
  HCERTSTORE hCertStore;
};
struct SignerCert {
  DWORD cbSize;
  DWORD dwCertChoice;  // 2: a certificate in a store
  SignerCertStoreInfo* pCertStoreInfo;
  HWND hwnd;
};
struct SignerSignatureInfo {
  DWORD cbSize;
  ALG_ID algidHash;
  DWORD dwAttrChoice;  // 0: no attributes
  void* pAttrAuthcode;
  PCRYPT_ATTRIBUTES psAuthenticated;
  PCRYPT_ATTRIBUTES psUnauthenticated;
};
struct SignerContext {
  DWORD cbSize;
  DWORD cbBlob;
  BYTE* pbBlob;
};
using SignerSignEx2Fn = HRESULT(WINAPI*)(DWORD, SignerSubjectInfo*, SignerCert*, SignerSignatureInfo*, void*, DWORD,
                                         PCSTR, PCWSTR, PCRYPT_ATTRIBUTES, void*, SignerContext**, void*, void*);
using SignerFreeSignerContextFn = HRESULT(WINAPI*)(SignerContext*);

std::wstring DataPath(const wchar_t* name) {
  return util::FullPath(std::wstring(FW_TEST_DATA_DIR) + L"\\updater\\" + name);
}

// A test certificate with its key, held in memory only.
class TestCert {
 public:
  explicit TestCert(const wchar_t* pfxName) {
    std::string bytes;
    if (!util::ReadFileBytes(DataPath(pfxName), bytes)) return;
    CRYPT_DATA_BLOB blob{static_cast<DWORD>(bytes.size()), reinterpret_cast<BYTE*>(bytes.data())};
    store_ = PFXImportCertStore(&blob, L"test", PKCS12_NO_PERSIST_KEY | PKCS12_ALWAYS_CNG_KSP);
    if (!store_) return;
    for (PCCERT_CONTEXT c = nullptr; (c = CertEnumCertificatesInStore(store_, c)) != nullptr;) {
      DWORD size = 0;
      if (CertGetCertificateContextProperty(c, CERT_NCRYPT_KEY_HANDLE_PROP_ID, nullptr, &size) ||
          CertGetCertificateContextProperty(c, CERT_KEY_PROV_INFO_PROP_ID, nullptr, &size)) {
        cert_ = CertDuplicateCertificateContext(c);
        CertFreeCertificateContext(c);
        break;
      }
    }
  }
  ~TestCert() {
    if (cert_) CertFreeCertificateContext(cert_);
    if (store_) CertCloseStore(store_, 0);
  }
  bool Ok() const { return cert_ != nullptr; }
  std::wstring Thumbprint() const {
    BYTE hash[20];
    DWORD size = sizeof(hash);
    if (!cert_ || !CertGetCertificateContextProperty(cert_, CERT_SHA1_HASH_PROP_ID, hash, &size)) return {};
    static const wchar_t digits[] = L"0123456789abcdef";
    std::wstring s;
    for (DWORD i = 0; i < size; ++i) s += {digits[hash[i] >> 4], digits[hash[i] & 15]};
    return s;
  }
  // Signs `path` in place (SHA-256, no timestamp: nothing goes to the network).
  bool Sign(const std::wstring& path) const {
    static HMODULE mssign = LoadLibraryExW(L"mssign32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!mssign || !cert_) return false;
    auto sign = reinterpret_cast<SignerSignEx2Fn>(GetProcAddress(mssign, "SignerSignEx2"));
    auto freeContext = reinterpret_cast<SignerFreeSignerContextFn>(GetProcAddress(mssign, "SignerFreeSignerContext"));
    if (!sign || !freeContext) return false;
    DWORD index = 0;
    SignerFileInfo file{sizeof(file), path.c_str(), nullptr};
    SignerSubjectInfo subject{sizeof(subject), &index, 1, &file};
    SignerCertStoreInfo storeInfo{sizeof(storeInfo), cert_, 2, store_};
    SignerCert signer{sizeof(signer), 2, &storeInfo, nullptr};
    SignerSignatureInfo info{sizeof(info), CALG_SHA_256, 0, nullptr, nullptr, nullptr};
    SignerContext* context = nullptr;
    HRESULT hr = sign(0, &subject, &signer, &info, nullptr, 0, nullptr, nullptr, nullptr, nullptr, &context, nullptr,
                      nullptr);
    if (context) freeContext(context);
    return SUCCEEDED(hr);
  }

 private:
  HCERTSTORE store_ = nullptr;
  PCCERT_CONTEXT cert_ = nullptr;
};

// A copy of this test program in the temporary folder, deleted afterwards.
class TempCopy {
 public:
  explicit TempCopy(const wchar_t* tag) {
    wchar_t dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    path_ = std::wstring(dir) + L"fw-updater-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + tag + L".exe";
    ok_ = CopyFileW(util::ExePath().c_str(), path_.c_str(), FALSE) != FALSE;
  }
  ~TempCopy() { DeleteFileW(path_.c_str()); }
  bool Ok() const { return ok_; }
  const std::wstring& Path() const { return path_; }
  // Changes one byte of the program's code after signing, as a tampered download would.
  bool Tamper() const {
    HANDLE h = CreateFileW(path_.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    BYTE b = 0;
    DWORD n = 0;
    LARGE_INTEGER at{};
    at.QuadPart = 0x1000;
    bool ok = SetFilePointerEx(h, at, nullptr, FILE_BEGIN) && ReadFile(h, &b, 1, &n, nullptr) && n == 1;
    b ^= 0x5a;
    ok = ok && SetFilePointerEx(h, at, nullptr, FILE_BEGIN) && WriteFile(h, &b, 1, &n, nullptr) && n == 1;
    CloseHandle(h);
    return ok;
  }

 private:
  std::wstring path_;
  bool ok_ = false;
};

using updater::Signature;
using updater::Verdict;

}  // namespace

TEST(UpdaterSignatureRule) {
  TestCert a(L"publisher-a.pfx"), b(L"publisher-b.pfx"), impostor(L"impostor-a.pfx");
  CHECK(a.Ok());
  CHECK(b.Ok());
  CHECK(impostor.Ok());
  if (!a.Ok() || !b.Ok() || !impostor.Ok()) return;

  TempCopy running(L"running"), sameA(L"same"), unsignedCopy(L"unsigned"), other(L"other"), fake(L"impostor"),
      tampered(L"tampered");
  CHECK(running.Ok() && sameA.Ok() && unsignedCopy.Ok() && other.Ok() && fake.Ok() && tampered.Ok());
  CHECK(a.Sign(running.Path()));
  CHECK(a.Sign(sameA.Path()));
  CHECK(b.Sign(other.Path()));
  CHECK(impostor.Sign(fake.Path()));
  CHECK(a.Sign(tampered.Path()));
  CHECK(tampered.Tamper());

  // Publishers A and B stand in for real ones (their roots are trusted for this test); the impostor's root isn't.
  updater::SetTestRootsForTesting({a.Thumbprint(), b.Thumbprint()});
  Signature run = updater::ReadSignature(running.Path());
  CHECK(run.state == Signature::State::Trusted);
  CHECK(run.subject.find(L"CN=Featherweight Test Publisher") != std::wstring::npos);

  CHECK(updater::CheckFile(run, updater::ReadSignature(sameA.Path()), L"") == Verdict::Accepted);
  CHECK(updater::CheckFile(run, updater::ReadSignature(unsignedCopy.Path()), L"") == Verdict::NotSigned);
  CHECK(updater::CheckFile(run, updater::ReadSignature(other.Path()), L"") == Verdict::OtherPublisher);
  Signature fakeSig = updater::ReadSignature(fake.Path());
  CHECK(fakeSig.subject == run.subject);  // the same name...
  CHECK(updater::CheckFile(run, fakeSig, L"") == Verdict::BadSignature);  // ...but not from a trusted root
  Signature tamperedSig = updater::ReadSignature(tampered.Path());
  CHECK(tamperedSig.state == Signature::State::Invalid);
  CHECK(updater::CheckFile(run, tamperedSig, L"") == Verdict::BadSignature);

  // Without the test roots, A's signature is as untrusted as any test certificate: the running exe then counts as
  // unsigned, and the updater falls back to SHA-256 (every verdict is Accepted, for the caller to hash).
  updater::SetTestRootsForTesting({});
  Signature untrusted = updater::ReadSignature(running.Path());
  CHECK(untrusted.state == Signature::State::Invalid);
  CHECK(updater::CheckFile(untrusted, updater::ReadSignature(unsignedCopy.Path()), L"") == Verdict::Accepted);
  CHECK(updater::ReadSignature(unsignedCopy.Path()).state == Signature::State::Unsigned);
}

TEST(UpdaterProductRule) {
  Signature run;
  run.state = Signature::State::Trusted;
  run.subject = L"CN=SignPath Foundation, O=SignPath Foundation, C=US";
  Signature file = run;
  file.productName = L"Featherweight";
  CHECK(updater::CheckFile(run, file, L"Featherweight") == Verdict::Accepted);
  CHECK(updater::CheckFile(run, file, L"featherweight") == Verdict::Accepted);
  file.productName = L"Some other project";  // signed by the same foundation for someone else
  CHECK(updater::CheckFile(run, file, L"Featherweight") == Verdict::OtherProduct);
  CHECK(updater::CheckFile(run, file, L"") == Verdict::Accepted);
  Signature unsignedRun;
  CHECK(updater::CheckFile(unsignedRun, Signature{}, L"Featherweight") == Verdict::Accepted);
}

// ---- The admin policy and its templates ----

TEST(PolicyReadsTheRegistry) {
  const std::wstring sub = L"Software\\FeatherweightTests\\Policy-" + std::to_wstring(GetCurrentProcessId());
  HKEY key = nullptr;
  CHECK(RegCreateKeyExW(HKEY_CURRENT_USER, sub.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) ==
        ERROR_SUCCESS);
  if (!key) return;
  policy::Values empty = policy::Read(HKEY_CURRENT_USER, sub.c_str());
  CHECK(!empty.updatesOff && empty.updateChannel.empty() && empty.language.empty());
  DWORD one = 1;
  RegSetValueExW(key, policy::kDisableUpdates, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&one), sizeof(one));
  auto setText = [&](const wchar_t* name, const std::wstring& value) {
    RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                   static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
  };
  setText(policy::kUpdateChannel, L"Beta");
  setText(policy::kLanguage, L"de");
  policy::Values v = policy::Read(HKEY_CURRENT_USER, sub.c_str());
  CHECK(v.updatesOff);
  CHECK(v.updateChannel == L"beta");
  CHECK(v.language == L"de");
  setText(policy::kUpdateChannel, L"nightly");  // not a channel: ignored
  CHECK(policy::Read(HKEY_CURRENT_USER, sub.c_str()).updateChannel.empty());
  RegCloseKey(key);
  RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\FeatherweightTests");
  // No policy on a machine that has none (and nothing here is written to HKLM).
  CHECK(policy::Read(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\FeatherweightTestsNone").language.empty());
}

TEST(PolicyTemplatesMatchWhatTheAppReads) {
  std::string admx, adml;
  CHECK(util::ReadFileBytes(std::wstring(FW_SOURCE_DIR) + L"/packaging/admx/Featherweight.admx", admx));
  CHECK(util::ReadFileBytes(std::wstring(FW_SOURCE_DIR) + L"/packaging/admx/en-US/Featherweight.adml", adml));
  auto has = [](const std::string& text, const std::wstring& what) { return text.find(fw::WideToUtf8(what)) != std::string::npos; };
  // Every policy writes the key the app reads, under the value names it reads.
  size_t keys = 0;
  for (size_t at = 0; (at = admx.find("key=\"", at)) != std::string::npos; ++keys) {
    at += 5;
    CHECK(_stricmp(admx.substr(at, admx.find('"', at) - at).c_str(), fw::WideToUtf8(policy::kKey).c_str()) == 0);
  }
  CHECK_EQ(keys, 3u);
  CHECK(has(admx, std::wstring(L"valueName=\"") + policy::kDisableUpdates + L"\""));
  CHECK(has(admx, std::wstring(L"valueName=\"") + policy::kUpdateChannel + L"\""));
  CHECK(has(admx, std::wstring(L"valueName=\"") + policy::kLanguage + L"\""));
  CHECK(has(admx, L"<string>stable</string>") && has(admx, L"<string>beta</string>"));
  // Every string and presentation the ADMX names is in the ADML.
  for (const char* kind : {"$(string.", "$(presentation."}) {
    for (size_t at = 0; (at = admx.find(kind, at)) != std::string::npos;) {
      at += strlen(kind);
      std::string id = admx.substr(at, admx.find(')', at) - at);
      CHECK(adml.find("id=\"" + id + "\"") != std::string::npos);
    }
  }
}

// ---- Choosing the release, and the PDFium build an install has ----

TEST(UpdaterPicksTheChannelsRelease) {
  auto list = fw::ParseJson(R"([
    {"tag_name":"v4.2.0","draft":true,"prerelease":false},
    {"tag_name":"v4.1.1-beta.1","draft":false,"prerelease":true},
    {"tag_name":"v4.1.2","draft":false,"prerelease":true},
    {"tag_name":"v4.1.0","draft":false,"prerelease":false},
    {"tag_name":"v4.0.0","draft":false,"prerelease":true}])");
  CHECK(list.has_value());
  if (!list) return;
  const fw::JsonValue* beta = updater::PickRelease(*list, true);
  const fw::JsonValue* stable = updater::PickRelease(*list, false);
  CHECK(beta && beta->StringOr("tag_name") == "v4.1.2");  // the newest, a pre-release; never the draft
  CHECK(stable && stable->StringOr("tag_name") == "v4.1.0");
  auto latest = fw::ParseJson(R"({"tag_name":"v4.1.0","prerelease":false})");
  CHECK(latest && updater::PickRelease(*latest, false) == &*latest);
  auto none = fw::ParseJson("[]");
  CHECK(none && !updater::PickRelease(*none, true));
}

TEST(UpdaterTellsThePdfiumBuilds) {
  CHECK_EQ(updater::IsScriptEngine(FW_PDFIUM_DLL), FW_PDFIUM_IS_V8 != 0);
  CHECK(!updater::IsScriptEngine(util::ExePath()));  // a program without that export
  CHECK(!updater::IsScriptEngine(std::wstring(FW_TEST_DATA_DIR) + L"\\updater\\publisher-a.pfx"));  // not a PE file
  CHECK(!updater::IsScriptEngine(L"Z:\\no\\such\\pdfium.dll"));
}

// ---- The updater end to end, offline: a release in a folder, an install in another ----

namespace {

void DeleteTree(const std::wstring& dir) {
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
  if (h != INVALID_HANDLE_VALUE) {
    do {
      std::wstring name = fd.cFileName, path = dir + L"\\" + name;
      if (name == L"." || name == L"..") continue;
      if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) DeleteTree(path);
      else DeleteFileW(path.c_str());
    } while (FindNextFileW(h, &fd));
    FindClose(h);
  }
  RemoveDirectoryW(dir.c_str());
}

// The release's name for this architecture's program (packaging/package.ps1's updater assets).
#if defined(_M_ARM64)
const std::string kExeAsset = "Featherweight-arm64.exe";
#else
const std::string kExeAsset = "Featherweight.exe";
#endif

// %TEMP%\fw-updater-e2e-<pid>\ with install\ (Featherweight.exe, an old build) and release\.
class Sandbox {
 public:
  Sandbox() {
    wchar_t dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    static int n = 0;
    root_ = std::wstring(dir) + L"fw-updater-e2e-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++n);
    DeleteTree(root_);
    CreateDirectoryW(root_.c_str(), nullptr);
    CreateDirectoryW(Install().c_str(), nullptr);
    CreateDirectoryW(Release().c_str(), nullptr);
    util::WriteFileAtomic(Install() + L"\\Featherweight.exe", "the old build");
  }
  ~Sandbox() {
    Updater::SetReleaseUrlForTesting(L"");
    Updater::SetInstallDirForTesting(L"");
    Updater::SetRunningSignatureForTesting(nullptr);
    DeleteTree(root_);
  }
  std::wstring Install() const { return root_ + L"\\install"; }
  std::wstring Release() const { return root_ + L"\\release"; }
  // Publishes `exe` (a file) as the release's program, with SHA256SUMS.txt and latest.json.
  void Publish(const std::wstring& exe, const char* tag) {
    const std::wstring asset = Release() + L"\\" + fw::Utf8ToWide(kExeAsset);
    CopyFileW(exe.c_str(), asset.c_str(), FALSE);
    std::string sums = util::Sha256File(asset) + "  " + kExeAsset + "\n";
    util::WriteFileAtomic(Release() + L"\\SHA256SUMS.txt", sums);
    std::string json = std::string("{\"tag_name\":\"") + tag + "\",\"html_url\":\"https://example.invalid/r\"," + Assets() + "}";
    util::WriteFileAtomic(Release() + L"\\latest.json", json);
    Updater::SetReleaseUrlForTesting(L"file:///" + Release());
    Updater::SetInstallDirForTesting(Install());
  }
  // The release's "assets" member: the program and SHA256SUMS.txt, as file: URLs.
  std::string Assets() const {
    std::string base = fw::WideToUtf8(Release());
    for (char& c : base) c = c == '\\' ? '/' : c;
    return "\"assets\":[{\"name\":\"" + kExeAsset + "\",\"browser_download_url\":\"file:///" + base + "/" + kExeAsset +
           "\"},{\"name\":\"SHA256SUMS.txt\",\"browser_download_url\":\"file:///" + base + "/SHA256SUMS.txt\"}]";
  }
  std::string InstalledHash() const { return util::Sha256File(Install() + L"\\Featherweight.exe"); }
  bool Leftovers() const { return util::FileExists(Install() + L"\\Featherweight.exe.new"); }

 private:
  std::wstring root_;
};

Updater::Options InstallOptions() {
  Updater::Options o;
  o.install = true;
  return o;
}


// A signed install (publisher A) and a release folder; the tests below publish a file and run the updater on it.
struct SignedInstall {
  TestCert a{L"publisher-a.pfx"}, b{L"publisher-b.pfx"};
  TempCopy signedA{L"e2e-a"}, signedB{L"e2e-b"}, plain{L"e2e-plain"};
  Signature running;
  Sandbox box;
  bool ok = false;
  SignedInstall() {
    if (!a.Ok() || !b.Ok() || !a.Sign(signedA.Path()) || !b.Sign(signedB.Path())) return;
    updater::SetTestRootsForTesting({a.Thumbprint(), b.Thumbprint()});
    running = updater::ReadSignature(signedA.Path());
    Updater::SetRunningSignatureForTesting(&running);
    ok = running.state == Signature::State::Trusted && running.fileVersion == L"9.9.9" &&
         running.productName == L"Featherweight";
  }
  ~SignedInstall() { updater::SetTestRootsForTesting({}); }
};

}  // namespace

TEST(UpdaterRefusesUnsigned) {
  SignedInstall s;
  CHECK(s.ok);
  const std::string old = s.box.InstalledHash();
  s.box.Publish(s.plain.Path(), "v9.9.9");  // unsigned: refused, nothing changes, nothing left behind
  Updater::Result r = Updater::RunForTesting(InstallOptions());
  CHECK(r.status == Updater::Status::Failed && r.error.find(L"isn’t signed") != std::wstring::npos);
  CHECK(s.box.InstalledHash() == old && !s.box.Leftovers());

  // An unsigned install (4.0, or 4.1 before SignPath) keeps the SHA-256 check instead.
  const Signature unsignedRunning;
  Updater::SetRunningSignatureForTesting(&unsignedRunning);
  util::WriteFileAtomic(s.box.Release() + L"\\SHA256SUMS.txt", std::string(64, '0') + "  " + kExeAsset + "\n");
  CHECK(Updater::RunForTesting(InstallOptions()).status == Updater::Status::Failed);  // the hash doesn't match
  CHECK(s.box.InstalledHash() == old && !s.box.Leftovers());
  s.box.Publish(s.plain.Path(), "v9.9.9");
  CHECK(Updater::RunForTesting(InstallOptions()).status == Updater::Status::Installed);
  CHECK(s.box.InstalledHash() == util::Sha256File(s.plain.Path()));
}

TEST(UpdaterRefusesOtherPublisher) {
  SignedInstall s;
  CHECK(s.ok);
  TestCert impostor(L"impostor-a.pfx");
  TempCopy fake(L"e2e-impostor"), tampered(L"e2e-tampered");
  CHECK(impostor.Sign(fake.Path()) && s.a.Sign(tampered.Path()) && tampered.Tamper());
  const std::string old = s.box.InstalledHash();
  struct Case {
    const std::wstring* file;
    const char* tag;
    const wchar_t* why;
  } cases[] = {
      {&s.signedB.Path(), "v9.9.9", L"another publisher"},
      {&fake.Path(), "v9.9.9", L"signature isn’t valid"},  // A's name on a certificate A never had
      {&tampered.Path(), "v9.9.9", L"signature isn’t valid"},
      {&s.signedA.Path(), "v9.9.8", L"release’s version"},  // no older signed build passed off as new
  };
  for (const Case& c : cases) {
    s.box.Publish(*c.file, c.tag);
    Updater::Result r = Updater::RunForTesting(InstallOptions());
    CHECK(r.status == Updater::Status::Failed && r.error.find(c.why) != std::wstring::npos);
    CHECK(s.box.InstalledHash() == old && !s.box.Leftovers());
  }
  s.box.Publish(s.signedA.Path(), "v9.9.9");  // the same publisher, product and version: installed
  CHECK(Updater::RunForTesting(InstallOptions()).status == Updater::Status::Installed);
  CHECK(s.box.InstalledHash() == util::Sha256File(s.signedA.Path()));
}

TEST(UpdaterChannels) {
  SignedInstall s;
  CHECK(s.ok);
  // Stable reads latest.json (GitHub's /releases/latest), beta the list (/releases) with pre-releases.
  s.box.Publish(s.signedA.Path(), "v9.9.9");
  const std::string asset = s.box.Assets();
  util::WriteFileAtomic(s.box.Release() + L"\\latest.json", "{\"tag_name\":\"v4.0.0\",\"prerelease\":false}");
  util::WriteFileAtomic(s.box.Release() + L"\\releases.json",
                        "[{\"tag_name\":\"v9.9.10\",\"draft\":true},{\"tag_name\":\"v9.9.9\",\"prerelease\":true," + asset +
                            "},{\"tag_name\":\"v4.0.0\",\"prerelease\":false}]");
  Updater::Options stable = InstallOptions(), beta = InstallOptions();
  beta.beta = true;
  CHECK(Updater::RunForTesting(stable).status == Updater::Status::UpToDate);  // stable: nothing newer than 4.0
  Updater::Result r = Updater::RunForTesting(beta);  // beta: the pre-release, never the draft
  CHECK(r.status == Updater::Status::Installed && r.version == L"9.9.9");
  CHECK(s.box.InstalledHash() == util::Sha256File(s.signedA.Path()));

  // A skipped version (Skip this version, or a rollback) is offered but never installed by itself.
  Sandbox again;
  again.Publish(s.signedA.Path(), "v9.9.9");
  Updater::Options skipped = InstallOptions();
  skipped.skippedVersion = L"9.9.9";
  CHECK(Updater::RunForTesting(skipped).status == Updater::Status::Available);
  CHECK(again.InstalledHash() == util::Sha256(std::string("the old build").data(), 13));
}

TEST(UpdaterRollback) {
  SignedInstall s;
  CHECK(s.ok);
  const std::string old = s.box.InstalledHash();
  CHECK(Updater::PreviousVersion().empty());
  s.box.Publish(s.signedA.Path(), "v9.9.9");
  CHECK(Updater::RunForTesting(InstallOptions()).status == Updater::Status::Installed);  // the old build kept
  CHECK(util::Sha256File(s.box.Install() + L"\\previous\\Featherweight.exe") == old);
  CHECK(Updater::PreviousVersion() == FW_VERSION_WSTRING);
  std::wstring error;  // one rollback: the old build is back, previous\ is gone
  CHECK(Updater::RollBack(&error));
  CHECK(s.box.InstalledHash() == old);
  CHECK(Updater::PreviousVersion().empty());
  CHECK(!Updater::RollBack(&error) && !error.empty());
  Updater::CleanupOldFiles();
  CHECK(!util::FileExists(s.box.Install() + L"\\Featherweight.exe.old"));
}

TEST(PolicyUpdatesOff) {
  policy::Values none, off, beta;
  off.updatesOff = true;
  beta.updateChannel = L"beta";
  CHECK(updater::Allowed(none, false));
  CHECK(!updater::Allowed(off, false));  // an administrator turned updates off
  CHECK(!updater::Allowed(none, true));  // the Store updates a packaged copy
  CHECK(updater::Allowed(beta, false));
}

TEST(UpdaterScriptEngine) {
#if defined(_M_ARM64)
  const std::wstring asset = L"pdfium-v8-arm64.dll";
#else
  const std::wstring asset = L"pdfium-v8.dll";
#endif
  Sandbox box;
  const Signature unsignedRunning;
  Updater::SetRunningSignatureForTesting(&unsignedRunning);
  Updater::SetInstallDirForTesting(box.Install());
  TempCopy standard(L"pdfium-standard");  // stands in for the standard build: a PE file without V8's export
  CopyFileW(standard.Path().c_str(), (box.Install() + L"\\pdfium.dll").c_str(), FALSE);
  // This version's release, with the PDFium this test was built with as its pdfium-v8 asset.
  CopyFileW(FW_PDFIUM_DLL, (box.Release() + L"\\" + asset).c_str(), FALSE);
  util::WriteFileAtomic(box.Release() + L"\\SHA256SUMS.txt",
                        util::Sha256File(box.Release() + L"\\" + asset) + "  " + fw::WideToUtf8(asset) + "\n");
  std::string base = fw::WideToUtf8(box.Release());
  for (char& c : base) c = c == '\\' ? '/' : c;
  auto tagJson = [&](const char* version) {
    return std::string("{\"tag_name\":\"v") + version + "\",\"assets\":[{\"name\":\"" + fw::WideToUtf8(asset) +
           "\",\"browser_download_url\":\"file:///" + base + "/" + fw::WideToUtf8(asset) +
           "\"},{\"name\":\"SHA256SUMS.txt\",\"browser_download_url\":\"file:///" + base + "/SHA256SUMS.txt\"}]}";
  };
  Updater::SetReleaseUrlForTesting(L"file:///" + box.Release());
  const std::string before = util::Sha256File(box.Install() + L"\\pdfium.dll");

  util::WriteFileAtomic(box.Release() + L"\\tag.json", tagJson("9.9.9"));  // another version's release: not used
  CHECK(Updater::ScriptEngineForTesting().status == Updater::Status::Failed);
  CHECK(util::Sha256File(box.Install() + L"\\pdfium.dll") == before);

  util::WriteFileAtomic(box.Release() + L"\\tag.json", tagJson(FW_VERSION_STRING));
  Updater::Result r = Updater::ScriptEngineForTesting();
  CHECK(r.component);
  if (FW_PDFIUM_IS_V8) {  // the V8 build: installed in place of pdfium.dll, the old one renamed
    CHECK(r.status == Updater::Status::Installed);
    CHECK(Updater::ScriptEngineInstalled());
    CHECK(util::FileExists(box.Install() + L"\\pdfium.dll.old"));
    CHECK(Updater::ScriptEngineForTesting().status == Updater::Status::Installed);  // already there: nothing to do
  } else {  // built with the standard PDFium: the "engine" isn't one, so it's refused
    CHECK(r.status == Updater::Status::Failed && r.error.find(L"isn’t the script engine") != std::wstring::npos);
    CHECK(util::Sha256File(box.Install() + L"\\pdfium.dll") == before);
  }
  CHECK(!util::FileExists(box.Install() + L"\\pdfium.dll.new"));
}