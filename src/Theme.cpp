#include "Theme.h"

#include <dwmapi.h>

using gfx::Alpha;
using gfx::Rgb;

namespace theme {
namespace {

ThemeSetting g_setting = ThemeSetting::System;
Palette g_palette;
bool g_animations = true;

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

bool SystemUsesLightTheme() {
  DWORD value = 1, size = sizeof(value);
  if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                   L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS) {
    return true;
  }
  return value != 0;
}

// Windows stores the accent palette as 8 RGBA entries: light3, light2, light1, base, dark1, dark2, dark3, extra.
bool ReadAccentPalette(COLORREF out[8]) {
  BYTE data[32];
  DWORD size = sizeof(data);
  if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent",
                   L"AccentPalette", RRF_RT_REG_BINARY, nullptr, data, &size) != ERROR_SUCCESS ||
      size < 32) {
    return false;
  }
  for (int i = 0; i < 8; ++i) out[i] = RGB(data[i * 4], data[i * 4 + 1], data[i * 4 + 2]);
  return true;
}

D2D1_COLOR_F FromColorRef(COLORREF c, float a = 1.f) {
  return D2D1::ColorF(GetRValue(c) / 255.f, GetGValue(c) / 255.f, GetBValue(c) / 255.f, a);
}

D2D1_COLOR_F Sys(int index) { return FromColorRef(GetSysColor(index)); }

void BuildPalette(bool dark, bool highContrast) {
  Palette p;
  p.dark = dark;
  p.highContrast = highContrast;

  COLORREF accent[8];
  bool haveAccent = ReadAccentPalette(accent);
  D2D1_COLOR_F accentLight = haveAccent ? FromColorRef(accent[1]) : Rgb(0x60CDFF);  // light2
  D2D1_COLOR_F accentDark = haveAccent ? FromColorRef(accent[4]) : Rgb(0x005FB8);   // dark1

  if (!dark) {
    p.titleBar = Rgb(0xEDEEF0);
    p.chrome = Rgb(0xFAFAFA);
    p.chromeBorder = Rgb(0xE3E3E5);
    p.tabHover = Rgb(0xE3E4E7);
    p.docBg = Rgb(0xE6E7EA);
    p.text = Rgb(0x1B1B1B);
    p.textSecondary = Rgb(0x5E5E5E);
    p.textDisabled = Rgb(0xA3A3A3);
    p.hover = Rgb(0x000000, 0.055f);
    p.pressed = Rgb(0x000000, 0.09f);
    p.accent = accentDark;
    p.accentHover = gfx::Mix(accentDark, Rgb(0xFFFFFF), 0.10f);
    p.accentPressed = gfx::Mix(accentDark, Rgb(0xFFFFFF), 0.20f);
    p.onAccent = Rgb(0xFFFFFF);
    p.accentText = accentDark;
    p.inputBg = Rgb(0xFFFFFF);
    p.inputBorder = Rgb(0x000000, 0.10f);
    p.inputBorderBottom = Rgb(0x000000, 0.42f);
    p.popupBg = Rgb(0xFCFCFC);
    p.popupBorder = Rgb(0x000000, 0.08f);
    p.popupShadow = Rgb(0x000000, 0.20f);
    p.pageShadow = Rgb(0x000000, 0.22f);
    p.scrollThumb = Rgb(0x000000, 0.34f);
    p.scrollThumbHover = Rgb(0x000000, 0.50f);
    p.scrollTrack = Rgb(0xFFFFFF, 0.55f);
    p.info = accentDark;
    p.success = Rgb(0x0F7B0F);
    p.warning = Rgb(0x9D5D00);
    p.error = Rgb(0xC42B1C);
  } else {
    p.titleBar = Rgb(0x1C1C1C);
    p.chrome = Rgb(0x2B2B2B);
    p.chromeBorder = Rgb(0x1A1A1A);
    p.tabHover = Rgb(0x262626);
    p.docBg = Rgb(0x1F1F1F);
    p.text = Rgb(0xF2F2F2);
    p.textSecondary = Rgb(0xB4B4B4);
    p.textDisabled = Rgb(0x6E6E6E);
    p.hover = Rgb(0xFFFFFF, 0.065f);
    p.pressed = Rgb(0xFFFFFF, 0.04f);
    p.accent = accentLight;
    p.accentHover = gfx::Mix(accentLight, Rgb(0x000000), 0.10f);
    p.accentPressed = gfx::Mix(accentLight, Rgb(0x000000), 0.20f);
    p.onAccent = Rgb(0x000000);
    p.accentText = accentLight;
    p.inputBg = Rgb(0xFFFFFF, 0.06f);
    p.inputBorder = Rgb(0xFFFFFF, 0.08f);
    p.inputBorderBottom = Rgb(0xFFFFFF, 0.55f);
    p.popupBg = Rgb(0x2C2C2C);
    p.popupBorder = Rgb(0x000000, 0.35f);
    p.popupShadow = Rgb(0x000000, 0.45f);
    p.pageShadow = Rgb(0x000000, 0.55f);
    p.scrollThumb = Rgb(0xFFFFFF, 0.38f);
    p.scrollThumbHover = Rgb(0xFFFFFF, 0.55f);
    p.scrollTrack = Rgb(0x000000, 0.35f);
    p.info = accentLight;
    p.success = Rgb(0x6CCB5F);
    p.warning = Rgb(0xFCE100);
    p.error = Rgb(0xFF99A4);
  }
  p.docBgInverted = Rgb(0x161616);
  p.paper = Rgb(0xFFFFFF);
  p.paperInverted = Rgb(0x242424);
  p.closeHover = Rgb(0xC42B1C);
  p.closePressed = Rgb(0xC42B1C, 0.85f);
  p.focusRing = dark ? Rgb(0xFFFFFF) : Rgb(0x000000, 0.9f);
  p.selection = Alpha(p.accent, 0.32f);
  p.searchHit = Rgb(0xFFC83D, 0.42f);
  p.searchCurrent = Rgb(0xFF8C00, 0.62f);
  p.dropOverlay = Alpha(p.accent, 0.10f);

  if (highContrast) {
    D2D1_COLOR_F window = Sys(COLOR_WINDOW), text = Sys(COLOR_WINDOWTEXT);
    D2D1_COLOR_F btn = Sys(COLOR_BTNFACE), btnText = Sys(COLOR_BTNTEXT);
    D2D1_COLOR_F hl = Sys(COLOR_HIGHLIGHT), hlText = Sys(COLOR_HIGHLIGHTTEXT);
    p.titleBar = p.chrome = p.tabHover = btn;
    p.chromeBorder = btnText;
    p.docBg = p.docBgInverted = window;
    p.text = p.textSecondary = btnText;
    p.textDisabled = Sys(COLOR_GRAYTEXT);
    p.hover = Alpha(hl, 0.35f);
    p.pressed = Alpha(hl, 0.55f);
    p.accent = p.accentHover = p.accentPressed = hl;
    p.onAccent = hlText;
    p.accentText = btnText;
    p.inputBg = window;
    p.inputBorder = p.inputBorderBottom = text;
    p.popupBg = window;
    p.popupBorder = text;
    p.popupShadow = Alpha(text, 0);
    p.pageShadow = Alpha(text, 0);
    p.focusRing = text;
    p.selection = Alpha(hl, 0.45f);
  }
  g_palette = p;
}

