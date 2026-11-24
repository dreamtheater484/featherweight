#include "slide/show/ShowDraw.h"

#include <d2d1_1helper.h>

#include <algorithm>
#include <cmath>

#include "slide/show/SlideRenderer.h"

// The show's drawing as shared functions (SlideShow.cpp draws the same way in its own Show class:
// Show::DrawTransition, Show::DrawSprite, the scene's sprites and layers).
namespace fw::slide::show {
namespace {

using Microsoft::WRL::ComPtr;
using M = D2D1::Matrix3x2F;

// The static parts of a run of items (with the slide's background for the first run).
class StaticParts : public DrawFilter {
 public:
  template <class Items>
  StaticParts(const Items& items, const std::vector<char>& on, bool bg) : on_(on), bg_(bg) {
    for (const auto& t : items) {
      whole_.push_back(t.whole >= 0 || t.background >= 0);
      paragraphs_.push_back(t.paragraphs);
      wholeOnly_.push_back(t.whole >= 0);
    }
  }
  bool Background() const override { return bg_; }
  bool Item(size_t i) const override { return i < on_.size() && on_[i]; }
  bool Body(size_t i) const override { return !whole_[i]; }
  bool Paragraph(size_t i, int p) const override {
    const std::vector<int>& ps = paragraphs_[i];
    return !wholeOnly_[i] && !(p >= 0 && p < static_cast<int>(ps.size()) && ps[p] >= 0);
  }

 private:
  const std::vector<char>& on_;
  bool bg_;
  std::vector<char> whole_, wholeOnly_;
  std::vector<std::vector<int>> paragraphs_;
};

class OneTarget : public DrawFilter {
 public:
  OneTarget(const TargetKey& key, size_t item, const std::vector<int>& paragraphSprites)
      : key_(key), item_(item), ps_(paragraphSprites) {}
  bool Background() const override { return false; }
  bool Item(size_t i) const override { return i == item_; }
  bool Body(size_t) const override { return key_.part != TargetKey::Paragraphs; }
  bool Paragraph(size_t, int p) const override {
    if (key_.part == TargetKey::Paragraphs) return key_.Contains(p);
    return key_.part == TargetKey::Whole && !(p >= 0 && p < static_cast<int>(ps_.size()) && ps_[p] >= 0);
  }

 private:
  TargetKey key_;
  size_t item_;
  const std::vector<int>& ps_;
};

Pose Combine(const Pose& outer, const Pose& inner) {
  Pose p = inner;
  p.visible = outer.visible && inner.visible;
  p.opacity = outer.opacity * inner.opacity;
  p.dx += outer.dx;
  p.dy += outer.dy;
  p.sx *= outer.sx;
  p.sy *= outer.sy;
  p.rotation += outer.rotation;
  if (p.reveal.kind == Reveal::None) p.reveal = outer.reveal;
  return p;
}

ComPtr<ID2D1Bitmap1> Render(ID2D1DeviceContext* ctx, SlideRenderer& r, size_t slide, const D2D1_MATRIX_3X2_F& toPx,
                            const D2D1_RECT_F& rect, const DrawFilter& filter) {
  const UINT w = static_cast<UINT>(std::max(1.f, rect.right - rect.left)), h = static_cast<UINT>(std::max(1.f, rect.bottom - rect.top));
  ComPtr<ID2D1Bitmap1> bmp;
  const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
      D2D1_BITMAP_OPTIONS_TARGET, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
  if (FAILED(ctx->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0, props, &bmp))) return nullptr;
  ctx->SetTarget(bmp.Get());
  ctx->BeginDraw();
  ctx->SetTransform(M::Identity());
  ctx->Clear(D2D1::ColorF(0, 0, 0, 0));
  r.Draw(ctx, slide, toPx * M::Translation(-rect.left, -rect.top), &filter);
  const HRESULT hr = ctx->EndDraw();
  ctx->SetTarget(nullptr);
  return SUCCEEDED(hr) ? bmp : nullptr;
}

}  // namespace

double Smooth(double p) { return p * p * (3 - 2 * p); }

