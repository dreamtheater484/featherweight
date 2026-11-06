#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

#include "ui/Anim.h"
#include "ui/Gfx.h"
#include "ui/Uia.h"
#include "ui/Widgets.h"

// Focusable custom-drawn controls: dropdown, combo box, split button, color picker, spin box and
// list. They live in a ControlGroup, which routes the pointer and the keyboard to them and keeps
// track of hover, focus and the one open list. Open lists are drawn inside the window, above
// everything else, by whoever owns the group.

namespace caret {
// Whether a caret that started blinking at `since` (the last key press or click) is showing.
bool On(double since);
// Snapshots keep the caret on, so the images don't depend on timing.
void SetSteady(bool steady);
}  // namespace caret

class ControlGroup;

class Control {
 public:
  virtual ~Control() = default;

  int id = 0;
  std::wstring label;    // its name for screen readers; the tooltip without its shortcut when empty
  std::wstring tooltip;
  D2D1_RECT_F rc{};
  bool visible = true;
  bool enabled = true;

  bool Focused() const;
  bool Hit(float x, float y) const { return visible && gfx::Contains(rc, x, y); }
  std::wstring Name() const;
  // Controls made of other controls (a list inside a dropdown) pass the group on.
  virtual void SetGroup(ControlGroup* g) { group_ = g; }

  virtual void Draw(gfx::Canvas& c) = 0;
  // The open list or palette.
  virtual void DrawPopup(gfx::Canvas&) {}
  virtual bool IsOpen() const { return false; }
  virtual void SetOpen(bool) {}
  virtual bool HitPopup(float, float) const { return false; }

  // Input in client pixels. OnMouseMove returns true when something changed visually.
  virtual bool OnMouseMove(float x, float y) = 0;
  virtual void OnMouseLeave() = 0;
  virtual void OnMouseDown(float x, float y, bool dbl, bool shift) = 0;
  virtual void OnMouseUp(float x, float y) = 0;
  virtual bool OnWheel(int, float, float) { return false; }
  // Returns true when the key was used. Tab is passed on to the group unless a control keeps it.
  virtual bool OnKeyDown(UINT vk, bool ctrl, bool shift, bool alt) = 0;
  virtual bool OnChar(wchar_t) { return false; }
  virtual void OnFocus(bool) {}
  virtual bool UsesCaret() const { return false; }
  virtual bool OverText(float, float) const { return false; }
  virtual std::wstring TooltipAt(float, float) const { return tooltip; }
  // Adds the control's elements under `parent`.
  virtual void Describe(uia::Tree& t, int parent) = 0;

 protected:
  float Ui() const;
  bool FocusVisible() const;
  bool CaretOn() const;
  void TakeFocus();
  HWND Window() const;
  D2D1_RECT_F PopupBounds() const;
  // Closes every other open list in the group.
  void Opened();
  ControlGroup* group_ = nullptr;
};

class ControlGroup {
 public:
  ControlGroup() = default;
  ControlGroup(const ControlGroup&) = delete;
  ControlGroup& operator=(const ControlGroup&) = delete;
  ~ControlGroup();

  HWND hwnd = nullptr;   // repainting, the caret and dialogs (More colors…)
  float ui = 1;
  D2D1_RECT_F bounds{};  // open lists stay inside this rectangle
  bool wrap = true;      // Tab past the last control comes back to the first; otherwise it leaves

  // Controls are in Tab order.
  void Add(Control* c);
  void Clear();
  // Replaces the controls, keeping hover and focus on those that stay.
  void Assign(const std::vector<Control*>& controls);
  const std::vector<Control*>& Controls() const { return controls_; }

  void Draw(gfx::Canvas& c);
  void DrawPopup(gfx::Canvas& c);
  bool Hit(float x, float y) const;
  bool HitPopup(float x, float y) const;

  bool OnMouseMove(float x, float y);
  void OnMouseLeave();
  // Returns false when the click missed every control; that also closes an open list.
  bool OnMouseDown(float x, float y, bool dbl, bool shift);
  void OnMouseUp(float x, float y);
  bool OnWheel(int delta, float x, float y);
  // Keys for the focused control. Tab and Shift+Tab move between controls.
  bool OnKeyDown(UINT vk, bool ctrl, bool shift, bool alt);
  bool OnChar(wchar_t ch);
  bool Dragging() const { return capture_ != nullptr; }