// Undocumented uxtheme entry points used by Explorer and Notepad for dark popup menus.
enum class PreferredAppMode { Default, AllowDark, ForceDark, ForceLight, Max };
using SetPreferredAppModeFn = PreferredAppMode(WINAPI*)(PreferredAppMode);
using FlushMenuThemesFn = void(WINAPI*)();
using AllowDarkModeForWindowFn = bool(WINAPI*)(HWND, bool);

HMODULE UxTheme() {
  static HMODULE h = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  return h;
}

}  // namespace

void Init(ThemeSetting setting) {
  g_setting = setting;
  Refresh();
}

void SetSetting(ThemeSetting setting) {
  g_setting = setting;
  Refresh();
}

ThemeSetting Setting() { return g_setting; }

bool Refresh() {
  HIGHCONTRASTW hc{sizeof(hc)};
  SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0);
  bool highContrast = (hc.dwFlags & HCF_HIGHCONTRASTON) != 0;
  bool dark = g_setting == ThemeSetting::Dark || (g_setting == ThemeSetting::System && !SystemUsesLightTheme());
  if (highContrast) {
    COLORREF bg = GetSysColor(COLOR_WINDOW);
    dark = (GetRValue(bg) + GetGValue(bg) + GetBValue(bg)) < 384;
  }
  BOOL anim = TRUE;
  SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &anim, 0);
  g_animations = anim != FALSE;

  Palette before = g_palette;
  BuildPalette(dark, highContrast);
  return memcmp(&before, &g_palette, sizeof(Palette)) != 0;
}

const Palette& P() { return g_palette; }
bool IsDark() { return g_palette.dark; }
bool AnimationsEnabled() { return g_animations; }

void ApplyToWindow(HWND hwnd) {
  BOOL dark = g_palette.dark ? TRUE : FALSE;
  DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
  if (HMODULE ux = UxTheme()) {
    if (auto allow = reinterpret_cast<AllowDarkModeForWindowFn>(GetProcAddress(ux, MAKEINTRESOURCEA(133)))) {
      allow(hwnd, g_palette.dark);
    }
  }
  ApplyToMenus();
}

void ApplyToMenus() {
  HMODULE ux = UxTheme();
  if (!ux) return;
  auto setMode = reinterpret_cast<SetPreferredAppModeFn>(GetProcAddress(ux, MAKEINTRESOURCEA(135)));
  auto flush = reinterpret_cast<FlushMenuThemesFn>(GetProcAddress(ux, MAKEINTRESOURCEA(136)));
  if (setMode) setMode(g_palette.dark ? PreferredAppMode::ForceDark : PreferredAppMode::ForceLight);
  if (flush) flush();
}

}  // namespace theme
