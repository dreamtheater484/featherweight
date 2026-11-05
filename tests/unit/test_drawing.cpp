// fwdraw's drawing formats: pictures, themes and colors, preset and custom geometry, DrawingML and
// VML shapes. Rendering goes through Direct2D's software renderer, as in test_draw.cpp.
#include "TestHarness.h"

#include <windows.h>

#include <wincodec.h>
#include <wrl/client.h>

#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "DrawTestUtil.h"
#include "draw/D2DPainter.h"
#include "draw/Drawing.h"
#include "draw/Geometry.h"
#include "draw/Images.h"
#include "draw/Theme.h"
#include "pkg/XmlDom.h"

using Microsoft::WRL::ComPtr;
using namespace draw;
using namespace drawtest;

namespace {

// Namespace declarations for XML snippets.
const std::string kNs =
    R"( xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main")"
    R"( xmlns:wp="http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing")"
    R"( xmlns:wps="http://schemas.microsoft.com/office/word/2010/wordprocessingShape")"
    R"( xmlns:wpg="http://schemas.microsoft.com/office/word/2010/wordprocessingGroup")"
    R"( xmlns:pic="http://schemas.openxmlformats.org/drawingml/2006/picture")"
    R"( xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main")"
    R"( xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships")"
    R"( xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main")"
    R"( xmlns:v="urn:schemas-microsoft-com:vml" xmlns:o="urn:schemas-microsoft-com:office:office")"
    R"( xmlns:mc="http://schemas.openxmlformats.org/markup-compatibility/2006")";

struct TestResources : DrawingResources {
  std::map<std::string, std::shared_ptr<const Image>, std::less<>> images;
  std::shared_ptr<const Theme> theme = Theme::Default();
  std::shared_ptr<const Theme> GetTheme() override { return theme; }
  std::shared_ptr<const Image> GetImage(std::string_view id) override {
    auto it = images.find(id);
    return it == images.end() ? nullptr : it->second;
  }
};

// Pixels as 0xAARRGGBB, top-down, encoded by WIC's built-in encoder for `container`.
std::vector<uint8_t> Encode(const GUID& container, int w, int h, const std::vector<uint32_t>& argb) {
  ComPtr<IWICImagingFactory> wic;
  ComPtr<IWICBitmap> bmp;
  ComPtr<IStream> stream;
  ComPtr<IWICBitmapEncoder> enc;
  ComPtr<IWICBitmapFrameEncode> frame;
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
  wic->CreateBitmapFromMemory(w, h, GUID_WICPixelFormat32bppBGRA, w * 4, static_cast<UINT>(argb.size() * 4),
                              reinterpret_cast<BYTE*>(const_cast<uint32_t*>(argb.data())), &bmp);
  CreateStreamOnHGlobal(nullptr, TRUE, &stream);
  wic->CreateEncoder(container, nullptr, &enc);
  enc->Initialize(stream.Get(), WICBitmapEncoderNoCache);
  enc->CreateNewFrame(&frame, nullptr);
  frame->Initialize(nullptr);
  frame->SetSize(w, h);
  WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
  if (container == GUID_ContainerFormatJpeg || container == GUID_ContainerFormatBmp) fmt = GUID_WICPixelFormat24bppBGR;
  if (container == GUID_ContainerFormatGif) fmt = GUID_WICPixelFormat8bppIndexed;
  frame->SetPixelFormat(&fmt);
  frame->WriteSource(bmp.Get(), nullptr);
  frame->Commit();
  enc->Commit();
  STATSTG st{};
  stream->Stat(&st, STATFLAG_NONAME);
  std::vector<uint8_t> out(static_cast<size_t>(st.cbSize.QuadPart));
  LARGE_INTEGER zero{};
  stream->Seek(zero, STREAM_SEEK_SET, nullptr);
  ULONG read = 0;
  stream->Read(out.data(), static_cast<ULONG>(out.size()), &read);
  return out;
}

// 16 x 16 in quarters: red, green / blue, white.
std::vector<uint8_t> QuadPng() {
  std::vector<uint32_t> px(16 * 16);
  for (int y = 0; y < 16; ++y)
    for (int x = 0; x < 16; ++x) px[y * 16 + x] = y < 8 ? (x < 8 ? 0xFFFF0000 : 0xFF00FF00) : (x < 8 ? 0xFF0000FF : 0xFFFFFFFF);
  return Encode(GUID_ContainerFormatPng, 16, 16, px);
}

std::vector<uint8_t> SolidImage(const GUID& container, uint32_t argb, int w = 8, int h = 8) {
  return Encode(container, w, h, std::vector<uint32_t>(static_cast<size_t>(w) * h, argb));
}

// An icon with one PNG image inside, as Windows Vista and later write them.
std::vector<uint8_t> IconOf(const std::vector<uint8_t>& png, int size) {
  std::vector<uint8_t> ico = {0, 0, 1, 0, 1, 0, static_cast<uint8_t>(size), static_cast<uint8_t>(size), 0, 0, 1, 0, 32, 0};
  const uint32_t len = static_cast<uint32_t>(png.size()), off = 22;
  for (uint32_t v : {len, off})
    for (int i = 0; i < 4; ++i) ico.push_back(static_cast<uint8_t>(v >> (8 * i)));
  ico.insert(ico.end(), png.begin(), png.end());
  return ico;
}

// A metafile 100 x 50 (0.01 mm) with its left half red and its right half blue, made by GDI.
std::vector<uint8_t> MakeEmf() {
  RECT frame{0, 0, 2000, 1000};  // 20 x 10 mm
  HDC dc = CreateEnhMetaFileW(nullptr, nullptr, &frame, nullptr);
  SetMapMode(dc, MM_ANISOTROPIC);
  SetWindowExtEx(dc, 200, 100, nullptr);
  // The header records the screen's real resolution; HORZRES is scaled in a process that isn't
  // DPI aware, so use DESKTOPHORZRES to match it.
  SetViewportExtEx(dc, GetDeviceCaps(dc, DESKTOPHORZRES) * 20 / GetDeviceCaps(dc, HORZSIZE),
                   GetDeviceCaps(dc, DESKTOPVERTRES) * 10 / GetDeviceCaps(dc, VERTSIZE), nullptr);
  HBRUSH red = CreateSolidBrush(RGB(255, 0, 0)), blue = CreateSolidBrush(RGB(0, 0, 255));
  RECT l{0, 0, 100, 100}, r{100, 0, 200, 100};
  FillRect(dc, &l, red);
  FillRect(dc, &r, blue);
  DeleteObject(red);
  DeleteObject(blue);
  HENHMETAFILE emf = CloseEnhMetaFile(dc);
  std::vector<uint8_t> bytes(GetEnhMetaFileBits(emf, 0, nullptr));
  GetEnhMetaFileBits(emf, static_cast<UINT>(bytes.size()), bytes.data());
  DeleteEnhMetaFile(emf);
  return bytes;
}

// A placeable WMF, 2 x 1 inches, left half red, right half blue.
std::vector<uint8_t> MakeWmf() {
  HDC dc = CreateMetaFileW(nullptr);
  SetWindowOrgEx(dc, 0, 0, nullptr);
  SetWindowExtEx(dc, 200, 100, nullptr);
  HBRUSH red = CreateSolidBrush(RGB(255, 0, 0)), blue = CreateSolidBrush(RGB(0, 0, 255));
  RECT l{0, 0, 100, 100}, r{100, 0, 200, 100};
  FillRect(dc, &l, red);
  FillRect(dc, &r, blue);
  HMETAFILE wmf = CloseMetaFile(dc);
  DeleteObject(red);
  DeleteObject(blue);
  std::vector<uint8_t> body(GetMetaFileBitsEx(wmf, 0, nullptr));
  GetMetaFileBitsEx(wmf, static_cast<UINT>(body.size()), body.data());
  DeleteMetaFile(wmf);
  std::vector<uint16_t> head = {0xCDD7, 0x9AC6, 0, 0, 0, 200, 100, 100, 0, 0};  // key, hmf, bbox, inch, reserved
  uint16_t sum = 0;
  for (uint16_t v : head) sum ^= v;
  head.push_back(sum);
  std::vector<uint8_t> out;
  for (uint16_t v : head) {
    out.push_back(static_cast<uint8_t>(v));
    out.push_back(static_cast<uint8_t>(v >> 8));
  }
  out.insert(out.end(), body.begin(), body.end());
  return out;
}

std::vector<uint8_t> Bytes(std::string_view s) { return std::vector<uint8_t>(s.begin(), s.end()); }

const char* kSvgHalves =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="40" height="20" viewBox="0 0 40 20">)"
    R"(<rect x="0" y="0" width="20" height="20" fill="#ff0000"/><rect x="20" y="0" width="20" height="20" fill="#0000ff"/></svg>)";

fw::pkg::XmlDom ParseXml(const std::string& xml) {
  fw::pkg::XmlDom dom;
  fw::pkg::Status st = dom.Parse(xml);
  CHECK(st.Ok());
  return dom;
}

