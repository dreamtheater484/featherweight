#include "draw/Images.h"

#include <windows.h>

#include <d2d1_1.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string_view>
#include <vector>

#include "draw/D2DPainter.h"
#include "pkg/XmlDom.h"

using Microsoft::WRL::ComPtr;

namespace draw {
namespace {

constexpr float kPointsPerInch = 72.f;

// COM for the calling thread while a picture is decoded: initialized here if nobody did, and only
// then uninitialized again.
class ComScope {
 public:
  ComScope() : hr_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
  ~ComScope() {
    if (SUCCEEDED(hr_)) CoUninitialize();
  }
  ComScope(const ComScope&) = delete;
  ComScope& operator=(const ComScope&) = delete;

 private:
  HRESULT hr_;
};

uint16_t U16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
int16_t S16(const uint8_t* p) { return static_cast<int16_t>(U16(p)); }
uint32_t U32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }
int32_t S32(const uint8_t* p) { return static_cast<int32_t>(U32(p)); }

// Windows' own decoders. CoCreateInstance with these classes never reaches a codec someone else
// installed, which WIC's own format detection (CreateDecoderFromStream) could.
const CLSID* BuiltInDecoder(ImageFormat f) {
  switch (f) {
    case ImageFormat::Png: return &CLSID_WICPngDecoder2;
    case ImageFormat::Jpeg: return &CLSID_WICJpegDecoder;
    case ImageFormat::Gif: return &CLSID_WICGifDecoder;
    case ImageFormat::Bmp: return &CLSID_WICBmpDecoder;
    case ImageFormat::Tiff: return &CLSID_WICTiffDecoder;
    case ImageFormat::Ico: return &CLSID_WICIcoDecoder;
    default: return nullptr;
  }
}

// One open WIC decoder and the frame to show: the first, or for icons the largest.
struct Decoder {
  ComPtr<IWICImagingFactory> factory;
  ComPtr<IWICStream> stream;
  ComPtr<IWICBitmapDecoder> decoder;
  ComPtr<IWICBitmapFrameDecode> frame;

  bool Open(ImageFormat format, const std::vector<uint8_t>& bytes) {
    const CLSID* clsid = BuiltInDecoder(format);
    if (!clsid || bytes.size() > UINT32_MAX) return false;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(bytes.data()), static_cast<DWORD>(bytes.size()))) ||
        FAILED(CoCreateInstance(*clsid, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&decoder))) ||
        FAILED(decoder->Initialize(stream.Get(), WICDecodeMetadataCacheOnDemand))) {
      return false;
    }
    UINT frames = 0;
    if (FAILED(decoder->GetFrameCount(&frames)) || frames == 0) return false;
    UINT best = 0;
    if (format == ImageFormat::Ico) {
      uint64_t bestArea = 0;
      for (UINT i = 0; i < std::min<UINT>(frames, 64); ++i) {
        ComPtr<IWICBitmapFrameDecode> f;
        UINT w = 0, h = 0;
        if (SUCCEEDED(decoder->GetFrame(i, &f)) && SUCCEEDED(f->GetSize(&w, &h)) && uint64_t{w} * h > bestArea) {
          bestArea = uint64_t{w} * h;
          best = i;
        }
      }
    }
    return SUCCEEDED(decoder->GetFrame(best, &frame));
  }
};

// The ENHMETAHEADER's frame (in 0.01 mm), as points.
bool EmfSize(const std::vector<uint8_t>& b, Size& size) {
  if (b.size() < 88 || U32(&b[0]) != 1 || U32(&b[40]) != 0x464D4520) return false;
  const uint32_t headerSize = U32(&b[4]), total = U32(&b[48]);
  if (headerSize < 88 || headerSize > b.size() || total < headerSize || total > b.size()) return false;
  const double w = (static_cast<double>(S32(&b[32])) - S32(&b[24])) / 100.0;  // mm
  const double h = (static_cast<double>(S32(&b[36])) - S32(&b[28])) / 100.0;
  size = {static_cast<float>(std::abs(w) * kPointsPerInch / 25.4), static_cast<float>(std::abs(h) * kPointsPerInch / 25.4)};
  return true;
}

