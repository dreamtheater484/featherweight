#include "Updater.h"

#include <appmodel.h>
#include <winhttp.h>
#include <wincrypt.h>
#include <wintrust.h>
#include <softpub.h>

#include <algorithm>
#include <map>
#include <optional>
#include <sstream>
#include <thread>
#include <vector>

#include "Util.h"
#include "Version.h"
#include "core/Json.h"
#include "core/SemVer.h"
#include "core/Strings.h"

namespace {

constexpr wchar_t kLatestUrl[] = L"https://api.github.com/repos/dreamtheater484/featherweight/releases/latest";
constexpr wchar_t kReleasesUrl[] = L"https://api.github.com/repos/dreamtheater484/featherweight/releases?per_page=30";
constexpr wchar_t kTagUrl[] = L"https://api.github.com/repos/dreamtheater484/featherweight/releases/tags/";
std::wstring g_testUrl, g_testInstallDir;
const updater::Signature* g_testRunning = nullptr;
// The files an install holds and the updater replaces.
const wchar_t* const kFiles[] = {L"Featherweight.exe", L"pdfium.dll"};
// Release assets (f1-ship): Featherweight.exe, and PDFium in two builds, pdfium-standard.dll and pdfium-v8.dll
// (with form scripts), each for x64, and with "-arm64" for Windows on ARM. A release has no "pdfium.dll" asset:
// 4.0's updater, which only knows that name, then keeps the V8 build it has. Older releases' pdfium.dll is V8.
#if defined(_M_ARM64)
constexpr wchar_t kArch[] = L"-arm64";
#else
constexpr wchar_t kArch[] = L"";
#endif
constexpr wchar_t kPrevious[] = L"previous";  // the files the last update replaced, for one rollback

std::wstring InstallDir() { return g_testInstallDir.empty() ? util::ExeDir() : g_testInstallDir; }

bool IsFileUrl(const std::wstring& url) { return _wcsnicmp(url.c_str(), L"file:", 5) == 0; }

// Simple HTTPS GET with redirects. Returns the HTTP status (0 on network failure).
int HttpGet(const std::wstring& url, std::string& body, size_t maxBytes) {
  body.clear();
  URL_COMPONENTS uc{sizeof(uc)};
  wchar_t host[256] = {}, path[2048] = {};
  uc.lpszHostName = host;
  uc.dwHostNameLength = 256;
  uc.lpszUrlPath = path;
  uc.dwUrlPathLength = 2048;
  wchar_t extra[2048] = {};
  uc.lpszExtraInfo = extra;
  uc.dwExtraInfoLength = 2048;
  if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS) return 0;

  std::wstring agent = L"Featherweight/" FW_VERSION_WSTRING;
  HINTERNET session = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                  WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) return 0;
  DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | 0x00002000 /*TLS1_3*/;
  if (!WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols))) {
    protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
    WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
  }
  WinHttpSetTimeouts(session, 10000, 10000, 15000, 30000);
  int status = 0;
  HINTERNET conn = WinHttpConnect(session, host, uc.nPort, 0);
  if (conn) {
    std::wstring object = std::wstring(path) + extra;
    HINTERNET req = WinHttpOpenRequest(conn, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (req) {
      const wchar_t* headers = L"Accept: application/vnd.github+json, application/octet-stream\r\n";
      if (WinHttpSendRequest(req, headers, static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
          WinHttpReceiveResponse(req, nullptr)) {
        DWORD code = 0, size = sizeof(code);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &code, &size, WINHTTP_NO_HEADER_INDEX);
        status = static_cast<int>(code);
        DWORD avail = 0;
        while (WinHttpQueryDataAvailable(req, &avail) && avail > 0) {
          size_t old = body.size();
          if (old + avail > maxBytes) {
            status = 0;
            break;
          }
          body.resize(old + avail);
          DWORD read = 0;
          if (!WinHttpReadData(req, body.data() + old, avail, &read)) {
            status = 0;
            break;
          }
          body.resize(old + read);
        }
      }
      WinHttpCloseHandle(req);
    }
    WinHttpCloseHandle(conn);
  }
  WinHttpCloseHandle(session);
  return status;
}

