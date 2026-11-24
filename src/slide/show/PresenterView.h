#pragma once

#include <windows.h>

#include <d2d1_1.h>

#include <memory>

// The presenter view (P3, p3-present; docs/p3-plan.md): on the second monitor (or, with Alt+F5, in a
// window), the current slide, the next slide or animation, the notes, a timer and the clock, and a
// strip of slides to jump to. It draws only when something changes, and once a second for the clock.
namespace fw::slide {
class Presentation;
}

namespace fw::slide::show {

class SlideRenderer;

// What the presenter view shows, from the show, whenever the slide or its step changes.
struct PresenterState {
  static constexpr size_t kEnd = static_cast<size_t>(-1);
  size_t slide = kEnd;               // the slide on screen; kEnd: the end of the show
  ID2D1Bitmap1* current = nullptr;   // as the audience sees it now (null: black)
  ID2D1Bitmap1* next = nullptr;      // what the next click shows (null: the end of the show)
  int stepsLeft = 0;                 // clicks left on this slide: `next` is this slide built
  int blank = 0;                     // 1 black screen, 2 white screen (B, W)
};

// The show, as the presenter view drives it.
class PresenterHost {
 public:
  virtual ~PresenterHost() = default;
  virtual void PresenterKey(UINT vk) = 0;        // the show's keys work in both windows
  virtual void PresenterGoTo(size_t slide) = 0;  // a slide clicked in the strip
  virtual void PresenterClosed() = 0;            // its window closed: the show ends
};

class PresenterView {
 public:
  // Full screen on `monitor`, or (windowed) in the right half of its work area, beside the show; with
  // no monitor, no window (--show-capture draws it off screen).
  static std::unique_ptr<PresenterView> Create(PresenterHost& host, Presentation& deck, SlideRenderer& renderer,
                                               HMONITOR monitor, bool windowed, HWND owner);
  ~PresenterView();
  PresenterView(const PresenterView&) = delete;
  PresenterView& operator=(const PresenterView&) = delete;

  void Update(const PresenterState& state);
  HWND Window() const;
  // Draws the view into `dc` (a target of w x h pixels): the window's paint, and checks off screen.
  // Prepare first, outside the target's BeginDraw: it makes the strip's thumbnails.
  void Prepare(float w, float h);
  void Draw(ID2D1DeviceContext* dc, float w, float h);

  struct Impl;

 private:
  explicit PresenterView(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace fw::slide::show
