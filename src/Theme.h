#pragma once

#include <windows.h>

#include "Gfx.h"

enum class ThemeSetting { System = 0, Light = 1, Dark = 2 };

struct Palette {
  bool dark = false;
  bool highContrast = false;

  D2D1_COLOR_F titleBar;      // strip behind the tabs
  D2D1_COLOR_F chrome;        // toolbar and active tab
  D2D1_COLOR_F chromeBorder;  // hairline under the toolbar
  D2D1_COLOR_F tabHover;
  D2D1_COLOR_F docBg;         // behind the pages
  D2D1_COLOR_F docBgInverted; // behind the pages in inverted reading mode
  D2D1_COLOR_F paper;
  D2D1_COLOR_F paperInverted;
  D2D1_COLOR_F pageShadow;

  D2D1_COLOR_F text, textSecondary, textDisabled;
  D2D1_COLOR_F hover, pressed;
  D2D1_COLOR_F accent, accentHover, accentPressed, onAccent;
  D2D1_COLOR_F accentText;  // accent color that is readable as text on chrome
  D2D1_COLOR_F focusRing;

  D2D1_COLOR_F inputBg, inputBorder, inputBorderBottom;
  D2D1_COLOR_F popupBg, popupBorder, popupShadow;
  D2D1_COLOR_F closeHover, closePressed;
  D2D1_COLOR_F scrollThumb, scrollThumbHover, scrollTrack;
  D2D1_COLOR_F selection, searchHit, searchCurrent;
  D2D1_COLOR_F info, success, warning, error;
  D2D1_COLOR_F dropOverlay;
};

namespace theme {

void Init(ThemeSetting setting);
void SetSetting(ThemeSetting setting);
ThemeSetting Setting();
// Re-reads system colors, dark mode and accent; returns true if anything changed.
bool Refresh();
const Palette& P();
bool IsDark();
bool AnimationsEnabled();

// Dark title bar border, dark popup menus.
void ApplyToWindow(HWND hwnd);
void ApplyToMenus();

}  // namespace theme
