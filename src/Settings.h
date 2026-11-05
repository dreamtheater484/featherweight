#pragma once

#include <windows.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/FileKinds.h"
#include "core/Ini.h"
#include "core/Layout.h"
#include "ui/Theme.h"

// Any kind of file: what it is follows from its extension.
struct RecentFile {
  std::wstring path;
  int64_t lastOpened = 0;
  fw::FileKind Kind() const { return fw::KindOfPath(path); }
};

struct FileState {
  int page = 0;
  fw::ZoomMode zoomMode = fw::ZoomMode::Automatic;
  double zoom = 1.0;
  fw::ViewMode viewMode = fw::ViewMode::Continuous;
  int rotation = 0;
  int64_t lastUsed = 0;
};

// Remembers everything automatically; there is no settings screen.
class Settings {
 public:
  static constexpr int kMaxRecent = 10;
  static constexpr int kMaxStates = 200;

  void Load();
  // Reads one settings file, which Save then writes back.
  void LoadFrom(const std::wstring& path);
  bool Save();
  const std::wstring& FilePath() const { return path_; }
  bool IsInstalled() const { return installed_; }

  ThemeSetting theme = ThemeSetting::System;
  bool invert = false;
  bool coverAlone = false;  // two-page view: first page on its own, like a book cover
  bool highlightFields = true;
  bool spelling = true;     // Word documents: check spelling as you type
  bool autoCorrect = true;  // Word documents: AutoCorrect as you type
  bool autoUpdate = true;
  int64_t lastUpdateCheck = 0;
  std::wstring skippedVersion;
  std::wstring lastRunVersion;
  // The one-time offer to become the default app, per kind of file (indexed by fw::FileKind).
  std::array<bool, fw::kFileKindCount> offeredDefaultApp{};

  bool hasWindowRect = false;
  RECT windowRect{};
  bool maximized = false;

  std::vector<RecentFile> recent;
  void AddRecent(const std::wstring& path);
  void RemoveRecent(const std::wstring& path);
  void ClearRecent();

  std::optional<FileState> GetState(const std::wstring& path) const;
  void SetState(const std::wstring& path, const FileState& state);

 private:
  static std::wstring StateKey(const std::wstring& path);

  std::wstring path_;
  bool installed_ = false;
  fw::Ini ini_;
};
