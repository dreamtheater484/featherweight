#pragma once

// Format Cells (Ctrl+1, S2 plan "Format Cells", s2-format): a side panel with the selected cells'
// number format, alignment, font, border and fill. Each change applies at once to every selected
// cell, changing only what that control sets (one undo step each), through the edit API.

#include <functional>
#include <string>
#include <vector>

#include "sheet/numfmt/FormatCells.h"
#include "sheet/view/WorkbookView.h"
#include "ui/Controls.h"
#include "ui/SidePanel.h"

namespace fw::sheet::view {

// A panel's button: a push button, or with `toggle` one that shows `checked`.
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

class WorkbookView::FormatPanel : public PanelContent {
 public:
  explicit FormatPanel(WorkbookView& view);
  std::wstring Title() const override { return L"Format cells"; }
  std::vector<Control*> Layout(const D2D1_RECT_F& body, float ui) override;
  void Draw(gfx::Canvas& c) override;
  enum Page { kNumber, kAlignment, kFont, kBorder, kFill, kProtection, kPages };
  void Open(int page);
  // The controls show the active cell's format (when it or the workbook changed).
  void Sync();

 private:
  void Place();  // positions for the page shown
  void ShowPage(int page);
  void SyncNumber();
  void ApplyNumber();
  std::wstring Sample(const std::wstring& code, double value) const;
  void ApplyBorders(int which);

  WorkbookView& v_;
  int page_ = kNumber;
  D2D1_RECT_F body_{};
  float ui_ = 1;
  std::vector<std::pair<std::wstring, D2D1_RECT_F>> labels_;  // drawn above controls
  std::wstring sample_;
  D2D1_RECT_F sampleRc_{};
  // What the controls were synced to: the sheet, the active cell and the format there.
  size_t syncSheet_ = SIZE_MAX;
  CellAddr syncCell_{};
  uint32_t syncXf_ = UINT32_MAX;
  bool syncMerged_ = false;

  PanelButton tabs_[kPages];
  // Number
  Dropdown category_, negative_;
  SpinBox decimals_;
  PanelButton thousands_;
  ListBox types_;
  ComboBox code_;
  FormatChoice choice_;
  std::vector<std::wstring> typeCodes_;
  // Alignment
  Dropdown horizontal_, vertical_, direction_;
  SpinBox indent_, rotation_;
  PanelButton wrap_, shrink_, merge_;
  // Font
  ComboBox font_, size_;
  Dropdown style_, underline_;
  ColorPicker color_;
  PanelButton strike_, superscript_, subscript_;
  // Border
  Dropdown line_;
  ColorPicker lineColor_;
  PanelButton borders_[10];  // presets, edges, the two diagonals
  // Fill
  ColorPicker fillColor_, patternColor_;
  Dropdown pattern_;
  // Protection (in effect once the sheet is protected)
  PanelButton locked_, hidden_;
};

}  // namespace fw::sheet::view