void DrawTransitionFrame(ID2D1DeviceContext* dc, const Transition& t, ID2D1Bitmap* a, ID2D1Bitmap* b, float W, float H, double p) {
  dc->Clear({0, 0, 0, 1});
  auto put = [&](ID2D1Bitmap* bmp, float x, float y, float opacity = 1) {
    if (bmp) dc->DrawBitmap(bmp, D2D1::RectF(x, y, x + W, y + H), opacity, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
  };
  auto clipped = [&](ID2D1Bitmap* bmp, D2D1_RECT_F clip) {
    dc->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_ALIASED);
    put(bmp, 0, 0);
    dc->PopAxisAlignedClip();
  };
  float sx = 0, sy = 0;  // the direction of motion
  switch (t.side) {
    case Side::Left: sx = -1; break;
    case Side::Up: sy = -1; break;
    case Side::Right: sx = 1; break;
    case Side::Down: sy = 1; break;
    case Side::LeftUp: sx = -1, sy = -1; break;
    case Side::RightUp: sx = 1, sy = -1; break;
    case Side::LeftDown: sx = -1, sy = 1; break;
    case Side::RightDown: sx = 1, sy = 1; break;
  }
  const float e = static_cast<float>(Smooth(p)), f = static_cast<float>(p);
  switch (t.kind) {
    case TransitionKind::Fade:
      if (t.throughBlack) {
        if (f < 0.5f) put(a, 0, 0, 1 - 2 * f);
        else put(b, 0, 0, 2 * f - 1);
      } else {
        put(a, 0, 0);
        put(b, 0, 0, f);
      }
      break;
    case TransitionKind::Push:
      put(a, sx * e * W, sy * e * H);
      put(b, sx * (e - 1) * W, sy * (e - 1) * H);
      break;
    case TransitionKind::Cover:
      put(a, 0, 0);
      put(b, sx * (e - 1) * W, sy * (e - 1) * H);
      break;
    case TransitionKind::Uncover:
      put(b, 0, 0);
      put(a, sx * e * W, sy * e * H);
      break;
    case TransitionKind::Wipe: {
      put(a, 0, 0);
      D2D1_RECT_F r = D2D1::RectF(0, 0, W, H);
      if (sx < 0) r.left = W * (1 - f);
      else if (sx > 0) r.right = W * f;
      else if (sy < 0) r.top = H * (1 - f);
      else r.bottom = H * f;
      clipped(b, r);
      break;
    }
    case TransitionKind::Split: {
      put(a, 0, 0);
      if (!t.in) {
        clipped(b, t.vertical ? D2D1::RectF(W * (1 - f) / 2, 0, W * (1 + f) / 2, H) : D2D1::RectF(0, H * (1 - f) / 2, W, H * (1 + f) / 2));
      } else if (t.vertical) {
        clipped(b, D2D1::RectF(0, 0, W * f / 2, H));
        clipped(b, D2D1::RectF(W - W * f / 2, 0, W, H));
      } else {
        clipped(b, D2D1::RectF(0, 0, W, H * f / 2));
        clipped(b, D2D1::RectF(0, H - H * f / 2, W, H));
      }
      break;
    }
    case TransitionKind::Zoom: {
      D2D1_MATRIX_3X2_F saved;
      dc->GetTransform(&saved);
      if (t.in) {
        put(a, 0, 0);
        const float s = 0.25f + 0.75f * e;
        dc->SetTransform(M::Scale(s, s, D2D1::Point2F(W / 2, H / 2)) * saved);
        put(b, 0, 0, f);
      } else {
        put(b, 0, 0);
        const float s = 1 - 0.75f * e;
        dc->SetTransform(M::Scale(s, s, D2D1::Point2F(W / 2, H / 2)) * saved);
        put(a, 0, 0, 1 - f);
      }
      dc->SetTransform(saved);
      break;
    }
    case TransitionKind::None: put(b, 0, 0); break;
  }
}

