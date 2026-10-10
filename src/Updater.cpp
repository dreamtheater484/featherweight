#include "Updater.h"

#include <winhttp.h>

#include <map>
#include <sstream>
#include <vector>

#include "Util.h"
#include "Version.h"
#include "core/Json.h"
#include "core/SemVer.h"
#include "core/Strings.h"

namespace {

constexpr wchar_t kLatestUrl[] = L"https://api.github.com/repos/dreamtheater484/featherweight/releases/latest";
const wchar_t* const kFiles[] = {L"Featherweight.exe", L"pdfium.dll"};

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

}  // namespace

Updater::~Updater() {
  if (thread_.joinable()) thread_.join();
}

void Updater::Start(HWND notify, bool install) {
  if (busy_) return;
  if (thread_.joinable()) thread_.join();
  busy_ = true;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    result_ = Result{Status::Checking};
  }
  thread_ = std::thread([this, notify, install] { Run(notify, install); });
}

Updater::Result Updater::Get() {
  std::lock_guard<std::mutex> lock(mutex_);
  return result_;
}

bool Updater::CanSelfUpdate() { return util::DirectoryWritable(util::ExeDir()); }

void Updater::CleanupOldFiles() {
  const std::wstring dir = util::ExeDir();
  for (const wchar_t* f : kFiles) {
    DeleteFileW((dir + L"\\" + f + L".old").c_str());
    DeleteFileW((dir + L"\\" + f + L".new").c_str());
  }
}

void Updater::Run(HWND notify, bool install) {
  Result r;
  auto finish = [&](Result res) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      result_ = res;
    }
    busy_ = false;
    PostMessageW(notify, kDoneMessage, 0, 0);
  };

  std::string body;
  int status = HttpGet(kLatestUrl, body, 4u << 20);
  if (status == 404) return finish({Status::UpToDate});  // no releases yet
  if (status != 200) return finish({Status::Failed, L"", L"", L"Couldn’t reach GitHub."});
  auto json = fw::ParseJson(body);
  if (!json) return finish({Status::Failed, L"", L"", L"Unexpected response from GitHub."});

  std::string tag = json->StringOr("tag_name");
  auto latest = fw::ParseSemVer(tag);
  auto current = fw::ParseSemVer(FW_VERSION_STRING);
  if (!latest || !current) return finish({Status::Failed, L"", L"", L"Unexpected version number."});
  r.version = fw::Utf8ToWide(tag.size() && (tag[0] == 'v' || tag[0] == 'V') ? tag.substr(1) : tag);
  r.pageUrl = fw::Utf8ToWide(json->StringOr("html_url", "https://github.com/dreamtheater484/featherweight/releases"));
  if (fw::CompareSemVer(*latest, *current) <= 0) {
    r.status = Status::UpToDate;
    return finish(r);
  }
  if (!install || !CanSelfUpdate()) {
    r.status = Status::Available;
    return finish(r);
  }

  // Collect asset URLs.
  std::map<std::wstring, std::wstring> assets;
  if (const fw::JsonValue* list = json->Find("assets"); list && list->type == fw::JsonValue::Type::Array) {
    for (const auto& a : list->array)
      assets[fw::ToLower(fw::Utf8ToWide(a.StringOr("name")))] = fw::Utf8ToWide(a.StringOr("browser_download_url"));
  }
  auto sumsUrl = assets.find(L"sha256sums.txt");
  if (sumsUrl == assets.end()) {
    r.status = Status::Available;  // can't verify: let the user download manually
    return finish(r);
  }
  std::string sumsText;
  if (HttpGet(sumsUrl->second, sumsText, 1u << 20) != 200) return finish({Status::Failed, r.version, r.pageUrl, L"Download failed."});
  auto sums = ParseSums(sumsText);

  const std::wstring dir = util::ExeDir();
  struct Planned {
    std::wstring target, fresh, old;
  };
  std::vector<Planned> plan;
  for (const wchar_t* name : kFiles) {
    std::wstring key = fw::ToLower(name);
    auto sum = sums.find(key);
    auto url = assets.find(key);
    if (sum == sums.end() || url == assets.end()) continue;
    std::wstring target = dir + L"\\" + name;
    if (util::Sha256File(target) == sum->second) continue;  // unchanged
    std::string data;
    if (HttpGet(url->second, data, 256u << 20) != 200 || util::Sha256(data.data(), data.size()) != sum->second) {
      for (auto& p : plan) DeleteFileW(p.fresh.c_str());
      return finish({Status::Failed, r.version, r.pageUrl, L"The download didn’t pass verification."});
    }
    Planned p{target, target + L".new", target + L".old"};
    if (!WriteBytes(p.fresh, data)) {
      for (auto& q : plan) DeleteFileW(q.fresh.c_str());
      DeleteFileW(p.fresh.c_str());
      return finish({Status::Failed, r.version, r.pageUrl, L"Couldn’t write the update."});
    }
    plan.push_back(p);
  }

  // Swap: running files can be renamed but not overwritten.
  std::vector<Planned> done;
  bool ok = true;
  for (auto& p : plan) {
    DeleteFileW(p.old.c_str());
    if (!MoveFileExW(p.target.c_str(), p.old.c_str(), 0) || !MoveFileExW(p.fresh.c_str(), p.target.c_str(), 0)) {
      if (util::FileExists(p.old) && !util::FileExists(p.target)) MoveFileExW(p.old.c_str(), p.target.c_str(), 0);
      ok = false;
      break;
    }
    done.push_back(p);
  }
  if (!ok) {
    for (auto& p : done) {
      DeleteFileW(p.target.c_str());
      MoveFileExW(p.old.c_str(), p.target.c_str(), 0);
    }
    for (auto& p : plan) DeleteFileW(p.fresh.c_str());
    return finish({Status::Failed, r.version, r.pageUrl, L"The update couldn’t be installed."});
  }
  r.status = Status::Installed;
  finish(r);
}