  Control* Focused() const { return focused_; }
  void Focus(Control* c, bool byKeyboard);
  void Blur();
  bool FocusVisible() const { return focusVisible_; }
  // Moves the keyboard to the next or previous control. False when there is none (or, without
  // `wrap`, when the keyboard is on the last one).
  bool MoveFocus(bool forward);
  Control* Open() const;
  void CloseAll(Control* except = nullptr);
  bool CaretOn() const { return caret::On(caretStart_); }
  void KeepCaret();
  std::wstring TooltipAt(float x, float y) const;
  bool OverText(float x, float y) const;
  void Describe(uia::Tree& t, int parent);
  void Invalidate() const;

 private:
  Control* At(float x, float y) const;
  void CaretTimer(bool on);

  std::vector<Control*> controls_;
  Control* hover_ = nullptr;
  Control* capture_ = nullptr;
  Control* focused_ = nullptr;
  bool focusVisible_ = false;
  double caretStart_ = 0;
};

// Where an open list sits, and its fade.
struct Popup {
  bool open = false;
  Anim show;
  D2D1_RECT_F rc{};
  bool above = false;

  // Below `anchor` when it fits (or when there is more room there), above otherwise.
  void Place(const D2D1_RECT_F& anchor, float w, float h, const D2D1_RECT_F& bounds, float ui);
  void Open();
  void Close();
  bool Visible() const { return open || show.V() > 0.01f; }
  bool Hit(float x, float y) const { return open && gfx::Contains(rc, x, y); }
  // Draws the frame and starts the fade; call End after the content.
  void Begin(gfx::Canvas& c, float ui);
  void End(gfx::Canvas& c);
  D2D1_RECT_F Inner(float ui) const;

 private:
  bool layered_ = false;
  D2D1_MATRIX_3X2_F saved_{};
};

// Drawing shared by the controls.
namespace ctl {
// A light filled box with a hairline border (dropdowns, swatch pickers).
void DrawBox(gfx::Canvas& c, const D2D1_RECT_F& r, float ui, float hover, float press);
// Transparent until hovered (toolbar-style buttons).
void DrawSubtle(gfx::Canvas& c, const D2D1_RECT_F& r, float ui, float hover, float press);
void DrawFocusRing(gfx::Canvas& c, const D2D1_RECT_F& r, float ui);
void DrawChevron(gfx::Canvas& c, const D2D1_RECT_F& r, D2D1_COLOR_F color, float ui, bool up = false);
void DrawSwatch(gfx::Canvas& c, const D2D1_RECT_F& r, D2D1_COLOR_F color, float ui);
bool StartsWithNoCase(const std::wstring& s, const std::wstring& prefix);
}  // namespace ctl

// A scrolling list with one selected item: on its own, or inside a dropdown, combo box or split
// button while it's open.
class ListBox : public Control {
 public:
  ListBox();

  std::vector<std::wstring> items;
  float rowHeight = 32;   // DIPs
  bool inPopup = false;   // the pointer and arrows move a highlight; picking happens on click or Enter
  std::function<void(int)> onSelect;    // the selection changed
  std::function<void(int)> onActivate;  // Enter or double-click (in a popup: a click)

  int Selected() const { return selected_; }
  void SetSelected(int i);
  int Highlight() const { return hot_; }
  void SetHighlight(int i);
  // Moves the highlight (popup) or the selection by `delta` rows, or to the ends (INT_MIN, INT_MAX).
  void Move(int delta);
  float ContentHeight() const;
  float PreferredWidth() const;
  void EnsureVisible(int i);
  int ItemAt(float x, float y) const;
  D2D1_RECT_F ItemRect(int i) const;
  // Finds an item by typed letters, like Explorer. Returns -1 for none.
  int TypeAhead(wchar_t ch);
  void ResetScroll() { scroll_ = 0; }

