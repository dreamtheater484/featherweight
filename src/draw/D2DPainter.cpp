#include "draw/D2DPainter.h"

#include <d2d1_3.h>
#include <d2d1effects.h>
#include <shlwapi.h>

#include <algorithm>

using Microsoft::WRL::ComPtr;

namespace draw {
namespace {

D2D1_POINT_2F Pt(Point p) { return {p.x, p.y}; }
D2D1_RECT_F RectF(const Rect& r) { return {r.left, r.top, r.right, r.bottom}; }
D2D1_COLOR_F ColorF(const Color& c) { return {c.r, c.g, c.b, c.a}; }

D2D1_MATRIX_3X2_F ToD2D(const Matrix& m) { return {m.m11, m.m12, m.m21, m.m22, m.dx, m.dy}; }
Matrix FromD2D(const D2D1_MATRIX_3X2_F& m) { return {m._11, m._12, m._21, m._22, m._31, m._32}; }

D2D1_CAP_STYLE Cap(LineCap c) {
  switch (c) {
    case LineCap::Square: return D2D1_CAP_STYLE_SQUARE;
    case LineCap::Round: return D2D1_CAP_STYLE_ROUND;
    default: return D2D1_CAP_STYLE_FLAT;
  }
}

D2D1_LINE_JOIN Join(LineJoin j) {
  switch (j) {
    case LineJoin::Bevel: return D2D1_LINE_JOIN_BEVEL;
    case LineJoin::Round: return D2D1_LINE_JOIN_ROUND;
    // Beyond the miter limit a corner is beveled, as in DrawingML, PDF and SVG.
    default: return D2D1_LINE_JOIN_MITER_OR_BEVEL;
  }
}

D2D1_EXTEND_MODE Extend(Brush::Extend e) {
  switch (e) {
    case Brush::Extend::Repeat: return D2D1_EXTEND_MODE_WRAP;
    case Brush::Extend::Mirror: return D2D1_EXTEND_MODE_MIRROR;
    default: return D2D1_EXTEND_MODE_CLAMP;
  }
}

bool SameStroke(const Pen& a, const Pen& b) {
  return a.cap == b.cap && a.join == b.join && a.miterLimit == b.miterLimit && a.dashes == b.dashes &&
         a.dashOffset == b.dashOffset;
}

}  // namespace

ID2D1Bitmap* D2DBitmapCache::Get(ID2D1DeviceContext* dc, const Bitmap& bitmap) {
  auto it = bitmaps_.find(bitmap.Id());
  if (it != bitmaps_.end()) {
    it->second.lastUse = ++clock_;
    return it->second.bitmap.Get();
  }
  if (bitmap.Width() <= 0 || bitmap.Height() <= 0) return nullptr;
  ComPtr<ID2D1Bitmap> b;
  D2D1_BITMAP_PROPERTIES props =
      D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f);
  if (FAILED(dc->CreateBitmap(D2D1::SizeU(bitmap.Width(), bitmap.Height()), bitmap.Pixels(), bitmap.Stride(), props,
                              &b))) {
    return nullptr;
  }
  const uint64_t bytes = static_cast<uint64_t>(bitmap.Stride()) * bitmap.Height();
  // Over the budget, the least recently drawn go (Direct2D keeps any still in use this frame).
  while (total_ + bytes > budget_ && !bitmaps_.empty()) {
    auto oldest = std::min_element(bitmaps_.begin(), bitmaps_.end(),
                                   [](const auto& a, const auto& b) { return a.second.lastUse < b.second.lastUse; });
    total_ -= oldest->second.bytes;
    bitmaps_.erase(oldest);
  }
  total_ += bytes;
  Entry& e = bitmaps_[bitmap.Id()];
  e = {b, bytes, ++clock_};
  return e.bitmap.Get();
}

void D2DBitmapCache::Forget(uint64_t id) {
  if (auto it = bitmaps_.find(id); it != bitmaps_.end()) {
    total_ -= it->second.bytes;
    bitmaps_.erase(it);
  }
  vectors_.erase(id);
}