// HttpGet, or for offline tests (only while a test URL is set) a file: URL read from disk. Returns 200 or 404.
int Fetch(const std::wstring& url, std::string& body, size_t maxBytes) {
  if (!IsFileUrl(url)) return HttpGet(url, body, maxBytes);
  if (g_testUrl.empty()) return 0;
  size_t start = 5;  // "file:///C:/x" and "file:C:\x" both name C:\x
  while (start < url.size() && (url[start] == L'/' || url[start] == L'\\')) ++start;
  std::wstring path = url.substr(start);
  body.clear();
  return util::ReadFileBytes(path, body, maxBytes) ? 200 : 404;
}

bool WriteBytes(const std::wstring& path, const std::string& data) {
  HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  bool ok = WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) && written == data.size();
  CloseHandle(h);
  return ok;
}

// Parses sha256sum output: "<hash>  <name>" or "<hash> *<name>".
std::map<std::wstring, std::string> ParseSums(const std::string& text) {
  std::map<std::wstring, std::string> sums;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    line = fw::Trim(line);
    size_t sp = line.find(' ');
    if (sp != 64) continue;
    std::string hash = line.substr(0, 64);
    std::string name = fw::Trim(line.substr(sp));
    if (!name.empty() && name[0] == '*') name.erase(0, 1);
    for (auto& ch : hash) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
    sums[fw::ToLower(fw::Utf8ToWide(name))] = hash;
  }
  return sums;
}

std::vector<std::wstring> g_testRoots;  // SetTestRootsForTesting

// A string from a file's version resource (the first language it has), or "".
std::wstring VersionString(const std::vector<BYTE>& block, const wchar_t* name) {
  struct Lang {
    WORD language, codePage;
  }* langs = nullptr;
  UINT size = 0;
  if (!VerQueryValueW(block.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&langs), &size) ||
      size < sizeof(Lang))
    return {};
  wchar_t key[96];
  swprintf_s(key, L"\\StringFileInfo\\%04x%04x\\%s", langs[0].language, langs[0].codePage, name);
  wchar_t* value = nullptr;
  if (!VerQueryValueW(block.data(), key, reinterpret_cast<void**>(&value), &size) || !value) return {};
  return std::wstring(value, wcsnlen(value, size));
}

std::wstring HexOf(const BYTE* data, DWORD size) {
  static const wchar_t digits[] = L"0123456789abcdef";
  std::wstring s;
  for (DWORD i = 0; i < size; ++i) {
    s += digits[data[i] >> 4];
    s += digits[data[i] & 15];
  }
  return s;
}

}  // namespace

