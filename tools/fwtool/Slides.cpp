// fwtool slides (P1): draws decks with the slide renderer (src/slide/render) into a PDF per deck,
// through the same Painter as the screen, for `fwtool pdf-compare` against PowerPoint's reference
// PDFs (plan §10 "Pixels"). --png also writes each slide as a PNG at --scale pixels per point.
// Prints how long each deck takes to reach its first slide (plan §11: < 500 ms for 50 slides):
// reading the package and the presentation, then preparing and drawing slide 1.
#include <windows.h>

#include <d2d1_1.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "Common.h"
#include "core/Strings.h"
#include "draw/Pdf.h"
#include "slide/Presentation.h"
#include "slide/render/SlideRender.h"
#include "text/Fonts.h"

using Microsoft::WRL::ComPtr;

namespace fwtool {
namespace {

double Ms(const LARGE_INTEGER& from) {
  LARGE_INTEGER now, freq;
  QueryPerformanceCounter(&now);
  QueryPerformanceFrequency(&freq);
  return (now.QuadPart - from.QuadPart) * 1000.0 / freq.QuadPart;
}

bool SavePng(const std::filesystem::path& file, const draw::Bitmap& b) {
  ComPtr<IWICImagingFactory> wic;
  ComPtr<IWICStream> stream;
  ComPtr<IWICBitmapEncoder> encoder;
  ComPtr<IWICBitmapFrameEncode> frame;
  WICPixelFormatGUID format = GUID_WICPixelFormat32bppPBGRA;
  return SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
         SUCCEEDED(wic->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(file.c_str(), GENERIC_WRITE)) &&
         SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
         SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) && SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) &&
         SUCCEEDED(frame->Initialize(nullptr)) && SUCCEEDED(frame->SetSize(b.Width(), b.Height())) &&
         SUCCEEDED(frame->SetPixelFormat(&format)) &&
         SUCCEEDED(frame->WritePixels(b.Height(), b.Stride(), b.Stride() * b.Height(), const_cast<BYTE*>(b.Pixels()))) &&
         SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
}

}  // namespace

int RunSlides(const Options& options) {
  if (options.outDir.empty()) {
    std::fputs("slides needs --out <folder>.\n", stderr);
    return 2;
  }
  std::error_code ec;
  std::filesystem::create_directories(options.outDir, ec);
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  int failures = 0;
  for (const auto& file : CollectFiles(options.inputs)) {
    const std::wstring ext = fw::ToLower(file.extension().wstring());
    if (!ext.starts_with(L".ppt") && !ext.starts_with(L".pot") && !ext.starts_with(L".pps")) continue;
    std::printf("%s\n", Display(file).c_str());
    LARGE_INTEGER start;
    QueryPerformanceCounter(&start);
    fw::pkg::Status st;
    auto pkg = OpenPackage(file, st);
    std::unique_ptr<fw::slide::Presentation> deck = pkg ? fw::slide::Read(std::move(pkg), st) : nullptr;
    if (!deck) {
      std::printf("  refused: %s\n", st.message.c_str());
      ++failures;
      continue;
    }
    const double read = Ms(start);
    fw::text::Fonts fonts;
    fw::slide::render::UseDeckFonts(fonts, *deck, options.png);  // a PDF keeps fonts only its viewer draws
    std::unique_ptr<fw::slide::render::SlideRender> first;
    double firstMs = 0, preparedMs = 0;
    if (deck->SlideCount() > 0) {
      first = fw::slide::render::SlideRender::Make(*deck, *deck->GetSlide(0), fonts);
      preparedMs = Ms(start);
      // As the slide view shows it: a slide's worth of pixels at 96 dpi.
      const draw::Size size = first->Size();
      fw::slide::render::RenderBitmap(*first, static_cast<int>(size.width * 4 / 3), static_cast<int>(size.height * 4 / 3));
      firstMs = Ms(start);
    }
    std::printf("  %zu slides; read %.0f ms, first slide prepared %.0f ms, shown %.0f ms\n", deck->SlideCount(), read,
                preparedMs, firstMs);
    draw::PdfDocument pdf;
    for (size_t i = 0; i < deck->SlideCount(); ++i) {
      std::unique_ptr<fw::slide::render::SlideRender> s =
          i == 0 && first ? std::move(first) : fw::slide::render::SlideRender::Make(*deck, *deck->GetSlide(i), fonts);
      const draw::Size size = s->Size();
      pdf.AddPage(size.width, size.height, [&](draw::Painter& p) { s->Draw(p, {0, 0, size.width, size.height}); });
      if (options.png) {
        auto b = fw::slide::render::RenderBitmap(*s, static_cast<int>(size.width * options.scale + 0.5f),
                                                 static_cast<int>(size.height * options.scale + 0.5f));
        const auto name = options.outDir / (file.stem().wstring() + L"-slide" + std::to_wstring(i + 1) + L".png");
        if (!b || !SavePng(name, *b)) {
          std::printf("  slide %zu: couldn't render\n", i + 1);
          ++failures;
        }
      }
    }
    const std::vector<uint8_t> bytes = pdf.Finish();
    std::ofstream(options.outDir / (file.filename().wstring() + L".pdf"), std::ios::binary | std::ios::trunc)
        .write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    for (const fw::text::Substitution& s : fonts.Substitutions())
      std::printf("  font %s drawn as %s\n", fw::WideToUtf8(s.requested).c_str(), fw::WideToUtf8(s.used).c_str());
  }
  return failures ? 1 : 0;
}

}  // namespace fwtool