  void Draw(gfx::Canvas& c) override;
  bool OnMouseMove(float x, float y) override;
  void OnMouseLeave() override;
  void OnMouseDown(float x, float y, bool dbl, bool shift) override;
  void OnMouseUp(float x, float y) override;
  bool OnWheel(int delta, float x, float y) override;
  bool OnKeyDown(UINT vk, bool ctrl, bool shift, bool alt) override;
  bool OnChar(wchar_t ch) override;
  void Describe(uia::Tree& t, int parent) override;
  // The list's elements, for a list that belongs to another control. `focusItems`: the highlight
  // has the keyboard. `pick` runs when a screen reader selects an item.
  int DescribeAsPart(uia::Tree& t, int parent, int64_t id, bool open, bool focusItems,
                     const std::function<void(int)>& pick);

 private:
  int RowsVisible() const;
  float MaxScroll() const;
  D2D1_RECT_F Viewport() const;
  D2D1_RECT_F Thumb() const;
  void ScrollTo(float px);
  int Current() const { return inPopup ? hot_ : selected_; }

  int selected_ = -1, hot_ = -1, hover_ = -1, pressed_ = -1;
  float scroll_ = 0;
  bool dragThumb_ = false;
  float dragFrom_ = 0, dragScroll_ = 0;
  bool overBar_ = false;
  Anim barHover_;
  std::wstring typed_;
  double typedAt_ = 0;
};

// A closed button showing the chosen item; opens a list.
class Dropdown : public Control {
 public:
  Dropdown();

  std::vector<std::wstring> items;
  int selected = -1;
  std::wstring placeholder;  // shown while nothing is chosen
  wchar_t glyph = 0;
  std::function<void(int)> onChange;

  void SetGroup(ControlGroup* g) override;
  void Draw(gfx::Canvas& c) override;
  void DrawPopup(gfx::Canvas& c) override;
  bool IsOpen() const override { return popup_.open; }
  void SetOpen(bool open) override;
  bool HitPopup(float x, float y) const override;
  bool OnMouseMove(float x, float y) override;
  void OnMouseLeave() override;
  void OnMouseDown(float x, float y, bool dbl, bool shift) override;
  void OnMouseUp(float x, float y) override;
  bool OnWheel(int delta, float x, float y) override;
  bool OnKeyDown(UINT vk, bool ctrl, bool shift, bool alt) override;
  bool OnChar(wchar_t ch) override;
  void OnFocus(bool focused) override;
  void Describe(uia::Tree& t, int parent) override;

 private:
  void Pick(int i);
  ListBox list_;
  Popup popup_;
  Anim hover_, press_;
  bool inList_ = false;
};

// Editable text with a list to pick from, like the font name and size boxes.
class ComboBox : public Control {
 public:
  ComboBox();

  std::vector<std::wstring> items;
  bool autoComplete = true;  // typing completes the first item that starts with the text
  float listWidth = 0;       // DIPs; 0 fits the box and the items
  // Called with the text and the item's index (or -1 for typed text) when it's applied.
  std::function<void(const std::wstring&, int)> onCommit;
  // Returns false for text that can't be applied (the box shakes); may tidy the text.
  std::function<bool(std::wstring&)> validate;

  // The applied value (ignored while the box is being edited).
  void SetText(const std::wstring& text);
  const std::wstring& Text() const { return box_.Text(); }

  void SetGroup(ControlGroup* g) override;
  void Draw(gfx::Canvas& c) override;
  void DrawPopup(gfx::Canvas& c) override;
  bool IsOpen() const override { return popup_.open; }
  void SetOpen(bool open) override;
  bool HitPopup(float x, float y) const override;
  bool OnMouseMove(float x, float y) override;
  void OnMouseLeave() override;
  void OnMouseDown(float x, float y, bool dbl, bool shift) override;
  void OnMouseUp(float x, float y) override;
  bool OnWheel(int delta, float x, float y) override;
  bool OnKeyDown(UINT vk, bool ctrl, bool shift, bool alt) override;
  bool OnChar(wchar_t ch) override;
  void OnFocus(bool focused) override;
  bool UsesCaret() const override { return true; }
  bool OverText(float x, float y) const override;
  void Describe(uia::Tree& t, int parent) override;

