// Fuzz target: fwdraw's readers and renderer (plan §4.5): DrawingML and VML into the shape
// model, preset and custom geometry, themes, picture decoding (WIC, metafiles, SVG), and drawing
// all of it into a small software surface.
// Seeds: tests/fuzz/seeds/drawing (drawings from the corpus and the unit tests, made by
// make_seeds.py, and a few pictures).
//
// An input is a picture when it doesn't start with '<'; otherwise it is a drawing's XML (a
// w:drawing, wp:inline, w:pict, a v: shape, or a p:/xdr:/dsp: shape) followed by optional
// sections, each starting on a line "@<name>": "@theme" is the theme part, any other name a
// relationship id whose content is a picture or, when it parses as XML, a part.
//   <wp:inline ...>...<a:blip r:embed="rId1"/>...</wp:inline>
//   @rId1
//   <svg xmlns="http://www.w3.org/2000/svg" ...>...</svg>
#include <windows.h>

#include <d2d1_1.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "draw/D2DPainter.h"
#include "draw/Drawing.h"
#include "draw/Images.h"
#include "draw/Theme.h"
#include "pkg/XmlDom.h"

using Microsoft::WRL::ComPtr;
using namespace draw;

namespace {

constexpr int kSurface = 64;

const ImageLimits& FuzzImageLimits() {
  static const ImageLimits limits = [] {
    ImageLimits l;
    l.maxBytes = 1u << 20;
    l.maxDimension = 4096;
    l.maxSourcePixels = 1u << 22;
    l.maxDecodedPixels = 1u << 18;
    return l;
  }();
  return limits;
}

fw::pkg::Limits FuzzXmlLimits() {
  fw::pkg::Limits limits;
  limits.maxStringLength = 1u << 20;
  return limits;
}

void Require(bool ok) {
  if (!ok) std::abort();
}

bool Finite(const Rect& r) {
  return std::isfinite(r.left) && std::isfinite(r.top) && std::isfinite(r.right) && std::isfinite(r.bottom);
}

// A software render target, made once.
struct Target {
  ComPtr<ID2D1Factory1> factory;
  ComPtr<IWICImagingFactory> wic;
  ComPtr<IWICBitmap> bitmap;
  ComPtr<ID2D1RenderTarget> target;
  ComPtr<ID2D1DeviceContext> dc;
  D2DBitmapCache cache;

  Target() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    D2D1_FACTORY_OPTIONS opts{};
    Require(SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &opts,
                                        reinterpret_cast<void**>(factory.GetAddressOf()))));
    Require(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))));
    Require(SUCCEEDED(wic->CreateBitmap(kSurface, kSurface, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap)));
    const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    Require(SUCCEEDED(factory->CreateWicBitmapRenderTarget(bitmap.Get(), props, &target)));
    Require(SUCCEEDED(target.As(&dc)));
  }
};

Target& Surface() {
  static Target t;
  return t;
}

class FuzzResources : public DrawingResources {
 public:
  std::map<std::string, std::string, std::less<>> sections;
  std::shared_ptr<const Theme> theme;

  std::shared_ptr<const Theme> GetTheme() override { return theme ? theme : Theme::Default(); }
  std::shared_ptr<const Image> GetImage(std::string_view relId) override {
    auto it = sections.find(relId);
    if (it == sections.end()) return nullptr;
    auto& slot = images_[it->first];
    if (!slot) slot = Image::Open(std::vector<uint8_t>(it->second.begin(), it->second.end()), FuzzImageLimits());
    return slot;
  }
  pugi::xml_node GetPartXml(std::string_view relId) override {
    auto it = sections.find(relId);
    if (it == sections.end()) return {};
    auto& dom = parts_[it->first];
    if (!dom) {
      dom = std::make_unique<fw::pkg::XmlDom>();
      if (!dom->Parse(it->second, FuzzXmlLimits()).Ok()) return {};
    }
    return dom->Root();
  }

