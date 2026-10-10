#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "Settings.h"
#include "Widgets.h"

// The start screen: logo, a big "Open PDF" button, a drop hint and recent files.
class EmptyState {
 public:
  struct Result {
    enum class Kind { None, Open, OpenRecent, RemoveRecent } kind = Kind::None;
    int index = -1;
  };

  EmptyState();
  void Layout(const RECT& area, float ui, const std::vector<RecentFile>& recent);
  void Draw(gfx::Canvas& c, const std::vector<RecentFile>& recent);
  bool OnMouseMove(int x, int y);
  void OnMouseLeave();
  void OnMouseDown(int x, int y);
  Result OnMouseUp(int x, int y);
  bool OverClickable(int x, int y) const;
  std::wstring TooltipAt(int x, int y, const std::vector<RecentFile>& recent) const;

 private:
  int RowAt(int x, int y) const;
  bool OverRemove(int row, int x, int y) const;

  RECT area_{};
  float ui_ = 1;
  Button open_;
  ButtonSet set_;
  D2D1_RECT_F logo_{}, title_{}, hint_{}, listHeader_{};
  std::vector<D2D1_RECT_F> rows_;
  std::vector<Anim> rowHover_;
  int hoverRow_ = -1;
  bool hoverRemove_ = false;
  int pressedRow_ = -1;
  bool pressedRemove_ = false;
};
