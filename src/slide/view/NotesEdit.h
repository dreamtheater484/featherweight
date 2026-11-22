#pragma once

#include <windows.h>

#include <d2d1_1.h>

#include <functional>
#include <string>

namespace fw::slide::view {

// The notes pane while its text is typed in (P2, p2-slides): a native multi-line edit control, so
// typing, IME, undo and the clipboard behave as everywhere in Windows. Plain text, until p2-text's
// editor takes the notes. It hands the text over after a pause in typing and when it's left.
class NotesEdit {
 public:
  NotesEdit() = default;
  NotesEdit(const NotesEdit&) = delete;
  NotesEdit& operator=(const NotesEdit&) = delete;
  ~NotesEdit();

  // Shown over the pane (client pixels of `parent`), focused, the caret at the end.
  bool Open(HWND parent, const D2D1_RECT_F& box, const std::wstring& text, float ui);
  void Place(const D2D1_RECT_F& box, float ui);
  bool IsOpen() const { return host_ != nullptr; }
  std::wstring Text() const;  // lines separated by '\n'
  void SetText(const std::wstring& text);
  // Hands the text over when it changed since the last time.
  void Commit();
  // Gone, without handing anything over (Commit first to keep the typing).
  void Close();

  std::function<void(const std::wstring&)> onCommit;
  std::function<void()> onLeave;  // focus went elsewhere, or Esc: the owner commits and closes

 private:
  static LRESULT CALLBACK HostProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
  static LRESULT CALLBACK EditProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data);
  void UpdateFont(float ui);

  HWND parent_ = nullptr, host_ = nullptr, edit_ = nullptr;
  HFONT font_ = nullptr;
  float fontUi_ = 0;
  HBRUSH brush_ = nullptr;
  COLORREF brushColor_ = 0;
  RECT placed_{};
  bool swallowChar_ = false;  // the control character of a shortcut handed to the app
  std::wstring committed_;
};

}  // namespace fw::slide::view
