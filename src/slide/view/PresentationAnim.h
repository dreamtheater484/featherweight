#pragma once

// The presentation tab's Animation Pane and Preview in the editor (P3, p3-anim): PresentationAnim.cpp.

#include <d2d1_1.h>
#include <wrl/client.h>

#include <memory>
#include <string>
#include <vector>

#include "slide/edit/TimingOps.h"
#include "slide/show/ShowDraw.h"
#include "slide/show/SlideRenderer.h"
#include "slide/show/Timing.h"
#include "slide/view/PresentationView.h"
#include "ui/PanelButton.h"
#include "ui/SidePanel.h"

namespace fw::slide::view {

// The current slide's main sequence as PowerPoint's pane lists it: the click each effect plays on,
// its trigger, class and name, and what it animates; picking effects selects them for the
// Animations tab (and their shapes on the slide).
class PresentationView::AnimationPane : public PanelContent {
 public:
  explicit AnimationPane(PresentationView& v);
  std::wstring Title() const override { return L"Animation Pane"; }
  // The slide's sequence again, when the deck or the slide changed.
  void Sync();
  std::vector<Control*> Layout(const D2D1_RECT_F& body, float ui) override;
  void Draw(gfx::Canvas& c) override;
  bool TakesFocus() const override { return true; }
  void SetFocus(bool has, bool byKeyboard) override;
  bool OnMouseDown(float x, float y, bool dbl) override;
  bool OnMouseMove(float x, float y) override;  // dragging the picked effects to another place
  void OnMouseUp(float x, float y) override;
  bool OnWheel(int delta, float x, float y) override;
  bool OnKeyDown(UINT vk, bool ctrl, bool shift) override;
  std::wstring TooltipAt(float x, float y) override;

 private:
  PresentationView& v_;
  PanelButton play_, up_, down_, remove_;
  std::vector<edit::AnimEffect> seq_;
  std::vector<std::wstring> names_;
  std::u16string why_;
  std::vector<size_t> picked_;
  uint64_t state_ = UINT64_MAX;
  size_t slide_ = SIZE_MAX;
  D2D1_RECT_F list_{};
  float ui_ = 1, scroll_ = 0, pressY_ = 0;
  size_t anchor_ = 0;
  size_t dropAt_ = SIZE_MAX;  // while dragging: the row the picked effects go in front of
  bool focus_ = false, pressed_ = false;
};

// A preview playing over the slide in the editor: the slide's transition from the slide before it,
// or its animations from an effect on, through the show's Timing reader and Animator.
struct PresentationView::AnimPreview {
  bool transition = false;
  size_t slide = 0;
  double start = 0;      // ms, the clock it started at
  double settled = -1;   // animations: when the last step ended (the last frame stays a moment)
  size_t fromStep = 0;   // animations: the click to play from
  double fixedAt = -1;   // script steps: the frame this many ms in, held (snapshots)
  bool prepared = false;
  show::Transition trans;
  show::Timing timing;
  std::unique_ptr<show::SlideRenderer> renderer;
  show::AnimationPlayer player;
  Microsoft::WRL::ComPtr<ID2D1Bitmap1> from, to, frame;  // transition: the slide before, this one, the frame
  UINT w = 0, h = 0;
};

}  // namespace fw::slide::view