namespace updater {

void SetTestRootsForTesting(const std::vector<std::wstring>& sha1Thumbprints) {
  g_testRoots.clear();
  for (const auto& t : sha1Thumbprints) g_testRoots.push_back(fw::ToLower(t));
}

Signature ReadSignature(const std::wstring& path) {
  Signature sig;
  DWORD ignored = 0;
  if (DWORD size = GetFileVersionInfoSizeExW(FILE_VER_GET_NEUTRAL, path.c_str(), &ignored)) {
    std::vector<BYTE> block(size);
    if (GetFileVersionInfoExW(FILE_VER_GET_NEUTRAL, path.c_str(), 0, size, block.data())) {
      sig.productName = VersionString(block, L"ProductName");
      sig.fileVersion = VersionString(block, L"FileVersion");
    }
  }

  // Authenticode, as Explorer and SmartScreen check it, but offline: no revocation checks, and missing
  // certificates only from the local cache.
  WINTRUST_FILE_INFO file{sizeof(file)};
  file.pcwszFilePath = path.c_str();
  WINTRUST_DATA data{sizeof(data)};
  data.dwUIChoice = WTD_UI_NONE;
  data.fdwRevocationChecks = WTD_REVOKE_NONE;
  data.dwUnionChoice = WTD_CHOICE_FILE;
  data.pFile = &file;
  data.dwStateAction = WTD_STATEACTION_VERIFY;
  data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL | WTD_REVOCATION_CHECK_NONE;
  GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
  LONG result = WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);
  sig.error = result;
  if (result == TRUST_E_NOSIGNATURE || result == TRUST_E_SUBJECT_FORM_UNKNOWN || result == TRUST_E_PROVIDER_UNKNOWN) {
    sig.state = Signature::State::Unsigned;
  } else {
    sig.state = result == ERROR_SUCCESS ? Signature::State::Trusted : Signature::State::Invalid;
    if (CRYPT_PROVIDER_DATA* provider = WTHelperProvDataFromStateData(data.hWVTStateData)) {
      if (CRYPT_PROVIDER_SGNR* signer = WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0);
          signer && signer->csCertChain > 0) {
        CRYPT_PROVIDER_CERT* leaf = WTHelperGetProvCertFromChain(signer, 0);
        CRYPT_PROVIDER_CERT* root = WTHelperGetProvCertFromChain(signer, signer->csCertChain - 1);
        if (leaf && leaf->pCert) {
          CERT_NAME_BLOB* name = &leaf->pCert->pCertInfo->Subject;
          DWORD n = CertNameToStrW(X509_ASN_ENCODING, name, CERT_X500_NAME_STR, nullptr, 0);
          if (n > 1) {
            sig.subject.resize(n);
            CertNameToStrW(X509_ASN_ENCODING, name, CERT_X500_NAME_STR, sig.subject.data(), n);
            sig.subject.resize(n - 1);
          }
        }
        // Tests: a root they name stands in for a trusted one. Only the root's trust is waived: a bad digest,
        // an expired certificate or a certificate not made for code signing still fail.
        if (result == CERT_E_UNTRUSTEDROOT && !g_testRoots.empty() && root && root->pCert) {
          BYTE hash[20];
          DWORD size = sizeof(hash);
          if (CertGetCertificateContextProperty(root->pCert, CERT_SHA1_HASH_PROP_ID, hash, &size) &&
              std::find(g_testRoots.begin(), g_testRoots.end(), HexOf(hash, size)) != g_testRoots.end())
            sig.state = Signature::State::Trusted;
        }
      }
    }
  }
  data.dwStateAction = WTD_STATEACTION_CLOSE;
  WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);
  if (sig.state != Signature::State::Trusted && sig.state != Signature::State::Invalid) sig.subject.clear();
  return sig;
}

const Signature& RunningSignature() {
  static const Signature running = ReadSignature(util::ExePath());
  return running;
}

Verdict CheckFile(const Signature& running, const Signature& file, const std::wstring& expectedProduct) {
  if (running.state != Signature::State::Trusted) return Verdict::Accepted;  // unsigned: SHA-256 decides
  if (file.state == Signature::State::Unsigned) return Verdict::NotSigned;
  if (file.state != Signature::State::Trusted) return Verdict::BadSignature;
  if (file.subject != running.subject) return Verdict::OtherPublisher;
  if (!expectedProduct.empty() && _wcsicmp(file.productName.c_str(), expectedProduct.c_str()) != 0)
    return Verdict::OtherProduct;
  return Verdict::Accepted;
}

const wchar_t* VerdictText(Verdict v) {
  switch (v) {
    case Verdict::Accepted: return L"accepted";
    case Verdict::NotSigned: return L"the download isn’t signed";
    case Verdict::BadSignature: return L"the download’s signature isn’t valid";
    case Verdict::OtherPublisher: return L"the download is signed by another publisher";
    case Verdict::OtherProduct: return L"the download is signed for another product";
  }
  return L"";
}

}  // namespace updater

