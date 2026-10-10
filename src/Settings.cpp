#include "Settings.h"

#include <algorithm>

#include "Util.h"
#include "core/Strings.h"

namespace {

constexpr wchar_t kGeneral[] = L"General";
constexpr wchar_t kRecent[] = L"Recent";
constexpr wchar_t kState[] = L"State";

// The installer records its folder here; a copy running from that folder is "installed".
std::wstring InstalledDir() {
  wchar_t buf[MAX_PATH * 2];
  DWORD size = sizeof(buf);
  if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Featherweight", L"InstallDir", RRF_RT_REG_SZ, nullptr, buf, &size) !=
      ERROR_SUCCESS) {
    return {};
  }
  return buf;
}

}  // namespace

void Settings::Load() {
  const std::wstring exeDir = util::ExeDir();
  std::wstring installDir = InstalledDir();
  while (!installDir.empty() && (installDir.back() == L'\\' || installDir.back() == L'/')) installDir.pop_back();
  installed_ = !installDir.empty() && fw::EqualsNoCase(installDir, exeDir);

  std::wstring dir;
  if (!installed_ && util::DirectoryWritable(exeDir)) {
    dir = exeDir;  // portable: keep settings next to the exe
  } else {
    dir = util::AppDataDir();
  }
  path_ = dir + L"\\Featherweight.ini";

  std::string data;
  if (util::ReadFileBytes(path_, data, 4u << 20)) ini_.Parse(data);

  std::wstring t = ini_.Get(kGeneral, L"Theme", L"system");
  theme = t == L"light" ? ThemeSetting::Light : t == L"dark" ? ThemeSetting::Dark : ThemeSetting::System;
  invert = ini_.GetBool(kGeneral, L"InvertPages", false);
  coverAlone = ini_.GetBool(kGeneral, L"CoverPageAlone", true);
  highlightFields = ini_.GetBool(kGeneral, L"HighlightFields", true);
  autoUpdate = ini_.GetBool(kGeneral, L"UpdateAutomatically", true);
  lastUpdateCheck = ini_.GetInt64(kGeneral, L"LastUpdateCheck", 0);
  skippedVersion = ini_.Get(kGeneral, L"SkippedVersion");
  lastRunVersion = ini_.Get(kGeneral, L"LastRunVersion");
  offeredDefaultApp = ini_.GetBool(kGeneral, L"OfferedDefaultApp", false);

  auto parts = fw::Split(ini_.Get(kGeneral, L"Window"), L',');
  if (parts.size() == 4) {
    int v[4];
    bool ok = true;
    for (int i = 0; i < 4; ++i) {
      std::wstring s = fw::Trim(parts[i]);
      bool neg = !s.empty() && s[0] == L'-';
      ok = ok && fw::ParseInt(neg ? s.substr(1) : s, v[i]);
      if (neg) v[i] = -v[i];
    }
    if (ok && v[2] > v[0] && v[3] > v[1]) {
      windowRect = {v[0], v[1], v[2], v[3]};
      hasWindowRect = true;
    }
  }
  maximized = ini_.GetBool(kGeneral, L"Maximized", false);

  recent.clear();
  for (const auto& [key, value] : ini_.Entries(kRecent)) {
    auto fields = fw::Split(value, L'|');
    if (fields.empty() || fields[0].empty()) continue;
    RecentFile r;
    r.path = fields[0];
    if (fields.size() > 1) r.lastOpened = _wtoi64(fields[1].c_str());
    recent.push_back(r);
  }
  if (recent.size() > kMaxRecent) recent.resize(kMaxRecent);
}