// WMF → EMF through GDI, honoring the placeable header (Aldus) when there is one.
bool WmfToEmf(const std::vector<uint8_t>& b, std::vector<uint8_t>& emf, Size& size) {
  size_t start = 0;
  METAFILEPICT pict{MM_ANISOTROPIC, 0, 0, nullptr};
  bool placeable = false;
  if (b.size() >= 22 && U32(&b[0]) == 0x9AC6CDD7u) {
    const int w = S16(&b[10]) - S16(&b[6]), h = S16(&b[12]) - S16(&b[8]), inch = U16(&b[14]);
    if (inch > 0 && w != 0 && h != 0) {
      pict.xExt = MulDiv(std::abs(w), 2540, inch);  // HIMETRIC
      pict.yExt = MulDiv(std::abs(h), 2540, inch);
      size = {std::abs(w) * kPointsPerInch / inch, std::abs(h) * kPointsPerInch / inch};
      placeable = true;
    }
    start = 22;
  }
  // META_HEADER: type 1 or 2, header size 9 words, total size in words.
  if (b.size() < start + 18) return false;
  const uint8_t* h = &b[start];
  if ((U16(h) != 1 && U16(h) != 2) || U16(h + 2) != 9) return false;
  const uint64_t words = U32(h + 6);
  if (words < 9 || words * 2 > b.size() - start) return false;
  HENHMETAFILE e = SetWinMetaFileBits(static_cast<UINT>(words * 2), h, nullptr, placeable ? &pict : nullptr);
  if (!e) return false;
  const UINT n = GetEnhMetaFileBits(e, 0, nullptr);
  emf.resize(n);
  const bool ok = n > 0 && GetEnhMetaFileBits(e, n, emf.data()) == n;
  DeleteEnhMetaFile(e);
  Size frame;
  if (!ok || !EmfSize(emf, frame)) return false;
  if (!placeable) size = frame;
  return true;
}

// An SVG length ("12pt", "3.5cm", "40", "100%") in points; 0 when it can't be used.
float SvgLength(std::string_view v) {
  while (!v.empty() && v.front() == ' ') v.remove_prefix(1);
  double n = 0;
  auto [end, ec] = std::from_chars(v.data(), v.data() + v.size(), n);
  if (ec != std::errc() || !(n > 0) || n > 1e6) return 0;
  std::string_view unit(end, v.data() + v.size() - end);
  while (!unit.empty() && unit.back() == ' ') unit.remove_suffix(1);
  double pt = 0;
  if (unit.empty() || unit == "px") pt = n * 0.75;
  else if (unit == "pt") pt = n;
  else if (unit == "pc") pt = n * 12;
  else if (unit == "in") pt = n * 72;
  else if (unit == "cm") pt = n * 72 / 2.54;
  else if (unit == "mm") pt = n * 72 / 25.4;
  else if (unit == "em") pt = n * 12;
  return static_cast<float>(pt);
}

