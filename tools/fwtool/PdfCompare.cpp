// fwtool pdf-compare (S3, plan §10 "Pixels"): two PDFs of the same pages, as a rule Excel's export
// and Featherweight's Export PDF of one workbook. Both are rendered by PDFium at 100 dpi on white,
// each page at its own size with the top-left corners aligned (the smaller padded with white), and
// compared page by page with structural similarity: standard SSIM on the gray image (11 x 11
// Gaussian windows, sigma 1.5, K1 0.01, K2 0.03) over the whole page. A page passes at >= 0.95
// (S1's threshold). Also printed: SSIM inside the box both pages' ink fills, for information:
// blank paper scores a perfect 1, so a sparse page can pass with different text (two different
// fixtures' pages: 0.96 whole, 0.70 inside the ink; the same fixture's: 0.99 and 0.92, its text a
// pixel apart). Wrong values are the fixtures' recalculation check's to find. A page one
// PDF has and the other doesn't fails. --out DIR writes each page that fails as the reference's
// render, ours and where they differ, side by side; --json the numbers.
//
// With --pngs DIR and only the reference PDF (P1's slides): ours are the pages the app drew,
// DIR\page-1.png, page-2.png, ... (the script op pagepngs:DIR|100, which draws each slide as the
// screen does, hidden ones too), at 100 dpi; the first missing number ends them.
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <fpdfview.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "Common.h"
#include "Reference.h"
#include "core/Strings.h"

using Microsoft::WRL::ComPtr;

namespace fwtool {
namespace {

constexpr double kMinSsim = 0.95;
constexpr double kDpi = 100;

struct Image {
  int width = 0, height = 0;
  std::vector<uint8_t> px;  // gray
};

bool ReadBytes(const std::filesystem::path& path, std::string& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return true;
}

// The page as PDFium draws it at 100 dpi, on white.
bool Render(FPDF_DOCUMENT doc, int index, Image& out) {
  FPDF_PAGE page = FPDF_LoadPage(doc, index);
  if (!page) return false;
  out.width = std::max(1, static_cast<int>(std::lround(FPDF_GetPageWidthF(page) * kDpi / 72)));
  out.height = std::max(1, static_cast<int>(std::lround(FPDF_GetPageHeightF(page) * kDpi / 72)));
  FPDF_BITMAP bitmap = FPDFBitmap_Create(out.width, out.height, 0);
  if (!bitmap) {
    FPDF_ClosePage(page);
    return false;
  }
  FPDFBitmap_FillRect(bitmap, 0, 0, out.width, out.height, 0xFFFFFFFF);
  FPDF_RenderPageBitmap(bitmap, page, 0, 0, out.width, out.height, 0, FPDF_ANNOT);
  const auto* data = static_cast<const uint8_t*>(FPDFBitmap_GetBuffer(bitmap));
  const int stride = FPDFBitmap_GetStride(bitmap);
  out.px.resize(static_cast<size_t>(out.width) * out.height);
  for (int y = 0; y < out.height; ++y)
    for (int x = 0; x < out.width; ++x) {
      const uint8_t* p = data + static_cast<size_t>(y) * stride + x * 4;  // BGRx
      out.px[static_cast<size_t>(y) * out.width + x] = static_cast<uint8_t>((p[2] * 299 + p[1] * 587 + p[0] * 114) / 1000);
    }
  FPDFBitmap_Destroy(bitmap);
  FPDF_ClosePage(page);
  return true;
}

// A PNG the app drew (pagepngs:), as gray on white (any transparency over white).
bool LoadPng(const std::filesystem::path& file, Image& out) {
  ComPtr<IWICImagingFactory> wic;
  ComPtr<IWICBitmapDecoder> decoder;
  ComPtr<IWICBitmapFrameDecode> frame;
  ComPtr<IWICFormatConverter> converter;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) ||
      FAILED(wic->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)) ||
      FAILED(decoder->GetFrame(0, &frame)) || FAILED(wic->CreateFormatConverter(&converter)) ||
      FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                   WICBitmapPaletteTypeCustom)))
    return false;
  UINT w = 0, h = 0;
  if (FAILED(converter->GetSize(&w, &h)) || w == 0 || h == 0 || w > 20000 || h > 20000) return false;
  std::vector<uint8_t> bgra(static_cast<size_t>(w) * h * 4);
  if (FAILED(converter->CopyPixels(nullptr, w * 4, static_cast<UINT>(bgra.size()), bgra.data()))) return false;
  out.width = static_cast<int>(w), out.height = static_cast<int>(h);
  out.px.resize(static_cast<size_t>(w) * h);
  for (size_t i = 0; i < out.px.size(); ++i) {
    const uint8_t* p = &bgra[i * 4];
    const int gray = (p[2] * 299 + p[1] * 587 + p[0] * 114) / 1000;
    out.px[i] = static_cast<uint8_t>((gray * p[3] + 255 * (255 - p[3])) / 255);
  }
  return true;
}