void D2DBitmapCache::Clear() {
  bitmaps_.clear();
  vectors_.clear();
  total_ = 0;
}

IUnknown* D2DBitmapCache::Get(ID2D1DeviceContext* dc, const VectorImage& image) {
  auto it = vectors_.find(image.Id());
  if (it != vectors_.end()) return it->second.Get();
  ComPtr<IUnknown>& slot = vectors_[image.Id()];
  const std::vector<uint8_t>& data = image.Data();
  if (data.empty() || data.size() > UINT_MAX) return nullptr;
  ComPtr<IStream> stream;
  stream.Attach(SHCreateMemStream(data.data(), static_cast<UINT>(data.size())));
  if (!stream) return nullptr;
  if (image.GetKind() == VectorImage::Kind::Emf) {
    ComPtr<ID2D1Factory> f;
    ComPtr<ID2D1Factory1> f1;
    ComPtr<ID2D1GdiMetafile> metafile;
    dc->GetFactory(&f);
    if (SUCCEEDED(f.As(&f1)) && SUCCEEDED(f1->CreateGdiMetafile(stream.Get(), &metafile))) slot = metafile;
  } else {
    // The viewport is the picture's natural size in pixels at 96 dpi; the painter scales it.
    ComPtr<ID2D1DeviceContext5> dc5;
    ComPtr<ID2D1SvgDocument> doc;
    const Size n = image.NaturalSize();
    if (SUCCEEDED(dc->QueryInterface(IID_PPV_ARGS(&dc5))) &&
        SUCCEEDED(dc5->CreateSvgDocument(stream.Get(), D2D1::SizeF(n.width / 0.75f, n.height / 0.75f), &doc))) {
      slot = doc;
    }
  }
  return slot.Get();
}

D2DPainter::D2DPainter(ID2D1DeviceContext* dc, D2DBitmapCache* cache)
    : dc_(dc), cache_(cache ? cache : &ownCache_) {
  dc_->GetFactory(&factory_);
  dc_->GetTransform(&initial_);
  current_ = initial_;
}

D2DPainter::~D2DPainter() {
  while (!saved_.empty()) Restore();
  PopTo(0);
  dc_->SetTransform(initial_);
}

void D2DPainter::Save() { saved_.push_back({current_, pushed_.size()}); }

void D2DPainter::Restore() {
  if (saved_.empty()) return;
  PopTo(saved_.back().pushed);
  Saved s = std::move(saved_.back());
  saved_.pop_back();
  if (s.recorder) {
    dc_->EndDraw();
    s.recording->Close();
    dc_ = s.outer.Get();
    DrawShadowed(s);
  }
  current_ = s.transform;
  dc_->SetTransform(current_);
}

void D2DPainter::BeginShadow(const Shadow& shadow) {
  ComPtr<ID2D1Device> device;
  ComPtr<ID2D1DeviceContext> recorder;
  ComPtr<ID2D1CommandList> recording;
  dc_->GetDevice(&device);
  if (!device || FAILED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &recorder)) ||
      FAILED(recorder->CreateCommandList(&recording))) {
    Save();
    return;
  }
  float dpiX = 96, dpiY = 96;
  dc_->GetDpi(&dpiX, &dpiY);
  recorder->SetDpi(dpiX, dpiY);
  recorder->SetUnitMode(dc_->GetUnitMode());
  recorder->SetAntialiasMode(dc_->GetAntialiasMode());
  recorder->SetTextAntialiasMode(dc_->GetTextAntialiasMode());
  recorder->SetTarget(recording.Get());
  recorder->BeginDraw();
  recorder->SetTransform(current_);
  saved_.push_back({current_, pushed_.size(), dc_, recorder, recording, shadow});
  dc_ = recorder.Get();
}

