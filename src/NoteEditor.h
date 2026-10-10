#pragma once

#include <windows.h>

#include <functional>
#include <string>

#include "DocumentView.h"
#include "Widgets.h"

// A small card for writing a note. The text area is a native multi-line edit control,
// so typing, IME, undo and the clipboard all behave exactly like everywhere else in Windows.
class NoteEditor {
 public:
  enum Action { kNone = 0, kSave = 1, kCancel = 2, kDelete = 3 };

  ~NoteEditor();
  void Create(HWND parent);
  void Open(DocumentView* view, const NoteTarget& note, const std::wstring& text, D2D1_POINT_2F anchor);
  void Close();
  bool IsOpen() const { return view_ != nullptr; }
  DocumentView* View() const { return view_; }
  const NoteTarget& Target() const { return note_; }
  std::wstring Text() const;

  void Layout(const RECT& docArea, float ui);
  void Draw(gfx::Canvas& c);
  bool Hit(int x, int y) const;
  void RepaintEdit();
  HBRUSH OnCtlColor(HDC dc);
  HWND Edit() const { return edit_; }

  bool OnMouseMove(int x, int y);
  void OnMouseLeave();
  void OnMouseDown(int x, int y);
  Action OnMouseUp(int x, int y);
  std::wstring TooltipAt(int x, int y) const;

  // Called for Esc (cancel) and Ctrl+Enter (save) typed in the text area.
  std::function<void(Action)> onAction;

 private:
  static LRESULT CALLBACK EditProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data);
  void UpdateFont();
  COLORREF EditBackground() const;

  HWND parent_ = nullptr;
  HWND edit_ = nullptr;
  HFONT font_ = nullptr;
  HBRUSH brush_ = nullptr;
  COLORREF brushColor_ = 0;
  float fontUi_ = 0;
  DocumentView* view_ = nullptr;
  NoteTarget note_;
  D2D1_POINT_2F anchor_{};
  RECT area_{};
  float ui_ = 1;
  D2D1_RECT_F card_{}, box_{};
  Button save_, cancel_, delete_;
  ButtonSet set_;
};
