#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

#include "Settings.h"
#include "core/FileKinds.h"
#include "ui/Uia.h"
#include "ui/Widgets.h"

// The start screen (plan §5.1): the logo, Open… with the drop hint, a New row with a tile for each
// editor that has shipped, and the recent files. Tab moves between Open…, the New tiles and the
// list; the arrows move through the files, Enter opens one and Delete takes it off the list.
class EmptyState {
 public:
  struct Result {
    enum class Kind { None, Open, New, OpenRecent, RemoveRecent } kind = Kind::None;
    int index = -1;  // the recent file, or the fw::FileKind to create
  };

  EmptyState();
  void Layout(const RECT& area, float ui, const std::vector<RecentFile>& recent);
  void Draw(gfx::Canvas& c, const std::vector<RecentFile>& recent);
  bool OnMouseMove(int x, int y);
  void OnMouseLeave();
  void OnMouseDown(int x, int y);
  Result OnMouseUp(int x, int y);
  // Keys pressed without Ctrl or Alt. Returns false for keys the start screen doesn't use.
  bool OnKeyDown(UINT vk, bool shift, Result& result);
  bool OverClickable(int x, int y) const;
  std::wstring TooltipAt(int x, int y, const std::vector<RecentFile>& recent) const;
  // UI Automation: Open…, the New tiles and the recent files. `act` gets what a click would return.
  void Describe(uia::Tree& t, int parent, const std::vector<RecentFile>& recent,
                const std::function<void(Result)>& act);

 private:
  enum class Focus { None, Open, New, List };

  int RowAt(int x, int y) const;
  D2D1_RECT_F RemoveRect(int row) const;
  bool OverRemove(int row, int x, int y) const;
  int TileAt(int x, int y) const;
  // The New tiles that are showing, as file kinds.
  std::vector<int> Tiles() const;
  void FocusOn(Focus f, int tile);
  void Select(int row);
  void MoveFocus(bool forward);

  RECT area_{};
  float ui_ = 1;
  Button open_;
  Button tiles_[fw::kFileKindCount];  // by file kind; PDF's never shows
  ButtonSet set_;
  D2D1_RECT_F logo_{}, title_{}, hint_{}, newLabel_{}, listHeader_{};
  bool hintBeside_ = true;  // next to Open…, or below it in a narrow window
  std::vector<D2D1_RECT_F> rows_;
  std::vector<std::wstring> paths_;  // the files the rows show
  std::vector<Anim> rowHover_;
  int hoverRow_ = -1;
  bool hoverRemove_ = false;
  int pressedRow_ = -1;
  bool pressedRemove_ = false;

  // Keyboard focus. The selected file is remembered by path, so it stays put when the list changes.
  Focus focus_ = Focus::None;
  int focusTile_ = -1;  // file kind
  bool focusVisible_ = false;
  int selected_ = -1;
  std::wstring selectedPath_;
};