void DrawPosed(ID2D1DeviceContext* dc, ID2D1Bitmap* bitmap, const D2D1_RECT_F& rect, const D2D1_RECT_F& b, const Pose& pose,
               float slideW, float slideH) {
  if (!pose.visible || pose.opacity <= 0.001f || !bitmap) return;
  const float cx = (b.left + b.right) / 2, cy = (b.top + b.bottom) / 2;
  const D2D1_MATRIX_3X2_F m = M::Translation(-cx, -cy) * M::Scale(pose.sx, pose.sy) * M::Rotation(pose.rotation) *
                              M::Translation(cx + pose.dx * slideW, cy + pose.dy * slideH);
  D2D1_MATRIX_3X2_F saved;
  dc->GetTransform(&saved);
  dc->SetTransform(m * saved);
  D2D1_RECT_F clips[2];
  int n = 0;
  const Reveal& rv = pose.reveal;
  if (rv.kind != Reveal::None) {
    D2D1_RECT_F r = rect;
    const float f = std::clamp(rv.shown, 0.f, 1.f);
    if (rv.kind == Reveal::Wipe) {
      switch (rv.side) {
        case Side::Up: r.bottom = b.top + (b.bottom - b.top) * f; break;
        case Side::Left: r.right = b.left + (b.right - b.left) * f; break;
        case Side::Right: r.left = b.right - (b.right - b.left) * f; break;
        default: r.top = b.bottom - (b.bottom - b.top) * f; break;
      }
      clips[n++] = r;
    } else if (rv.outward) {
      if (rv.vertical) {
        const float half = (b.right - b.left) * f / 2, c = (b.left + b.right) / 2;
        r.left = c - half;
        r.right = c + half;
      } else {
        const float half = (b.bottom - b.top) * f / 2, c = (b.top + b.bottom) / 2;
        r.top = c - half;
        r.bottom = c + half;
      }
      clips[n++] = r;
    } else {
      D2D1_RECT_F a = r, z = r;
      if (rv.vertical) {
        a.right = b.left + (b.right - b.left) * f / 2;
        z.left = b.right - (b.right - b.left) * f / 2;
      } else {
        a.bottom = b.top + (b.bottom - b.top) * f / 2;
        z.top = b.bottom - (b.bottom - b.top) * f / 2;
      }
      clips[n++] = a;
      clips[n++] = z;
    }
  }
  for (int k = 0; k < std::max(1, n); ++k) {
    if (n) dc->PushAxisAlignedClip(clips[k], D2D1_ANTIALIAS_MODE_ALIASED);
    dc->DrawBitmap(bitmap, rect, pose.opacity, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, nullptr);
    if (n) dc->PopAxisAlignedClip();
  }
  dc->SetTransform(saved);
}

// ---------------------------------------------------------------------------------------------
// AnimationPlayer

TargetBox AnimationPlayer::BoxOf(void* ctx, const TargetKey& key) {
  auto* self = static_cast<AnimationPlayer*>(ctx);
  for (size_t i = 0; i < self->sprites_.size(); ++i)
    if (self->sprites_[i].key == key) return self->boxes_[i];
  for (size_t i = 0; i < self->sprites_.size(); ++i)
    if (self->sprites_[i].key.spid == key.spid) return self->boxes_[i];
  return {};
}

Pose AnimationPlayer::PoseOf(int i) const {
  const Sprite& sp = sprites_[static_cast<size_t>(i)];
  Pose p = animator_.PoseOf(sp.key);
  const Item& t = items_[sp.item];
  if (sp.key.part == TargetKey::Paragraphs && t.whole >= 0) p = Combine(animator_.PoseOf(sprites_[static_cast<size_t>(t.whole)].key), p);
  return p;
}

