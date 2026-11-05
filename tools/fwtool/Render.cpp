// fwtool render: draws the pictures and shapes in Office files to PNG with fwdraw (plan §4.5, §10),
// to compare with Office's reference renders by eye:
//  - Word: each drawing (DrawingML or VML) in the body, headers and footers, at its own size;
//  - PowerPoint: each slide's shapes where the slide puts them, on a slide-sized white page;
//  - Excel: each shape of each drawing part, at its own size.
// Rendering uses Direct2D's software renderer through WIC, so no graphics device or window is
// involved and the output is the same on every PC.
#include <windows.h>

#include <d2d1_1.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "Common.h"
#include "core/Strings.h"
#include "draw/D2DPainter.h"
#include "draw/Drawing.h"
#include "pkg/Mce.h"
#include "pkg/XmlDom.h"

using Microsoft::WRL::ComPtr;
using namespace draw;

namespace fwtool {
namespace {

constexpr std::string_view kW = "http://schemas.openxmlformats.org/wordprocessingml/2006/main";
constexpr std::string_view kV = "urn:schemas-microsoft-com:vml";
constexpr std::string_view kP = "http://schemas.openxmlformats.org/presentationml/2006/main";
constexpr std::string_view kA = "http://schemas.openxmlformats.org/drawingml/2006/main";
constexpr std::string_view kXdr = "http://schemas.openxmlformats.org/drawingml/2006/spreadsheetDrawing";

// Draws `size` points at `scale` pixels per point, on white, and saves a PNG.
bool RenderPng(const std::filesystem::path& file, Size size, float scale, const std::function<void(Painter&)>& draw) {
  const UINT w = static_cast<UINT>(std::clamp(std::ceil(size.width * scale), 1.f, 8192.f));
  const UINT h = static_cast<UINT>(std::clamp(std::ceil(size.height * scale), 1.f, 8192.f));
  ComPtr<ID2D1Factory1> factory;
  ComPtr<IWICImagingFactory> wic;
  ComPtr<IWICBitmap> bitmap;
  ComPtr<ID2D1RenderTarget> target;
  ComPtr<ID2D1DeviceContext> dc;
  D2D1_FACTORY_OPTIONS opts{};
  if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &opts,
                               reinterpret_cast<void**>(factory.GetAddressOf()))) ||
      FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) ||
      FAILED(wic->CreateBitmap(w, h, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap))) {
    return false;
  }
  D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
      D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
  if (FAILED(factory->CreateWicBitmapRenderTarget(bitmap.Get(), props, &target)) || FAILED(target.As(&dc))) return false;
  dc->BeginDraw();
  dc->Clear(D2D1::ColorF(1, 1, 1, 1));
  {
    D2DPainter p(dc.Get());
    p.Transform(Matrix::Scale(scale, scale));
    draw(p);
  }
  if (FAILED(dc->EndDraw())) return false;
  ComPtr<IWICStream> stream;
  ComPtr<IWICBitmapEncoder> encoder;
  ComPtr<IWICBitmapFrameEncode> frame;
  WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
  return SUCCEEDED(wic->CreateStream(&stream)) &&
         SUCCEEDED(stream->InitializeFromFilename(file.c_str(), GENERIC_WRITE)) &&
         SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
         SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
         SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
         SUCCEEDED(frame->SetSize(w, h)) && SUCCEEDED(frame->SetPixelFormat(&format)) &&
         SUCCEEDED(frame->WriteSource(bitmap.Get(), nullptr)) && SUCCEEDED(frame->Commit()) &&
         SUCCEEDED(encoder->Commit());
}

std::unique_ptr<fw::pkg::XmlDom> ReadXml(const fw::pkg::Package& pkg, const std::string& part) {
  std::vector<uint8_t> bytes;
  if (part.empty() || !pkg.ReadPart(part, bytes).Ok()) return nullptr;
  auto dom = std::make_unique<fw::pkg::XmlDom>();
  if (!dom->Parse(AsText(bytes), pkg.GetLimits(), part).Ok()) return nullptr;
  return dom;
}

// Internal targets of `source`'s relationships whose type ends with "/<kind>", in order.
std::vector<std::string> Targets(const fw::pkg::Package& pkg, std::string_view source, std::string_view kind) {
  std::vector<std::string> out;
  if (const fw::pkg::RelationshipSet* rels = pkg.RelationshipsOf(source)) {
    for (const auto& r : rels->items) {
      std::string_view type = r.type;
      if (!r.external && type.ends_with(kind) && type.size() > kind.size() && type[type.size() - kind.size() - 1] == '/')
        out.push_back(r.targetPart);
    }
  }
  return out;
}