namespace updater {

const fw::JsonValue* PickRelease(const fw::JsonValue& answer, bool beta) {
  auto flag = [](const fw::JsonValue& release, const char* key) {
    const fw::JsonValue* v = release.Find(key);
    return v && v->type == fw::JsonValue::Type::Bool && v->boolean;
  };
  if (answer.type == fw::JsonValue::Type::Object) return flag(answer, "draft") ? nullptr : &answer;
  if (answer.type != fw::JsonValue::Type::Array) return nullptr;
  const fw::JsonValue* best = nullptr;
  std::optional<fw::SemVer> bestVersion;
  for (const fw::JsonValue& release : answer.array) {
    if (release.type != fw::JsonValue::Type::Object || flag(release, "draft")) continue;
    if (!beta && flag(release, "prerelease")) continue;
    auto version = fw::ParseSemVer(release.StringOr("tag_name"));
    if (!version) continue;
    if (!best || fw::CompareSemVer(*version, *bestVersion) > 0) {
      best = &release;
      bestVersion = version;
    }
  }
  return best;
}

bool Allowed(const policy::Values& policy, bool inPackage) { return !policy.updatesOff && !inPackage; }

bool IsScriptEngine(const std::wstring& pdfiumPath) {
  HANDLE file = CreateFileW(pdfiumPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                            0, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER size{};
  GetFileSizeEx(file, &size);
  HANDLE mapping = size.QuadPart > 0 ? CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr) : nullptr;
  CloseHandle(file);
  if (!mapping) return false;
  const BYTE* base = static_cast<const BYTE*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
  CloseHandle(mapping);
  if (!base) return false;
  const uint64_t n = static_cast<uint64_t>(size.QuadPart);
  bool found = false;
  // The export table of a 64-bit PE file (x64 or ARM64), read as data: nothing is loaded or run.
  auto in = [&](uint64_t offset, uint64_t length) { return offset <= n && length <= n - offset; };
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  if (in(0, sizeof(*dos)) && dos->e_magic == IMAGE_DOS_SIGNATURE && in(dos->e_lfanew, sizeof(IMAGE_NT_HEADERS64))) {
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature == IMAGE_NT_SIGNATURE && nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC &&
        nt->OptionalHeader.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_EXPORT) {
      const IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(nt);
      const WORD count = nt->FileHeader.NumberOfSections;
      auto offsetOf = [&](DWORD rva, uint64_t& out) {
        if (!in(reinterpret_cast<const BYTE*>(sections) - base, uint64_t(count) * sizeof(IMAGE_SECTION_HEADER)))
          return false;
        for (WORD i = 0; i < count; ++i) {
          const IMAGE_SECTION_HEADER& s = sections[i];
          DWORD span = std::max(s.Misc.VirtualSize, s.SizeOfRawData);
          if (rva >= s.VirtualAddress && rva - s.VirtualAddress < span) {
            out = uint64_t(s.PointerToRawData) + (rva - s.VirtualAddress);
            return true;
          }
        }
        return false;
      };
      const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
      uint64_t exportsAt = 0, namesAt = 0;
      if (dir.VirtualAddress && offsetOf(dir.VirtualAddress, exportsAt) && in(exportsAt, sizeof(IMAGE_EXPORT_DIRECTORY))) {
        const auto* exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + exportsAt);
        if (offsetOf(exports->AddressOfNames, namesAt) && in(namesAt, uint64_t(exports->NumberOfNames) * 4)) {
          const DWORD* names = reinterpret_cast<const DWORD*>(base + namesAt);
          static const char kWanted[] = "FPDF_LoadXFA";
          for (DWORD i = 0; i < exports->NumberOfNames && !found; ++i) {
            uint64_t at = 0;
            if (offsetOf(names[i], at) && in(at, sizeof(kWanted)))
              found = memcmp(base + at, kWanted, sizeof(kWanted)) == 0;
          }
        }
      }
    }
  }
  UnmapViewOfFile(base);
  return found;
}

}  // namespace updater

namespace {

// One release asset downloaded to `fresh` and checked: its SHA-256 against SHA256SUMS.txt, then the trust rule
// (signed by the running exe's publisher, for `product`). Returns "" when it passed; else why not, and `fresh` is
// gone.
std::wstring FetchChecked(const std::wstring& url, const std::string& sha256, const std::wstring& fresh,
                          const wchar_t* product, const updater::Signature& running, updater::Signature* signature) {
  std::string data;
  if (Fetch(url, data, 256u << 20) != 200 || util::Sha256(data.data(), data.size()) != sha256)
    return L"The download didn’t pass verification.";
  if (!WriteBytes(fresh, data)) {
    DeleteFileW(fresh.c_str());
    return L"Couldn’t write the update.";
  }
  updater::Signature sig = updater::ReadSignature(fresh);
  updater::Verdict verdict = updater::CheckFile(running, sig, product);
  if (verdict != updater::Verdict::Accepted) {
    DeleteFileW(fresh.c_str());
    return std::wstring(L"The update was refused: ") + updater::VerdictText(verdict) + L".";
  }
  if (signature) *signature = sig;
  return {};
}

}  // namespace

