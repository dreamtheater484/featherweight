#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

#include "ui/Anim.h"
#include "ui/Controls.h"
#include "ui/Gfx.h"
#include "ui/Uia.h"
#include "ui/Widgets.h"

// A command command search can find: the window's own commands and the active tab's.
struct CommandEntry {
  int cmd = 0;
  std::wstring label;     // "Rotate clockwise"
  std::wstring shortcut;  // "Ctrl+Shift+="
  bool enabled = true;
  bool checked = false;
};

// Command search (Alt+Q): a box over the toolbar that finds commands by the words of their names
// ("page num" finds Insert page numbers), with the results in a list below it. Up and Down move
// through the results, Enter runs one, Esc closes. It is drawn inside the window, above
// everything else. The window gives the box the keyboard as it does its other text boxes.
class CommandSearch {
 public:
  CommandSearch();
  void SetWindow(HWND hwnd) { hwnd_ = hwnd; }

  void Open(std::vector<CommandEntry> commands);
  void Close();
  bool IsOpen() const { return open_; }
  bool Visible() const { return open_ || show_.V() > 0.01f; }
  // Over the toolbar `bar`, with the results below it inside `client`.
  void Layout(const RECT& bar, const RECT& client, float ui);
  void Draw(gfx::Canvas& c, bool caretVisible);
  bool Hit(int x, int y) const;
  bool HitBox(int x, int y) const;

  bool OnMouseMove(int x, int y);
  void OnMouseLeave();
  void OnMouseDown(int x, int y, bool shift, bool dbl);
  void OnMouseDrag(int x, int y);
  void OnMouseUp(int x, int y);
  bool OnWheel(int delta, int x, int y);

  TextBox& Box() { return box_; }
  // The results for what is typed, best first.
  const std::vector<CommandEntry>& Results() const { return results_; }
  int Highlight() const { return hot_; }

  // A result was chosen. The search has closed when it's called.
  std::function<void(int cmd)> onRun;
  // Esc: the window closes the search and gives the keyboard back.
  std::function<void()> onEscape;

  // UI Automation: an edit for the box and a list of the results. `focusBox` gives the box the keyboard.
  void Describe(uia::Tree& t, int parent, const std::function<void()>& focusBox);

  // Whether `label` matches `query`, and how well (lower is better), or -1. Every word of the
  // query must start a word of the label.
  static int Match(const std::wstring& label, const std::wstring& query);

 private:
  void Filter();
  void SetHot(int i, bool byKeyboard);
  void Run(int i);
  int Rows() const;
  int RowAt(int x, int y) const;
  D2D1_RECT_F RowRect(int i) const;
  float MaxScroll() const;
  void EnsureVisible(int i);
  D2D1_RECT_F Card() const { return card_; }
  void Invalidate() const;

  HWND hwnd_ = nullptr;
  bool open_ = false;
  Anim show_;
  float ui_ = 1;
  D2D1_RECT_F card_{}, icon_{};
  D2D1_RECT_F bounds_{};
  TextBox box_;
  Popup popup_;
  std::vector<CommandEntry> all_, results_;
  std::wstring query_;
  int hot_ = -1;
  int pressed_ = -1;
  bool inList_ = false;  // the keyboard moved into the results (screen readers follow the highlight)
  float scroll_ = 0;
};