void D2DPainter::DrawShadowed(const Saved& s) {
  // The recording is in device units; the shadow's offset and blur are in the coordinates that
  // were current at BeginShadow.
  const Matrix user = FromD2D(s.transform);
  ComPtr<ID2D1Effect> effect;
  if (s.shadow.color.a > 0 && SUCCEEDED(dc_->CreateEffect(CLSID_D2D1Shadow, &effect))) {
    effect->SetInput(0, s.recording.Get());
    // Office's blur radius is about two standard deviations of a Gaussian blur.
    const float sigma = std::clamp(s.shadow.blur * (user.ScaleX() + user.ScaleY()) / 4, 0.f, 250.f);
    effect->SetValue(D2D1_SHADOW_PROP_BLUR_STANDARD_DEVIATION, sigma);
    const Color& c = s.shadow.color;
    effect->SetValue(D2D1_SHADOW_PROP_COLOR, D2D1::Vector4F(c.r, c.g, c.b, c.a));
    dc_->SetTransform(ToD2D(user.Inverse() * s.shadow.transform * user));
    dc_->DrawImage(effect.Get());
  }
  dc_->SetTransform(D2D1::IdentityMatrix());
  dc_->DrawImage(s.recording.Get());
}

void D2DPainter::PopTo(size_t depth) {
  while (pushed_.size() > depth) {
    if (pushed_.back() == Pushed::AxisClip) dc_->PopAxisAlignedClip();
    else dc_->PopLayer();
    pushed_.pop_back();
  }
}

void D2DPainter::Transform(const Matrix& m) {
  current_ = ToD2D(m * FromD2D(current_));
  dc_->SetTransform(current_);
}

Matrix D2DPainter::CurrentTransform() const { return FromD2D(current_); }

void D2DPainter::ClipRect(const Rect& r) {
  if (!FromD2D(current_).AxisAligned()) {
    ClipPath(Path::FromRect(r));
    return;
  }
  dc_->PushAxisAlignedClip(RectF(r), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
  pushed_.push_back(Pushed::AxisClip);
}

void D2DPainter::ClipPath(const Path& p) {
  // An empty path clips everything away; a layer needs a geometry, so use an empty rectangle.
  ComPtr<ID2D1PathGeometry> g = MakeGeometry(p.Empty() ? Path::FromRect({}) : p);
  if (!g) return;
  // The mask is in the current coordinates: the layer applies the world transform to it.
  dc_->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), g.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                        D2D1::IdentityMatrix(), 1.f, nullptr, D2D1_LAYER_OPTIONS1_NONE),
                 nullptr);
  pushed_.push_back(Pushed::Layer);
}

void D2DPainter::BeginGroup(float opacity) {
  Save();
  dc_->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                        D2D1::IdentityMatrix(), std::clamp(opacity, 0.f, 1.f), nullptr,
                                        D2D1_LAYER_OPTIONS1_NONE),
                 nullptr);
  pushed_.push_back(Pushed::Layer);
}

