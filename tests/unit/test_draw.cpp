#include "TestHarness.h"

#include <windows.h>

#include <d2d1_1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

#include "DrawTestUtil.h"
#include "draw/D2DPainter.h"

using Microsoft::WRL::ComPtr;
using namespace draw;
using namespace drawtest;

namespace {

const Color kRed{1, 0, 0, 1}, kGreen{0, 1, 0, 1}, kBlue{0, 0, 1, 1}, kBlack{0, 0, 0, 1};

}  // namespace

TEST(Painter_Matrix) {
  Matrix m = Matrix::Translate(10, 0) * Matrix::Rotate(90);  // translate first, then rotate
  Point p = m.Apply({1, 0});
  CHECK_NEAR(p.x, 0, 1e-4);
  CHECK_NEAR(p.y, 11, 1e-4);
  CHECK(Matrix::Scale(2, 3).AxisAligned());
  CHECK(!Matrix::Rotate(30).AxisAligned());
}

TEST(Painter_FillAndClip) {
  Surface s(48, 48);
  {
    D2DPainter p(s.dc.Get());
    p.FillRect({0, 0, 48, 48}, Brush::Solid(kRed));
    p.Save();
    p.ClipRect({10, 10, 20, 20});
    p.FillRect({0, 0, 48, 48}, Brush::Solid(kBlue));
    p.Restore();
    p.FillRect({30, 30, 40, 40}, Brush::Solid(kGreen));
  }
  CHECK(s.End());
  CHECK_EQ(s.At(15, 15), 0x0000FFu);
  CHECK_EQ(s.At(5, 5), 0xFF0000u);
  CHECK_EQ(s.At(25, 25), 0xFF0000u);
  CHECK_EQ(s.At(35, 35), 0x00FF00u);
}

TEST(Painter_RotatedClip) {
  Surface s(64, 64);
  {
    D2DPainter p(s.dc.Get());
    p.Transform(Matrix::Rotate(45) * Matrix::Translate(32, 32));
    p.ClipRect({-10, -10, 10, 10});  // a diamond on screen, not its bounding box
    p.FillRect({-40, -40, 40, 40}, Brush::Solid(kBlack));
  }
  CHECK(s.End());
  CHECK_EQ(s.At(32, 32), 0x000000u);
  CHECK_EQ(s.At(20, 32), 0x000000u);   // 12 px left of the center: inside the diamond
  CHECK_EQ(s.At(22, 22), 0xFFFFFFu);   // a corner of the bounding box: outside it
}

TEST(Painter_GroupOpacity) {
  Surface s(48, 24);
  {
    D2DPainter p(s.dc.Get());
    p.BeginGroup(0.5f);
    p.FillRect({0, 0, 30, 24}, Brush::Solid(kBlack));
    p.FillRect({18, 0, 48, 24}, Brush::Solid(kBlack));
    p.Restore();
  }
  CHECK(s.End());
  // The overlap is blended once with the rest, so it isn't darker.
  CHECK_EQ(s.At(24, 12), s.At(6, 12));
  CHECK_EQ(s.At(24, 12), s.At(40, 12));
  CHECK(std::abs(Red(s.At(6, 12)) - 128) <= 2);
}

TEST(Painter_DashedStroke) {
  Surface s(48, 20);
  {
    D2DPainter p(s.dc.Get());
    Path line;
    line.MoveTo({0, 10});
    line.LineTo({48, 10});
    Pen pen;
    pen.brush = Brush::Solid(kBlack);
    pen.width = 2;
    pen.dashes = {2, 2};  // in widths: 4 px on, 4 px off
    p.StrokePath(line, pen);
  }
  CHECK(s.End());
  CHECK_EQ(s.At(1, 9), 0x000000u);
  CHECK_EQ(s.At(5, 9), 0xFFFFFFu);
  CHECK_EQ(s.At(9, 9), 0x000000u);
  CHECK_EQ(s.At(1, 4), 0xFFFFFFu);
}

TEST(Painter_FillRules) {
  Surface s(80, 40);
  {
    D2DPainter p(s.dc.Get());
    for (int i = 0; i < 2; ++i) {
      Path ring = Path::FromRect({i * 40.f, 0, i * 40.f + 40, 40});
      Path inner = Path::FromRect({i * 40.f + 10, 10, i * 40.f + 30, 30});
      for (const auto& seg : inner.Segments()) {
        if (seg.kind == Path::Kind::Move) ring.MoveTo(seg.p[0]);
        else if (seg.kind == Path::Kind::Line) ring.LineTo(seg.p[0]);
        else ring.Close();
      }
      ring.fillRule = i == 0 ? Path::FillRule::EvenOdd : Path::FillRule::Winding;
      p.FillPath(ring, Brush::Solid(kBlack));
    }
  }
  CHECK(s.End());
  CHECK_EQ(s.At(20, 20), 0xFFFFFFu);  // even-odd: a hole
  CHECK_EQ(s.At(60, 20), 0x000000u);  // winding, both figures the same way round: filled
  CHECK_EQ(s.At(4, 20), 0x000000u);
}