// Every element under `root` that `want` accepts, in document order, inside mc:AlternateContent
// only the branch a reader would take (so a drawing and its VML fallback aren't both found).
std::vector<pugi::xml_node> FindAll(pugi::xml_node root, const std::function<bool(pugi::xml_node)>& want) {
  static const fw::pkg::mce::Understood understood{
      kW, kA, "http://schemas.microsoft.com/office/word/2010/wordprocessingShape",
      "http://schemas.microsoft.com/office/word/2010/wordprocessingGroup",
      "http://schemas.microsoft.com/office/word/2010/wordprocessingCanvas",
      "http://schemas.microsoft.com/office/drawing/2010/main"};
  std::vector<pugi::xml_node> out, stack{root};
  while (!stack.empty()) {
    pugi::xml_node n = stack.back();
    stack.pop_back();
    if (n.type() != pugi::node_element) continue;
    if (fw::pkg::IsElement(n, fw::pkg::mce::kNamespace, "AlternateContent")) {
      if (pugi::xml_node branch = fw::pkg::mce::SelectBranch(n, understood)) {
        std::vector<pugi::xml_node> kids(branch.begin(), branch.end());
        stack.insert(stack.end(), kids.rbegin(), kids.rend());
      }
      continue;
    }
    if (want(n)) {
      out.push_back(n);
      continue;
    }
    std::vector<pugi::xml_node> kids(n.begin(), n.end());
    stack.insert(stack.end(), kids.rbegin(), kids.rend());
  }
  return out;
}

std::string Stem(const std::filesystem::path& file) { return fw::WideToUtf8(file.stem().wstring()); }

std::string PartStem(std::string_view part) {
  std::string_view name = part.substr(part.rfind('/') + 1);
  return std::string(name.substr(0, name.rfind('.')));
}

struct Counter {
  int drawings = 0, failed = 0, placeholders = 0;
};

bool Save(const Options& options, const std::string& name, Size size, Counter& count,
          const std::function<void(Painter&)>& draw) {
  const std::filesystem::path out = options.outDir / fw::Utf8ToWide(name + ".png");
  if (RenderPng(out, size, options.scale, draw)) return true;
  std::printf("  %s: couldn't render\n", name.c_str());
  ++count.failed;
  return false;
}

// One drawing, framed by what it paints (lines and shadows reach past the extent).
void RenderDrawing(const Options& options, const std::string& name, const Drawing& d, Counter& count) {
  ++count.drawings;
  if (d.IsPlaceholder()) ++count.placeholders;
  const Rect ink = d.InkBounds();
  const Size ext = d.Extent();
  const float pad = 4;
  const Rect area{std::min(0.f, ink.left) - pad, std::min(0.f, ink.top) - pad, std::max(ext.width, ink.right) + pad,
                  std::max(ext.height, ink.bottom) + pad};
  std::printf("  %s: %.1f x %.1f pt%s%s%s\n", name.c_str(), ext.width, ext.height, d.IsPlaceholder() ? ", placeholder" : "",
              d.TextBoxes().empty() ? "" : ", text box", d.HasWrapPolygon() ? ", wrap polygon" : "");
  Save(options, name, {area.Width(), area.Height()}, count, [&](Painter& p) {
    d.Draw(p, {-area.left, -area.top, ext.width - area.left, ext.height - area.top});
  });
}

void RenderWord(const Options& options, const fw::pkg::Package& pkg, const std::string& stem, Counter& count) {
  std::vector<std::string> parts = Targets(pkg, "/", "officeDocument");
  if (parts.empty()) return;
  const std::string main = parts.front();
  for (std::string_view kind : {"header", "footer", "footnotes", "endnotes"})
    for (const std::string& p : Targets(pkg, main, kind)) parts.push_back(p);
  auto images = std::make_shared<ImageCache>();
  std::shared_ptr<const Theme> theme = PackageResources::FindTheme(pkg, main);
  ColorMap map = PackageResources::FindColorMap(pkg, main);
  for (const std::string& part : parts) {
    std::unique_ptr<fw::pkg::XmlDom> dom = ReadXml(pkg, part);
    if (!dom) continue;
    PackageResources res(pkg, part, theme, map, images);
    int n = 0;
    for (pugi::xml_node node : FindAll(dom->Root(), [](pugi::xml_node e) {
           return fw::pkg::NamespaceOf(e) == kW &&
                  (fw::pkg::LocalNameOf(e) == "drawing" || fw::pkg::LocalNameOf(e) == "pict" ||
                   fw::pkg::LocalNameOf(e) == "object");
         })) {
      std::vector<std::unique_ptr<Drawing>> found;
      if (fw::pkg::LocalNameOf(node) == "drawing") {
        found.push_back(Drawing::FromWordDrawing(node, res));
      } else {
        // A w:pict may hold more than one shape: each is a drawing of its own.
        for (pugi::xml_node c : node.children()) {
          if (fw::pkg::NamespaceOf(c) != kV || fw::pkg::LocalNameOf(c) == "shapetype") continue;
          found.push_back(Drawing::FromVml(c, res));
        }
      }
      for (const std::unique_ptr<Drawing>& d : found) {
        if (!d || d->IsHidden()) continue;
        RenderDrawing(options, stem + "-" + PartStem(part) + "-" + std::to_string(++n), *d, count);
      }
    }
  }
}

