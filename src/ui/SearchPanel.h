#pragma once

#include <windows.h>

#include <functional>
#include <string>

#include "Commands.h"
#include "ui/Uia.h"
#include "ui/Widgets.h"

// Floating find panel in the top-right corner of the document area.
class SearchPanel {
 public:
  SearchPanel();
  void Open();
  void Close();
  bool IsOpen() const { return open_; }
  bool Visible() const { return open_ || show_.V() > 0.01f; }
  void Layout(const RECT& docArea, float ui);
  void Draw(gfx::Canvas& c, const std::wstring& status, bool caretVisible, bool noMatches);
  bool Hit(int x, int y) const;

  bool OnMouseMove(int x, int y);
  void OnMouseLeave();
  void OnMouseDown(int x, int y, bool shift, bool dbl);
  void OnMouseDrag(int x, int y);
  int OnMouseUp(int x, int y);
  std::wstring TooltipAt(int x, int y) const;

  // UI Automation: the panel and its parts, under `parent`. `run` runs a button's command.
  void Describe(uia::Tree& t, int parent, const std::wstring& status, const std::function<void(int)>& run,
                const std::function<void()>& focusBox);

  TextBox& Box() { return box_; }
  bool MatchCase() const { return case_.checked; }
  void ToggleMatchCase() { case_.checked = !case_.checked; }

 private:
  D2D1_RECT_F Card() const;

  bool open_ = false;
  Anim show_;
  RECT area_{};
  float ui_ = 1;
  TextBox box_;
  Button prev_, next_, case_, close_;
  ButtonSet set_;
  D2D1_RECT_F status_{};
};