// Reads an SVG's size and decides whether Direct2D draws it faithfully: no text (Direct2D's SVG
// support draws none), no foreign content, and no reference to anything outside the document,
// which must never be fetched (plan §4.7).
bool InspectSvg(const std::vector<uint8_t>& bytes, Size& size, bool& faithful) {
  fw::pkg::XmlDom dom;
  if (!dom.Parse(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size())).Ok()) return false;
  pugi::xml_node root = dom.Root();
  constexpr std::string_view kSvg = "http://www.w3.org/2000/svg";
  if (!fw::pkg::IsElement(root, kSvg, "svg")) return false;
  float w = SvgLength(root.attribute("width").value()), h = SvgLength(root.attribute("height").value());
  float vb[4] = {};
  std::string_view box = root.attribute("viewBox").value();
  for (float& f : vb) {
    while (!box.empty() && (box.front() == ' ' || box.front() == ',')) box.remove_prefix(1);
    auto [end, ec] = std::from_chars(box.data(), box.data() + box.size(), f);
    if (ec != std::errc()) break;
    box.remove_prefix(end - box.data());
  }
  if (w <= 0) w = vb[2] > 0 ? vb[2] * 0.75f : 225;
  if (h <= 0) h = vb[3] > 0 ? vb[3] * 0.75f : 112.5f;
  size = {w, h};
  faithful = true;
  // Iterative walk: SVG nesting is untrusted.
  for (pugi::xml_node n = root; n && faithful;) {
    if (n.type() == pugi::node_element) {
      std::string_view local = fw::pkg::LocalNameOf(n);
      if (local == "text" || local == "foreignObject" || local == "script" || local == "switch") faithful = false;
      for (pugi::xml_attribute a : n.attributes()) {
        std::string_view name = fw::pkg::LocalNameOf(a), value = a.value();
        if (name != "href" && name != "src") continue;
        while (!value.empty() && value.front() == ' ') value.remove_prefix(1);
        if (!value.starts_with("#") && !value.starts_with("data:")) faithful = false;
      }
      if (std::string_view(n.child_value()).find("url(") != std::string_view::npos && local == "style") {
        faithful = false;  // stylesheets can name outside files
      }
    }
    if (n.first_child()) {
      n = n.first_child();
      continue;
    }
    while (n && n != root && !n.next_sibling()) n = n.parent();
    if (!n || n == root) break;
    n = n.next_sibling();
  }
  return true;
}

}  // namespace

ImageFormat SniffImageFormat(const uint8_t* d, size_t n) {
  auto starts = [&](std::initializer_list<uint8_t> sig) {
    return n >= sig.size() && std::equal(sig.begin(), sig.end(), d);
  };
  if (starts({0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A})) return ImageFormat::Png;
  if (starts({0xFF, 0xD8, 0xFF})) return ImageFormat::Jpeg;
  if (starts({'G', 'I', 'F', '8'})) return ImageFormat::Gif;
  if (starts({'B', 'M'})) return ImageFormat::Bmp;
  if (starts({'I', 'I', 42, 0}) || starts({'M', 'M', 0, 42})) return ImageFormat::Tiff;
  if (starts({0, 0, 1, 0}) && n >= 6 && U16(d + 4) > 0) return ImageFormat::Ico;
  if (n >= 44 && U32(d) == 1 && U32(d + 40) == 0x464D4520) return ImageFormat::Emf;
  if (starts({0xD7, 0xCD, 0xC6, 0x9A})) return ImageFormat::Wmf;
  if (n >= 18 && (U16(d) == 1 || U16(d) == 2) && U16(d + 2) == 9 && (U16(d + 4) == 0x100 || U16(d + 4) == 0x300)) {
    return ImageFormat::Wmf;
  }
  // SVG: XML text whose root is <svg>. Look past a BOM, the declaration and comments.
  std::string_view text(reinterpret_cast<const char*>(d), std::min<size_t>(n, 4096));
  if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
  size_t at = text.find("<svg");
  if (at == std::string_view::npos) return ImageFormat::Unknown;
  if (text.substr(0, at).find_first_not_of(" \t\r\n") == std::string_view::npos) return ImageFormat::Svg;
  if (text.starts_with("<?xml") || text.starts_with("<!--") || text.starts_with("<!DOCTYPE")) return ImageFormat::Svg;
  return ImageFormat::Unknown;
}

namespace {

// What every PixelHolder keeps, for the budget. A few hundred holders at most, so a list scanned
// in full is enough.
struct PixelBudget {
  struct Kept {
    const PixelHolder* holder;
    std::weak_ptr<const PixelHolder> ref;
    uint64_t bytes;
    uint64_t lastUse;
  };
  std::mutex mutex;
  std::vector<Kept> kept;
  uint64_t budget = 48ull << 20, total = 0, clock = 0;

