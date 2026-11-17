#pragma once

// The workbook tab's side panels (S2): row height and column width, and Go to (Ctrl+G, F5).
// Each is a WorkbookView member and changes the workbook through the edit API.

#include <string>
#include <vector>

#include "sheet/view/WorkbookView.h"
#include "ui/Controls.h"
#include "ui/SidePanel.h"

namespace fw::sheet::view {

// Boxes a panel can put the keyboard in once it is laid out.
class FocusSpinBox : public SpinBox {
 public:
  using SpinBox::TakeFocus;
};
class FocusComboBox : public ComboBox {
 public:
  using ComboBox::TakeFocus;
};

// The selected rows' height in points, or the columns' width in characters, applied as it changes.
class WorkbookView::SizePanel : public PanelContent {
 public:
  explicit SizePanel(WorkbookView& view);
  std::wstring Title() const override { return rows_ ? L"Row height" : L"Column width"; }
  std::vector<Control*> Layout(const D2D1_RECT_F& body, float ui) override;
  void Draw(gfx::Canvas& c) override;
  void Open(bool rows);

 private:
  WorkbookView& v_;
  bool rows_ = true;
  FocusSpinBox size_;
  D2D1_RECT_F labelRc_{}, noteRc_{};
  float ui_ = 1;
  bool focusWanted_ = false;
};

// A cell, a range or a name to go to; Enter goes there.
class WorkbookView::GoToPanel : public PanelContent {
 public:
  explicit GoToPanel(WorkbookView& view);
  std::wstring Title() const override { return L"Go to"; }
  std::vector<Control*> Layout(const D2D1_RECT_F& body, float ui) override;
  void Draw(gfx::Canvas& c) override;
  void Open();

 private:
  WorkbookView& v_;
  FocusComboBox ref_;
  std::wstring note_;
  D2D1_RECT_F labelRc_{}, noteRc_{};
  float ui_ = 1;
  bool focusWanted_ = false;
};

}  // namespace fw::sheet::view