 private:
  std::map<std::string, std::shared_ptr<const Image>> images_;
  std::map<std::string, std::unique_ptr<fw::pkg::XmlDom>> parts_;
};

// Everything a caller asks of a drawing, then the drawing itself, scaled into the surface.
void Exercise(const Drawing& d) {
  Require(Finite(d.Bounds()) && d.Extent().width >= 0 && d.Extent().height >= 0);
  Require(Finite(d.InkBounds()));
  for (const Path::Polygon& poly : d.WrapOutline())
    for (const Point& p : poly.points) Require(std::isfinite(p.x) && std::isfinite(p.y));
  for (const TextBox& box : d.TextBoxes()) Require(Finite(box.rect));
  (void)d.Name();
  (void)d.Description();

  Target& t = Surface();
  const Size e = d.Extent();
  const float scale = std::min(1.f, kSurface / std::max({e.width, e.height, 1e-3f}));
  t.target->BeginDraw();
  t.target->Clear(D2D1::ColorF(1, 1, 1, 1));
  {
    D2DPainter painter(t.dc.Get(), &t.cache);
    d.Draw(painter, {0, 0, e.width * scale, e.height * scale});
  }
  t.target->EndDraw();  // a failure (a device that can't draw something) is fine; a crash isn't
  t.cache.Clear();
}

void FuzzImage(const uint8_t* data, size_t size) {
  std::string error;
  std::shared_ptr<Image> img = Image::Open(std::vector<uint8_t>(data, data + size), FuzzImageLimits(), &error);
  if (!img) {
    Require(!error.empty());
    return;
  }
  Require(img->Width() >= 0 && img->Height() >= 0);
  Require(img->Width() <= static_cast<int>(FuzzImageLimits().maxDimension));
  if (std::shared_ptr<const Bitmap> px = img->Pixels(48, 32)) {
    Require(px->Width() > 0 && px->Height() > 0);
    Require(static_cast<uint64_t>(px->Width()) * px->Height() <= FuzzImageLimits().maxDecodedPixels);
  }
  if (img->Vector()) (void)RasterizeVectorImage(*img->Vector(), 24, 16);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  std::string_view input(reinterpret_cast<const char*>(data), size);
  if (input.empty() || input.front() != '<') {
    FuzzImage(data, size);
    return 0;
  }
  FuzzResources res;
  size_t end = input.find("\n@");
  std::string_view xml = input.substr(0, end);
  while (end != std::string_view::npos) {
    const size_t start = end + 2, eol = input.find('\n', start);
    if (eol == std::string_view::npos) break;
    std::string_view name = input.substr(start, eol - start);
    if (!name.empty() && name.back() == '\r') name.remove_suffix(1);
    end = input.find("\n@", eol);
    std::string_view body = input.substr(eol + 1, (end == std::string_view::npos ? input.size() : end) - eol - 1);
    if (name == "theme") res.theme = Theme::Parse(body);
    else res.sections.emplace(std::string(name), std::string(body));
  }

  fw::pkg::XmlDom dom;
  if (!dom.Parse(xml, FuzzXmlLimits()).Ok()) return 0;
  pugi::xml_node root = dom.Root();
  // Every reader on the root, and the shape-tree reader on its children (p:spTree and the like).
  std::unique_ptr<Drawing> made[] = {Drawing::FromWordDrawing(root, res), Drawing::FromVml(root, res),
                                      Drawing::FromShapeTree(root, res)};
  for (const auto& d : made) {
    if (d) Exercise(*d);
  }
  int children = 0;
  for (pugi::xml_node c = root.first_child(); c && children < 64; c = c.next_sibling(), ++children) {
    if (c.type() != pugi::node_element) continue;
    if (auto d = Drawing::FromShapeTree(c, res)) Exercise(*d);
    if (auto d = Drawing::FromVml(c, res)) Exercise(*d);
  }
  return 0;
}