Updater::Updater() : shared_(std::make_shared<Shared>()) {}

Updater::~Updater() { WaitForSwap(); }

void Updater::Start(HWND notify, const Options& options) {
  {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    if (shared_->busy) return;
    shared_->busy = true;
    shared_->result = Result{Status::Checking};
  }
  // Detached: if Featherweight closes during a download, the download is simply abandoned.
  std::thread([s = shared_, notify, options] { Run(s, notify, options); }).detach();
}

bool Updater::Busy() const {
  std::lock_guard<std::mutex> lock(shared_->mutex);
  return shared_->busy;
}

Updater::Result Updater::Get() {
  std::lock_guard<std::mutex> lock(shared_->mutex);
  return shared_->result;
}

void Updater::WaitForSwap() {
  for (int i = 0; i < 500; ++i) {
    {
      std::lock_guard<std::mutex> lock(shared_->mutex);
      if (!shared_->swapping) return;
    }
    Sleep(10);
  }
}

void Updater::SetReleaseUrlForTesting(const std::wstring& url) { g_testUrl = url; }
void Updater::SetInstallDirForTesting(const std::wstring& dir) { g_testInstallDir = dir; }
void Updater::SetRunningSignatureForTesting(const updater::Signature* signature) { g_testRunning = signature; }

Updater::Result Updater::RunForTesting(const Options& options) {
  auto shared = std::make_shared<Shared>();
  shared->busy = true;
  Run(shared, nullptr, options);
  return shared->result;
}

bool Updater::CanSelfUpdate() { return !InPackage() && util::DirectoryWritable(InstallDir()); }

bool Updater::InPackage() {
  UINT32 length = 0;
  return GetCurrentPackageFullName(&length, nullptr) != APPMODEL_ERROR_NO_PACKAGE;
}

void Updater::CleanupOldFiles() {
  const std::wstring dir = InstallDir();
  for (const wchar_t* f : kFiles) {
    DeleteFileW((dir + L"\\" + f + L".old").c_str());
    DeleteFileW((dir + L"\\" + f + L".new").c_str());
  }
}

std::wstring Updater::PreviousVersion() {
  const std::wstring prev = InstallDir() + L"\\" + kPrevious;
  bool any = false;
  for (const wchar_t* f : kFiles) any = any || util::FileExists(prev + L"\\" + f);
  std::string version;
  if (!any || !util::ReadFileBytes(prev + L"\\version.txt", version, 64)) return {};
  return fw::Utf8ToWide(fw::Trim(version));
}

bool Updater::RollBack(std::wstring* error) {
  const std::wstring dir = InstallDir(), prev = dir + L"\\" + kPrevious;
  if (PreviousVersion().empty()) {
    if (error) *error = L"There’s no earlier version to go back to.";
    return false;
  }
  // The running files can be renamed but not replaced: each goes to .old (removed at the next start).
  std::vector<std::wstring> done;
  for (const wchar_t* f : kFiles) {
    const std::wstring from = prev + L"\\" + f, target = dir + L"\\" + f, old = target + L".old";
    if (!util::FileExists(from)) continue;
    DeleteFileW(old.c_str());
    bool moved = !util::FileExists(target) || MoveFileExW(target.c_str(), old.c_str(), 0);
    if (!moved || !MoveFileExW(from.c_str(), target.c_str(), 0)) {
      if (moved && util::FileExists(old) && !util::FileExists(target)) MoveFileExW(old.c_str(), target.c_str(), 0);
      for (const std::wstring& g : done) {  // undo the files already swapped
        const std::wstring t = dir + L"\\" + g;
        MoveFileExW(t.c_str(), (prev + L"\\" + g).c_str(), 0);
        MoveFileExW((t + L".old").c_str(), t.c_str(), 0);
      }
      if (error) *error = L"The earlier version couldn’t be put back.";
      return false;
    }
    done.push_back(f);
  }
  DeleteFileW((prev + L"\\version.txt").c_str());
  RemoveDirectoryW(prev.c_str());
  return true;
}

