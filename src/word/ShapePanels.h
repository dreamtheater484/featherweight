#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "doc/edit/Shapes.h"
#include "doc/edit/Watermark.h"
#include "ui/Controls.h"
#include "ui/SidePanel.h"

// Labeled rows of controls under headings, as the Picture panel lays them out.
class RowsPanel : public PanelContent {
 public:
  std::vector<Control*> Layout(const D2D1_RECT_F& body, float ui) override;
  void Draw(gfx::Canvas& c) override;

 protected:
  bool shown_ = false;  // else `why_` is shown
  std::wstring why_;
  std::vector<std::pair<std::wstring, Control*>> rows_;  // label, control; nullptr starts a heading
  float ui_ = 1;
  D2D1_RECT_F body_{};
  std::vector<std::pair<std::wstring, D2D1_RECT_F>> headings_;
};

// The Shape panel of the Word tab (plan §6.2, milestone D4): how text wraps around the selected
// shape or text box, its size and rotation, its fill and outline. Each change is one undo step of
// the tab's editor, made through `Actions`.
class ShapePanel : public RowsPanel {
 public:
  struct Actions {
    std::function<void(fw::doc::edit::Wrap)> wrap;
    std::function<void(double widthPt, double heightPt)> size;
    std::function<void(double degrees)> rotate;
    std::function<void(std::optional<uint32_t> rgb)> fill;
    std::function<void(std::optional<uint32_t> rgb, double points)> outline;
    std::function<void()> addText;
  };
  explicit ShapePanel(Actions actions);

  std::wstring Title() const override { return L"Shape"; }
  std::vector<Control*> Layout(const D2D1_RECT_F& body, float ui) override;

  void Show(const std::optional<fw::doc::edit::ShapeInfo>& info, const std::wstring& why = {});

 private:
  double ToUnit(int64_t emu) const;
  double FromUnit(double v) const;  // points

  Actions actions_;
  bool metric_ = true;
  bool line_ = false, text_ = false;
  std::optional<uint32_t> outlineColor_;
  Dropdown wrap_;
  SpinBox width_, height_, rotation_, weight_;
  ColorPicker fill_, outline_;
  SplitButton addText_;
};

// The Watermark panel (plan §6.2 "Text watermark", milestone D4): text or a picture behind the text
// of every page, or none. Each change is one undo step.
class WatermarkPanel : public RowsPanel {
 public:
  struct Actions {
    std::function<void(const fw::doc::edit::TextWatermark&)> text;
    std::function<void(bool washout)> picture;  // asks for the file
    std::function<void()> remove;
  };
  explicit WatermarkPanel(Actions actions);

  std::wstring Title() const override { return L"Watermark"; }
  std::vector<Control*> Layout(const D2D1_RECT_F& body, float ui) override;

  void Show(const fw::doc::edit::WatermarkInfo& info, bool editable);

 private:
  fw::doc::edit::TextWatermark Mark() const;
  void ApplyText();

  Actions actions_;
  fw::doc::edit::WatermarkInfo::Kind kind_ = fw::doc::edit::WatermarkInfo::Kind::None;
  Dropdown kindBox_, layout_, look_, washout_;
  ComboBox text_, font_;
  ColorPicker color_;
  SplitButton picture_, remove_;
  bool washOut_ = true;
};
