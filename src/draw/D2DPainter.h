#pragma once

#include <d2d1_1.h>
#include <wrl/client.h>

#include <map>
#include <vector>

#include "draw/Painter.h"

namespace draw {

// Device copies of Bitmaps, and the metafiles and SVG documents made from VectorImages, kept
// between frames. The bitmaps stay within a budget (32 MB by default): those drawn least recently
// go first. Clear it when the device is lost.
class D2DBitmapCache {
 public:
  ID2D1Bitmap* Get(ID2D1DeviceContext* dc, const Bitmap& bitmap);
  // An ID2D1GdiMetafile (EMF) or ID2D1SvgDocument (SVG); null when Direct2D can't read it.
  IUnknown* Get(ID2D1DeviceContext* dc, const VectorImage& image);
  void Forget(uint64_t id);
  void Clear();
  void SetBudget(uint64_t bytes) { budget_ = bytes; }
  uint64_t BytesKept() const { return total_; }

 private:
  struct Entry {
    Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
    uint64_t bytes = 0, lastUse = 0;
  };
  std::map<uint64_t, Entry> bitmaps_;
  std::map<uint64_t, Microsoft::WRL::ComPtr<IUnknown>> vectors_;  // null entries: unreadable
  uint64_t budget_ = 32ull << 20, total_ = 0, clock_ = 0;
};

// Painter on a Direct2D device context: a window's swap chain, an off-screen bitmap, or a command
// list that becomes a printed page. Use it between the context's BeginDraw and EndDraw. The
// context's transform when the painter is made is where it starts, and the destructor puts it back
// (and undoes any Save left open).
class D2DPainter : public Painter {
 public:
  explicit D2DPainter(ID2D1DeviceContext* dc, D2DBitmapCache* cache = nullptr);
  ~D2DPainter() override;
  D2DPainter(const D2DPainter&) = delete;
  D2DPainter& operator=(const D2DPainter&) = delete;

  void Save() override;
  void Restore() override;
  void Transform(const Matrix& m) override;
  Matrix CurrentTransform() const override;
  void ClipRect(const Rect& r) override;
  void ClipPath(const Path& p) override;
  void BeginGroup(float opacity) override;

  void FillPath(const Path& p, const Brush& b) override;
  void StrokePath(const Path& p, const Pen& pen) override;
  void FillRect(const Rect& r, const Brush& b) override;
  void DrawGlyphs(Point origin, const GlyphRun& run, const Brush& b) override;
  void DrawBitmap(const Bitmap& bitmap, const Rect& dest, const Rect* src = nullptr, float opacity = 1) override;
  void DrawVectorImage(const VectorImage& image, const Rect& dest, float opacity = 1) override;
  // Records what follows into a command list on a second context of the same device; Restore
  // draws it twice, through Direct2D's shadow effect and then as it is.
  void BeginShadow(const Shadow& shadow) override;
  // Restore draws only the shadow: PDF export draws shadows as pictures under the vectors.
  void SetShadowsOnly(bool on) { shadowsOnly_ = on; }

 private:
  enum class Pushed : uint8_t { AxisClip, Layer };
  struct Saved {
    D2D1_MATRIX_3X2_F transform;
    size_t pushed;
    // BeginShadow: the context drawing goes back to, the recording, and the shadow to give it.
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> outer, recorder;
    Microsoft::WRL::ComPtr<ID2D1CommandList> recording;
    Shadow shadow;
  };

  ID2D1Brush* MakeBrush(const Brush& b);
  Microsoft::WRL::ComPtr<ID2D1PathGeometry> MakeGeometry(const Path& p);
  ID2D1StrokeStyle* MakeStrokeStyle(const Pen& pen);
  void PopTo(size_t depth);
  void DrawShadowed(const Saved& s);

  ID2D1DeviceContext* dc_;
  Microsoft::WRL::ComPtr<ID2D1Factory> factory_;
  D2DBitmapCache* cache_;
  D2DBitmapCache ownCache_;
  D2D1_MATRIX_3X2_F initial_{};
  D2D1_MATRIX_3X2_F current_{};
  std::vector<Pushed> pushed_;
  std::vector<Saved> saved_;
  Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> solid_;
  Microsoft::WRL::ComPtr<ID2D1Brush> gradient_;
  // The last stroke style, reused while pens keep the same caps, join and dashes.
  Pen strokeKey_;
  Microsoft::WRL::ComPtr<ID2D1StrokeStyle> stroke_;
  bool shadowsOnly_ = false;
};

}  // namespace draw
