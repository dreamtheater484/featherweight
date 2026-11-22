#pragma once

// The presentation tab's design panels (P2, p2-slides; plan §8.2 "Design", "Slide numbers, footer
// and date"): Design (built-in themes and theme colors, the background, the slide size) and Header
// & footer. Each change goes through the edit layer as one undo step, as the panel's control sets it.

#include <dwrite.h>
#include <wrl/client.h>

#include <map>
#include <string>
#include <vector>

#include "slide/view/PresentationView.h"
#include "ui/Controls.h"
#include "ui/PanelButton.h"
#include "ui/SidePanel.h"

namespace fw::slide::view {

class PresentationView::DesignPanel : public PanelContent {
 public:
  enum Tab { kThemes, kBackground, kSize, kTabs };
  explicit DesignPanel(PresentationView& view);
  std::wstring Title() const override { return L"Design"; }
  std::vector<Control*> Layout(const D2D1_RECT_F& body, float ui) override;
  void Draw(gfx::Canvas& c) override;
  bool OnMouseDown(float x, float y, bool dbl) override;
  bool OnMouseMove(float x, float y) override;
  void OnMouseLeave() override;
  bool OnWheel(int delta, float x, float y) override;
  std::wstring TooltipAt(float x, float y) override;
  void Open(Tab tab);
  // The controls from the deck as it is now (another slide, an edit, an undo).
  void Sync();

 private:
  // The themes tab draws its own grid and list: what's under the pointer.
  struct Hit {
    int theme = -1, colors = -1;
  };
  Hit At(float x, float y) const;
  D2D1_RECT_F ThemeRect(int i) const;
  D2D1_RECT_F ColorsRect(int i) const;
  float ContentHeight() const;
  void DrawTheme(gfx::Canvas& c, int i, const D2D1_RECT_F& r);
  IDWriteTextFormat* FontFor(const char* family, float size);
  void ApplyBackground();

  PresentationView& v_;
  Tab tab_ = kThemes;
  PanelButton tabs_[kTabs];
  // Background
  PanelButton kind_[3], picture_, hide_, all_, reset_;
  ColorPicker color1_, color2_;
  Dropdown direction_;
  Background bg_;
  std::wstring picturePath_;
  // Size
  PanelButton sizes_[2];
  Dropdown scale_;
  int sizeChoice_ = 0;

  D2D1_RECT_F body_{}, area_{};
  float ui_ = 1, scroll_ = 0;
  Hit hover_;
  int currentColors_ = -1;
  std::vector<std::pair<std::wstring, D2D1_RECT_F>> labels_;
  std::map<std::pair<std::string, int>, Microsoft::WRL::ComPtr<IDWriteTextFormat>> fonts_;
};

class PresentationView::HeaderFooterPanel : public PanelContent {
 public:
  explicit HeaderFooterPanel(PresentationView& view);
  std::wstring Title() const override { return L"Header & footer"; }
  std::vector<Control*> Layout(const D2D1_RECT_F& body, float ui) override;
  void Draw(gfx::Canvas& c) override;
  // The settings of the current slide.
  void Sync();

 private:
  void Refresh();
  PresentationView& v_;
  HeaderFooterSettings s_;
  PanelButton date_, auto_, fixed_, number_, footer_, notTitle_, apply_, applyAll_;
  Dropdown format_;
  ComboBox fixedText_, footerText_;
  std::vector<std::pair<std::wstring, D2D1_RECT_F>> labels_;
  float ui_ = 1;
};

}  // namespace fw::slide::view