void Updater::Run(std::shared_ptr<Shared> shared, HWND notify, Options options) {
  Result r;
  auto finish = [&](Result res) {
    {
      std::lock_guard<std::mutex> lock(shared->mutex);
      shared->result = res;
      shared->busy = false;
      shared->swapping = false;
    }
    if (notify) PostMessageW(notify, kDoneMessage, 0, 0);
  };

  std::wstring url = options.beta ? kReleasesUrl : kLatestUrl;
  if (!g_testUrl.empty())
    url = IsFileUrl(g_testUrl) ? g_testUrl + (options.beta ? L"\\releases.json" : L"\\latest.json") : g_testUrl;
  std::string body;
  int status = Fetch(url, body, 4u << 20);
  if (status == 404) return finish({Status::UpToDate});  // no releases yet
  if (status != 200) return finish({Status::Failed, L"", L"", L"Couldn’t reach GitHub."});
  auto json = fw::ParseJson(body);
  if (!json) return finish({Status::Failed, L"", L"", L"Unexpected response from GitHub."});
  const fw::JsonValue* release = updater::PickRelease(*json, options.beta);
  if (!release) return finish({Status::UpToDate});

  std::string tag = release->StringOr("tag_name");
  auto latest = fw::ParseSemVer(tag);
  auto current = fw::ParseSemVer(FW_VERSION_STRING);
  if (!latest || !current) return finish({Status::Failed, L"", L"", L"Unexpected version number."});
  r.version = fw::Utf8ToWide(tag.size() && (tag[0] == 'v' || tag[0] == 'V') ? tag.substr(1) : tag);
  r.pageUrl = fw::Utf8ToWide(release->StringOr("html_url", "https://github.com/dreamtheater484/featherweight/releases"));
  if (fw::CompareSemVer(*latest, *current) <= 0) {
    r.status = Status::UpToDate;
    return finish(r);
  }
  if (!options.install || !CanSelfUpdate() || r.version == options.skippedVersion) {
    r.status = Status::Available;
    return finish(r);
  }

  // Collect asset URLs.
  std::map<std::wstring, std::wstring> assets;
  if (const fw::JsonValue* list = release->Find("assets"); list && list->type == fw::JsonValue::Type::Array) {
    for (const auto& a : list->array)
      assets[fw::ToLower(fw::Utf8ToWide(a.StringOr("name")))] = fw::Utf8ToWide(a.StringOr("browser_download_url"));
  }
  auto sumsUrl = assets.find(L"sha256sums.txt");
  if (sumsUrl == assets.end()) {
    r.status = Status::Available;  // can't verify: let the user download manually
    return finish(r);
  }
  std::string sumsText;
  if (Fetch(sumsUrl->second, sumsText, 1u << 20) != 200) return finish({Status::Failed, r.version, r.pageUrl, L"Download failed."});
  auto sums = ParseSums(sumsText);

  const std::wstring dir = InstallDir();
  const updater::Signature& running = g_testRunning ? *g_testRunning : updater::RunningSignature();
  const bool scripts = updater::IsScriptEngine(dir + L"\\pdfium.dll");  // an install with V8 keeps it
  struct Wanted {
    const wchar_t* file;     // in the install
    std::wstring asset;      // in the release
    const wchar_t* product;  // the signed file's ProductName
  };
  std::vector<Wanted> wanted = {
      {L"Featherweight.exe", std::wstring(L"Featherweight") + kArch + L".exe", L"Featherweight"},
      {L"pdfium.dll", std::wstring(scripts ? L"pdfium-v8" : L"pdfium-standard") + kArch + L".dll", L"pdfium"}};
  if (scripts && !*kArch && !assets.count(wanted[1].asset)) wanted[1].asset = L"pdfium.dll";  // releases up to 4.0

  struct Planned {
    std::wstring name, target, fresh;
  };
  std::vector<Planned> plan;
  auto fail = [&](const std::wstring& why) {
    for (auto& p : plan) DeleteFileW(p.fresh.c_str());
    return finish({Status::Failed, r.version, r.pageUrl, why});
  };
  for (const Wanted& w : wanted) {
    auto sum = sums.find(fw::ToLower(w.asset));
    auto assetUrl = assets.find(fw::ToLower(w.asset));
    if (sum == sums.end() || assetUrl == assets.end()) continue;
    std::wstring target = dir + L"\\" + w.file;
    if (util::Sha256File(target) == sum->second) continue;  // unchanged
    // The trust rule: once this copy is signed, the same publisher must have signed every file, for this
    // product, and the program must be the release's version (no older signed copy passed off as new).
    Planned p{w.file, target, target + L".new"};
    updater::Signature sig;
    if (std::wstring why = FetchChecked(assetUrl->second, sum->second, p.fresh, w.product, running, &sig); !why.empty())
      return fail(why);
    plan.push_back(p);
    if (running.state == updater::Signature::State::Trusted && w.product == std::wstring(L"Featherweight")) {
      auto fileVersion = fw::ParseSemVer(fw::WideToUtf8(sig.fileVersion));
      if (!fileVersion || fw::CompareSemVer(*fileVersion, *latest) != 0)
        return fail(L"The update was refused: the program isn’t the release’s version.");
    }
  }
  if (plan.empty()) {
    r.status = Status::Installed;  // nothing differs from this release
    return finish(r);
  }

  // Swap: running files can be renamed but not overwritten. Each replaced file goes to previous\, which keeps
  // one rollback; the last update's files there are removed first. This part is short and must not be
  // interrupted.
  {
    std::lock_guard<std::mutex> lock(shared->mutex);
    shared->swapping = true;
  }
  const std::wstring prev = dir + L"\\" + kPrevious;
  for (const wchar_t* f : kFiles) DeleteFileW((prev + L"\\" + f).c_str());
  DeleteFileW((prev + L"\\version.txt").c_str());
  bool keep = CreateDirectoryW(prev.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
  std::vector<Planned> done;
  bool ok = true;
  for (auto& p : plan) {
    const std::wstring kept = keep ? prev + L"\\" + p.name : p.target + L".old";
    if (!keep) DeleteFileW(kept.c_str());
    bool moved = !util::FileExists(p.target) || MoveFileExW(p.target.c_str(), kept.c_str(), 0);
    if (!moved || !MoveFileExW(p.fresh.c_str(), p.target.c_str(), 0)) {
      if (moved && util::FileExists(kept) && !util::FileExists(p.target)) MoveFileExW(kept.c_str(), p.target.c_str(), 0);
      ok = false;
      break;
    }
    done.push_back(p);
  }
  if (!ok) {
    for (auto& p : done) {
      DeleteFileW(p.target.c_str());
      MoveFileExW((keep ? prev + L"\\" + p.name : p.target + L".old").c_str(), p.target.c_str(), 0);
    }
    for (auto& p : plan) DeleteFileW(p.fresh.c_str());
    return finish({Status::Failed, r.version, r.pageUrl, L"The update couldn’t be installed."});
  }
  if (keep) util::WriteFileAtomic(prev + L"\\version.txt", FW_VERSION_STRING);
  r.status = Status::Installed;
  finish(r);
}

void Updater::StartScriptEngine(HWND notify) {
  {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    if (shared_->busy) return;
    shared_->busy = true;
    shared_->result = Result{Status::Checking};
    shared_->result.component = true;
  }
  std::thread([s = shared_, notify] { RunScriptEngine(s, notify); }).detach();
}

Updater::Result Updater::ScriptEngineForTesting() {
  auto shared = std::make_shared<Shared>();
  shared->busy = true;
  RunScriptEngine(shared, nullptr);
  return shared->result;
}

bool Updater::ScriptEngineInstalled() { return updater::IsScriptEngine(InstallDir() + L"\\pdfium.dll"); }

// The form script engine (f1-ship): this version's PDFium with V8 and XFA, from this version's own release, checked
// like an update, in place of pdfium.dll from the next start. Only after the user asks for it.
void Updater::RunScriptEngine(std::shared_ptr<Shared> shared, HWND notify) {
  Result r;
  r.component = true;
  r.version = FW_VERSION_WSTRING;
  auto finish = [&](Status status, const std::wstring& error = {}) {
    r.status = status;
    r.error = error;
    {
      std::lock_guard<std::mutex> lock(shared->mutex);
      shared->result = r;
      shared->busy = false;
      shared->swapping = false;
    }
    if (notify) PostMessageW(notify, kDoneMessage, 0, 0);
  };
  const std::wstring dir = InstallDir(), target = dir + L"\\pdfium.dll", fresh = target + L".new";
  if (updater::IsScriptEngine(target)) return finish(Status::Installed);
  if (!CanSelfUpdate()) return finish(Status::Failed, L"Featherweight can’t change its own folder.");

  std::wstring url = std::wstring(kTagUrl) + L"v" + FW_VERSION_WSTRING;
  if (!g_testUrl.empty()) url = IsFileUrl(g_testUrl) ? g_testUrl + L"\\tag.json" : g_testUrl;
  std::string body;
  int status = Fetch(url, body, 4u << 20);
  if (status != 200) return finish(Status::Failed, L"Couldn’t reach GitHub.");
  auto json = fw::ParseJson(body);
  if (!json || json->type != fw::JsonValue::Type::Object) return finish(Status::Failed, L"Unexpected response from GitHub.");
  r.pageUrl = fw::Utf8ToWide(json->StringOr("html_url", "https://github.com/dreamtheater484/featherweight/releases"));
  auto tagVersion = fw::ParseSemVer(json->StringOr("tag_name"));
  auto current = fw::ParseSemVer(FW_VERSION_STRING);
  if (!tagVersion || !current || fw::CompareSemVer(*tagVersion, *current) != 0)
    return finish(Status::Failed, L"This version’s release wasn’t found.");
  std::map<std::wstring, std::wstring> assets;
  if (const fw::JsonValue* list = json->Find("assets"); list && list->type == fw::JsonValue::Type::Array) {
    for (const auto& a : list->array)
      assets[fw::ToLower(fw::Utf8ToWide(a.StringOr("name")))] = fw::Utf8ToWide(a.StringOr("browser_download_url"));
  }
  const std::wstring asset = fw::ToLower(std::wstring(L"pdfium-v8") + kArch + L".dll");
  auto sumsUrl = assets.find(L"sha256sums.txt");
  auto assetUrl = assets.find(asset);
  if (sumsUrl == assets.end() || assetUrl == assets.end())
    return finish(Status::Failed, L"This version’s release has no script engine.");
  std::string sumsText;
  if (Fetch(sumsUrl->second, sumsText, 1u << 20) != 200) return finish(Status::Failed, L"Download failed.");
  auto sums = ParseSums(sumsText);
  auto sum = sums.find(asset);
  if (sum == sums.end()) return finish(Status::Failed, L"The download didn’t pass verification.");

  const updater::Signature& running = g_testRunning ? *g_testRunning : updater::RunningSignature();
  if (std::wstring why = FetchChecked(assetUrl->second, sum->second, fresh, L"pdfium", running, nullptr); !why.empty())
    return finish(Status::Failed, why);
  if (!updater::IsScriptEngine(fresh)) {
    DeleteFileW(fresh.c_str());
    return finish(Status::Failed, L"The download isn’t the script engine.");
  }
  // pdfium.dll may be loaded: it's renamed, not replaced, and removed at the next start.
  {
    std::lock_guard<std::mutex> lock(shared->mutex);
    shared->swapping = true;
  }
  const std::wstring old = target + L".old";
  DeleteFileW(old.c_str());
  bool moved = MoveFileExW(target.c_str(), old.c_str(), 0) != FALSE;
  if (!moved || !MoveFileExW(fresh.c_str(), target.c_str(), 0)) {
    if (moved && !util::FileExists(target)) MoveFileExW(old.c_str(), target.c_str(), 0);
    DeleteFileW(fresh.c_str());
    return finish(Status::Failed, L"The script engine couldn’t be installed.");
  }
  finish(Status::Installed);
}