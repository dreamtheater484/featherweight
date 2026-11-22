#pragma once

#include <functional>
#include <string>

#include "ui/Controls.h"

// A side panel's button: a push button, or with `toggle` one that shows `checked`. Shared by the
// workbook's panels (S2) and the presentation's (P2).
class PanelButton : public Control {
 public:
  std::wstring text;
  bool toggle = false, checked = false;
  float textSize = 13;  // DIPs
  std::function<void()> onInvoke;

  void Draw(gfx::Canvas& c) override;
  bool OnMouseMove(float x, float y) override;
  void OnMouseLeave() override { hover_ = false; }
  void OnMouseDown(float x, float y, bool dbl, bool shift) override;
  void OnMouseUp(float x, float y) override;
  bool OnKeyDown(UINT vk, bool ctrl, bool shift, bool alt) override;
  void Describe(uia::Tree& t, int parent) override;

 private:
  void Invoke();
  bool hover_ = false, pressed_ = false;
};