  // Over the budget: the least recently used holders' entries, removed; they drop their pixels
  // once the lock is released.
  void Shrink(const PixelHolder* keep, std::vector<std::shared_ptr<const PixelHolder>>& drop) {
    while (total > budget) {
      auto victim = kept.end();
      for (auto it = kept.begin(); it != kept.end(); ++it) {
        if (it->holder != keep && (victim == kept.end() || it->lastUse < victim->lastUse)) victim = it;
      }
      if (victim == kept.end()) break;
      total -= victim->bytes;
      if (auto holder = victim->ref.lock()) drop.push_back(std::move(holder));
      kept.erase(victim);
    }
  }
};

PixelBudget& Budget() {
  static PixelBudget budget;
  return budget;
}

}  // namespace

PixelHolder::~PixelHolder() {
  PixelBudget& b = Budget();
  std::lock_guard<std::mutex> lock(b.mutex);
  auto it = std::find_if(b.kept.begin(), b.kept.end(), [&](const PixelBudget::Kept& k) { return k.holder == this; });
  if (it == b.kept.end()) return;
  b.total -= it->bytes;
  b.kept.erase(it);
}

void KeepPixels(const std::shared_ptr<const PixelHolder>& holder, uint64_t bytes) {
  std::vector<std::shared_ptr<const PixelHolder>> drop;
  {
    PixelBudget& b = Budget();
    std::lock_guard<std::mutex> lock(b.mutex);
    auto it = std::find_if(b.kept.begin(), b.kept.end(),
                           [&](const PixelBudget::Kept& k) { return k.holder == holder.get(); });
    if (it == b.kept.end()) it = b.kept.insert(b.kept.end(), {holder.get(), holder, 0, 0});
    b.total = b.total - it->bytes + bytes;
    it->bytes = bytes;
    it->lastUse = ++b.clock;
    b.Shrink(holder.get(), drop);
  }
  for (const auto& h : drop) h->DropPixels();
}

void UsedPixels(const PixelHolder* holder) {
  PixelBudget& b = Budget();
  std::lock_guard<std::mutex> lock(b.mutex);
  for (PixelBudget::Kept& k : b.kept) {
    if (k.holder == holder) k.lastUse = ++b.clock;
  }
}

void SetPixelBudget(uint64_t bytes) {
  std::vector<std::shared_ptr<const PixelHolder>> drop;
  {
    PixelBudget& b = Budget();
    std::lock_guard<std::mutex> lock(b.mutex);
    b.budget = bytes;
    b.Shrink(nullptr, drop);
  }
  for (const auto& h : drop) h->DropPixels();
}

uint64_t PixelBytesKept() {
  PixelBudget& b = Budget();
  std::lock_guard<std::mutex> lock(b.mutex);
  return b.total;
}

const ImageLimits& ImageLimits::Default() {
  static const ImageLimits limits;
  return limits;
}