TEST(Painter_Shapes) {
  Surface s(60, 40);
  {
    D2DPainter p(s.dc.Get());
    p.FillPath(Path::Ellipse({0, 0, 40, 40}), Brush::Solid(kBlue));
    p.FillPath(Path::RoundedRect({42, 0, 60, 40}, 9, 9), Brush::Solid(kRed));
  }
  CHECK(s.End());
  CHECK_EQ(s.At(20, 20), 0x0000FFu);
  CHECK_EQ(s.At(2, 2), 0xFFFFFFu);    // outside the circle
  CHECK_EQ(s.At(51, 20), 0xFF0000u);
  CHECK_EQ(s.At(43, 1), 0xFFFFFFu);   // the rounded corner
}

TEST(Painter_LinearGradient) {
  Surface s(64, 8);
  {
    D2DPainter p(s.dc.Get());
    p.FillRect({0, 0, 64, 8}, Brush::Linear({0, 0}, {64, 0}, {{0, kRed}, {1, kBlue}}));
  }
  CHECK(s.End());
  uint32_t left = s.At(1, 4), right = s.At(62, 4), mid = s.At(32, 4);
  CHECK(Red(left) > 240 && Blue(left) < 20);
  CHECK(Blue(right) > 240 && Red(right) < 20);
  CHECK(Red(mid) > 100 && Blue(mid) > 100);
}

TEST(Painter_Bitmap) {
  // 2x2: red, green / blue, white (premultiplied BGRA).
  std::vector<uint8_t> px = {0, 0, 255, 255, 0, 255, 0, 255, 255, 0, 0, 255, 255, 255, 255, 255};
  Bitmap bmp(2, 2, px);
  Surface s(40, 20);
  {
    D2DBitmapCache cache;
    D2DPainter p(s.dc.Get(), &cache);
    Rect topLeft{0, 0, 1, 1};
    p.DrawBitmap(bmp, {0, 0, 20, 20}, &topLeft);
    p.DrawBitmap(bmp, {20, 0, 40, 20}, nullptr, 0.5f);
  }
  CHECK(s.End());
  uint32_t c = s.At(10, 10);
  CHECK(Red(c) > 240 && Green(c) < 15 && Blue(c) < 15);
  uint32_t faded = s.At(22, 2);  // the red corner at half opacity over white
  CHECK(Red(faded) > 200 && Green(faded) > 100 && Green(faded) < 180);
}

TEST(Painter_Glyphs) {
  Surface s(80, 40);
  ComPtr<IDWriteFactory> dw;
  DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dw.GetAddressOf()));
  ComPtr<IDWriteFontCollection> fonts;
  ComPtr<IDWriteFontFamily> family;
  ComPtr<IDWriteFont> font;
  ComPtr<IDWriteFontFace> face;
  UINT32 index = 0;
  BOOL exists = FALSE;
  dw->GetSystemFontCollection(&fonts);
  fonts->FindFamilyName(L"Arial", &index, &exists);
  CHECK(exists);
  if (!exists) return;
  fonts->GetFontFamily(index, &family);
  family->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, &font);
  font->CreateFontFace(&face);
  const UINT32 codepoints[] = {'H', 'I'};
  UINT16 glyphs[2];
  face->GetGlyphIndices(codepoints, 2, glyphs);
  const float advances[] = {26, 12};
  GlyphRun run;
  run.face = face.Get();
  run.emSize = 32;
  run.count = 2;
  run.indices = glyphs;
  run.advances = advances;
  {
    D2DPainter p(s.dc.Get());
    p.DrawGlyphs({4, 32}, run, Brush::Solid(kBlack));
  }
  CHECK(s.End());
  CHECK_EQ(s.At(7, 20), 0x000000u);   // the left stem of the H
  CHECK_EQ(s.At(14, 6), 0xFFFFFFu);   // between the stems, above the bar
  CHECK_EQ(s.At(60, 20), 0xFFFFFFu);  // past the end of the run
}

TEST(Painter_UnbalancedSaveIsUndone) {
  Surface s(16, 16);
  D2D1_MATRIX_3X2_F before{};
  s.dc->GetTransform(&before);
  {
    D2DPainter p(s.dc.Get());
    p.Save();
    p.Transform(Matrix::Scale(2, 2));
    p.ClipRect({0, 0, 4, 4});
    p.BeginGroup(0.5f);
    p.ClipPath(Path::Ellipse({0, 0, 4, 4}));
    p.FillRect({0, 0, 16, 16}, Brush::Solid(kBlack));
  }  // never restored
  D2D1_MATRIX_3X2_F after{};
  s.dc->GetTransform(&after);
  CHECK(s.End());
  CHECK(after._11 == before._11 && after._22 == before._22 && after._31 == before._31);
}

