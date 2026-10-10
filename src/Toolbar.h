#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "Commands.h"
#include "Widgets.h"
#include "core/Layout.h"

struct ToolbarState {
  bool hasDoc = false;
  bool ready = false;  // document loaded (not waiting for a password)
  bool modified = false;
  bool canPrint = false;
  bool inverted = false;
  bool searchOpen = false;
  bool fullscreen = false;
  int page = 0, pageCount = 0;
  std::wstring zoomText;
  fw::ViewMode viewMode = fw::ViewMode::Continuous;
};

class Toolbar {
 public:
  Toolbar();
  void Layout(const RECT& rc, float ui, const ToolbarState& s);
  void Draw(gfx::Canvas& c, const ToolbarState& s, bool caretVisible);

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
  Button* Make(int cmd, wchar_t glyph, const wchar_t* tooltip);

  RECT rc_{};
  float ui_ = 1;
  std::vector<Button*> all_;
  ButtonSet set_;
  Button open_, save_, saveMenu_, print_, prev_, next_, zoomOut_, zoom_, zoomIn_, view_, rotate_, search_, invert_, full_, more_;
  TextBox pageBox_;
  D2D1_RECT_F pageLabel_{};
  std::vector<D2D1_RECT_F> separators_;
  std::wstring pageLabelText_;
};