 private:
  D2D1_RECT_F ArrowRect() const;
  int IndexOf(const std::wstring& text) const;
  // Applies the text (`always`: even when it didn't change); false, and a shake, when
  // `validate` refuses it.
  bool Commit(bool always);
  void Pick(int i);
  void Revert();
  void FollowText();

  TextBox box_;
  ListBox list_;
  Popup popup_;
  Anim arrowHover_, arrowPress_;
  std::wstring committed_, beforeOpen_;
  bool selecting_ = false, inList_ = false, justFocused_ = false;
};

struct NamedColor {
  D2D1_COLOR_F color;
  std::wstring name;
};

// Theme colors with tints and shades, the standard colors, recent colors and More colors…
class ColorPalette : public Control {
 public:
  ColorPalette();

  std::wstring automaticLabel;  // "Automatic" or "No color"; no such row when empty
  D2D1_COLOR_F automaticColor{};
  std::vector<NamedColor> theme;  // ten base colors; their tints and shades are worked out
  D2D1_COLOR_F current{};
  bool currentAutomatic = false;
  std::function<void(D2D1_COLOR_F, bool automatic)> onPick;
  std::function<void()> beforeDialog;  // More colors… is about to open (the owner closes its popup)

  // The default document theme (white, black, two grays and six accents).
  static std::vector<NamedColor> DefaultTheme();
  // Custom colors chosen with More colors…, newest first, shared by every palette.
  static std::vector<NamedColor>& Recent();
  // A readable name for a color: the palette's own name, or its red, green and blue.
  std::wstring NameOf(D2D1_COLOR_F c, bool automatic) const;

  D2D1_SIZE_F PreferredSize() const;
  void Layout();
  void HighlightCurrent();

  void Draw(gfx::Canvas& c) override;
  bool OnMouseMove(float x, float y) override;
  void OnMouseLeave() override;
  void OnMouseDown(float x, float y, bool dbl, bool shift) override;
  void OnMouseUp(float x, float y) override;
  bool OnKeyDown(UINT vk, bool ctrl, bool shift, bool alt) override;
  std::wstring TooltipAt(float x, float y) const override;
  void Describe(uia::Tree& t, int parent) override;
  int DescribeAsPart(uia::Tree& t, int parent, int64_t id, bool focusCells);

 private:
  enum Section { kAutomatic, kTheme, kStandard, kRecent, kMore };
  struct Cell {
    Section section;
    int row, col;  // col -1: the whole row
    D2D1_COLOR_F color;
    std::wstring name;
    D2D1_RECT_F rc;
  };
  int CellAt(float x, float y) const;
  void Pick(int cell);
  void MoreColors();
  bool IsCurrent(const Cell& cell) const;

  std::vector<Cell> cells_;
  std::vector<std::pair<Section, D2D1_RECT_F>> headers_;
  int hot_ = -1, hover_ = -1, pressed_ = -1;
};

// A button that picks a color from a palette: a glyph over a color bar, or a plain swatch.
class ColorPicker : public Control {
 public:
  ColorPicker();

  wchar_t glyph = 0;
  D2D1_COLOR_F color{};
  bool automatic = false;
  std::wstring automaticLabel;
  D2D1_COLOR_F automaticColor{};
  std::vector<NamedColor> theme = ColorPalette::DefaultTheme();
  std::function<void(D2D1_COLOR_F, bool automatic)> onChange;

  void SetGroup(ControlGroup* g) override;
  void Draw(gfx::Canvas& c) override;
  void DrawPopup(gfx::Canvas& c) override;
  bool IsOpen() const override { return popup_.open; }
  void SetOpen(bool open) override;
  bool HitPopup(float x, float y) const override;
  bool OnMouseMove(float x, float y) override;
  void OnMouseLeave() override;
  void OnMouseDown(float x, float y, bool dbl, bool shift) override;
  void OnMouseUp(float x, float y) override;
  bool OnKeyDown(UINT vk, bool ctrl, bool shift, bool alt) override;
  void OnFocus(bool focused) override;
  std::wstring TooltipAt(float x, float y) const override;
  void Describe(uia::Tree& t, int parent) override;

