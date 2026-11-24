#pragma once

#include <windows.h>

#include <d2d1_1.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// A video or audio of the slide show (P3, p3-present; docs/p3-plan.md), played by Media Foundation's
// media engine. Media Foundation loads (mfplat.dll) only when media first plays. A video decodes on a
// Direct3D 11 device of its own, made then too; each new frame is copied into a texture of the shared
// device, which the show draws like any bitmap, so the shared device and its Direct2D stay as they are.
// Nothing is ever fetched: the source is always bytes, the deck's embedded file or a local file.
namespace fw::slide::show {

class MediaPlayer {
 public:
  struct Source {
    std::vector<uint8_t> bytes;  // the embedded file; or
    std::wstring path;           // a local file (linked media), checked by the caller
    std::wstring extension;      // ".mp4": how Media Foundation knows the format
    bool video = true;
  };
  struct Settings {
    float volume = 0.5f;  // 0..1
    bool mute = false;
    bool loop = false;
  };

  // Null when Media Foundation isn't there or the source can't be read.
  static std::unique_ptr<MediaPlayer> Open(Source source, const Settings& settings);
  ~MediaPlayer();
  MediaPlayer(const MediaPlayer&) = delete;
  MediaPlayer& operator=(const MediaPlayer&) = delete;

  void Play(double fromMs);
  void TogglePause();
  void Pause();
  void Resume();
  void Stop();
  // Playing: started, not paused, not ended (a video then needs frames).
  bool Playing() const;
  bool IsVideo() const;
  // A video's newest frame into a bitmap of the shared device, before the show's BeginDraw. Null
  // before its first frame (the poster shows). `changed`: a new frame since the last call.
  ID2D1Bitmap1* Frame(ID2D1DeviceContext* dc, bool* changed);
  // A video's frames as the show took them (--show-media-report): `frames` taken; `skipped`, frames it
  // never took (its time ran on more than one frame between two takes); `frameMs`, the smallest step.
  struct Stats {
    uint64_t frames = 0, skipped = 0;
    double frameMs = 0, coveredMs = 0;
    int error = 0;         // the engine's MF_MEDIA_ENGINE_ERR, 0 none
    long extended = 0;     // its HRESULT
    bool paused = false, ended = false;
  };
  Stats VideoStats() const;

  struct Impl;

 private:
  explicit MediaPlayer(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

// A linked media's target as a local file, or empty: file:/// URLs and paths relative to the deck's
// folder only; other schemes, network shares and network drives are refused (nothing is fetched).
std::wstring LocalMediaPath(const std::string& target, const std::wstring& deckPath);

// Extensions the show plays (".mp4"); playlists and anything else are refused.
bool PlayableMediaExtension(const std::wstring& extension);

// What Insert › Video or Audio stores with a file: a video's first frame as a PNG (its poster) and its
// size, and the length. False when Media Foundation can't read it. Loads Media Foundation.
struct MediaInfo {
  std::vector<uint8_t> posterPng;  // a video's
  UINT width = 0, height = 0;      // a video's, pixels
  double durationMs = 0;
};
bool ReadMediaInfo(const std::vector<uint8_t>& bytes, const std::wstring& extension, bool video, MediaInfo* out);

// An audio's picture: a speaker, `size` pixels square, as a PNG.
std::vector<uint8_t> AudioIconPng(UINT size);

}  // namespace fw::slide::show
