#pragma once

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "doc/edit/Pictures.h"
#include "ui/Controls.h"
#include "ui/SidePanel.h"

// The Picture panel of the Word tab (plan §6.2, milestone D3): how text wraps around the selected
// picture, its size and rotation, and its alt text. Each change is one undo step of the tab's
// editor, made through `Actions`.
class PicturePanel : public PanelContent {
 public:
  struct Actions {
    std::function<void(fw::doc::edit::Wrap)> wrap;
    std::function<void(double widthPt, double heightPt)> size;
    std::function<void(double degrees)> rotate;
    std::function<void(const std::wstring&)> altText;
    std::function<void()> crop, reset;
  };
  explicit PicturePanel(Actions actions);

  std::wstring Title() const override { return L"Picture"; }
  std::vector<Control*> Layout(const D2D1_RECT_F& body, float ui) override;
  void Draw(gfx::Canvas& c) override;

  // Shows a picture's settings; nothing selected (or not a picture): `why` says so instead.
  void Show(const std::optional<fw::doc::edit::PictureInfo>& info, const std::wstring& why = {});

 private:
  double ToUnit(int64_t emu) const;
  double FromUnit(double v) const;  // points

  Actions actions_;
  bool shown_ = false;
  std::wstring why_;
  bool metric_ = true;  // centimeters, else inches (the user's measurement system)
  bool lockAspect_ = true;
  double aspect_ = 1;  // height / width
  float ui_ = 1;
  D2D1_RECT_F body_{};
  std::vector<std::pair<std::wstring, D2D1_RECT_F>> headings_;
  std::vector<std::pair<std::wstring, Control*>> rows_;  // label, control; nullptr starts a heading
  Dropdown wrap_;
  SpinBox width_, height_, rotation_;
  ComboBox alt_;
  SplitButton crop_, reset_;
};