bool AnimationPlayer::Prepare(ID2D1DeviceContext* ctx, SlideRenderer& r, size_t slide, const Timing& timing, UINT w, UINT h,
                              double now) {
  timing_ = &timing;
  items_.clear();
  sprites_.clear();
  boxes_.clear();
  pieces_.clear();
  w_ = static_cast<float>(w);
  h_ = static_cast<float>(h);
  const std::vector<ItemInfo>& items = r.Items(slide);
  const draw::Size size = r.SlideSize();
  if (!ctx || size.width <= 0 || size.height <= 0 || !w || !h) return false;
  const D2D1_MATRIX_3X2_F toPx = M::Scale(w_ / size.width, h_ / size.height);
  auto px = [&](const draw::Rect& rc) {
    return D2D1::RectF(rc.left * w_ / size.width, rc.top * h_ / size.height, rc.right * w_ / size.width, rc.bottom * h_ / size.height);
  };
  items_.assign(items.size(), {});
  for (const TargetKey& k : timing.targets) {
    size_t i = 0;
    while (i < items.size() && items[i].spid != k.spid) ++i;
    if (i == items.size() || !k.spid) continue;
    Item& t = items_[i];
    Sprite sp;
    sp.key = k;
    sp.item = i;
    draw::Rect box = items[i].bounds, ink = items[i].ink;
    const int idx = static_cast<int>(sprites_.size());
    if (k.part == TargetKey::Whole) {
      if (t.whole >= 0) continue;
      t.whole = idx;
    } else if (k.part == TargetKey::Background) {
      if (t.background >= 0) continue;
      t.background = idx;
    } else {
      const int last = std::min(k.last, items[i].paragraphs - 1);
      if (k.first > last) continue;
      t.paragraphs.resize(static_cast<size_t>(items[i].paragraphs), -1);
      bool taken = false;
      for (int p = k.first; p <= last; ++p) taken |= t.paragraphs[p] >= 0;
      if (taken) continue;
      for (int p = k.first; p <= last; ++p) t.paragraphs[p] = idx;
      box = r.ParagraphBox(slide, i, k.first);
      for (int p = k.first + 1; p <= last; ++p) {
        const draw::Rect pb = r.ParagraphBox(slide, i, p);
        box = {std::min(box.left, pb.left), std::min(box.top, pb.top), std::max(box.right, pb.right), std::max(box.bottom, pb.bottom)};
      }
      ink = box;
    }
    sp.box = px(box);
    const D2D1_RECT_F rr = px({std::min(box.left, ink.left), std::min(box.top, ink.top), std::max(box.right, ink.right),
                               std::max(box.bottom, ink.bottom)});
    sp.rect = {std::floor(rr.left) - 2, std::floor(rr.top) - 2, std::ceil(rr.right) + 2, std::ceil(rr.bottom) + 2};
    TargetBox tb;
    tb.x = (box.left + box.right) / 2 / size.width;
    tb.y = (box.top + box.bottom) / 2 / size.height;
    tb.w = box.Width() / size.width;
    tb.h = box.Height() / size.height;
    sprites_.push_back(std::move(sp));
    boxes_.push_back(tb);
  }
  // Sprites drawn once; the static parts between them as layers, in z-order.
  for (Sprite& sp : sprites_) sp.bitmap = Render(ctx, r, slide, toPx, sp.rect, OneTarget(sp.key, sp.item, items_[sp.item].paragraphs));
  std::vector<char> run(items.size(), 0);
  bool any = false, first = true;
  auto flush = [&] {
    if (!any && !first) return;
    Piece layer;
    layer.layer = Render(ctx, r, slide, toPx, D2D1::RectF(0, 0, w_, h_), StaticParts(items_, run, first));
    pieces_.push_back(std::move(layer));
    std::fill(run.begin(), run.end(), static_cast<char>(0));
    any = first = false;
  };
  for (size_t i = 0; i < items.size(); ++i) {
    const Item& t = items_[i];
    const int front = t.whole >= 0 ? t.whole : t.background;
    if (front >= 0) {
      flush();
      pieces_.push_back({front, nullptr});
    }
    if (t.whole < 0) {
      run[i] = 1;
      any = true;
    }
    int lastSprite = -1;
    for (int sp : t.paragraphs)
      if (sp >= 0 && sp != lastSprite) {
        flush();
        pieces_.push_back({sp, nullptr});
        lastSprite = sp;
      }
  }
  flush();
  animator_ = Animator(timing_, &AnimationPlayer::BoxOf, this);
  animator_.Start(now, true);
  return true;
}

void AnimationPlayer::Draw(ID2D1DeviceContext* dc) {
  for (const Piece& p : pieces_) {
    if (p.sprite < 0) {
      if (p.layer) dc->DrawBitmap(p.layer.Get(), D2D1::RectF(0, 0, w_, h_), 1, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
      continue;
    }
    const Sprite& sp = sprites_[static_cast<size_t>(p.sprite)];
    DrawPosed(dc, sp.bitmap.Get(), sp.rect, sp.box, PoseOf(p.sprite), w_, h_);
  }
}

}  // namespace fw::slide::show