// A Word anchor around one graphic, extent in points.
std::string WordAnchor(float w, float h, const std::string& uri, const std::string& graphic, const std::string& extra = "") {
  const std::string cx = std::to_string(static_cast<long long>(w * 12700)), cy = std::to_string(static_cast<long long>(h * 12700));
  return "<wp:anchor" + kNs + "><wp:extent cx=\"" + cx + "\" cy=\"" + cy + "\"/>" + extra +
         R"(<wp:docPr id="1" name="Shape 1" descr="Alt text"/><a:graphic><a:graphicData uri=")" + uri + "\">" + graphic +
         "</a:graphicData></a:graphic></wp:anchor>";
}

const std::string kWpsUri = "http://schemas.microsoft.com/office/word/2010/wordprocessingShape";
const std::string kPicUri = "http://schemas.openxmlformats.org/drawingml/2006/picture";
const std::string kWpgUri = "http://schemas.microsoft.com/office/word/2010/wordprocessingGroup";

std::string Wsp(const std::string& spPr, const std::string& rest = "") {
  return "<wps:wsp><wps:cNvSpPr/><wps:spPr>" + spPr + "</wps:spPr>" + rest + "<wps:bodyPr/></wps:wsp>";
}

std::string Xfrm(int x, int y, int w, int h, const std::string& attrs = "") {
  return "<a:xfrm" + attrs + "><a:off x=\"" + std::to_string(x * 12700) + "\" y=\"" + std::to_string(y * 12700) +
         "\"/><a:ext cx=\"" + std::to_string(w * 12700) + "\" cy=\"" + std::to_string(h * 12700) + "\"/></a:xfrm>";
}

std::unique_ptr<Drawing> WordDrawing(const std::string& xml, DrawingResources& res, fw::pkg::XmlDom& dom) {
  dom = ParseXml(xml);
  return Drawing::FromWordDrawing(dom.Root(), res);
}

std::unique_ptr<Drawing> VmlDrawing(const std::string& inner, DrawingResources& res, fw::pkg::XmlDom& dom) {
  dom = ParseXml("<w:pict" + kNs + ">" + inner + "</w:pict>");
  return Drawing::FromVml(dom.Root(), res);
}

// Draws `d` at 1 pixel per point, the extent at the origin.
void Render(Surface& s, const Drawing& d, float scale = 1) {
  D2DPainter p(s.dc.Get());
  p.Transform(Matrix::Scale(scale, scale));
  const Size e = d.Extent();
  d.Draw(p, {0, 0, e.width, e.height});
}

bool Near(uint32_t c, uint32_t want, int tolerance = 12) {
  return std::abs(Red(c) - Red(want)) <= tolerance && std::abs(Green(c) - Green(want)) <= tolerance &&
         std::abs(Blue(c) - Blue(want)) <= tolerance;
}

}  // namespace

// ---- Pictures ----

TEST(Images_SniffFormats) {
  auto sniff = [](const std::vector<uint8_t>& b) { return SniffImageFormat(b.data(), b.size()); };
  CHECK(sniff(QuadPng()) == ImageFormat::Png);
  CHECK(sniff(SolidImage(GUID_ContainerFormatJpeg, 0xFF808080)) == ImageFormat::Jpeg);
  CHECK(sniff(SolidImage(GUID_ContainerFormatGif, 0xFF808080)) == ImageFormat::Gif);
  CHECK(sniff(SolidImage(GUID_ContainerFormatBmp, 0xFF808080)) == ImageFormat::Bmp);
  CHECK(sniff(SolidImage(GUID_ContainerFormatTiff, 0xFF808080)) == ImageFormat::Tiff);
  CHECK(sniff(IconOf(QuadPng(), 16)) == ImageFormat::Ico);
  CHECK(sniff(MakeEmf()) == ImageFormat::Emf);
  CHECK(sniff(MakeWmf()) == ImageFormat::Wmf);
  CHECK(sniff(Bytes(kSvgHalves)) == ImageFormat::Svg);
  CHECK(sniff(Bytes("<?xml version=\"1.0\"?>\n<svg xmlns=\"http://www.w3.org/2000/svg\"/>")) == ImageFormat::Svg);
  CHECK(sniff(Bytes("not a picture")) == ImageFormat::Unknown);
}

TEST(Images_DecodeEveryRasterFormat) {
  struct Case {
    std::vector<uint8_t> bytes;
    ImageFormat format;
  };
  const Case cases[] = {{SolidImage(GUID_ContainerFormatPng, 0xFFFF0000), ImageFormat::Png},
                        {SolidImage(GUID_ContainerFormatJpeg, 0xFFFF0000), ImageFormat::Jpeg},
                        {SolidImage(GUID_ContainerFormatGif, 0xFFFF0000), ImageFormat::Gif},
                        {SolidImage(GUID_ContainerFormatBmp, 0xFFFF0000), ImageFormat::Bmp},
                        {SolidImage(GUID_ContainerFormatTiff, 0xFFFF0000), ImageFormat::Tiff},
                        {IconOf(SolidImage(GUID_ContainerFormatPng, 0xFFFF0000), 8), ImageFormat::Ico}};
  for (const Case& c : cases) {
    std::string error;
    std::shared_ptr<Image> img = Image::Open(c.bytes, ImageLimits::Default(), &error);
    CHECK(img != nullptr);
    if (!img) continue;
    CHECK(img->Format() == c.format);
    CHECK_EQ(img->Width(), 8);
    CHECK_EQ(img->Height(), 8);
    CHECK_NEAR(img->NaturalSize().width, 6, 0.01);  // 8 px at 96 dpi
    std::shared_ptr<const Bitmap> px = img->Pixels();
    CHECK(px != nullptr);
    if (!px) continue;
    const uint8_t* p = px->Pixels() + (4 * 8 + 4) * 4;  // BGRA
    CHECK(p[2] > 230 && p[1] < 30 && p[0] < 30 && p[3] == 255);
  }
}

TEST(Images_Limits) {
  std::vector<uint8_t> png = SolidImage(GUID_ContainerFormatPng, 0xFF00FF00, 64, 32);
  ImageLimits tight;
  tight.maxDimension = 32;
  std::string error;
  CHECK(Image::Open(png, tight, &error) == nullptr);
  CHECK(!error.empty());
  tight = ImageLimits{};
  tight.maxSourcePixels = 100;
  CHECK(Image::Open(png, tight) == nullptr);
  tight = ImageLimits{};
  tight.maxBytes = 10;
  CHECK(Image::Open(png, tight) == nullptr);
  // Over the decoded-pixel budget: scaled down while decoding, aspect kept.
  tight = ImageLimits{};
  tight.maxDecodedPixels = 64 * 32 / 16;
  std::shared_ptr<Image> img = Image::Open(png, tight);
  CHECK(img != nullptr);
  if (img) {
    std::shared_ptr<const Bitmap> px = img->Pixels();
    CHECK(px && px->Width() == 16 && px->Height() == 8);
  }
}

TEST(Images_AskedSizeDecodesLess) {
  // A 2048 x 1024 picture shown 300 px wide decodes at a power-of-two reduction above that.
  std::vector<uint8_t> png = SolidImage(GUID_ContainerFormatPng, 0xFF0000FF, 2048, 1024);
  std::shared_ptr<Image> img = Image::Open(png);
  CHECK(img != nullptr);
  if (!img) return;
  std::shared_ptr<const Bitmap> reduced = img->Pixels(300, 150);
  CHECK(reduced && reduced->Width() == 512 && reduced->Height() == 256);
  std::shared_ptr<const Bitmap> again = img->Pixels(200, 100);
  CHECK(again == reduced);  // kept
  std::shared_ptr<const Bitmap> big = img->Pixels(1500, 750);
  CHECK(big && big->Width() == 2048);
}

TEST(Images_PixelBudgetAndReader) {
  // Pictures opened from a reader keep no copy of their bytes: they read them again to decode.
  const std::vector<uint8_t> png = SolidImage(GUID_ContainerFormatPng, 0xFF336699, 64, 64);  // 16 KB decoded
  int readsA = 0, readsB = 0;
  std::shared_ptr<Image> a = Image::OpenFrom([&](std::vector<uint8_t>& out) {
    ++readsA;
    out = png;
    return true;
  });
  std::shared_ptr<Image> b = Image::OpenFrom([&](std::vector<uint8_t>& out) {
    ++readsB;
    out = png;
    return true;
  });
  CHECK(a && b && readsA == 1 && readsB == 1);
  if (!a || !b) return;
  // Under a 20 KB budget only one of them keeps its pixels: the one drawn last.
  SetPixelBudget(20000);
  CHECK(a->Pixels() != nullptr && readsA == 2);
  CHECK(a->Pixels() != nullptr && readsA == 2);  // kept
  CHECK(b->Pixels() != nullptr && readsB == 2);
  CHECK(PixelBytesKept() <= 20000);
  CHECK(a->Pixels() != nullptr && readsA == 3);  // dropped for b, decoded again
  SetPixelBudget(48ull << 20);
  CHECK(Image::OpenFrom([](std::vector<uint8_t>&) { return false; }) == nullptr);
}

