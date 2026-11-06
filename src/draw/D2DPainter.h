#pragma once

#include <d2d1_1.h>
#include <wrl/client.h>

#include <map>
#include <vector>

#include "draw/Painter.h"

namespace draw {

// Device copies of Bitmaps, kept between frames. Clear it when the device is lost.
class D2DBitmapCache {
 public:
  ID2D1Bitmap* Get(ID2D1DeviceContext* dc, const Bitmap& bitmap);
  void Forget(uint64_t id) { bitmaps_.erase(id); }
  void Clear() { bitmaps_.clear(); }

 private:
  std::map<uint64_t, Microsoft::WRL::ComPtr<ID2D1Bitmap>> bitmaps_;
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

 private:
  enum class Pushed : uint8_t { AxisClip, Layer };
  struct Saved {
    D2D1_MATRIX_3X2_F transform;
    size_t pushed;
  };

  ID2D1Brush* MakeBrush(const Brush& b);
  Microsoft::WRL::ComPtr<ID2D1PathGeometry> MakeGeometry(const Path& p);
  ID2D1StrokeStyle* MakeStrokeStyle(const Pen& pen);
  void PopTo(size_t depth);

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
};

}  // namespace draw
