#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "Widgets.h"

struct TabInfo {
  std::wstring title;
  std::wstring path;
  bool modified = false;
};

// The custom title bar: app mark, tabs, new-tab button and the caption buttons.
class TitleBar {
 public:
  enum class Part { None, Caption, Tab, TabClose, NewTab, Minimize, Maximize, Close };
  struct Hit {
    Part part = Part::None;
    int index = -1;
  };
  struct Action {
    Part part = Part::None;
    int index = -1;
  };

  TitleBar();
  void Layout(const RECT& rc, float ui, int tabCount);
  void Draw(gfx::Canvas& c, const std::vector<TabInfo>& tabs, int active, bool windowActive, bool maximized);
  Hit HitTest(int x, int y) const;
  int Height() const { return rc_.bottom - rc_.top; }

  bool OnMouseMove(int x, int y);
  void OnMouseLeave();
  void OnMouseDown(int x, int y, int button);
  Action OnMouseUp(int x, int y, int button);
  std::wstring TooltipAt(int x, int y, const std::vector<TabInfo>& tabs) const;
  // Rectangle of a caption button in client coordinates (for Snap Layouts hit-testing).
  const D2D1_RECT_F& MaxButtonRect() const { return max_.rc; }

 private:
  D2D1_RECT_F TabRect(int i) const;
  D2D1_RECT_F TabCloseRect(int i) const;

  RECT rc_{};
  float ui_ = 1;
  int tabCount_ = 0;
  float tabsLeft_ = 0, tabW_ = 0;
  Button min_, max_, close_, newTab_;
  ButtonSet buttons_;
  std::vector<Anim> tabHover_;
  int hoverTab_ = -1;
  bool hoverClose_ = false;
  Hit pressed_;
};