TEST(Images_DamagedAndUnknown) {
  std::vector<uint8_t> png = QuadPng();
  png.resize(30);  // signature and part of the header
  CHECK(Image::Open(png) == nullptr);
  CHECK(Image::Open(Bytes("GIF89a garbage")) == nullptr);
  CHECK(Image::Open(Bytes("plain text")) == nullptr);
  CHECK(Image::Open({}) == nullptr);
  std::vector<uint8_t> emf = MakeEmf();
  emf[48] = 0xFF;  // nBytes beyond the data
  emf[49] = 0xFF;
  emf[50] = 0xFF;
  CHECK(Image::Open(emf) == nullptr);
  CHECK(Image::Open(Bytes("<svg xmlns=\"http://www.w3.org/2000/svg\"><rect></svg>")) == nullptr);
}

TEST(Images_Metafiles) {
  for (const std::vector<uint8_t>& bytes : {MakeEmf(), MakeWmf()}) {
    std::shared_ptr<Image> img = Image::Open(bytes);
    CHECK(img != nullptr);
    if (!img) continue;
    CHECK(img->Vector() != nullptr);
    CHECK(img->Vector() && img->Vector()->GetKind() == VectorImage::Kind::Emf);
    CHECK(img->NaturalSize().width > img->NaturalSize().height * 1.8f);
    Surface s(80, 40);
    {
      D2DPainter p(s.dc.Get());
      p.DrawVectorImage(*img->Vector(), {0, 0, 80, 40});
    }
    CHECK(s.End());
    CHECK(Near(s.At(20, 20), 0xFF0000, 40));
    CHECK(Near(s.At(60, 20), 0x0000FF, 40));
    // The default (Painter's) path: rasterized.
    std::shared_ptr<const Bitmap> px = RasterizeVectorImage(*img->Vector(), 40, 20);
    CHECK(px != nullptr);
    if (px) {
      const uint8_t* left = px->Pixels() + (10 * 40 + 8) * 4;
      CHECK(left[2] > 200 && left[0] < 60);
    }
  }
}

