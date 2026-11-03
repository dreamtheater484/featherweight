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

// What a side panel shows: formatting, page setup, comments, styles or the navigation pane. A tab
// owns the content of its panels and opens them through ViewHost::ViewShowPanel. The content is
// either a set of controls, which the panel lays out, draws and routes input to, or something the
// content draws and handles itself.
class PanelContent {
 public:
  virtual ~PanelContent() = default;
  virtual std::wstring Title() const = 0;
  // Places the content in `body`, the panel below its title, and returns its controls in Tab
  // order. They're drawn after Draw, and their open lists stay inside the panel.
  virtual std::vector<Control*> Layout(const D2D1_RECT_F& body, float ui) = 0;
  // Everything that isn't a control: headings and labels, or the whole content.
  virtual void Draw(gfx::Canvas& c) = 0;

  // Content without controls handles the pointer itself, and the keyboard when it takes focus.
  // Coordinates are client pixels; the mouse and wheel functions return true when something changed.
  virtual bool TakesFocus() const { return false; }
  virtual void SetFocus(bool /*has*/, bool /*byKeyboard*/) {}
  virtual bool OnMouseDown(float /*x*/, float /*y*/, bool /*dbl*/) { return false; }
  virtual bool OnMouseMove(float /*x*/, float /*y*/) { return false; }
  virtual void OnMouseUp(float /*x*/, float /*y*/) {}
  virtual void OnMouseLeave() {}
  virtual bool OnWheel(int /*delta*/, float /*x*/, float /*y*/) { return false; }
  virtual bool OnKeyDown(UINT /*vk*/, bool /*ctrl*/, bool /*shift*/) { return false; }
  virtual std::wstring TooltipAt(float /*x*/, float /*y*/) { return {}; }
  // UI Automation: the content's own elements under `parent` (the panel's pane); the controls
  // describe themselves. `takeFocus` gives the content the keyboard, for its focusable elements.
  virtual void Describe(uia::Tree& /*t*/, int /*parent*/, const std::function<void()>& /*takeFocus*/) {}
};

// The panel at the right of the document area. It slides in, and the document area narrows with
// it. One content shows at a time; Esc and the close button close it.
class SidePanel {
 public:
  static constexpr float kWidth = 320;  // DIPs

  SidePanel();
  // The window, for repainting, the caret and dialogs opened by the panel's controls.
  void SetWindow(HWND hwnd) { group_.hwnd = hwnd; }

  // Shows `content` in place of whatever was shown; null closes the panel. `animate`: slide.
  void Show(PanelContent* content, bool animate = true);
  PanelContent* Content() const { return content_; }
  bool IsOpen() const { return content_ != nullptr; }
  bool Visible() const { return content_ || slide_.V() > 0.001f; }
  // The width it takes from the document area now, in pixels.
  int Width(float ui) const;

  // `area`: the document area with the panel open; the panel takes its right edge.
  void Layout(const RECT& area, float ui);
  void Draw(gfx::Canvas& c, bool windowActive);
  // An open list of one of the panel's controls.
  void DrawPopup(gfx::Canvas& c) { group_.DrawPopup(c); }
  bool Hit(int x, int y) const;
  D2D1_RECT_F Rect() const { return rc_; }

  bool OnMouseMove(int x, int y);
  void OnMouseLeave();
  void OnMouseDown(int x, int y, bool dbl, bool shift);
  // Returns true when the close button was clicked.
  bool OnMouseUp(int x, int y);
  bool OnWheel(int delta, int x, int y);
  std::wstring TooltipAt(int x, int y) const;
  bool OverText(int x, int y) const;

  // The keyboard is in the panel: on one of its controls, or on the content itself.
  bool HasKeyFocus() const { return group_.Focused() != nullptr || contentFocus_; }
  // Moves the keyboard into the panel: its first control (`forward`) or its last, or with
  // `resume` the one that had it last.
  bool FocusIn(bool forward, bool resume);
  void Blur();
  // Keys while the panel has the keyboard. Tab and Shift+Tab move between its controls. Returns
  // false for keys it didn't use, such as an Esc that closed no list.
  bool OnKeyDown(UINT vk, bool ctrl, bool shift, bool alt);
  bool OnChar(wchar_t ch);
  bool Dragging() const { return group_.Dragging() || pressed_; }

  // UI Automation: a pane named after the content, with its close button and its content.
  // `close` closes it.
  void Describe(uia::Tree& t, int parent, const std::function<void()>& close);

 private:
  D2D1_RECT_F Body() const;

  PanelContent* content_ = nullptr;
  PanelContent* shown_ = nullptr;  // still drawn while the panel slides out
  Anim slide_;
  RECT area_{};
  float ui_ = 1;
  D2D1_RECT_F rc_{};
  D2D1_RECT_F titleRc_{};
  Button close_;
  ButtonSet set_;
  ControlGroup group_;  // the content's controls (the content owns them)
  bool contentFocus_ = false;
  Control* last_ = nullptr;  // the control that had the keyboard last
  bool pressed_ = false;  // the content has the pointer
};