// The image grown to w x h with white, its top-left corner kept.
Image Padded(const Image& a, int w, int h) {
  Image out{w, h, std::vector<uint8_t>(static_cast<size_t>(w) * h, 255)};
  for (int y = 0; y < a.height; ++y) std::copy_n(&a.px[static_cast<size_t>(y) * a.width], a.width, &out.px[static_cast<size_t>(y) * w]);
  return out;
}

// The smallest box holding every inked pixel (darker than 200) of both pages, 16 pixels wider all
// round, clipped to the page: SSIM is measured there, since blank paper scores a perfect 1 and
// drowns the content (two different sparse sheets scored 0.96 over the whole page).
void InkBox(const Image& a, const Image& b, int& x0, int& y0, int& x1, int& y1) {
  x0 = a.width, y0 = a.height, x1 = -1, y1 = -1;
  for (const Image* im : {&a, &b})
    for (int y = 0; y < im->height; ++y)
      for (int x = 0; x < im->width; ++x)
        if (im->px[static_cast<size_t>(y) * im->width + x] < 200)
          x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x), y1 = std::max(y1, y);
  if (x1 < 0) x0 = y0 = 0, x1 = a.width - 1, y1 = a.height - 1;  // both blank
  x0 = std::max(0, x0 - 16), y0 = std::max(0, y0 - 16);
  x1 = std::min(a.width - 1, x1 + 16), y1 = std::min(a.height - 1, y1 + 16);
}

Image Crop(const Image& a, int x0, int y0, int x1, int y1) {
  Image out{x1 - x0 + 1, y1 - y0 + 1, {}};
  out.px.resize(static_cast<size_t>(out.width) * out.height);
  for (int y = 0; y < out.height; ++y)
    std::copy_n(&a.px[static_cast<size_t>(y + y0) * a.width + x0], out.width, &out.px[static_cast<size_t>(y) * out.width]);
  return out;
}

// The image blurred by the 11-tap Gaussian (sigma 1.5) of standard SSIM, across then down, the
// edges clamped.
std::vector<float> Gauss(const std::vector<float>& in, int w, int h) {
  static const std::vector<float> k = [] {
    std::vector<float> t(11);
    float s = 0;
    for (int i = 0; i < 11; ++i) s += t[i] = std::exp(-((i - 5) * (i - 5)) / (2 * 1.5f * 1.5f));
    for (float& v : t) v /= s;
    return t;
  }();
  std::vector<float> tmp(in.size()), out(in.size());
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      float s = 0;
      for (int i = -5; i <= 5; ++i) s += k[i + 5] * in[static_cast<size_t>(y) * w + std::clamp(x + i, 0, w - 1)];
      tmp[static_cast<size_t>(y) * w + x] = s;
    }
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      float s = 0;
      for (int i = -5; i <= 5; ++i) s += k[i + 5] * tmp[static_cast<size_t>(std::clamp(y + i, 0, h - 1)) * w + x];
      out[static_cast<size_t>(y) * w + x] = s;
    }
  return out;
}