std::shared_ptr<Image> Image::Open(std::vector<uint8_t> bytes, const ImageLimits& limits, std::string* error) {
  auto fail = [&](const char* why) {
    if (error) *error = why;
    return nullptr;
  };
  if (bytes.size() > limits.maxBytes) return fail("The picture is larger than Featherweight opens.");
  std::shared_ptr<Image> img(new Image());
  img->limits_ = limits;
  img->format_ = SniffImageFormat(bytes.data(), bytes.size());
  switch (img->format_) {
    case ImageFormat::Unknown:
      return fail("The picture is in a format Featherweight can't show.");
    case ImageFormat::Emf: {
      Size size;
      if (!EmfSize(bytes, size)) return fail("The picture is damaged.");
      img->natural_ = size;
      img->vector_ = std::make_shared<VectorImage>(VectorImage::Kind::Emf, std::move(bytes), size);
      return img;
    }
    case ImageFormat::Wmf: {
      std::vector<uint8_t> emf;
      Size size;
      if (!WmfToEmf(bytes, emf, size)) return fail("The picture is damaged.");
      img->natural_ = size;
      img->vector_ = std::make_shared<VectorImage>(VectorImage::Kind::Emf, std::move(emf), size);
      return img;
    }
    case ImageFormat::Svg: {
      Size size;
      bool faithful = false;
      if (!InspectSvg(bytes, size, faithful)) return fail("The picture is damaged.");
      img->natural_ = size;
      img->faithful_ = faithful;
      img->vector_ = std::make_shared<VectorImage>(VectorImage::Kind::Svg, std::move(bytes), size);
      return img;
    }
    default:
      break;
  }
  ComScope com;
  Decoder dec;
  UINT w = 0, h = 0;
  if (!dec.Open(img->format_, bytes) || FAILED(dec.frame->GetSize(&w, &h)) || w == 0 || h == 0) {
    return fail("The picture is damaged.");
  }
  if (w > limits.maxDimension || h > limits.maxDimension || uint64_t{w} * h > limits.maxSourcePixels) {
    return fail("The picture is larger than Featherweight shows.");
  }
  double dpiX = 96, dpiY = 96;
  if (FAILED(dec.frame->GetResolution(&dpiX, &dpiY)) || !(dpiX >= 1 && dpiX <= 10000)) dpiX = 96;
  if (!(dpiY >= 1 && dpiY <= 10000)) dpiY = dpiX;
  img->width_ = static_cast<int>(w);
  img->height_ = static_cast<int>(h);
  img->natural_ = {static_cast<float>(w * kPointsPerInch / dpiX), static_cast<float>(h * kPointsPerInch / dpiY)};
  img->bytes_ = std::move(bytes);
  return img;
}

std::shared_ptr<Image> Image::OpenFrom(Reader read, const ImageLimits& limits, std::string* error) {
  std::vector<uint8_t> bytes;
  if (!read || !read(bytes)) {
    if (error) *error = "The picture couldn't be read.";
    return nullptr;
  }
  std::shared_ptr<Image> img = Open(std::move(bytes), limits, error);
  if (img && !img->vector_) {
    img->bytes_ = {};  // read again for each decode
    img->reader_ = std::move(read);
  }
  return img;
}

std::shared_ptr<const Bitmap> Image::Pixels(int width, int height) const {
  std::shared_ptr<const Bitmap> px;
  bool made = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (vector_) {
      // Rasterized at the size asked for (the natural size at 96 dpi by default).
      int w = width > 0 ? width : static_cast<int>(std::ceil(natural_.width / 0.75f));
      int h = height > 0 ? height : static_cast<int>(std::ceil(natural_.height / 0.75f));
      w = std::clamp(w, 1, 4096);
      h = std::clamp(h, 1, 4096);
      if (!pixels_ || pixels_->Width() < w || pixels_->Height() < h) {
        if (auto r = RasterizeVectorImage(*vector_, w, h)) {
          pixels_ = std::move(r);
          made = true;
        }
      }
    } else if (!failed_) {
      // Small pictures are decoded whole once. Larger ones at the size asked for, rounded up to a
      // power-of-two reduction, so zooming in doesn't decode again at every step.
      int w = width_, h = height_;
      if (width > 0 && height > 0 && uint64_t{static_cast<uint32_t>(width_)} * height_ > (1u << 20)) {
        while (w / 2 >= width && h / 2 >= height && w / 2 >= 1 && h / 2 >= 1) {
          w /= 2;
          h /= 2;
        }
      }
      const double area = static_cast<double>(w) * h;
      if (area > static_cast<double>(limits_.maxDecodedPixels)) {
        const double f = std::sqrt(static_cast<double>(limits_.maxDecodedPixels) / area);
        w = std::max(1, static_cast<int>(w * f));
        h = std::max(1, static_cast<int>(h * f));
      }
      if (!pixels_ || (pixels_->Width() < w && pixels_->Width() != width_)) {
        if (auto d = Decode(w, h)) {
          pixels_ = std::move(d);
          made = true;
        } else if (!pixels_) {
          failed_ = true;
        }
      }
    }
    px = pixels_;
  }
  // Outside the lock: the budget may ask other pictures to drop theirs.
  if (made) KeepPixels(shared_from_this(), static_cast<uint64_t>(px->Stride()) * px->Height());
  else if (px) UsedPixels(this);
  return px;
}

