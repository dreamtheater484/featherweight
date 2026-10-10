#pragma once

#include <windows.h>

#include <memory>
#include <string>
#include <vector>

#include "DocumentView.h"
#include "Widgets.h"

// A slim, non-blocking notification that floats over the top of the document area.
class Banner {
 public:
  struct Action {
    int id;
    std::wstring text;
    bool primary = false;
  };

  void Show(BannerKind kind, const std::wstring& text, std::vector<Action> actions = {}, int autoHideMs = 0);
  void Hide();
  bool Visible() const { return visible_ || show_.V() > 0.01f; }
  bool IsShowing() const { return visible_; }
  void Layout(const RECT& docArea, float ui);
  void Draw(gfx::Canvas& c);
  bool Hit(int x, int y) const;
  // Called regularly; hides the banner when its time is up. Returns true if it changed.
  bool Tick();
  // Milliseconds until the banner hides itself, or -1 if it stays.
  double MsUntilHide() const;

  bool OnMouseMove(int x, int y);
  void OnMouseLeave();
  void OnMouseDown(int x, int y);
  // Returns the action id that was clicked, -1 for the close button, 0 for nothing.
  int OnMouseUp(int x, int y);

 private:
  D2D1_RECT_F Card() const;

  bool visible_ = false;
  Anim show_;
  BannerKind kind_ = BannerKind::Info;
  std::wstring text_;
  std::vector<Action> actions_;
  std::vector<std::unique_ptr<Button>> buttons_;
  Button close_;
  ButtonSet set_;
  double hideAt_ = 0;
  bool hovered_ = false;
  RECT area_{};
  float ui_ = 1;
  float width_ = 0;
};