// Mean SSIM, as Wang et al. define it: 11 x 11 Gaussian windows (sigma 1.5), K1 0.01, K2 0.03.
double Ssim(const Image& a, const Image& b) {
  constexpr float c1 = (0.01f * 255) * (0.01f * 255), c2 = (0.03f * 255) * (0.03f * 255);
  const int w = a.width, h = a.height;
  const size_t n = static_cast<size_t>(w) * h;
  std::vector<float> fa(n), fb(n), aa(n), bb(n), ab(n);
  for (size_t i = 0; i < n; ++i) {
    fa[i] = a.px[i], fb[i] = b.px[i];
    aa[i] = fa[i] * fa[i], bb[i] = fb[i] * fb[i], ab[i] = fa[i] * fb[i];
  }
  const std::vector<float> ma = Gauss(fa, w, h), mb = Gauss(fb, w, h), saa = Gauss(aa, w, h), sbb = Gauss(bb, w, h),
                           sab = Gauss(ab, w, h);
  double sum = 0;
  for (size_t i = 0; i < n; ++i) {
    const float va = saa[i] - ma[i] * ma[i], vb = sbb[i] - mb[i] * mb[i], cov = sab[i] - ma[i] * mb[i];
    sum += ((2 * ma[i] * mb[i] + c1) * (2 * cov + c2)) / ((ma[i] * ma[i] + mb[i] * mb[i] + c1) * (va + vb + c2));
  }
  return n ? sum / static_cast<double>(n) : 1.0;
}

bool WritePng(const std::filesystem::path& file, const Image& image) {
  ComPtr<IWICImagingFactory> wic;
  ComPtr<IWICStream> stream;
  ComPtr<IWICBitmapEncoder> encoder;
  ComPtr<IWICBitmapFrameEncode> frame;
  WICPixelFormatGUID format = GUID_WICPixelFormat8bppGray;
  return SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
         SUCCEEDED(wic->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(file.c_str(), GENERIC_WRITE)) &&
         SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
         SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
         SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
         SUCCEEDED(frame->SetSize(static_cast<UINT>(image.width), static_cast<UINT>(image.height))) &&
         SUCCEEDED(frame->SetPixelFormat(&format)) &&
         SUCCEEDED(frame->WritePixels(static_cast<UINT>(image.height), static_cast<UINT>(image.width),
                                      static_cast<UINT>(image.px.size()), const_cast<BYTE*>(image.px.data()))) &&
         SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
}

// The reference, ours, and where they differ (dark), side by side.
void WriteDifference(const std::filesystem::path& file, const Image& a, const Image& b) {
  Image out{a.width * 3, a.height, std::vector<uint8_t>(static_cast<size_t>(a.width) * 3 * a.height, 255)};
  for (int y = 0; y < a.height; ++y)
    for (int x = 0; x < a.width; ++x) {
      const size_t at = static_cast<size_t>(y) * a.width + x, row = static_cast<size_t>(y) * out.width;
      out.px[row + x] = a.px[at];
      out.px[row + a.width + x] = b.px[at];
      out.px[row + 2 * a.width + x] = static_cast<uint8_t>(255 - std::abs(a.px[at] - b.px[at]));
    }
  WritePng(file, out);
}

}  // namespace

