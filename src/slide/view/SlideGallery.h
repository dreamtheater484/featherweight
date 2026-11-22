#pragma once

#include <functional>
#include <string>
#include <vector>

#include "ui/Anim.h"
#include "ui/Controls.h"

// A toolbar button that opens a gallery of pictures with their names (P2, p2-slides): ＋ Slide ▾
// and Layout ▾ show the deck's layouts as P1's renderer draws them, Design shows the themes. With
// `split`, the button's left part runs `onInvoke` and its arrow opens the gallery; otherwise the
// whole button opens it. The owner draws each picture (`paint`), so the gallery holds no bitmaps.
namespace fw::slide::view {

class SlideGallery : public Control {
 public:
  std::vector<std::wstring> items;  // the names under the pictures
  int current = -1;                 // marked (the selected slides' layout)
  std::wstring text;                // the button's label
  wchar_t glyph = 0;
  bool split = false;
  float aspect = 16.0f / 9.0f;      // the pictures' width over height
  float pictureWidth = 120;         // DIPs
  int columns = 3;
  std::wstring heading;             // over the pictures ("Office Theme"), or none
  // Draws item `i`'s picture into `r` (client pixels).
  std::function<void(gfx::Canvas&, int, const D2D1_RECT_F&)> paint;
  std::function<void()> onInvoke;   // the split button's main part
  std::function<void(int)> onPick;
  std::function<void()> onOpen;     // the gallery opened: its pictures will be asked for

  void Draw(gfx::Canvas& c) override;
  void DrawPopup(gfx::Canvas& c) override;
  bool IsOpen() const override { return popup_.open; }
  void SetOpen(bool open) override;
  bool HitPopup(float x, float y) const override { return popup_.Hit(x, y); }
  bool OnMouseMove(float x, float y) override;
  void OnMouseLeave() override;
  void OnMouseDown(float x, float y, bool dbl, bool shift) override;
  void OnMouseUp(float x, float y) override;
  bool OnWheel(int delta, float x, float y) override;
  bool OnKeyDown(UINT vk, bool ctrl, bool shift, bool alt) override;
  void OnFocus(bool focused) override;
  std::wstring TooltipAt(float x, float y) const override;
  void Describe(uia::Tree& t, int parent) override;

 private:
  enum Part { kNone, kMain, kArrow };
  Part PartAt(float x, float y) const;
  D2D1_RECT_F ArrowRect() const;
  D2D1_SIZE_F CellSize() const;
  D2D1_RECT_F CellRect(int i) const;   // in the popup, scrolled
  D2D1_RECT_F Pictures() const;        // the popup's scrolling area
  int ItemAt(float x, float y) const;
  float MaxScroll() const;
  void EnsureVisible(int i);
  void Pick(int i);

  Popup popup_;
  Anim hover_, press_;
  Part hoverPart_ = kNone, pressPart_ = kNone;
  int hot_ = -1;
  float scroll_ = 0;
};

}  // namespace fw::slide::view