ID2D1Brush* D2DPainter::MakeBrush(const Brush& b) {
  if (b.kind == Brush::Kind::Bitmap) {
    ID2D1Bitmap* bitmap = b.bitmap ? cache_->Get(dc_, *b.bitmap) : nullptr;
    ComPtr<ID2D1BitmapBrush1> brush;
    if (bitmap &&
        SUCCEEDED(dc_->CreateBitmapBrush(
            bitmap,
            D2D1::BitmapBrushProperties1(Extend(b.extendX), Extend(b.extendY),
                                         b.smooth ? D2D1_INTERPOLATION_MODE_LINEAR
                                                  : D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR),
            D2D1::BrushProperties(1.f, ToD2D(b.bitmapTransform)), &brush))) {
      gradient_ = brush;
      return gradient_.Get();
    }
    return MakeBrush(Brush::Solid({0, 0, 0, 0}));
  }
  if (b.kind == Brush::Kind::Solid || b.stops.empty()) {
    D2D1_COLOR_F c = ColorF(b.kind == Brush::Kind::Solid ? b.color : Color{0, 0, 0, 0});
    if (!solid_) dc_->CreateSolidColorBrush(c, &solid_);
    else solid_->SetColor(c);
    return solid_.Get();
  }
  std::vector<D2D1_GRADIENT_STOP> stops;
  for (const auto& s : b.stops) stops.push_back({s.offset, ColorF(s.color)});
  ComPtr<ID2D1GradientStopCollection> collection;
  if (FAILED(dc_->CreateGradientStopCollection(stops.data(), static_cast<UINT32>(stops.size()), D2D1_GAMMA_2_2,
                                               D2D1_EXTEND_MODE_CLAMP, &collection))) {
    return nullptr;
  }
  gradient_.Reset();
  if (b.kind == Brush::Kind::Linear) {
    ComPtr<ID2D1LinearGradientBrush> lin;
    dc_->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(Pt(b.start), Pt(b.end)), collection.Get(), &lin);
    gradient_ = lin;
  } else {
    ComPtr<ID2D1RadialGradientBrush> rad;
    dc_->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(Pt(b.center), Pt(b.focusOffset), b.rx, b.ry),
                                   collection.Get(), &rad);
    gradient_ = rad;
  }
  return gradient_.Get();
}

ComPtr<ID2D1PathGeometry> D2DPainter::MakeGeometry(const Path& p) {
  ComPtr<ID2D1PathGeometry> g;
  ComPtr<ID2D1GeometrySink> sink;
  if (FAILED(factory_->CreatePathGeometry(&g)) || FAILED(g->Open(&sink))) return nullptr;
  sink->SetFillMode(p.fillRule == Path::FillRule::EvenOdd ? D2D1_FILL_MODE_ALTERNATE : D2D1_FILL_MODE_WINDING);
  bool open = false;
  D2D1_POINT_2F last{}, figureStart{};
  auto begin = [&](D2D1_POINT_2F at) {
    sink->BeginFigure(at, D2D1_FIGURE_BEGIN_FILLED);
    open = true;
    figureStart = last = at;
  };
  for (const auto& s : p.Segments()) {
    if (s.kind == Path::Kind::Move) {
      if (open) sink->EndFigure(D2D1_FIGURE_END_OPEN);
      begin(Pt(s.p[0]));
      continue;
    }
    if (s.kind == Path::Kind::Close) {
      if (open) sink->EndFigure(D2D1_FIGURE_END_CLOSED);
      open = false;
      last = figureStart;
      continue;
    }
    if (!open) begin(last);
    switch (s.kind) {
      case Path::Kind::Line:
        sink->AddLine(last = Pt(s.p[0]));
        break;
      case Path::Kind::Quad:
        sink->AddQuadraticBezier({Pt(s.p[0]), Pt(s.p[1])});
        last = Pt(s.p[1]);
        break;
      case Path::Kind::Cubic:
        sink->AddBezier({Pt(s.p[0]), Pt(s.p[1]), Pt(s.p[2])});
        last = Pt(s.p[2]);
        break;
      case Path::Kind::Arc:
        sink->AddArc({Pt(s.p[0]), {s.rx, s.ry}, s.rotation,
                      s.clockwise ? D2D1_SWEEP_DIRECTION_CLOCKWISE : D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE,
                      s.largeArc ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL});
        last = Pt(s.p[0]);
        break;
      default:
        break;
    }
  }
  if (open) sink->EndFigure(D2D1_FIGURE_END_OPEN);
  if (FAILED(sink->Close())) return nullptr;
  return g;
}

