#pragma once

#include <windows.h>

#include <map>
#include <string>
#include <vector>

#include "Commands.h"
#include "ui/Widgets.h"

// One button on the toolbar, as a tab describes it.
struct ToolItem {
  int cmd = 0;
  wchar_t glyph = 0;
  std::wstring tooltip;
  float width = 36;       // DIPs
  float glyphSize = 16;   // DIPs
  bool dropdown = false;  // a small chevron after the icon
  bool enabled = true;
  bool checked = false;
  bool joinNext = false;   // no gap before the next item (a button and its arrow)
  bool sepBefore = false;  // a thin separator before this item
  int priority = 9;        // right group: lower is dropped first when space is tight
};

// What the toolbar shows. The active tab fills it each frame; the window adds Open and More.
struct ToolbarModel {
  std::vector<ToolItem> left;   // after Open
  std::vector<ToolItem> right;  // before More
  // Previous and next page, the page box and zoom, in the center.
  bool pager = false;
  int page = 0, pageCount = 0;
  std::wstring zoomText;
};

class Toolbar {
 public:
  Toolbar();
  void Layout(const RECT& rc, float ui, const ToolbarModel& m);
  void Draw(gfx::Canvas& c, const ToolbarModel& m, bool caretVisible);

  bool OnMouseMove(int x, int y);
  void OnMouseLeave();
  bool OnMouseDown(int x, int y);
  int OnMouseUp(int x, int y);
  std::wstring TooltipAt(int x, int y) const;
  D2D1_RECT_F ButtonRect(int cmd) const;

  TextBox& PageBox() { return pageBox_; }
  bool HitPageBox(int x, int y) const { return pageBox_.rc.right > 0 && gfx::Contains(pageBox_.rc, (float)x, (float)y); }
  int Height() const { return rc_.bottom - rc_.top; }

 private:
  Button* Make(Button& b, int cmd, wchar_t glyph, const wchar_t* tooltip);
  // The button for a tab's item, created the first time it appears (it keeps its hover state).
  Button& ItemButton(const ToolItem& item);

  RECT rc_{};
  float ui_ = 1;
  std::vector<Button*> all_;
  ButtonSet set_;
  Button open_, prev_, next_, zoomOut_, zoom_, zoomIn_, more_;
  std::map<int, Button> items_;
  TextBox pageBox_;
  D2D1_RECT_F pageLabel_{};
  std::vector<D2D1_RECT_F> separators_;
  std::wstring pageLabelText_;
};