bool Settings::Save() {
  ini_.Set(kGeneral, L"Theme",
           theme == ThemeSetting::Light ? L"light" : theme == ThemeSetting::Dark ? L"dark" : L"system");
  ini_.SetBool(kGeneral, L"InvertPages", invert);
  ini_.SetBool(kGeneral, L"CoverPageAlone", coverAlone);
  ini_.SetBool(kGeneral, L"HighlightFields", highlightFields);
  ini_.SetBool(kGeneral, L"UpdateAutomatically", autoUpdate);
  ini_.SetInt(kGeneral, L"LastUpdateCheck", lastUpdateCheck);
  ini_.Set(kGeneral, L"SkippedVersion", skippedVersion);
  ini_.Set(kGeneral, L"LastRunVersion", lastRunVersion);
  ini_.SetBool(kGeneral, L"OfferedDefaultApp", offeredDefaultApp);
  if (hasWindowRect) {
    ini_.Set(kGeneral, L"Window",
             std::to_wstring(windowRect.left) + L"," + std::to_wstring(windowRect.top) + L"," +
                 std::to_wstring(windowRect.right) + L"," + std::to_wstring(windowRect.bottom));
  }
  ini_.SetBool(kGeneral, L"Maximized", maximized);

  std::vector<fw::Ini::Entry> entries;
  for (size_t i = 0; i < recent.size(); ++i)
    entries.emplace_back(std::to_wstring(i + 1), recent[i].path + L"|" + std::to_wstring(recent[i].lastOpened));
  ini_.ReplaceSection(kRecent, entries);

  // Keep only the most recently used file states.
  auto states = ini_.Entries(kState);
  if (states.size() > kMaxStates) {
    std::sort(states.begin(), states.end(), [](const fw::Ini::Entry& a, const fw::Ini::Entry& b) {
      auto ta = fw::Split(a.second, L',');
      auto tb = fw::Split(b.second, L',');
      return _wtoi64(ta.back().c_str()) > _wtoi64(tb.back().c_str());
    });
    states.resize(kMaxStates);
    ini_.ReplaceSection(kState, states);
  }

  std::string text = "; Featherweight settings. Safe to delete.\r\n" + ini_.Serialize();
  return util::WriteFileAtomic(path_, text);
}

void Settings::AddRecent(const std::wstring& path) {
  RemoveRecent(path);
  recent.insert(recent.begin(), RecentFile{path, util::UnixNow()});
  if (recent.size() > kMaxRecent) recent.resize(kMaxRecent);
}

void Settings::RemoveRecent(const std::wstring& path) {
  recent.erase(std::remove_if(recent.begin(), recent.end(),
                              [&](const RecentFile& r) { return fw::EqualsNoCase(r.path, path); }),
               recent.end());
}

void Settings::ClearRecent() { recent.clear(); }

std::wstring Settings::StateKey(const std::wstring& path) { return fw::Hex64(fw::Fnv1a64(fw::ToLower(path))); }

std::optional<FileState> Settings::GetState(const std::wstring& path) const {
  std::wstring v = ini_.Get(kState, StateKey(path));
  if (v.empty()) return std::nullopt;
  auto f = fw::Split(v, L',');
  if (f.size() < 6) return std::nullopt;
  FileState s;
  s.page = std::max(0, _wtoi(f[0].c_str()));
  s.zoomMode = static_cast<fw::ZoomMode>(std::clamp(_wtoi(f[1].c_str()), 0, 4));
  s.zoom = fw::ClampZoom(_wtof(f[2].c_str()));
  s.viewMode = static_cast<fw::ViewMode>(std::clamp(_wtoi(f[3].c_str()), 0, 2));
  s.rotation = std::clamp(_wtoi(f[4].c_str()), 0, 3);
  s.lastUsed = _wtoi64(f[5].c_str());
  return s;
}

void Settings::SetState(const std::wstring& path, const FileState& s) {
  wchar_t zoom[32];
  swprintf_s(zoom, L"%.4f", s.zoom);
  ini_.Set(kState, StateKey(path),
           std::to_wstring(s.page) + L"," + std::to_wstring(static_cast<int>(s.zoomMode)) + L"," + zoom + L"," +
               std::to_wstring(static_cast<int>(s.viewMode)) + L"," + std::to_wstring(s.rotation) + L"," +
               std::to_wstring(util::UnixNow()));
}