TEST(Images_Svg) {
  std::shared_ptr<Image> img = Image::Open(Bytes(kSvgHalves));
  CHECK(img != nullptr);
  if (!img) return;
  CHECK(img->Faithful());
  CHECK_NEAR(img->NaturalSize().width, 30, 0.01);  // 40 px
  Surface s(80, 40);
  {
    D2DPainter p(s.dc.Get());
    p.DrawVectorImage(*img->Vector(), {0, 0, 80, 40});
  }
  CHECK(s.End());
  CHECK(Near(s.At(20, 20), 0xFF0000, 40));
  CHECK(Near(s.At(60, 20), 0x0000FF, 40));
  // Text, or anything outside the document: Office's PNG copy is shown instead.
  std::shared_ptr<Image> text = Image::Open(
      Bytes(R"(<svg xmlns="http://www.w3.org/2000/svg" width="10" height="10"><text x="0" y="9">A</text></svg>)"));
  CHECK(text && !text->Faithful());
  std::shared_ptr<Image> remote = Image::Open(Bytes(
      R"(<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" width="10" height="10">)"
      R"(<image xlink:href="https://example.com/a.png" width="10" height="10"/></svg>)"));
  CHECK(remote && !remote->Faithful());
  // A DTD is refused, as in every other XML Featherweight reads.
  CHECK(Image::Open(Bytes("<!DOCTYPE svg [<!ENTITY a \"b\">]><svg xmlns=\"http://www.w3.org/2000/svg\"/>")) == nullptr);
}

// ---- Themes and colors ----

TEST(Theme_DefaultAndParse) {
  const Theme& def = *Theme::Default();
  Color a1 = def.GetColor(ThemeColor::Accent1);
  CHECK_NEAR(a1.r, 0x44 / 255.f, 1e-3);
  CHECK_EQ(def.MinorFont().latin, std::string("Calibri"));
  CHECK(def.Fill(1) && def.Fill(3) && !def.Fill(4) && def.Fill(1001) && def.Line(2) && def.Effect(3));
  std::shared_ptr<const Theme> t = Theme::Parse(
      R"(<a:theme xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main" name="Mine"><a:themeElements>)"
      R"(<a:clrScheme name="x"><a:dk1><a:srgbClr val="111111"/></a:dk1><a:lt1><a:srgbClr val="EEEEEE"/></a:lt1>)"
      R"(<a:accent2><a:srgbClr val="00FF00"/></a:accent2></a:clrScheme>)"
      R"(<a:fontScheme name="f"><a:majorFont><a:latin typeface="Georgia"/><a:font script="Jpan" typeface="Yu Mincho"/></a:majorFont>)"
      R"(<a:minorFont><a:latin typeface="Verdana"/></a:minorFont></a:fontScheme></a:themeElements></a:theme>)");
  CHECK(t != nullptr);
  if (!t) return;
  CHECK_EQ(t->Name(), std::string("Mine"));
  CHECK_NEAR(t->GetColor(ThemeColor::Dark1).r, 0x11 / 255.f, 1e-3);
  CHECK_NEAR(t->GetColor(ThemeColor::Accent2).g, 1, 1e-3);
  CHECK_EQ(t->MajorFont().latin, std::string("Georgia"));
  CHECK(t->MajorFont().scripts.size() == 1 && t->MajorFont().scripts[0].second == "Yu Mincho");
  CHECK(Theme::Parse("<notATheme/>") == nullptr);
}

TEST(Theme_ColorTransforms) {
  auto rgb = [](const Color& c) {
    return (static_cast<uint32_t>(std::lround(c.r * 255)) << 16) | (static_cast<uint32_t>(std::lround(c.g * 255)) << 8) |
           static_cast<uint32_t>(std::lround(c.b * 255));
  };
  ColorContext ctx;
  ctx.theme = Theme::Default().get();
  auto color = [&](const std::string& inner) {
    fw::pkg::XmlDom dom = ParseXml("<a:solidFill" + kNs + ">" + inner + "</a:solidFill>");
    return ReadColor(dom.Root(), ctx);
  };
  // Office's palette: accent 1 lighter 80 % and darker 25 %.
  CHECK(Near(rgb(*color(R"(<a:schemeClr val="accent1"><a:lumMod val="20000"/><a:lumOff val="80000"/></a:schemeClr>)")),
             0xDAE3F3, 2));
  CHECK(Near(rgb(*color(R"(<a:schemeClr val="accent1"><a:lumMod val="75000"/></a:schemeClr>)")), 0x2F5597, 2));
  CHECK(Near(rgb(*color(R"(<a:srgbClr val="808080"><a:shade val="0"/></a:srgbClr>)")), 0x000000, 1));
  CHECK(Near(rgb(*color(R"(<a:srgbClr val="808080"><a:tint val="0"/></a:srgbClr>)")), 0xFFFFFF, 1));
  CHECK(Near(rgb(*color(R"(<a:srgbClr val="FF0000"><a:comp/></a:srgbClr>)")), 0x00FFFF, 1));
  CHECK(Near(rgb(*color(R"(<a:srgbClr val="FF0000"><a:inv/></a:srgbClr>)")), 0x00FFFF, 1));
  CHECK_NEAR(color(R"(<a:srgbClr val="FF0000"><a:alpha val="25000"/></a:srgbClr>)")->a, 0.25, 1e-4);
  CHECK(Near(rgb(*color(R"(<a:prstClr val="dkBlue"/>)")), 0x00008B, 0));
  CHECK(Near(rgb(*color(R"(<a:sysClr val="windowText" lastClr="123456"/>)")), 0x123456, 0));
  CHECK(Near(rgb(*color(R"(<a:hslClr hue="0" sat="100000" lum="50000"/>)")), 0xFF0000, 1));
  CHECK(Near(rgb(*color(R"(<a:schemeClr val="tx1"/>)")), 0x000000, 0));
  CHECK(!color(R"(<a:schemeClr val="phClr"/>)"));  // no placeholder color here
  ctx.placeholder = Color{0, 0, 1, 1};
  CHECK(Near(rgb(*color(R"(<a:schemeClr val="phClr"/>)")), 0x0000FF, 0));
  // A color map can swap the text and background colors (dark slides).
  fw::pkg::XmlDom map = ParseXml(R"(<p:clrMap)" + kNs + R"( bg1="dk1" tx1="lt1" bg2="dk2" tx2="lt2"/>)");
  ColorMap m = ColorMap::Read(map.Root());
  ctx.colorMap = &m;
  CHECK(Near(rgb(*color(R"(<a:schemeClr val="bg1"/>)")), 0x000000, 0));
  fw::pkg::XmlDom word = ParseXml(R"(<w:clrSchemeMapping)" + kNs + R"( w:bg1="dark1" w:t1="light1"/>)");
  CHECK(ColorMap::Read(word.Root()).map[0] == ThemeColor::Dark1);
}

// ---- Geometry ----

TEST(Geometry_EveryPresetHasAValidOutline) {
  std::vector<std::string_view> names = Geometry::PresetNames();
  CHECK(names.size() >= 186);
  const Size sizes[] = {{100, 50}, {50, 100}, {10, 10}, {0.5f, 200}, {300, 1}};
  const double values[] = {0, 25000, 50000, 100000, -50000, 1000000};
  int checked = 0;
  for (std::string_view name : names) {
    std::shared_ptr<const Geometry> g = Geometry::Preset(name);
    CHECK(g != nullptr);
    if (!g) continue;
    for (const Size& size : sizes) {
      for (int variant = -1; variant < static_cast<int>(std::size(values)); ++variant) {
        std::vector<Geometry::Adjust> adjust;
        if (variant >= 0) {
          for (const char* a : {"adj", "adj1", "adj2", "adj3", "adj4", "adj5", "adj6", "adj7", "adj8"})
            adjust.push_back({a, values[variant]});
        }
        Geometry::Outline o = g->Evaluate(size, adjust);
        bool any = false, finite = true;
        // Default adjust values stay near the box (callouts and some arrows reach outside it);
        // wild ones may go anywhere, but never to infinity.
        const float reach = variant < 0 ? 1.5f * std::max(size.width, size.height) : 1e7f;
        bool inside = true;
        for (const Geometry::Figure& f : o.figures) {
          for (const Path::Polygon& poly : f.path.Flatten(0.5f)) {
            if (poly.points.size() >= 2) any = true;
            for (const Point& p : poly.points) {
              finite = finite && std::isfinite(p.x) && std::isfinite(p.y);
              inside = inside && p.x >= -reach && p.y >= -reach && p.x <= size.width + reach && p.y <= size.height + reach;
            }
          }
        }
        const Rect t = o.textRect;
        finite = finite && std::isfinite(t.left) && std::isfinite(t.top) && std::isfinite(t.right) && std::isfinite(t.bottom);
        if (!any || !finite || !inside) {
          std::printf("    preset %.*s at %g x %g, variant %d: %s\n", static_cast<int>(name.size()), name.data(),
                      size.width, size.height, variant, !any ? "no outline" : !finite ? "not finite" : "out of range");
        }
        CHECK(any && finite && inside);
        ++checked;
      }
    }
  }
  CHECK(checked == static_cast<int>(names.size()) * 5 * 7);
  CHECK(Geometry::Preset("noSuchShape") == nullptr);
}

TEST(Geometry_EveryPresetDraws) {
  // Each preset filled and stroked in black at 64 x 48: Direct2D accepts the path, and it shows.
  for (std::string_view name : Geometry::PresetNames()) {
    Surface s(80, 64);
    {
      D2DPainter p(s.dc.Get());
      p.Transform(Matrix::Translate(8, 8));
      Pen pen{Brush::Solid({0, 0, 0, 1}), 2};
      for (const Geometry::Figure& f : Geometry::Preset(name)->Evaluate({64, 48}).figures) {
        if (f.fill != Geometry::PathFill::None) p.FillPath(f.path, Brush::Solid({0, 0, 0, 1}));
        if (f.stroke) p.StrokePath(f.path, pen);
      }
    }
    CHECK(s.End());
    int dark = 0;
    for (int y = 0; y < 64; y += 2)
      for (int x = 0; x < 80; x += 2) dark += Red(s.At(x, y)) < 128;
    if (dark < 4) std::printf("    preset %.*s draws nothing\n", static_cast<int>(name.size()), name.data());
    CHECK(dark >= 4);
  }
}

TEST(Geometry_KnownShapes) {
  Geometry::Outline rect = Geometry::Preset("rect")->Evaluate({100, 50});
  CHECK_EQ(rect.figures.size(), 1u);
  Rect b = rect.figures[0].path.Bounds();
  CHECK_NEAR(b.left, 0, 1e-3);
  CHECK_NEAR(b.right, 100, 1e-3);
  CHECK_NEAR(b.bottom, 50, 1e-3);
  // An ellipse made of arcs: every flattened point is on it.
  for (const Path::Polygon& poly : Geometry::Preset("ellipse")->Evaluate({100, 50}).figures[0].path.Flatten(0.05f)) {
    for (const Point& p : poly.points) {
      const float u = (p.x - 50) / 50, v = (p.y - 25) / 25;
      CHECK_NEAR(u * u + v * v, 1, 0.01);
    }
  }
  // roundRect's adjust value sets the corner radius: 50000 is half the shorter side.
  Geometry::Outline round = Geometry::Preset("roundRect")->Evaluate({100, 40}, {{"adj", 50000}});
  Surface s(110, 50);
  {
    D2DPainter p(s.dc.Get());
    for (const auto& f : round.figures) p.FillPath(f.path, Brush::Solid({0, 0, 0, 1}));
  }
  CHECK(s.End());
  CHECK_EQ(s.At(2, 2), 0xFFFFFFu);    // the rounded corner
  CHECK_EQ(s.At(50, 20), 0x000000u);
  CHECK_EQ(s.At(20, 2), 0x000000u);   // past the corner's radius of 20
  // Every ST_ShapeType value, including upArrow, which the standard's file leaves out: its head is
  // at the top, its shaft (half the width by default) below.
  CHECK_EQ(Geometry::PresetNames().size(), 187u);
  Surface up(40, 80);
  {
    D2DPainter p(up.dc.Get());
    for (const auto& f : Geometry::Preset("upArrow")->Evaluate({40, 80}).figures) p.FillPath(f.path, Brush::Solid({0, 0, 0, 1}));
  }
  CHECK(up.End());
  CHECK_EQ(up.At(3, 17), 0x000000u);
  CHECK_EQ(up.At(3, 70), 0xFFFFFFu);
  CHECK_EQ(up.At(20, 70), 0x000000u);
  CHECK_EQ(up.At(3, 3), 0xFFFFFFu);
  // circularArrow's guides multiply squared distances; with defaults it is the upper half of a ring
  // with the head on the right, pointing down.
  Rect ring = Geometry::Preset("circularArrow")->Evaluate({48, 36}).figures[0].path.Bounds();
  CHECK_NEAR(ring.left, 2.25, 0.1);
  CHECK_NEAR(ring.top, 2.25, 0.1);
  CHECK_NEAR(ring.bottom, 18, 0.1);
  CHECK(ring.right > 45 && ring.right < 46.5);
  // The text rectangle comes from the preset (an ellipse's is inset).
  Rect tr = Geometry::Preset("ellipse")->Evaluate({100, 100}).textRect;
  CHECK(tr.left > 10 && tr.left < 20 && tr.right > 80 && tr.right < 90);
}

TEST(Geometry_EvenOddHoles) {
  // donut and frame have holes, made by a second, inner figure.
  for (const char* name : {"donut", "frame"}) {
    Surface s(64, 64);
    {
      D2DPainter p(s.dc.Get());
      for (const auto& f : Geometry::Preset(name)->Evaluate({64, 64}).figures) p.FillPath(f.path, Brush::Solid({0, 0, 0, 1}));
    }
    CHECK(s.End());
    CHECK_EQ(s.At(32, 32), 0xFFFFFFu);
    CHECK_EQ(s.At(32, 3), 0x000000u);
  }
}

TEST(Geometry_CustomGeometry) {
  // A triangle in its own 2 x 2 path space, stretched to the shape, with a guide and an arc.
  Geometry g;
  g.AddAdjust("adj", "val 50000");
  g.AddGuide("x", "*/ w adj 100000");
  g.BeginPath(2, 2, Geometry::PathFill::Normal, true);
  g.MoveTo("1", "0");
  g.LineTo("2", "2");
  g.LineTo("0", "2");
  g.Close();
  g.BeginPath(0, 0, Geometry::PathFill::None, true);
  g.MoveTo("x", "t");
  g.ArcTo("wd4", "hd4", "cd2", "21600000");  // a full ellipse to the right of (x, t)
  g.SetTextRect("l", "t", "x", "b");
  Geometry::Outline o = g.Evaluate({40, 20});
  CHECK_EQ(o.figures.size(), 2u);
  Rect tri = o.figures[0].path.Bounds();
  CHECK_NEAR(tri.right, 40, 1e-3);
  CHECK_NEAR(tri.bottom, 20, 1e-3);
  CHECK(o.figures[1].fill == Geometry::PathFill::None);
  Rect arc = o.figures[1].path.Bounds();
  CHECK_NEAR(arc.left, 20, 0.1);
  CHECK_NEAR(arc.right, 40, 0.1);
  CHECK_NEAR(arc.top, -5, 0.1);
  CHECK_NEAR(arc.bottom, 5, 0.1);
  CHECK_NEAR(o.textRect.right, 20, 1e-3);
  // An adjust value from the shape replaces the default.
  CHECK_NEAR(g.Evaluate({40, 20}, {{"adj", 25000}}).textRect.right, 10, 1e-3);
  // Garbage formulas count as 0 instead of failing.
  Geometry bad;
  bad.AddGuide("g", "*/ nothing 1 0");
  bad.AddGuide("h", "sqrt -5");
  bad.MoveTo("g", "h");
  bad.LineTo("1e400", "nan");
  for (const auto& f : bad.Evaluate({10, 10}).figures)
    for (const auto& poly : f.path.Flatten(1))
      for (const Point& p : poly.points) CHECK(std::isfinite(p.x) && std::isfinite(p.y));
}

// ---- DrawingML ----

TEST(DrawingML_WordShape) {
  TestResources res;
  fw::pkg::XmlDom dom;
  auto d = WordDrawing(
      WordAnchor(60, 30, kWpsUri,
                 Wsp(Xfrm(0, 0, 60, 30) + R"(<a:prstGeom prst="rect"><a:avLst/></a:prstGeom>)"
                     R"(<a:solidFill><a:srgbClr val="FF0000"/></a:solidFill>)"
                     R"(<a:ln w="50800"><a:solidFill><a:srgbClr val="0000FF"/></a:solidFill></a:ln>)"),
                 R"(<wp:wrapTight><wp:wrapPolygon edited="0"><wp:start x="0" y="0"/><wp:lineTo x="0" y="21600"/>)"
                 R"(<wp:lineTo x="21600" y="21600"/><wp:lineTo x="21600" y="0"/><wp:lineTo x="0" y="0"/></wp:wrapPolygon></wp:wrapTight>)"),
      res, dom);
  CHECK(d != nullptr);
  if (!d) return;
  CHECK_NEAR(d->Extent().width, 60, 1e-3);
  CHECK_NEAR(d->Extent().height, 30, 1e-3);
  CHECK_EQ(d->Name(), std::string("Shape 1"));
  CHECK_EQ(d->Description(), std::string("Alt text"));
  CHECK(d->HasWrapPolygon());
  CHECK(d->WrapOutline().size() == 1 && d->WrapOutline()[0].points.size() == 5);
  CHECK_NEAR(d->WrapOutline()[0].points[2].x, 60, 1e-3);
  Rect ink = d->InkBounds();
  CHECK_NEAR(ink.left, -2, 0.01);  // half the 4 pt line
  CHECK_NEAR(ink.bottom, 32, 0.01);
  Surface s(80, 40);
  {
    D2DPainter p(s.dc.Get());
    d->Draw(p, {10, 5, 70, 35});
  }
  CHECK(s.End());
  CHECK(Near(s.At(40, 20), 0xFF0000));
  CHECK(Near(s.At(10, 20), 0x0000FF));  // the line, centered on the edge
  CHECK(Near(s.At(5, 20), 0xFFFFFF));
}

TEST(DrawingML_StyleAndTheme) {
  // No fill in spPr: the style's fill reference picks the theme's first fill in accent 2.
  TestResources res;
  fw::pkg::XmlDom dom;
  auto d = WordDrawing(WordAnchor(40, 40, kWpsUri,
                                  Wsp(Xfrm(0, 0, 40, 40) + R"(<a:prstGeom prst="ellipse"><a:avLst/></a:prstGeom>)",
                                      R"(<wps:style><a:lnRef idx="0"><a:schemeClr val="accent1"/></a:lnRef>)"
                                      R"(<a:fillRef idx="1"><a:schemeClr val="accent2"/></a:fillRef>)"
                                      R"(<a:effectRef idx="0"><a:schemeClr val="accent1"/></a:effectRef>)"
                                      R"(<a:fontRef idx="minor"><a:schemeClr val="lt1"/></a:fontRef></wps:style>)")),
                       res, dom);
  CHECK(d != nullptr);
  if (!d) return;
  Surface s(40, 40);
  Render(s, *d);
  CHECK(s.End());
  CHECK(Near(s.At(20, 20), 0xED7D31, 2));
  CHECK(Near(s.At(1, 1), 0xFFFFFF, 2));  // an ellipse: the corner is empty
}

TEST(DrawingML_GradientsAndPatterns) {
  TestResources res;
  fw::pkg::XmlDom dom;
  auto shape = [&](const std::string& fill) {
    return WordDrawing(WordAnchor(64, 32, kWpsUri, Wsp(Xfrm(0, 0, 64, 32) + R"(<a:prstGeom prst="rect"/>)" + fill)), res,
                       dom);
  };
  {
    auto d = shape(R"(<a:gradFill><a:gsLst><a:gs pos="0"><a:srgbClr val="FF0000"/></a:gs>)"
                   R"(<a:gs pos="100000"><a:srgbClr val="0000FF"/></a:gs></a:gsLst><a:lin ang="0" scaled="0"/></a:gradFill>)");
    Surface s(64, 32);
    Render(s, *d);
    CHECK(s.End());
    CHECK(Red(s.At(1, 16)) > 230 && Blue(s.At(62, 16)) > 230);
    // Word blends two stops through linear light: halfway is (188, 0, 188), not (128, 0, 128).
    CHECK(Near(s.At(32, 16), 0xBC00BC, 12));
  }
  {
    // A rectangular path gradient: the first stop at the focus (the center), the last at the edges.
    auto d = shape(R"(<a:gradFill><a:gsLst><a:gs pos="0"><a:srgbClr val="FFFFFF"/></a:gs>)"
                   R"(<a:gs pos="100000"><a:srgbClr val="000000"/></a:gs></a:gsLst>)"
                   R"(<a:path path="rect"><a:fillToRect l="50000" t="50000" r="50000" b="50000"/></a:path></a:gradFill>)");
    Surface s(64, 32);
    Render(s, *d);
    CHECK(s.End());
    CHECK(Red(s.At(32, 16)) > 200);
    CHECK(Red(s.At(1, 16)) < 40 && Red(s.At(32, 1)) < 50);
    CHECK(Red(s.At(16, 16)) > 170 && Red(s.At(16, 16)) < 205);  // halfway in linear light, as Word
  }
  {
    // A 50 % pattern: alternating pixels (0.75 pt each), here 4 device pixels wide.
    auto d = shape(R"(<a:pattFill prst="pct50"><a:fgClr><a:srgbClr val="000000"/></a:fgClr>)"
                   R"(<a:bgClr><a:srgbClr val="FFFFFF"/></a:bgClr></a:pattFill>)");
    Surface s(160, 80);
    Render(s, *d, 4 / 0.75f / 2);
    CHECK(s.End());
    CHECK(Red(s.At(1, 1)) != Red(s.At(3, 1)) || Red(s.At(1, 1)) != Red(s.At(1, 3)));
  }
}

TEST(DrawingML_PictureCropAndRecolor) {
  TestResources res;
  res.images["rId1"] = Image::Open(QuadPng());
  fw::pkg::XmlDom dom;
  auto pic = [&](const std::string& blipFill) {
    return WordDrawing(
        WordAnchor(40, 40, kPicUri,
                   R"(<pic:pic><pic:nvPicPr><pic:cNvPr id="1" name="p"/><pic:cNvPicPr/></pic:nvPicPr><pic:blipFill>)" +
                       blipFill + R"(</pic:blipFill><pic:spPr>)" + Xfrm(0, 0, 40, 40) +
                       R"(<a:prstGeom prst="rect"/></pic:spPr></pic:pic>)"),
        res, dom);
  };
  {
    auto d = pic(R"(<a:blip r:embed="rId1"/><a:stretch><a:fillRect/></a:stretch>)");
    Surface s(40, 40);
    Render(s, *d);
    CHECK(s.End());
    CHECK(Near(s.At(5, 5), 0xFF0000, 60));
    CHECK(Near(s.At(35, 35), 0xFFFFFF, 60));
  }
  {
    // The right half only: green above, white below, each filling the whole width.
    auto d = pic(R"(<a:blip r:embed="rId1"/><a:srcRect l="50000"/><a:stretch><a:fillRect/></a:stretch>)");
    Surface s(40, 40);
    Render(s, *d);
    CHECK(s.End());
    CHECK(Near(s.At(5, 5), 0x00FF00, 60));
    CHECK(Near(s.At(5, 35), 0xFFFFFF, 60));
  }
  {
    auto d = pic(R"(<a:blip r:embed="rId1"><a:grayscl/></a:blip><a:stretch><a:fillRect/></a:stretch>)");
    Surface s(40, 40);
    Render(s, *d);
    CHECK(s.End());
    uint32_t c = s.At(5, 5);
    CHECK(std::abs(Red(c) - Green(c)) < 4 && std::abs(Red(c) - Blue(c)) < 4 && Red(c) < 200);
  }
  {
    // Colors Word gives (read from its PDFs): grayscale weighs by Rec. 709, and Word 2007's
    // washout (bright 70 %, contrast -70 %, or VML's gain and blacklevel) turns #2828A0 into
    // #D9D9FD.
    res.images["rTeal"] = Image::Open(SolidImage(GUID_ContainerFormatPng, 0xFF18E4C8));
    res.images["rNavy"] = Image::Open(SolidImage(GUID_ContainerFormatPng, 0xFF2828A0));
    auto check = [](const std::unique_ptr<Drawing>& d, uint32_t want) {
      Surface s(40, 40);
      Render(s, *d);
      CHECK(s.End());
      CHECK(Near(s.At(20, 20), want, 6));
    };
    check(pic(R"(<a:blip r:embed="rTeal"><a:grayscl/></a:blip><a:stretch><a:fillRect/></a:stretch>)"), 0xB5B5B5);
    check(pic(R"(<a:blip r:embed="rNavy"><a:lum bright="70000" contrast="-70000"/></a:blip>)"
              R"(<a:stretch><a:fillRect/></a:stretch>)"),
          0xD9D9FD);
    check(VmlDrawing(R"(<v:shape style="width:40pt;height:40pt"><v:imagedata r:id="rNavy" gain="19661f" )"
                     R"(blacklevel="22938f"/></v:shape>)",
                     res, dom),
          0xD9D9FD);
  }
  {
    // Half transparent over white.
    auto d = pic(R"(<a:blip r:embed="rId1"><a:alphaModFix amt="50000"/></a:blip><a:stretch><a:fillRect/></a:stretch>)");
    Surface s(40, 40);
    Render(s, *d);
    CHECK(s.End());
    uint32_t c = s.At(5, 5);
    CHECK(Red(c) > 230 && Green(c) > 90 && Green(c) < 170);
  }
  {
    // A missing picture keeps its frame and shows a placeholder.
    auto d = pic(R"(<a:blip r:embed="rId9"/><a:stretch><a:fillRect/></a:stretch>)");
    Surface s(40, 40);
    Render(s, *d);
    CHECK(s.End());
    CHECK(Near(s.At(3, 3), 0xF2F2F2, 4));
  }
}

TEST(DrawingML_SvgFallsBackToPng) {
  TestResources res;
  res.images["rPng"] = Image::Open(SolidImage(GUID_ContainerFormatPng, 0xFF00FF00));
  res.images["rSvg"] = Image::Open(Bytes(kSvgHalves));
  res.images["rText"] = Image::Open(
      Bytes(R"(<svg xmlns="http://www.w3.org/2000/svg" width="10" height="10"><text x="0" y="9">A</text></svg>)"));
  fw::pkg::XmlDom dom;
  auto pic = [&](const std::string& svgRel) {
    return WordDrawing(
        WordAnchor(40, 20, kPicUri,
                   R"(<pic:pic><pic:nvPicPr><pic:cNvPr id="1" name="p"/><pic:cNvPicPr/></pic:nvPicPr><pic:blipFill>)"
                   R"(<a:blip r:embed="rPng"><a:extLst><a:ext uri="{96DAC541-7B7A-43D3-8B79-37D633B846F1}">)"
                   R"(<asvg:svgBlip xmlns:asvg="http://schemas.microsoft.com/office/drawing/2016/SVG/main" r:embed=")" +
                       svgRel + R"("/></a:ext></a:extLst></a:blip><a:stretch><a:fillRect/></a:stretch></pic:blipFill><pic:spPr>)" +
                       Xfrm(0, 0, 40, 20) + R"(<a:prstGeom prst="rect"/></pic:spPr></pic:pic>)"),
        res, dom);
  };
  {
    auto d = pic("rSvg");
    Surface s(40, 20);
    Render(s, *d);
    CHECK(s.End());
    CHECK(Near(s.At(5, 10), 0xFF0000, 40));  // the SVG
  }
  {
    auto d = pic("rText");
    Surface s(40, 20);
    Render(s, *d);
    CHECK(s.End());
    CHECK(Near(s.At(5, 10), 0x00FF00, 40));  // the PNG copy
  }
}

TEST(DrawingML_GroupsRotationAndFlips) {
  TestResources res;
  fw::pkg::XmlDom dom;
  // A group 40 x 40 whose children are in 20 x 20 units: a child at (0, 0, 10, 10) fills the top
  // left quarter. A second child, an arrow pointing right, flipped, points left.
  std::string group =
      R"(<wpg:wgp><wpg:cNvGrpSpPr/><wpg:grpSpPr><a:xfrm><a:off x="0" y="0"/><a:ext cx="508000" cy="508000"/>)"
      R"(<a:chOff x="0" y="0"/><a:chExt cx="254000" cy="254000"/></a:xfrm></wpg:grpSpPr>)" +
      Wsp(Xfrm(0, 0, 10, 10) + R"(<a:prstGeom prst="rect"/><a:solidFill><a:srgbClr val="0000FF"/></a:solidFill>)") +
      Wsp(Xfrm(0, 10, 20, 10, R"( flipH="1")") +
          R"(<a:prstGeom prst="rtTriangle"/><a:solidFill><a:srgbClr val="FF0000"/></a:solidFill>)") +
      "</wpg:wgp>";
  auto d = WordDrawing(WordAnchor(40, 40, kWpgUri, group), res, dom);
  CHECK(d != nullptr);
  if (!d) return;
  Surface s(40, 40);
  Render(s, *d);
  CHECK(s.End());
  CHECK(Near(s.At(10, 10), 0x0000FF));
  CHECK(Near(s.At(30, 10), 0xFFFFFF));
  // rtTriangle has its right angle at the bottom left; flipped, at the bottom right.
  CHECK(Near(s.At(37, 37), 0xFF0000));
  CHECK(Near(s.At(3, 23), 0xFFFFFF));
  // A shape rotated 90°: a wide bar becomes a tall one.
  auto r = WordDrawing(WordAnchor(40, 10, kWpsUri,
                                  Wsp(Xfrm(0, 0, 40, 10, R"( rot="5400000")") +
                                      R"(<a:prstGeom prst="rect"/><a:solidFill><a:srgbClr val="000000"/></a:solidFill>)")),
                       res, dom);
  Surface t(60, 60);
  {
    D2DPainter p(t.dc.Get());
    r->Draw(p, {10, 25, 50, 35});
  }
  CHECK(t.End());
  CHECK(Near(t.At(30, 10), 0x000000));
  CHECK(Near(t.At(15, 30), 0xFFFFFF));
  // Its outline is rotated too.
  Rect b{};
  for (const auto& poly : r->WrapOutline())
    for (const Point& p : poly.points) b = {std::min(b.left, p.x), std::min(b.top, p.y), std::max(b.right, p.x), std::max(b.bottom, p.y)};
  CHECK_NEAR(b.top, -15, 0.5);
  CHECK_NEAR(b.bottom, 25, 0.5);
}

TEST(DrawingML_LinesAndArrows) {
  TestResources res;
  fw::pkg::XmlDom dom;
  auto d = WordDrawing(
      WordAnchor(60, 0, kWpsUri,
                 Wsp(Xfrm(0, 0, 60, 0) + R"(<a:prstGeom prst="line"/>)"
                     R"(<a:ln w="25400"><a:solidFill><a:srgbClr val="000000"/></a:solidFill><a:prstDash val="dash"/>)"
                     R"(<a:tailEnd type="triangle" w="lg" len="lg"/></a:ln>)")),
      res, dom);
  CHECK(d != nullptr);
  if (!d) return;
  Surface s(80, 40);
  {
    D2DPainter p(s.dc.Get());
    d->Draw(p, {10, 20, 70, 20});
  }
  CHECK(s.End());
  CHECK(Near(s.At(12, 20), 0x000000, 30));  // the first dash (4 widths = 8 pt)
  CHECK(Near(s.At(19, 20), 0xFFFFFF, 30));  // the first gap
  CHECK(Near(s.At(66, 20), 0x000000, 30));  // the arrowhead at the end
  CHECK(Near(s.At(64, 18), 0x000000, 60));  // wider than the line
  CHECK(d->InkBounds().right > 60);
}

TEST(DrawingML_OuterShadow) {
  TestResources res;
  fw::pkg::XmlDom dom;
  auto d = WordDrawing(
      WordAnchor(20, 20, kWpsUri,
                 Wsp(Xfrm(0, 0, 20, 20) + R"(<a:prstGeom prst="rect"/><a:solidFill><a:srgbClr val="FF0000"/></a:solidFill>)"
                     R"(<a:effectLst><a:outerShdw blurRad="0" dist="127000" dir="2700000"><a:srgbClr val="000000"/></a:outerShdw></a:effectLst>)")),
      res, dom);
  CHECK(d != nullptr);
  if (!d) return;
  Surface s(48, 48);
  {
    D2DPainter p(s.dc.Get());
    d->Draw(p, {10, 10, 30, 30});
  }
  CHECK(s.End());
  CHECK(Near(s.At(20, 20), 0xFF0000));
  CHECK(Near(s.At(36, 36), 0x000000, 40));  // 10 pt down and right at 45°: about 7 pt each way
  CHECK(Near(s.At(5, 5), 0xFFFFFF));
  CHECK(d->InkBounds().right > 25);
}

TEST(DrawingML_TextBoxes) {
  TestResources res;
  fw::pkg::XmlDom dom;
  auto d = WordDrawing(
      WordAnchor(100, 50, kWpsUri,
                 R"(<wps:wsp><wps:cNvSpPr txBox="1"/><wps:spPr>)" + Xfrm(0, 0, 100, 50) +
                     R"(<a:prstGeom prst="rect"/></wps:spPr><wps:txbx><w:txbxContent><w:p/></w:txbxContent></wps:txbx>)"
                     R"(<wps:bodyPr rot="5400000" vert="vert" lIns="127000" tIns="0" rIns="0" bIns="25400" anchor="ctr" wrap="none">)"
                     R"(<a:normAutofit fontScale="62500" lnSpcReduction="20000"/></wps:bodyPr></wps:wsp>)"),
      res, dom);
  CHECK(d != nullptr);
  if (!d) return;
  CHECK_EQ(d->TextBoxes().size(), 1u);
  const TextBox& b = d->TextBoxes()[0];
  CHECK(b.content && std::string_view(b.content.name()) == "w:txbxContent");
  CHECK(std::string_view(b.properties.name()) == "wps:bodyPr");
  CHECK_NEAR(b.rect.left, 10, 1e-3);
  CHECK_NEAR(b.rect.top, 0, 1e-3);
  CHECK_NEAR(b.rect.right, 100, 1e-3);
  CHECK_NEAR(b.rect.bottom, 48, 1e-3);
  CHECK(b.flow == TextBox::Flow::Vertical);
  CHECK(b.anchor == TextBox::Anchor::Middle);
  CHECK(!b.wrap);
  CHECK(b.autofit == TextBox::Autofit::ShrinkText);
  CHECK_NEAR(b.fontScale, 0.625, 1e-4);
  CHECK_NEAR(b.rotation, 90, 1e-3);
  // Default insets: 0.1 in left and right, 0.05 in top and bottom.
  auto plain = WordDrawing(WordAnchor(100, 50, kWpsUri,
                                      R"(<wps:wsp><wps:cNvSpPr/><wps:spPr>)" + Xfrm(0, 0, 100, 50) +
                                          R"(<a:prstGeom prst="rect"/></wps:spPr><wps:txbx><w:txbxContent/></wps:txbx><wps:bodyPr/></wps:wsp>)"),
                           res, dom);
  CHECK(plain && plain->TextBoxes().size() == 1);
  if (plain && !plain->TextBoxes().empty()) {
    CHECK_NEAR(plain->TextBoxes()[0].rect.left, 7.2, 1e-3);
    CHECK_NEAR(plain->TextBoxes()[0].rect.top, 3.6, 1e-3);
  }
}

TEST(DrawingML_ChartIsPlaceholder) {
  TestResources res;
  fw::pkg::XmlDom dom;
  auto d = WordDrawing(WordAnchor(100, 60, "http://schemas.openxmlformats.org/drawingml/2006/chart",
                                  R"(<c:chart xmlns:c="http://schemas.openxmlformats.org/drawingml/2006/chart" r:id="rId5"/>)"),
                       res, dom);
  CHECK(d && d->IsPlaceholder());
  CHECK(d && d->Extent().width == 100);
}

TEST(DrawingML_ShapeTree) {
  // A slide shape: its bounds come from its xfrm, in the slide's coordinates.
  TestResources res;
  fw::pkg::XmlDom dom = ParseXml(
      "<p:sp" + kNs + R"(><p:nvSpPr><p:cNvPr id="2" name="Box"/><p:cNvSpPr/><p:nvPr/></p:nvSpPr><p:spPr>)" +
      Xfrm(72, 36, 100, 50) + R"(<a:custGeom><a:pathLst><a:path w="2" h="2"><a:moveTo><a:pt x="0" y="0"/></a:moveTo>)"
                              R"(<a:lnTo><a:pt x="2" y="0"/></a:lnTo><a:lnTo><a:pt x="0" y="2"/></a:lnTo><a:close/></a:path></a:pathLst></a:custGeom>)"
                              R"(<a:solidFill><a:srgbClr val="00FF00"/></a:solidFill></p:spPr><p:txBody><a:bodyPr/><a:p/></p:txBody></p:sp>)");
  auto d = Drawing::FromShapeTree(dom.Root(), res);
  CHECK(d != nullptr);
  if (!d) return;
  CHECK_NEAR(d->Bounds().left, 72, 1e-3);
  CHECK_NEAR(d->Bounds().top, 36, 1e-3);
  CHECK_EQ(d->Name(), std::string("Box"));
  CHECK_EQ(d->TextBoxes().size(), 1u);
  Surface s(200, 100);
  {
    D2DPainter p(s.dc.Get());
    d->Draw(p, d->Bounds());
  }
  CHECK(s.End());
  CHECK(Near(s.At(80, 40), 0x00FF00));
  CHECK(Near(s.At(165, 80), 0xFFFFFF));  // the custom triangle leaves the bottom right empty
}

TEST(DrawingML_SheetPictureSizedByItsAnchor) {
  // openpyxl leaves a picture's xfrm empty: its one-cell anchor's ext gives the size.
  TestResources res;
  res.images["rId1"] = Image::Open(SolidImage(GUID_ContainerFormatPng, 0xFF0000FF));
  fw::pkg::XmlDom dom = ParseXml(
      R"(<oneCellAnchor xmlns="http://schemas.openxmlformats.org/drawingml/2006/spreadsheetDrawing")" + kNs +
      R"(><from><col>1</col><colOff>0</colOff><row>2</row><rowOff>0</rowOff></from><ext cx="508000" cy="254000"/>)"
      R"(<pic><nvPicPr><cNvPr id="1" name="Image 1"/><cNvPicPr/></nvPicPr><blipFill><a:blip r:embed="rId1"/>)"
      R"(<a:stretch><a:fillRect/></a:stretch></blipFill><spPr><a:xfrm/><a:prstGeom prst="rect"/></spPr></pic>)"
      R"(<clientData/></oneCellAnchor>)");
  auto d = Drawing::FromShapeTree(dom.Root().child("pic"), res);
  CHECK(d != nullptr);
  if (!d) return;
  CHECK_NEAR(d->Extent().width, 40, 1e-3);
  CHECK_NEAR(d->Extent().height, 20, 1e-3);
  Surface s(40, 20);
  Render(s, *d);
  CHECK(s.End());
  CHECK(Near(s.At(20, 10), 0x0000FF, 8));
}

// ---- VML ----

TEST(Vml_BasicShapes) {
  TestResources res;
  fw::pkg::XmlDom dom;
  {
    auto d = VmlDrawing(R"(<v:rect style="width:40pt;height:20pt" fillcolor="#00ff00" stroked="f"/>)", res, dom);
    CHECK(d != nullptr);
    if (!d) return;
    CHECK_NEAR(d->Extent().width, 40, 1e-3);
    Surface s(40, 20);
    Render(s, *d);
    CHECK(s.End());
    CHECK(Near(s.At(20, 10), 0x00FF00));
  }
  {
    auto d = VmlDrawing(R"(<v:oval style="width:40pt;height:40pt" fillcolor="red" strokecolor="blue" strokeweight="2pt"/>)",
                        res, dom);
    Surface s(40, 40);
    Render(s, *d);
    CHECK(s.End());
    CHECK(Near(s.At(20, 20), 0xFF0000));
    CHECK(Near(s.At(1, 1), 0xFFFFFF));
    CHECK(Near(s.At(20, 0), 0x0000FF, 40));
  }
  {
    // arcsize: the corner radius as a share of half the shorter side.
    auto d = VmlDrawing(R"(<v:roundrect style="width:40pt;height:20pt" arcsize="1" fillcolor="black" stroked="f"/>)", res, dom);
    Surface s(40, 20);
    Render(s, *d);
    CHECK(s.End());
    CHECK(Near(s.At(1, 1), 0xFFFFFF));
    CHECK(Near(s.At(20, 10), 0x000000));
  }
  {
    auto d = VmlDrawing(R"(<v:line from="0,0" to="40pt,20pt" strokecolor="black" strokeweight="3pt"/>)", res, dom);
    CHECK(d != nullptr);
    Surface s(40, 20);
    Render(s, *d);
    CHECK(s.End());
    CHECK(Near(s.At(20, 10), 0x000000, 40));
    CHECK(Near(s.At(36, 3), 0xFFFFFF));
  }
  {
    // A v:shape with neither a path nor a known type: Word draws nothing.
    auto d = VmlDrawing(R"(<v:shape style="width:40pt;height:20pt" fillcolor="black"/>)", res, dom);
    CHECK(d != nullptr);
    Surface s(40, 20);
    Render(s, *d);
    CHECK(s.End());
    CHECK(Near(s.At(20, 10), 0xFFFFFF));
  }
  // Gradients, as Word draws them: angle 0 runs from color2 at the top to color at the bottom,
  // 90 from left to right, a negative angle is that plus 180; focus 50 % puts color2 in the
  // middle, -50 % color.
  auto gradient = [&](const char* fill) {
    auto d = VmlDrawing(std::string(R"(<v:rect style="width:40pt;height:40pt" fillcolor="red" stroked="f">)") +
                            R"(<v:fill type="gradient" color2="blue" )" + fill + "/></v:rect>",
                        res, dom);
    auto s = std::make_unique<Surface>(40, 40);
    Render(*s, *d);
    CHECK(s->End());
    return s;
  };
  {
    auto s = gradient(R"(angle="0")");
    CHECK(Blue(s->At(20, 1)) > 230 && Red(s->At(20, 38)) > 230);
  }
  for (const char* angle : {R"(angle="90")", R"(angle="-90")"}) {
    auto s = gradient(angle);
    CHECK(Blue(s->At(1, 20)) > 230 && Red(s->At(38, 20)) > 230);
  }
  {
    auto s = gradient(R"(angle="90" focus="50%")");
    CHECK(Red(s->At(1, 20)) > 230 && Blue(s->At(20, 20)) > 230 && Red(s->At(38, 20)) > 230);
  }
  {
    auto s = gradient(R"(angle="90" focus="-50%")");
    CHECK(Blue(s->At(1, 20)) > 230 && Red(s->At(20, 20)) > 230 && Blue(s->At(38, 20)) > 230);
  }
}

TEST(Vml_ShapeTypesAndFormulas) {
  TestResources res;
  fw::pkg::XmlDom dom;
  // The shape type's path and formulas: a band from #0 to width - #0.
  auto d = VmlDrawing(
      R"(<v:shapetype id="_x0000_t9999" coordsize="21600,21600" adj="5400" path="m@0,l@1,0@1,21600@0,21600xe">)"
      R"(<v:formulas><v:f eqn="val #0"/><v:f eqn="sum width 0 #0"/></v:formulas></v:shapetype>)"
      R"(<v:shape type="#_x0000_t9999" style="width:40pt;height:20pt" fillcolor="black" stroked="f"/>)",
      res, dom);
  CHECK(d != nullptr);
  if (!d) return;
  Surface s(40, 20);
  Render(s, *d);
  CHECK(s.End());
  CHECK(Near(s.At(5, 10), 0xFFFFFF));
  CHECK(Near(s.At(20, 10), 0x000000));
  CHECK(Near(s.At(35, 10), 0xFFFFFF));
  // The shape's own adj overrides the type's.
  auto wide = VmlDrawing(
      R"(<v:shapetype id="t2" coordsize="21600,21600" adj="5400" path="m@0,l@1,0@1,21600@0,21600xe">)"
      R"(<v:formulas><v:f eqn="val #0"/><v:f eqn="sum width 0 #0"/></v:formulas></v:shapetype>)"
      R"(<v:shape type="#t2" adj="0" style="width:40pt;height:20pt" fillcolor="black" stroked="f"/>)",
      res, dom);
  Surface t(40, 20);
  Render(t, *wide);
  CHECK(t.End());
  CHECK(Near(t.At(3, 10), 0x000000));
  // Without a shape type in the file, the o:spt number picks the preset.
  auto oval = VmlDrawing(R"(<v:shape type="#_x0000_t3" o:spt="3" style="width:20pt;height:20pt" fillcolor="black" stroked="f"/>)",
                         res, dom);
  Surface u(20, 20);
  Render(u, *oval);
  CHECK(u.End());
  CHECK(Near(u.At(1, 1), 0xFFFFFF));
  CHECK(Near(u.At(10, 10), 0x000000));
  // Arcs and quadrants: a circle drawn with qx/qy.
  auto round = VmlDrawing(
      R"(<v:shape coordsize="200,200" path="m100,qx0,100,100,200,200,100,100,xe" style="width:20pt;height:20pt" fillcolor="black" stroked="f"/>)",
      res, dom);
  Surface w(20, 20);
  Render(w, *round);
  CHECK(w.End());
  CHECK(Near(w.At(1, 1), 0xFFFFFF));
  CHECK(Near(w.At(10, 10), 0x000000));
  CHECK(Near(w.At(10, 1), 0x000000, 80));
}

TEST(Vml_GroupsPicturesAndWatermarks) {
  TestResources res;
  res.images["rId1"] = Image::Open(QuadPng());
  fw::pkg::XmlDom dom;
  {
    auto d = VmlDrawing(R"(<v:group style="width:40pt;height:40pt" coordsize="400,400">)"
                        R"(<v:rect style="left:0;top:0;width:200;height:200" fillcolor="blue" stroked="f"/>)"
                        R"(<v:oval style="left:200;top:200;width:200;height:200" fillcolor="red" stroked="f"/></v:group>)",
                        res, dom);
    CHECK(d != nullptr);
    if (!d) return;
    Surface s(40, 40);
    Render(s, *d);
    CHECK(s.End());
    CHECK(Near(s.At(10, 10), 0x0000FF));
    CHECK(Near(s.At(30, 30), 0xFF0000));
    CHECK(Near(s.At(30, 10), 0xFFFFFF));
  }
  {
    // A picture frame, cropped to its right half.
    auto d = VmlDrawing(R"(<v:shape type="#_x0000_t75" o:spt="75" style="width:40pt;height:40pt">)"
                        R"(<v:imagedata r:id="rId1" cropleft="32768f"/></v:shape>)",
                        res, dom);
    Surface s(40, 40);
    Render(s, *d);
    CHECK(s.End());
    CHECK(Near(s.At(5, 5), 0x00FF00, 60));
    CHECK(Near(s.At(5, 35), 0xFFFFFF, 60));
  }
  {
    // A watermark: WordArt text, half transparent silver, stretched to the shape.
    auto d = VmlDrawing(
        R"(<v:shapetype id="_x0000_t136" coordsize="21600,21600" o:spt="136" adj="10800" path="m@7,l@8,m@5,21600l@6,21600e">)"
        R"(<v:path textpathok="t" o:connecttype="custom"/><v:textpath on="t" fitshape="t"/></v:shapetype>)"
        R"(<v:shape id="PowerPlusWaterMarkObject" type="#_x0000_t136" style="position:absolute;margin-left:0;margin-top:0;)"
        R"(width:100pt;height:40pt;rotation:315" fillcolor="black" stroked="f"><v:fill opacity=".5"/>)"
        R"(<v:textpath style="font-family:&quot;Arial&quot;;font-size:1pt" string="HH"/></v:shape>)",
        res, dom);
    CHECK(d != nullptr);
    if (!d) return;
    CHECK_EQ(d->Name(), std::string("PowerPlusWaterMarkObject"));
    Surface s(140, 140);
    {
      D2DPainter p(s.dc.Get());
      d->Draw(p, {20, 50, 120, 90});
    }
    CHECK(s.End());
    int gray = 0, black = 0;
    for (int y = 0; y < 140; ++y)
      for (int x = 0; x < 140; ++x) {
        uint32_t c = s.At(x, y);
        if (Red(c) > 100 && Red(c) < 160) ++gray;
        if (Red(c) < 60) ++black;
      }
    CHECK(gray > 300);  // the letters, at half opacity
    CHECK_EQ(black, 0);
  }
}

TEST(Vml_TextBoxAndWrapCoords) {
  TestResources res;
  fw::pkg::XmlDom dom;
  auto d = VmlDrawing(
      R"(<v:shape type="#_x0000_t202" o:spt="202" style="position:absolute;margin-left:12pt;margin-top:6pt;width:144pt;height:36pt;)"
      R"(v-text-anchor:middle" o:wrapcoords="0 0 0 21600 21600 21600 21600 0">)"
      R"(<v:textbox inset="2pt,1pt,2pt,1pt" style="mso-fit-shape-to-text:t"><w:txbxContent><w:p/></w:txbxContent></v:textbox></v:shape>)",
      res, dom);
  CHECK(d != nullptr);
  if (!d) return;
  CHECK_NEAR(d->Bounds().left, 12, 1e-3);
  CHECK_NEAR(d->Bounds().top, 6, 1e-3);
  CHECK_EQ(d->TextBoxes().size(), 1u);
  if (d->TextBoxes().empty()) return;
  const TextBox& b = d->TextBoxes()[0];
  CHECK_NEAR(b.rect.left, 2, 1e-3);
  CHECK_NEAR(b.rect.right, 142, 1e-3);
  CHECK(b.anchor == TextBox::Anchor::Middle);
  CHECK(b.autofit == TextBox::Autofit::ResizeShape);
  CHECK(d->HasWrapPolygon());
  CHECK(d->WrapOutline().size() == 1 && d->WrapOutline()[0].points.size() == 4);
}

TEST(Vml_StyleHelpers) {
  auto style = ParseVmlStyle("position:absolute; margin-left:12.5pt;width:1in ;height:2cm;z-index:-5");
  CHECK_EQ(style.size(), 5u);
  CHECK(style[1].first == "margin-left" && style[1].second == "12.5pt");
  CHECK_NEAR(VmlLength("1in"), 72, 1e-4);
  CHECK_NEAR(VmlLength("2cm"), 56.6929, 1e-3);
  CHECK_NEAR(VmlLength("96"), 72, 1e-4);  // pixels
  CHECK_NEAR(VmlLength("10", 1), 10, 1e-4);
  CHECK_NEAR(VmlLength("garbage"), 0, 1e-4);
}

TEST(Drawing_RejectsWhatIsntOne) {
  TestResources res;
  fw::pkg::XmlDom dom = ParseXml("<w:p" + kNs + "/>");
  CHECK(Drawing::FromWordDrawing(dom.Root(), res) == nullptr);
  CHECK(Drawing::FromVml(dom.Root(), res) == nullptr);
  CHECK(Drawing::FromShapeTree(dom.Root(), res) == nullptr);
  // A drawing without a graphic keeps its size, as a placeholder.
  fw::pkg::XmlDom empty = ParseXml("<wp:inline" + kNs + R"(><wp:extent cx="127000" cy="127000"/><wp:docPr id="1" name="x"/></wp:inline>)");
  auto d = Drawing::FromWordDrawing(empty.Root(), res);
  CHECK(d && d->IsPlaceholder() && d->Extent().width == 10);
}