TEST(Painter_MatrixInverseAndScale) {
  const Matrix m = Matrix::Scale(2, 3) * Matrix::Rotate(30) * Matrix::Translate(5, 7);
  const Point p = m.Inverse().Apply(m.Apply({4, -6}));
  CHECK_NEAR(p.x, 4, 1e-4);
  CHECK_NEAR(p.y, -6, 1e-4);
  // How much a unit along each axis grows, rotation aside.
  CHECK_NEAR(m.ScaleX(), 2, 1e-4);
  CHECK_NEAR(m.ScaleY(), 3, 1e-4);
}

TEST(Painter_PathFlattenAndBounds) {
  // A half circle of radius 10 around (10, 0), closed by its diameter.
  Path half;
  half.MoveTo({0, 0});
  half.ArcTo({20, 0}, 10, 10, 0, false, true);
  half.Close();
  std::vector<Path::Polygon> polys = half.Flatten(0.05f);
  CHECK_EQ(polys.size(), 1u);
  CHECK(polys[0].closed);
  CHECK(polys[0].points.size() > 8);
  for (const Point& pt : polys[0].points) CHECK_NEAR(std::hypot(pt.x - 10, pt.y), 10, 0.1);
  // Clockwise in y-down coordinates: through the top.
  const Rect b = half.Bounds();
  CHECK_NEAR(b.left, 0, 0.01);
  CHECK_NEAR(b.right, 20, 0.01);
  CHECK_NEAR(b.top, -10, 0.05);
  CHECK_NEAR(b.bottom, 0, 0.01);
}

TEST(Painter_BitmapBrushTiles) {
  // 2x2: red, green / blue, white, each pixel 4 units, repeated; mirrored across.
  auto bmp = std::make_shared<Bitmap>(
      2, 2, std::vector<uint8_t>{0, 0, 255, 255, 0, 255, 0, 255, 255, 0, 0, 255, 255, 255, 255, 255});
  Surface s(32, 32);
  {
    D2DBitmapCache cache;
    D2DPainter p(s.dc.Get(), &cache);
    Brush tiles = Brush::FromBitmap(bmp, Matrix::Scale(4, 4), Brush::Extend::Repeat);
    tiles.smooth = false;
    p.FillRect({0, 0, 32, 16}, tiles);
    tiles.extendX = Brush::Extend::Mirror;
    p.FillRect({0, 16, 32, 32}, tiles);
  }
  CHECK(s.End());
  CHECK_EQ(s.At(2, 2), 0xFF0000u);
  CHECK_EQ(s.At(6, 2), 0x00FF00u);
  CHECK_EQ(s.At(10, 2), 0xFF0000u);  // repeated
  CHECK_EQ(s.At(2, 6), 0x0000FFu);
  CHECK_EQ(s.At(2, 18), 0xFF0000u);
  CHECK_EQ(s.At(10, 18), 0x00FF00u);  // mirrored: green comes first again
}

TEST(Painter_Shadow) {
  // A red square with a black shadow 10 units down and right, slightly blurred.
  Surface s(40, 40);
  {
    D2DPainter p(s.dc.Get());
    p.BeginShadow({kBlack, 1, Matrix::Translate(10, 10)});
    p.FillRect({5, 5, 20, 20}, Brush::Solid(kRed));
    p.Restore();
  }
  CHECK(s.End());
  CHECK_EQ(s.At(12, 12), 0xFF0000u);        // the square, over its shadow
  CHECK(Red(s.At(25, 25)) < 40);            // the shadow
  CHECK_EQ(s.At(2, 2), 0xFFFFFFu);
  CHECK_EQ(s.At(36, 8), 0xFFFFFFu);
}

TEST(Painter_BitmapCacheBudget) {
  // Device copies stay within the cache's budget: the least recently drawn go first.
  Surface s(8, 8);
  D2DBitmapCache cache;
  cache.SetBudget(10000);
  Bitmap a(32, 32, std::vector<uint8_t>(32 * 32 * 4, 255)), b(32, 32, std::vector<uint8_t>(32 * 32 * 4, 128));
  Bitmap c(32, 32, std::vector<uint8_t>(32 * 32 * 4, 0));
  ID2D1Bitmap* first = cache.Get(s.dc.Get(), a);
  CHECK(first != nullptr && cache.BytesKept() == 4096);
  CHECK(cache.Get(s.dc.Get(), b) != nullptr && cache.BytesKept() == 8192);
  CHECK(cache.Get(s.dc.Get(), a) == first);  // a is now the most recent
  CHECK(cache.Get(s.dc.Get(), c) != nullptr && cache.BytesKept() == 8192);  // b went
  CHECK(cache.Get(s.dc.Get(), a) == first);
  cache.Forget(a.Id());
  CHECK(cache.BytesKept() == 4096);
  CHECK(s.End());
}
