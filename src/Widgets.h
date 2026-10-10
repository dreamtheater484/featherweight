#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

#include "Anim.h"
#include "Gfx.h"

using gfx::Align;

enum class ButtonStyle {
  Subtle,        // toolbar icon button: transparent until hovered
  Primary,       // accent-filled call to action
  Standard,      // light filled button
  Caption,       // window caption button (minimize, maximize)
  CaptionClose,  // window close button (red on hover)
};

struct Button {
  int id = 0;
  wchar_t glyph = 0;
  std::wstring text;
  std::wstring tooltip;
  ButtonStyle style = ButtonStyle::Subtle;
  D2D1_RECT_F rc{};
  bool visible = true;
  bool enabled = true;
  bool checked = false;
  bool dropdown = false;  // shows a small chevron after the label
  float glyphSize = 16;   // DIPs
  Anim hover, press;

  bool Hit(float x, float y) const { return visible && enabled && gfx::Contains(rc, x, y); }
};

void DrawButton(gfx::Canvas& c, const Button& b, float ui, bool windowActive = true);

// Hover/press tracking for a set of buttons owned by a component.
class ButtonSet {
 public:
  void Add(Button* b) { buttons_.push_back(b); }
  Button* HitTest(float x, float y) const;
  // Updates hover state; returns true when something changed visually.
  bool OnMouseMove(float x, float y);
  void OnMouseLeave();
  bool OnMouseDown(float x, float y);
  // Returns the id of the clicked button, or 0.
  int OnMouseUp(float x, float y);
  Button* Hovered() const { return hovered_; }
  Button* Pressed() const { return pressed_; }
  void Reset();

 private:
  std::vector<Button*> buttons_;
  Button* hovered_ = nullptr;
  Button* pressed_ = nullptr;
};

// A single-line text box drawn with DirectWrite: caret, selection, clipboard, undo.
class TextBox {
 public:
  std::wstring placeholder;
  bool password = false;
  bool digitsOnly = false;
  size_t maxLength = 512;
  D2D1_RECT_F rc{};
  bool focused = false;
  bool error = false;  // red underline
  float fontSize = 13;  // DIPs
  Align align = Align::Leading;

  std::function<void()> onChange;
  std::function<void(bool shift)> onEnter;
  std::function<void()> onEscape;
  // Lets the owner handle keys like Up/Down before the text box does. Return true if handled.
  std::function<bool(UINT vk, bool shift)> onKey;

  const std::wstring& Text() const { return text_; }
  void SetText(const std::wstring& t, bool selectAll = false);
  void SelectAll();
  void Focus(bool f);

  void Draw(gfx::Canvas& c, float ui, bool caretVisible);
  bool OnKeyDown(UINT vk, bool ctrl, bool shift);
  bool OnChar(wchar_t ch);
  void OnMouseDown(float x, float y, bool shift, float ui);
  void OnMouseDrag(float x, float y, float ui);
  void OnDoubleClick();
  // Caret position in client pixels (for IME placement).
  D2D1_POINT_2F CaretPos(float ui);
  void ShakeError();
  float ShakeOffset() const;

 private:
  std::wstring Display() const;
  int IndexAt(float x, float ui);
  float XOf(int index, float ui);
  void Insert(const std::wstring& s);
  void DeleteSelection();
  void SaveUndo();
  void Changed();
  bool HasSelection() const { return caret_ != anchor_; }

  std::wstring text_;
  int caret_ = 0, anchor_ = 0;
  float scroll_ = 0;
  std::wstring undo_;
  int undoCaret_ = 0;
  double shakeStart_ = -1e9;
};
