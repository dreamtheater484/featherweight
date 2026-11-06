// fwtool print-sample: prints two test pages through Painter and Direct2D's print control
// (plan §4.6), to check vector printing end to end without the app.
#include <windows.h>

#include <d2d1_1.h>
#include <dwrite.h>
#include <shlwapi.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "Common.h"
#include "core/Strings.h"
#include "draw/D2DDevice.h"
#include "draw/PrintJob.h"

#pragma comment(lib, "shlwapi.lib")

using Microsoft::WRL::ComPtr;
using namespace draw;

namespace fwtool {
namespace {

// Glyphs for plain text, one per character, with the font's own advances. Real text layout comes
// with fwtext; this is enough for a test page.
struct ShapedText {
  std::vector<UINT16> glyphs;
  std::vector<float> advances;
  GlyphRun run;
};

ShapedText Shape(IDWriteFontFace* face, const std::wstring& text, float size) {
  ShapedText t;
  std::vector<UINT32> cps(text.begin(), text.end());
  t.glyphs.resize(cps.size());
  face->GetGlyphIndices(cps.data(), static_cast<UINT32>(cps.size()), t.glyphs.data());
  std::vector<DWRITE_GLYPH_METRICS> metrics(cps.size());
  face->GetDesignGlyphMetrics(t.glyphs.data(), static_cast<UINT32>(t.glyphs.size()), metrics.data(), FALSE);
  DWRITE_FONT_METRICS fm{};
  face->GetMetrics(&fm);
  for (const auto& m : metrics) t.advances.push_back(m.advanceWidth * size / fm.designUnitsPerEm);
  t.run.face = face;
  t.run.emSize = size;
  t.run.count = static_cast<uint32_t>(t.glyphs.size());
  t.run.indices = t.glyphs.data();
  t.run.advances = t.advances.data();
  return t;
}

ComPtr<IDWriteFontFace> FindFace(IDWriteFactory* dw, const wchar_t* family, DWRITE_FONT_WEIGHT weight) {
  ComPtr<IDWriteFontCollection> fonts;
  ComPtr<IDWriteFontFamily> fam;
  ComPtr<IDWriteFont> font;
  ComPtr<IDWriteFontFace> face;
  UINT32 index = 0;
  BOOL exists = FALSE;
  if (FAILED(dw->GetSystemFontCollection(&fonts)) || FAILED(fonts->FindFamilyName(family, &index, &exists)) ||
      !exists || FAILED(fonts->GetFontFamily(index, &fam)) ||
      FAILED(fam->GetFirstMatchingFont(weight, DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, &font)) ||
      FAILED(font->CreateFontFace(&face))) {
    return nullptr;
  }
  return face;
}

void Text(Painter& p, IDWriteFontFace* face, const std::wstring& s, float size, Point at, Color c) {
  if (!face) return;
  ShapedText t = Shape(face, s, size);
  p.DrawGlyphs(at, t.run, Brush::Solid(c));
}

// A 64 x 64 picture: a color ramp with a checkerboard, premultiplied BGRA.
Bitmap SamplePicture() {
  std::vector<uint8_t> px(64 * 64 * 4);
  for (int y = 0; y < 64; ++y)
    for (int x = 0; x < 64; ++x) {
      uint8_t* p = &px[(y * 64 + x) * 4];
      bool dark = ((x / 8) + (y / 8)) % 2 == 0;
      p[0] = static_cast<uint8_t>(dark ? 255 - y * 3 : 255);
      p[1] = static_cast<uint8_t>(dark ? x * 3 : 230);
      p[2] = static_cast<uint8_t>(dark ? 90 : 240);
      p[3] = 255;
    }
  return Bitmap(64, 64, std::move(px));
}

void DrawPage(Painter& p, IDWriteFontFace* regular, IDWriteFontFace* bold, const Bitmap& picture, float w, float h,
              int page) {
  const Color ink{0.1f, 0.1f, 0.15f, 1}, accent{0.29f, 0.36f, 0.93f, 1}, orange{0.95f, 0.55f, 0.1f, 1};
  Pen frame{Brush::Solid(ink), 0.75f};
  p.StrokePath(Path::FromRect({36, 36, w - 36, h - 36}), frame);
  Text(p, bold, L"Featherweight print sample", 24, {54, 80}, ink);
  Text(p, regular, L"Page " + std::to_wstring(page) + L" • vectors through Painter and ID2D1PrintControl", 11,
       {54, 100}, ink);

  // Fills: solid, linear and radial gradients.
  p.FillPath(Path::RoundedRect({54, 124, 204, 204}, 12, 12), Brush::Solid(accent));
  p.FillPath(Path::RoundedRect({222, 124, 372, 204}, 12, 12),
             Brush::Linear({222, 124}, {372, 204}, {{0, orange}, {1, accent}}));
  p.FillPath(Path::Ellipse({390, 124, 470, 204}),
             Brush::Radial({430, 164}, 40, 40, {{0, {1, 1, 1, 1}}, {1, orange}}));

  // Lines: widths, dashes, caps and joins.
  for (int i = 0; i < 4; ++i) {
    Path line;
    line.MoveTo({54, 232.f + i * 18});
    line.LineTo({300, 232.f + i * 18});
    Pen pen{Brush::Solid(ink), 0.5f + i * 1.5f};
    if (i == 1) pen.dashes = {4, 2};
    if (i == 2) {
      pen.dashes = {1, 2};
      pen.cap = LineCap::Round;
    }
    if (i == 3) pen.dashes = {6, 2, 1, 2};
    p.StrokePath(line, pen);
  }
  Path zigzag;
  zigzag.MoveTo({330, 290});
  for (int i = 1; i <= 6; ++i) zigzag.LineTo({330.f + i * 22, i % 2 ? 232.f : 290.f});
  Pen joins{Brush::Solid(accent), 6};
  joins.join = page == 1 ? LineJoin::Miter : LineJoin::Round;
  p.StrokePath(zigzag, joins);

  // Curves and fill rules: a star with a hole (even-odd), arcs.
  Path star;
  for (int i = 0; i < 5; ++i) {
    double a = -3.14159265 / 2 + i * 4 * 3.14159265 / 5;
    Point pt{static_cast<float>(130 + 60 * std::cos(a)), static_cast<float>(390 + 60 * std::sin(a))};
    if (i == 0) star.MoveTo(pt);
    else star.LineTo(pt);
  }
  star.Close();
  star.fillRule = Path::FillRule::EvenOdd;
  p.FillPath(star, Brush::Solid(orange));
  Path wave;
  wave.MoveTo({230, 390});
  wave.CubicTo({260, 330}, {300, 450}, {330, 390});
  wave.QuadTo({360, 340}, {390, 390});
  wave.ArcTo({450, 390}, 30, 30, 0, false, true);
  p.StrokePath(wave, Pen{Brush::Solid(ink), 2});

  // A picture, a clipped circle of text, and a rotated group at half opacity.
  p.DrawBitmap(picture, {54, 470, 182, 598});
  p.Save();
  p.ClipPath(Path::Ellipse({210, 470, 338, 598}));
  p.FillRect({210, 470, 338, 598}, Brush::Solid({0.93f, 0.94f, 1, 1}));
  for (int i = 0; i < 9; ++i) Text(p, regular, L"clipped clipped clipped", 12, {200, 484.f + i * 14}, accent);
  p.Restore();
  p.Save();
  p.Transform(Matrix::Rotate(-15) * Matrix::Translate(420, 534));
  p.BeginGroup(0.5f);
  p.FillRect({-50, -40, 30, 40}, Brush::Solid(accent));
  p.FillRect({-10, -40, 70, 40}, Brush::Solid(orange));
  p.Restore();
  p.Restore();

  Text(p, regular, std::to_wstring(static_cast<int>(w)) + L" × " + std::to_wstring(static_cast<int>(h)) + L" pt", 9,
       {54, h - 48}, ink);
}

}  // namespace

int RunPrintSample(const Options& options) {
  const std::filesystem::path& out = options.inputs.front();
  const std::wstring printer = options.printer.empty() ? L"Microsoft Print to PDF" : options.printer;
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

  ComPtr<ID2D1Factory1> factory;
  D2D1_FACTORY_OPTIONS fo{};
  ComPtr<ID3D11Device> d3d;
  ComPtr<ID2D1Device> device;
  ComPtr<IDWriteFactory> dw;
  HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &fo,
                                 reinterpret_cast<void**>(factory.GetAddressOf()));
  bool warp = options.warp;
  if (SUCCEEDED(hr) && (warp || FAILED(CreateD2DDevice(factory.Get(), false, d3d, device)))) {
    warp = true;
    hr = CreateD2DDevice(factory.Get(), true, d3d, device);
  }
  if (SUCCEEDED(hr))
    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                             reinterpret_cast<IUnknown**>(dw.GetAddressOf()));
  if (FAILED(hr)) {
    std::fprintf(stderr, "Couldn't create the Direct2D device (0x%08lX).\n", hr);
    return 1;
  }
  ComPtr<IDWriteFontFace> regular = FindFace(dw.Get(), L"Segoe UI", DWRITE_FONT_WEIGHT_REGULAR);
  ComPtr<IDWriteFontFace> bold = FindFace(dw.Get(), L"Segoe UI", DWRITE_FONT_WEIGHT_SEMI_BOLD);
  Bitmap picture = SamplePicture();

  ComPtr<IStream> stream;
  hr = SHCreateStreamOnFileEx(out.c_str(), STGM_CREATE | STGM_WRITE | STGM_SHARE_DENY_WRITE, FILE_ATTRIBUTE_NORMAL,
                              TRUE, nullptr, &stream);
  if (FAILED(hr)) {
    std::fprintf(stderr, "Couldn't create %s (0x%08lX).\n", Display(out).c_str(), hr);
    return 1;
  }
  PrintJob::Outcome outcome;
  int pages = 0;
  {
    PrintJob job;
    hr = job.Start(device.Get(), printer, nullptr, L"Featherweight print sample", stream.Get());
    // A4 portrait, then US Letter landscape.
    const float sizes[2][2] = {{595.3f, 841.9f}, {792, 612}};
    for (int i = 0; i < 2 && SUCCEEDED(hr); ++i) {
      float w = sizes[i][0], h = sizes[i][1];
      hr = job.AddPage(w, h, [&](Painter& p) { DrawPage(p, regular.Get(), bold.Get(), picture, w, h, i + 1); });
    }
    if (SUCCEEDED(hr)) hr = job.Finish();
    if (FAILED(hr)) {
      std::fprintf(stderr, "Printing to %s failed (0x%08lX).\n", fw::WideToUtf8(printer).c_str(), hr);
      return 1;
    }
    outcome = job.Wait(120000);
    pages = job.Pages();
  }
  stream.Reset();  // the job let go of it too, so the file is closed and complete
  const char* names[] = {"still in progress", "completed", "canceled", "failed"};
  std::error_code ec;
  auto size = std::filesystem::file_size(out, ec);
  std::printf("%s: %d pages to %s, %s, %llu bytes (%s)\n", Display(out).c_str(), pages,
              fw::WideToUtf8(printer).c_str(), names[static_cast<int>(outcome)],
              static_cast<unsigned long long>(ec ? 0 : size), warp ? "WARP" : "graphics adapter");
  return outcome == PrintJob::Outcome::Completed ? 0 : 1;
}

}  // namespace fwtool