void RenderPowerPoint(const Options& options, const fw::pkg::Package& pkg, const std::string& stem, Counter& count) {
  std::vector<std::string> pres = Targets(pkg, "/", "officeDocument");
  if (pres.empty()) return;
  std::unique_ptr<fw::pkg::XmlDom> presDom = ReadXml(pkg, pres.front());
  if (!presDom) return;
  pugi::xml_node sz;
  for (pugi::xml_node c : presDom->Root().children())
    if (fw::pkg::IsElement(c, kP, "sldSz")) sz = c;
  const Size slide{sz.attribute("cx").as_float(9144000) / 12700, sz.attribute("cy").as_float(6858000) / 12700};
  auto images = std::make_shared<ImageCache>();
  int number = 0;
  for (const std::string& part : Targets(pkg, pres.front(), "slide")) {
    std::unique_ptr<fw::pkg::XmlDom> dom = ReadXml(pkg, part);
    if (!dom) continue;
    PackageResources res(pkg, part, PackageResources::FindTheme(pkg, part), PackageResources::FindColorMap(pkg, part),
                         images);
    std::vector<std::unique_ptr<Drawing>> shapes;
    for (pugi::xml_node tree : FindAll(dom->Root(), [](pugi::xml_node e) {
           return fw::pkg::IsElement(e, kP, "spTree");
         })) {
      for (pugi::xml_node c : tree.children()) {
        std::string_view k = fw::pkg::LocalNameOf(c);
        if (k == "nvGrpSpPr" || k == "grpSpPr" || c.type() != pugi::node_element) continue;
        if (auto d = Drawing::FromShapeTree(c, res); d && !d->IsHidden() && d->Extent().width > 0) {
          ++count.drawings;
          if (d->IsPlaceholder()) ++count.placeholders;
          shapes.push_back(std::move(d));
        }
      }
    }
    const std::string name = stem + "-slide" + std::to_string(++number);
    std::printf("  %s: %zu shapes\n", name.c_str(), shapes.size());
    Save(options, name, slide, count, [&](Painter& p) {
      for (const auto& d : shapes) d->Draw(p, d->Bounds());
    });
  }
}

void RenderExcel(const Options& options, const fw::pkg::Package& pkg, const std::string& stem, Counter& count) {
  auto images = std::make_shared<ImageCache>();
  for (const fw::pkg::Part& part : pkg.Parts()) {
    if (part.name.find("/drawings/drawing") == std::string::npos || !part.name.ends_with(".xml")) continue;
    std::unique_ptr<fw::pkg::XmlDom> dom = ReadXml(pkg, part.name);
    if (!dom) continue;
    PackageResources res(pkg, part.name, PackageResources::FindTheme(pkg, "/xl/workbook.xml"), {}, images);
    int n = 0;
    for (pugi::xml_node anchor : dom->Root().children()) {
      for (pugi::xml_node c : anchor.children()) {
        if (fw::pkg::NamespaceOf(c) != kXdr) continue;
        std::string_view k = fw::pkg::LocalNameOf(c);
        if (k != "sp" && k != "pic" && k != "grpSp" && k != "cxnSp" && k != "graphicFrame") continue;
        if (auto d = Drawing::FromShapeTree(c, res); d && !d->IsHidden()) {
          RenderDrawing(options, stem + "-" + PartStem(part.name) + "-" + std::to_string(++n), *d, count);
        }
      }
    }
  }
}

}  // namespace

int RunRender(const Options& options) {
  if (options.outDir.empty()) {
    std::fputs("render needs --out <folder>.\n", stderr);
    return 2;
  }
  std::error_code ec;
  std::filesystem::create_directories(options.outDir, ec);
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  int failures = 0;
  for (const auto& file : CollectFiles(options.inputs)) {
    fw::pkg::Status st;
    auto pkg = OpenPackage(file, st);
    std::printf("%s\n", Display(file).c_str());
    if (!pkg) {
      std::printf("  refused: %s\n", st.message.c_str());
      ++failures;
      continue;
    }
    Counter count;
    const std::wstring ext = fw::ToLower(file.extension().wstring());
    const std::string stem = Stem(file);
    if (ext.starts_with(L".doc") || ext.starts_with(L".dot")) {
      RenderWord(options, *pkg, stem, count);
    } else if (ext.starts_with(L".ppt") || ext.starts_with(L".pot") || ext.starts_with(L".pps")) {
      RenderPowerPoint(options, *pkg, stem, count);
    } else {
      RenderExcel(options, *pkg, stem, count);
    }
    std::printf("  %d drawings, %d placeholders\n", count.drawings, count.placeholders);
    failures += count.failed;
  }
  return failures ? 1 : 0;
}

}  // namespace fwtool