int RunPdfCompare(const Options& options) {
  const bool pngs = !options.pngs.empty();
  if (options.inputs.size() != (pngs ? 1u : 2u)) {
    std::fprintf(stderr, pngs ? "pdf-compare --pngs DIR takes one PDF: the reference (PowerPoint's export)\n"
                              : "pdf-compare takes two PDFs: the reference (Excel's export) and ours\n");
    return 2;
  }
  std::string error;
  if (!InitPdfium(error)) return std::fprintf(stderr, "%s\n", error.c_str()), 2;
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  std::string bytes[2];
  FPDF_DOCUMENT docs[2] = {};
  for (size_t i = 0; i < options.inputs.size(); ++i) {
    if (!ReadBytes(options.inputs[i], bytes[i])) return std::fprintf(stderr, "Can't read %s\n", Display(options.inputs[i]).c_str()), 2;
    docs[i] = FPDF_LoadMemDocument64(bytes[i].data(), bytes[i].size(), nullptr);
    if (!docs[i]) return std::fprintf(stderr, "PDFium can't read %s\n", Display(options.inputs[i]).c_str()), 2;
  }
  // Ours as the app drew them: page-1.png on, up to the first number that's missing.
  std::vector<std::filesystem::path> drawn;
  for (int i = 1; pngs; ++i) {
    std::filesystem::path png = options.pngs / (L"page-" + std::to_wstring(i) + L".png");
    if (!std::filesystem::exists(png)) break;
    drawn.push_back(png);
  }
  const std::wstring stem = pngs ? options.inputs[0].stem().wstring() : options.inputs[1].stem().wstring();
  if (!options.outDir.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(options.outDir, ec);
  }
  const int counts[2] = {FPDF_GetPageCount(docs[0]), pngs ? static_cast<int>(drawn.size()) : FPDF_GetPageCount(docs[1])};
  const int pages = std::max(counts[0], counts[1]);
  int passing = 0;
  std::string json;
  for (int p = 0; p < pages; ++p) {
    double ssim = 0, content = 0;
    std::string note;
    if (p >= counts[0] || p >= counts[1]) {
      note = p >= counts[0] ? "only in ours" : "only in the reference";
    } else {
      Image a, b;
      if (!Render(docs[0], p, a) || !(pngs ? LoadPng(drawn[p], b) : Render(docs[1], p, b))) {
        note = pngs ? "can't render the reference or read our PNG" : "can't render";
      } else {
        if (a.width != b.width || a.height != b.height) note = "sizes " + std::to_string(a.width) + "x" + std::to_string(a.height) + " and " +
                                                               std::to_string(b.width) + "x" + std::to_string(b.height) + " px";
        const int w = std::max(a.width, b.width), h = std::max(a.height, b.height);
        const Image pa = Padded(a, w, h), pb = Padded(b, w, h);
        int x0, y0, x1, y1;
        InkBox(pa, pb, x0, y0, x1, y1);
        ssim = Ssim(pa, pb);
        content = Ssim(Crop(pa, x0, y0, x1, y1), Crop(pb, x0, y0, x1, y1));
        if (ssim < kMinSsim && !options.outDir.empty())
          WriteDifference(options.outDir / (stem + L"-p" + std::to_wstring(p + 1) + L".png"), pa, pb);
      }
    }
    const bool ok = ssim >= kMinSsim;
    passing += ok ? 1 : 0;
    std::printf("  page %d: SSIM %.4f %s (inside the ink: %.4f)%s%s\n", p + 1, ssim, ok ? "ok" : "FAILS", content,
                note.empty() ? "" : ", ", note.c_str());
    json += std::string(json.empty() ? "" : ",\n") + "    {\"page\": " + std::to_string(p + 1) + ", \"ssim\": " + std::to_string(ssim) +
            ", \"inkSsim\": " + std::to_string(content) + ", \"ok\": " + (ok ? "true" : "false") + "}";
  }
  for (FPDF_DOCUMENT d : docs)
    if (d) FPDF_CloseDocument(d);
  std::printf("%d of %d pages at SSIM >= %.2f (%d pages in the reference, %d in ours)\n", passing, pages, kMinSsim, counts[0], counts[1]);
  if (!options.jsonOut.empty())
    std::ofstream(options.jsonOut, std::ios::trunc) << "{\n  \"minSsim\": " << kMinSsim << ", \"passing\": " << passing
                                                    << ", \"referencePages\": " << counts[0] << ", \"oursPages\": " << counts[1]
                                                    << ",\n  \"pages\": [\n" << json << "\n  ]\n}\n";
  return passing == pages && pages > 0 ? 0 : 1;
}

}  // namespace fwtool