 private:
  void Configure();
  ColorPalette palette_;
  Popup popup_;
  Anim hover_, press_;
  bool inPalette_ = false;
};

// A main action and an arrow that opens a list or a color palette.
class SplitButton : public Control {
 public:
  SplitButton();

  wchar_t glyph = 0;
  std::wstring text;
  bool showColor = false;  // a bar under the glyph in `color`
  D2D1_COLOR_F color{};
  // The arrow opens `items`, or a color palette when `colors` is set.
  std::vector<std::wstring> items;
  int selected = -1;
  bool colors = false;
  std::wstring automaticLabel;
  std::vector<NamedColor> theme = ColorPalette::DefaultTheme();
  std::function<void()> onInvoke;                            // the main part
  std::function<void(int)> onPick;                           // an item from the list
  std::function<void(D2D1_COLOR_F, bool automatic)> onColor;  // a color; the main part applies it next

  void SetGroup(ControlGroup* g) override;
  void Draw(gfx::Canvas& c) override;
  void DrawPopup(gfx::Canvas& c) override;
  bool IsOpen() const override { return popup_.open; }
  void SetOpen(bool open) override;
  bool HitPopup(float x, float y) const override;
  bool OnMouseMove(float x, float y) override;
  void OnMouseLeave() override;
  void OnMouseDown(float x, float y, bool dbl, bool shift) override;
  void OnMouseUp(float x, float y) override;
  bool OnWheel(int delta, float x, float y) override;
  bool OnKeyDown(UINT vk, bool ctrl, bool shift, bool alt) override;
  bool OnChar(wchar_t ch) override;
  void OnFocus(bool focused) override;
  std::wstring TooltipAt(float x, float y) const override;
  void Describe(uia::Tree& t, int parent) override;

 private:
  enum Part { kNone, kMain, kArrow };
  Part PartAt(float x, float y) const;
  D2D1_RECT_F MainRect() const;
  D2D1_RECT_F ArrowRect() const;
  Control& Content() { return colors ? static_cast<Control&>(palette_) : list_; }
  void Invoke();

  ListBox list_;
  ColorPalette palette_;
  Popup popup_;
  Anim mainHover_, arrowHover_, mainPress_, arrowPress_;
  Part pressed_ = kNone;
  bool inPopup_ = false;
};

// A number with up and down buttons; typed numbers are checked against the range.
class SpinBox : public Control {
 public:
  SpinBox();
  ~SpinBox() override;

  double value = 0, minValue = 0, maxValue = 100, step = 1;
  int decimals = 0;
  std::wstring unit;  // shown after the number, e.g. " pt"
  std::function<void(double)> onChange;

  // Sets the value (ignored while the box is being edited); `notify` calls onChange.
  void SetValue(double v, bool notify = false);
  std::wstring Format(double v) const;
  bool Parse(const std::wstring& text, double& out) const;

  void Draw(gfx::Canvas& c) override;
  bool OnMouseMove(float x, float y) override;
  void OnMouseLeave() override;
  void OnMouseDown(float x, float y, bool dbl, bool shift) override;
  void OnMouseUp(float x, float y) override;
  bool OnWheel(int delta, float x, float y) override;
  bool OnKeyDown(UINT vk, bool ctrl, bool shift, bool alt) override;
  bool OnChar(wchar_t ch) override;
  void OnFocus(bool focused) override;
  bool UsesCaret() const override { return true; }
  bool OverText(float x, float y) const override;
  void Describe(uia::Tree& t, int parent) override;

 private:
  static void CALLBACK RepeatProc(HWND hwnd, UINT, UINT_PTR id, DWORD);
  D2D1_RECT_F UpRect() const;
  D2D1_RECT_F DownRect() const;
  void Step(double amount);
  // Applies the typed number; false (and a shake) when it isn't one.
  bool Commit();
  void StopRepeat();

  TextBox box_;
  Anim upHover_, downHover_, upPress_, downPress_;
  int pressed_ = 0;  // 1 up, -1 down
  bool selecting_ = false, justFocused_ = false;
  int repeats_ = 0;
};
