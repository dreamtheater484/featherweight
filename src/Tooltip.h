#pragma once

#include <windows.h>

#include <string>

#include "Gfx.h"

// A small themed tooltip window. It never takes focus.
class Tooltip {
 public:
  bool Create(HWND owner);
  void Destroy();
  // Shows `text` below (or above) the screen rectangle `anchor`.
  void Show(const std::wstring& text, const RECT& anchorScreen, float ui);
  void Hide();
  bool Visible() const { return visible_; }
  const std::wstring& Text() const { return text_; }

 private:
  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
  void Paint();

  HWND hwnd_ = nullptr;
  HWND owner_ = nullptr;
  ComPtr<ID2D1HwndRenderTarget> target_;
  std::wstring text_;
  bool visible_ = false;
  float ui_ = 1;
};
