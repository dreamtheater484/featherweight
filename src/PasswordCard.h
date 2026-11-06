#pragma once

#include <windows.h>

#include <functional>
#include <string>

#include "TabContent.h"
#include "ui/Gfx.h"
#include "ui/Widgets.h"

namespace uia {
class Tree;
}

// What a protected file shows until its password is given (PDFs, and Word documents since D5): a
// lock, the file's name, a password box and Open, centred in the view.
class PasswordCard {
 public:
  // `submit` gets the password when Open is clicked or Enter pressed.
  PasswordCard(ViewHost& host, std::function<void(const std::wstring&)> submit);

  TextBox& Box() { return box_; }
  void Draw(gfx::Canvas& c, const RECT& vp, float ui, const std::wstring& title);
  void OnMouseDown(int x, int y, UINT mods, bool doubleClick, float ui);
  void OnMouseMove(int x, int y, float ui);
  void OnMouseUp(int x, int y);
  void OnMouseLeave() { buttons_.OnMouseLeave(); }
  HCURSOR CursorAt(int x, int y) const;
  void DescribeUia(uia::Tree& t, int parent);

  // What came of a try: the password was wrong (the box shakes), another problem, or it opened.
  void Wrong();
  void Failed(const std::wstring& message) { error_ = message; }
  void Accepted();

 private:
  void Submit() { submit_(box_.Text()); }

  ViewHost& host_;
  std::function<void(const std::wstring&)> submit_;
  TextBox box_;
  Button open_;
  ButtonSet buttons_;
  std::wstring error_;
};
