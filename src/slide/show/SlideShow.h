#pragma once

#include <windows.h>

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace fw::slide {
class Presentation;
}

// The slide show (plan §8.2, §8.3): full screen on the chosen monitor, the next slide drawn ahead,
// transitions and animations at the display's refresh rate on the shared Direct3D 11 device, the
// keys and clicks PowerPoint has, hyperlinks and actions. One show at a time.
//
// The presentation tab starts it with F5 (CmdSlideShowFromStart) or Shift+F5
// (CmdSlideShowFromCurrent); a .ppsx starts in it. Esc ends it back in the tab.
namespace fw::slide::show {

class SlideRenderer;

struct ShowOptions {
  size_t first = 0;      // the slide to start on (a hidden one too, when it's the current slide)
  HWND owner = nullptr;  // the main window: the show is its owned window
  int monitor = -1;      // into Monitors(); -1: the owner's monitor
  std::wstring deckPath;  // links to files are relative to its folder
  // The show ended on this slide (index), or at the end (the last slide shown).
  std::function<void(size_t)> ended;
  // A link names a file: open it in Featherweight (after the show ends). False when it can't.
  std::function<bool(const std::wstring&)> openFile;
  // Measuring (--show-bench): every slide and step plays by itself, and the frame timings go to this
  // JSON file when the show ends.
  std::wstring benchReport;
  // Checking (--show-capture): no window on screen; every transition and animation step is drawn at
  // its middle and end, on a clock of its own, into PNG files in this folder.
  std::wstring captureDir;
  // P3 (p3-present): the show in a window of its own instead of full screen (Alt+F5: with the presenter
  // view beside it, as on one monitor; and any check that mustn't take the screen).
  bool windowed = false;
  // The presenter view: full screen on another monitor, or in a window of its own when windowed.
  bool presenter = false;
  // Checking video playback (--show-media-report): each video's frames shown and skipped go to this
  // JSON file when the show ends.
  std::wstring mediaReport;
};

// Starts a show of `deck` drawn by `renderer` (both outlive it). False when a show runs already or
// no graphics device can be used.
bool Start(Presentation& deck, SlideRenderer& renderer, const ShowOptions& options);
bool Running();
void Stop();

// The monitors a show can use, as "1: 2560 × 1440 (main)".
std::vector<std::wstring> Monitors();

// `Featherweight.exe --show <deck> [--show-bench <report.json>] [--show-capture <folder>]`: the deck
// straight into a show, without the app's window: trying the show, the frame-rate bench, and the
// pictures that check every transition and animation. Returns the exit code: 0, 1 when the deck
// can't be opened, 2 when the bench dropped frames, 3 when the bench was stopped early (Esc, Alt+F4,
// closing it, or another program taking the focus). Other keys, clicks and the mouse change nothing
// during the bench.
// `--show-windowed`: the show in a window; `--show-presenter`: with the presenter view;
// `--show-media-report <report.json>`: the videos' frames (P3).
int RunStandalone(const std::wstring& path, const std::wstring& benchReport, const std::wstring& captureDir = {},
                  bool windowed = false, bool presenter = false, const std::wstring& mediaReport = {});

}  // namespace fw::slide::show