ID2D1StrokeStyle* D2DPainter::MakeStrokeStyle(const Pen& pen) {
  if (stroke_ && SameStroke(pen, strokeKey_)) return stroke_.Get();
  D2D1_STROKE_STYLE_PROPERTIES props = D2D1::StrokeStyleProperties(
      Cap(pen.cap), Cap(pen.cap), Cap(pen.cap), Join(pen.join), pen.miterLimit,
      pen.dashes.empty() ? D2D1_DASH_STYLE_SOLID : D2D1_DASH_STYLE_CUSTOM, pen.dashOffset);
  stroke_.Reset();
  factory_->CreateStrokeStyle(props, pen.dashes.empty() ? nullptr : pen.dashes.data(),
                              static_cast<UINT32>(pen.dashes.size()), &stroke_);
  strokeKey_ = pen;
  return stroke_.Get();
}

void D2DPainter::FillPath(const Path& p, const Brush& b) {
  if (p.Empty()) return;
  ComPtr<ID2D1PathGeometry> g = MakeGeometry(p);
  ID2D1Brush* brush = MakeBrush(b);
  if (g && brush) dc_->FillGeometry(g.Get(), brush);
}

void D2DPainter::StrokePath(const Path& p, const Pen& pen) {
  if (p.Empty() || pen.width <= 0) return;
  ComPtr<ID2D1PathGeometry> g = MakeGeometry(p);
  ID2D1Brush* brush = MakeBrush(pen.brush);
  if (g && brush) dc_->DrawGeometry(g.Get(), brush, pen.width, MakeStrokeStyle(pen));
}

void D2DPainter::FillRect(const Rect& r, const Brush& b) {
  if (ID2D1Brush* brush = MakeBrush(b)) dc_->FillRectangle(RectF(r), brush);
}

void D2DPainter::DrawGlyphs(Point origin, const GlyphRun& run, const Brush& b) {
  if (!run.face || run.count == 0) return;
  DWRITE_GLYPH_RUN g{run.face, run.emSize, run.count, run.indices, run.advances, run.offsets, run.sideways,
                     run.bidiLevel};
  if (ID2D1Brush* brush = MakeBrush(b)) dc_->DrawGlyphRun(Pt(origin), &g, brush, DWRITE_MEASURING_MODE_NATURAL);
}

void D2DPainter::DrawBitmap(const Bitmap& bitmap, const Rect& dest, const Rect* src, float opacity) {
  ID2D1Bitmap* b = cache_->Get(dc_, bitmap);
  if (!b) return;
  D2D1_RECT_F d = RectF(dest), s{};
  if (src) s = RectF(*src);
  dc_->DrawBitmap(b, &d, std::clamp(opacity, 0.f, 1.f), D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC,
                  src ? &s : nullptr, nullptr);
}

void D2DPainter::DrawVectorImage(const VectorImage& image, const Rect& dest, float opacity) {
  IUnknown* object = cache_->Get(dc_, image);
  if (!object || dest.Width() == 0 || dest.Height() == 0) return;
  ComPtr<ID2D1GdiMetafile> metafile;
  ComPtr<ID2D1SvgDocument> svg;
  ComPtr<ID2D1DeviceContext5> dc5;
  D2D1_RECT_F from{};
  if (SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(&metafile)))) {
    if (FAILED(metafile->GetBounds(&from))) return;
  } else if (SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(&svg))) &&
             SUCCEEDED(dc_->QueryInterface(IID_PPV_ARGS(&dc5)))) {
    const D2D1_SIZE_F v = svg->GetViewportSize();
    from = {0, 0, v.width, v.height};
  } else {
    return;
  }
  if (from.right - from.left <= 0 || from.bottom - from.top <= 0) return;
  Save();
  if (opacity < 1) BeginGroup(opacity);
  ClipRect(dest);
  Transform(Matrix::Translate(-from.left, -from.top) *
            Matrix::Scale(dest.Width() / (from.right - from.left), dest.Height() / (from.bottom - from.top)) *
            Matrix::Translate(dest.left, dest.top));
  if (metafile) dc_->DrawGdiMetafile(metafile.Get(), nullptr);
  else dc5->DrawSvgDocument(svg.Get());
  if (opacity < 1) Restore();
  Restore();
}

}  // namespace draw
