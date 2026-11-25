#pragma once

// The PDF tab's Pages panel (F1, f1-pdf): every page as a thumbnail, two to a row, to select,
// reorder by dragging, rotate, delete, insert from another PDF, extract and split. The actions are
// the icons along its top; the keys work while it has the keyboard (Delete, Ctrl+A, arrows,
// Ctrl+Z). The page tools themselves are PdfView's (PdfPages.cpp).

#include <vector>

#include "pdf/PdfView.h"
#include "ui/SidePanel.h"

class PdfView::PagesPanel : public PanelContent {
 public:
  explicit PagesPanel(PdfView& view) : v_(view) {}

  std::wstring Title() const override;
  std::vector<Control*> Layout(const D2D1_RECT_F& body, float ui) override;
  void Draw(gfx::Canvas& c) override;
  bool TakesFocus() const override { return true; }
  void SetFocus(bool has, bool byKeyboard) override;
  bool OnMouseDown(float x, float y, bool dbl) override;
  bool OnMouseMove(float x, float y) override;
  void OnMouseUp(float x, float y) override;
  void OnMouseLeave() override;
  bool OnWheel(int delta, float x, float y) override;
  bool OnKeyDown(UINT vk, bool ctrl, bool shift) override;
  std::wstring TooltipAt(float x, float y) override;
  void Describe(uia::Tree& t, int parent, const std::function<void()>& takeFocus) override;

  // The selected pages, in page order.
  std::vector<int> Selection() const;
  void Select(const std::vector<int>& pages, int focus);
  // The page count changed: the selection stays inside it.
  void Clamp();
  void ScrollIntoView(int page);

 private:
  enum Action { kRotateLeft, kRotateRight, kDelete, kInsert, kExtract, kSplit, kUndo, kRedo, kActionCount };
  struct Cell {
    D2D1_RECT_F box{};    // where the thumbnail fits
    D2D1_RECT_F label{};  // the page number under it
  };
  int Count() const;
  int Columns() const { return 2; }
  float CellW() const;
  float CellH() const;
  D2D1_RECT_F Grid() const;  // the thumbnails' area
  Cell CellOf(int page) const;
  int PageAt(float x, float y) const;
  // The gap a drop at (x, y) goes into: 0..Count().
  int GapAt(float x, float y) const;
  bool ActionEnabled(int a) const;
  int ActionAt(float x, float y) const;
  void RunAction(int a);
  void ClampScroll();
  void Click(int page, bool ctrl, bool shift);

  PdfView& v_;
  float ui_ = 1;
  D2D1_RECT_F body_{};
  D2D1_RECT_F actions_[kActionCount]{};
  std::vector<bool> selected_;
  int anchor_ = -1, focus_ = -1;
  float scroll_ = 0;
  int pendingScroll_ = -1;
  int hover_ = -1, hoverAction_ = -1, pressedAction_ = -1;
  bool focused_ = false;
  // A press on a thumbnail: a click, or the start of a drag that moves the selected pages.
  bool pressed_ = false, dragging_ = false;
  int pressPage_ = -1, gap_ = -1;
  float pressX_ = 0, pressY_ = 0, dragY_ = 0;
  bool deferSelect_ = false;  // a press on a selected page selects it alone on release, unless it drags
};