void Image::DropPixels() const {
  std::lock_guard<std::mutex> lock(mutex_);
  pixels_.reset();
}

std::shared_ptr<const Bitmap> Image::Decode(int width, int height) const {
  std::vector<uint8_t> read;
  if (reader_ && !reader_(read)) return nullptr;
  ComScope com;
  Decoder dec;
  UINT w = 0, h = 0;
  if (!dec.Open(format_, reader_ ? read : bytes_) || FAILED(dec.frame->GetSize(&w, &h)) ||
      w != static_cast<UINT>(width_) || h != static_cast<UINT>(height_)) {
    return nullptr;
  }
  // A JPEG is scaled straight from the decoder, which lets WIC decode it at a reduced size; other
  // formats are premultiplied first, so transparent edges don't bleed when scaled.
  const bool scale = width != width_ || height != height_;
  const bool scaleFirst = scale && format_ == ImageFormat::Jpeg;
  ComPtr<IWICBitmapSource> source = dec.frame;
  auto scaled = [&](IWICBitmapSource* from) -> bool {
    ComPtr<IWICBitmapScaler> scaler;
    if (FAILED(dec.factory->CreateBitmapScaler(&scaler)) ||
        FAILED(scaler->Initialize(from, width, height, WICBitmapInterpolationModeFant))) {
      return false;
    }
    source = scaler;
    return true;
  };
  if (scaleFirst && !scaled(source.Get())) return nullptr;
  ComPtr<IWICFormatConverter> converter;
  if (FAILED(dec.factory->CreateFormatConverter(&converter)) ||
      FAILED(converter->Initialize(source.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                   WICBitmapPaletteTypeCustom))) {
    return nullptr;
  }
  source = converter;
  if (scale && !scaleFirst && !scaled(source.Get())) return nullptr;
  std::vector<uint8_t> px(static_cast<size_t>(width) * height * 4);
  if (FAILED(source->CopyPixels(nullptr, width * 4, static_cast<UINT>(px.size()), px.data()))) return nullptr;
  return std::make_shared<Bitmap>(width, height, std::move(px));
}

std::shared_ptr<const Bitmap> RasterizeVectorImage(const VectorImage& image, int width, int height) {
  if (width <= 0 || height <= 0 || width > 16384 || height > 16384) return nullptr;
  ComScope com;
  ComPtr<ID2D1Factory1> factory;
  ComPtr<IWICImagingFactory> wic;
  ComPtr<IWICBitmap> bitmap;
  ComPtr<ID2D1RenderTarget> target;
  ComPtr<ID2D1DeviceContext> dc;
  D2D1_FACTORY_OPTIONS opts{};
  if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &opts,
                               reinterpret_cast<void**>(factory.GetAddressOf()))) ||
      FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) ||
      FAILED(wic->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap))) {
    return nullptr;
  }
  D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
      D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
      96.f, 96.f);
  if (FAILED(factory->CreateWicBitmapRenderTarget(bitmap.Get(), props, &target)) || FAILED(target.As(&dc))) {
    return nullptr;
  }
  dc->BeginDraw();
  dc->Clear(D2D1::ColorF(0, 0, 0, 0));
  {
    D2DPainter painter(dc.Get());
    painter.DrawVectorImage(image, {0, 0, static_cast<float>(width), static_cast<float>(height)});
  }
  if (FAILED(dc->EndDraw())) return nullptr;
  std::vector<uint8_t> px(static_cast<size_t>(width) * height * 4);
  if (FAILED(bitmap->CopyPixels(nullptr, width * 4, static_cast<UINT>(px.size()), px.data()))) return nullptr;
  return std::make_shared<Bitmap>(width, height, std::move(px));
}

}  // namespace draw
