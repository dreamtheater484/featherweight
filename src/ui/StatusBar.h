#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

#include "ui/Anim.h"
#include "ui/Gfx.h"
#include "ui/Uia.h"
#include "ui/Widgets.h"

// What an editor's status bar shows. The active tab fills it each frame; PDFs have none.
struct StatusBarModel {
  // Left to right, with dots between them: "Page 3 of 12", "1,234 words", "English (UK)".
  std::vector<std::wstring> segments;
  // The zoom control at the right: −, a slider, + and the zoom. 1 is 100 %.
  bool zoom = false;
  double zoomValue = 1;
  std::function<void(double)> onZoom;  // a new zoom from the slider or its buttons
};

// The bar along the bottom of the window, below the document area and the side panel. Its zoom
// slider takes the keyboard (F6): arrows step by 10 %, Home and End go to the ends.
class StatusBar {
 public:
  static constexpr float kHeight = 28;  // DIPs
  // The slider runs from 10 % to 500 %, with 100 % in the middle, as Office's does.
  static constexpr double kMinZoom = 0.1, kMaxZoom = 5;

  StatusBar();
  void SetWindow(HWND hwnd) { hwnd_ = hwnd; }
  void Layout(const RECT& rc, float ui, StatusBarModel m);
  void Draw(gfx::Canvas& c);
  bool Hit(int x, int y) const;

  bool OnMouseMove(int x, int y);
  void OnMouseLeave();
  void OnMouseDown(int x, int y);
  void OnMouseUp(int x, int y);
  bool OnWheel(int delta, int x, int y);
  std::wstring TooltipAt(int x, int y) const;
  bool Dragging() const { return dragging_; }

  // The slider has the keyboard.
  bool HasKeyFocus() const { return focused_; }
  bool CanFocus() const { return m_.zoom; }
  bool FocusIn();  // false when there is nothing to focus
  void Blur();
  // Keys while the slider has the keyboard; false for keys it doesn't use (Esc, Tab).
  bool OnKeyDown(UINT vk, bool ctrl, bool shift);

  // UI Automation: a status bar with the segments as text, the zoom buttons and the slider.
  // `focus` gives the slider the keyboard.
  void Describe(uia::Tree& t, int parent, const std::function<void()>& focus);

  // Where the slider's thumb sits (0 to 1) for a zoom, and back.
  static double PositionOf(double zoom);
  static double ZoomAt(double position);

 private:
  void SetZoom(double zoom);
  // The next zoom in steps of 10 %, up or down.
  double Step(int direction) const;
  void DragTo(float x);
  D2D1_RECT_F Track() const;
  D2D1_RECT_F Thumb() const;
  void Invalidate() const;

  HWND hwnd_ = nullptr;
  RECT rc_{};
  float ui_ = 1;
  StatusBarModel m_;
  std::vector<D2D1_RECT_F> segmentRc_;
  D2D1_RECT_F slider_{};   // the whole slider, thumb included
  D2D1_RECT_F percent_{};
  Button zoomOut_, zoomIn_;
  ButtonSet set_;
  Anim thumbHover_;
  bool dragging_ = false;
  bool focused_ = false;
  bool focusVisible_ = false;
};
