#pragma once

// Drawing a slide's transition and animations at a moment, for the editor's Preview (P3, p3-anim):
// the show's own way of drawing them (SlideShow.cpp's Show::DrawTransition and Show::DrawSprite, as
// shared functions), on any Direct2D context, without the show's window.

#include <d2d1_1.h>
#include <wrl/client.h>

#include <cstddef>
#include <vector>

#include "slide/show/Timing.h"

namespace fw::slide::show {

class SlideRenderer;

double Smooth(double p);  // ease in and out, 0..1

// A transition at progress `p` (0..1) from `from` (the slide before; null: black) to `to`, both
// W x H pixels at the context's origin. Clears the context's target first.
void DrawTransitionFrame(ID2D1DeviceContext* dc, const Transition& t, ID2D1Bitmap* from, ID2D1Bitmap* to, float W, float H,
                         double p);

// A target's picture `bitmap`, placed at `rect` when unanimated, drawn at `pose`: `box` is the
// target's own box (its center, and what wipes and splits show); slideW and slideH the slide in
// pixels (a pose's offsets are fractions of it).
void DrawPosed(ID2D1DeviceContext* dc, ID2D1Bitmap* bitmap, const D2D1_RECT_F& rect, const D2D1_RECT_F& box, const Pose& pose,
               float slideW, float slideH);

// One slide's animations, played: each target a picture drawn once, the parts that don't animate
// as layers between them in z-order, so a frame is a few bitmap draws.
class AnimationPlayer {
 public:
  // Prepares slide `slide` of `renderer` at w x h pixels with `ctx` (an off-screen context on the
  // device the frames are drawn with). The animator starts at the slide's start.
  bool Prepare(ID2D1DeviceContext* ctx, SlideRenderer& renderer, size_t slide, const Timing& timing, UINT w, UINT h,
               double now);
  Animator& Anim() { return animator_; }
  // The slide as it is now, at the context's origin (its transform kept).
  void Draw(ID2D1DeviceContext* dc);

 private:
  struct Sprite {
    TargetKey key;
    size_t item = 0;
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> bitmap;
    D2D1_RECT_F rect{}, box{};
  };
  struct Item {
    int whole = -1, background = -1;
    std::vector<int> paragraphs;
    bool Targeted(int p) const { return p >= 0 && p < static_cast<int>(paragraphs.size()) && paragraphs[p] >= 0; }
  };
  struct Piece {
    int sprite = -1;                               // a sprite, or
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> layer;    // the static parts between two sprites
  };
  static TargetBox BoxOf(void* ctx, const TargetKey& key);
  Pose PoseOf(int sprite) const;

  const Timing* timing_ = nullptr;
  Animator animator_;
  std::vector<Item> items_;
  std::vector<Sprite> sprites_;
  std::vector<TargetBox> boxes_;
  std::vector<Piece> pieces_;
  float w_ = 0, h_ = 0;
};

}  // namespace fw::slide::show
