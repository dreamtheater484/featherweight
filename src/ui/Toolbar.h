#pragma once

#include <windows.h>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "Commands.h"
#include "ui/Controls.h"
#include "ui/Uia.h"
#include "ui/Widgets.h"

// What kind of control a toolbar item is.
enum class ToolKind { Button, Dropdown, ComboBox, SplitButton, ColorPicker, SpinBox };

// What a toolbar control was set to: a chosen item, typed text, a number or a color.
struct ToolValue {
  int index = -1;  // into `choices`, or -1
  std::wstring text;
  double number = 0;
  D2D1_COLOR_F color{};
  bool automatic = false;  // "Automatic" or "No color" was chosen
};

// One item on the toolbar, as a tab describes it.
struct ToolItem {
  ToolKind kind = ToolKind::Button;
  int cmd = 0;
  wchar_t glyph = 0;
  std::wstring tooltip;
  float width = 36;       // DIPs
  float glyphSize = 16;   // DIPs
  bool dropdown = false;  // a small chevron after the icon
  bool toggle = false;    // stays on while `checked` (screen readers hear "on" and "off")
  bool enabled = true;
  bool checked = false;
  bool joinNext = false;   // no gap before the next item (a button and its arrow)
  bool sepBefore = false;  // a thin separator before this item
  int priority = 9;        // right group: lower is dropped first when space is tight

  // The other kinds. Dropdowns, combo boxes and split buttons list `choices`, and `choice` is
  // the chosen one; `text` is what a combo box shows (or a dropdown, with nothing chosen).
  std::vector<std::wstring> choices;
  int choice = -1;
  std::wstring text;
  std::function<bool(std::wstring&)> validate;  // combo boxes: tidies typed text, or refuses it
  // Spin boxes.
  double value = 0, minValue = 0, maxValue = 100, step = 1;
  int decimals = 0;
  std::wstring unit;
  // Color pickers, and split buttons whose arrow opens colors (`colors`).
  bool colors = false;
  D2D1_COLOR_F color{};
  bool automatic = false;
  std::wstring automaticLabel;     // "Automatic" or "No color"
  D2D1_COLOR_F automaticColor{};   // how "Automatic" looks (transparent for "No color")
  // Called with what the control was set to. A split button's main part runs `cmd`.
  std::function<void(const ToolValue&)> onChange;
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
  // The window, for repainting, the caret and dialogs opened by the tab's controls.
  void SetWindow(HWND hwnd) { group_.hwnd = hwnd; }
  void Layout(const RECT& rc, float ui, const ToolbarModel& m);
  void Draw(gfx::Canvas& c, const ToolbarModel& m, bool caretVisible);
  // An open list of one of the tab's controls, drawn above everything else in the window.
  void DrawPopup(gfx::Canvas& c) { group_.DrawPopup(c); }

  bool OnMouseMove(int x, int y);
  void OnMouseLeave();
  bool OnMouseDown(int x, int y, bool dbl = false, bool shift = false);
  int OnMouseUp(int x, int y);
  bool OnWheel(int delta, int x, int y);
  std::wstring TooltipAt(int x, int y) const;
  D2D1_RECT_F ButtonRect(int cmd) const;
  bool HitPopup(int x, int y) const { return group_.HitPopup((float)x, (float)y); }
  bool OverText(int x, int y) const { return group_.OverText((float)x, (float)y); }

  // The keyboard is on one of the tab's controls (after a click, say). Enter and Esc hand it
  // back to the document.
  bool HasKeyFocus() const { return group_.Focused() != nullptr; }
  void OnKeyDown(UINT vk, bool ctrl, bool shift, bool alt);
  void OnChar(wchar_t ch) { group_.OnChar(ch); }
  void Blur() { group_.Blur(); }

  // Runs a command: a split button's main part, or a button UI Automation invoked.
  std::function<void(int cmd)> onCommand;
  // UI Automation: the toolbar's elements under `parent`. `isMenu(cmd)` tells which buttons open
  // a menu; `openMenu` is the command whose menu is open.
  void Describe(uia::Tree& t, int parent, const std::function<bool(int)>& isMenu, int openMenu,
                const std::function<void(TextBox*)>& focusBox);

  TextBox& PageBox() { return pageBox_; }
  bool HitPageBox(int x, int y) const { return pageBox_.rc.right > 0 && gfx::Contains(pageBox_.rc, (float)x, (float)y); }
  int Height() const { return rc_.bottom - rc_.top; }

 private:
  Button* Make(Button& b, int cmd, wchar_t glyph, const wchar_t* tooltip);
  // The button for a tab's item, created the first time it appears (it keeps its hover state).
  Button& ItemButton(const ToolItem& item);
  // The same for the other kinds of item.
  Control& ItemControl(const ToolItem& item);

  RECT rc_{};
  float ui_ = 1;
  std::vector<Button*> all_;
  ButtonSet set_;
  Button open_, prev_, next_, zoomOut_, zoom_, zoomIn_, more_;
  std::map<int, Button> items_;
  std::map<int, bool> toggles_;
  ControlGroup group_;  // before the controls, so it outlives them
  std::map<int, std::unique_ptr<Control>> controls_;
  std::map<int, ToolKind> controlKinds_;
  std::vector<Control*> shown_;
  TextBox pageBox_;
  D2D1_RECT_F pageLabel_{};
  std::vector<D2D1_RECT_F> separators_;
  std::wstring pageLabelText_;
};
