#include <windows.h>

#include <d2d1_1.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>

#include "draw/D2DPainter.h"
#include "slide/render/SlideRender.h"

using Microsoft::WRL::ComPtr;

namespace fw::slide::render {

std::shared_ptr<const draw::Bitmap> RenderBitmap(const SlideRender& slide, int width, int height, DrawFilter* filter) {
  if (width <= 0 || height <= 0 || width > 16384 || height > 16384) return nullptr;
  // One factory per thread: making them costs more than drawing a thumbnail.
  thread_local ComPtr<ID2D1Factory1> factory;
  thread_local ComPtr<IWICImagingFactory> wic;
  if (!factory) {
    D2D1_FACTORY_OPTIONS opts{};
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &opts,
                                 reinterpret_cast<void**>(factory.GetAddressOf()))))
      return nullptr;
  }
  if (!wic && FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return nullptr;
  ComPtr<IWICBitmap> bitmap;
  ComPtr<ID2D1RenderTarget> target;
  ComPtr<ID2D1DeviceContext> dc;
  if (FAILED(wic->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap))) return nullptr;
  const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
      D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
  if (FAILED(factory->CreateWicBitmapRenderTarget(bitmap.Get(), props, &target)) || FAILED(target.As(&dc))) return nullptr;
  if (FAILED(draw::DrawFrame(dc.Get(), [&](bool fallback) {
        dc->Clear(D2D1::ColorF(1, 1, 1, 1));
        draw::D2DPainter p(dc.Get());
        p.UsePdfLikeDrawing();
        p.SetNoShadows(fallback);
        slide.Draw(p, {0, 0, static_cast<float>(width), static_cast<float>(height)}, filter);
      })))
    return nullptr;
  WICRect all{0, 0, width, height};
  ComPtr<IWICBitmapLock> lock;
  if (FAILED(bitmap->Lock(&all, WICBitmapLockRead, &lock))) return nullptr;
  UINT stride = 0, size = 0;
  BYTE* data = nullptr;
  if (FAILED(lock->GetStride(&stride)) || FAILED(lock->GetDataPointer(&size, &data)) || !data) return nullptr;
  std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
  for (int y = 0; y < height; ++y) std::memcpy(&pixels[static_cast<size_t>(y) * width * 4], data + static_cast<size_t>(y) * stride, width * 4);
  return std::make_shared<draw::Bitmap>(width, height, std::move(pixels));
}

Thumbnails::Thumbnails(MakeSlide make) : make_(std::move(make)) {}

void Thumbnails::Reset(size_t count, int width, int height) {
  bitmaps_.assign(count, nullptr);
  due_.assign(count, true);
  width_ = width;
  height_ = height;
  first_ = last_ = 0;
}

void Thumbnails::Invalidate(size_t index) {
  if (index < due_.size()) due_[index] = true;
}

void Thumbnails::Prefer(size_t first, size_t last) {
  first_ = first;
  last_ = last;
}

bool Thumbnails::Done() const { return std::find(due_.begin(), due_.end(), true) == due_.end(); }

bool Thumbnails::RenderNext() {
  size_t next = due_.size();
  for (size_t i = first_; i <= last_ && i < due_.size(); ++i)
    if (due_[i]) {
      next = i;
      break;
    }
  if (next == due_.size()) next = std::find(due_.begin(), due_.end(), true) - due_.begin();
  if (next >= due_.size()) return false;
  due_[next] = false;
  if (std::unique_ptr<SlideRender> slide = make_ ? make_(next) : nullptr) bitmaps_[next] = RenderBitmap(*slide, width_, height_);
  return !Done();
}

std::shared_ptr<const draw::Bitmap> Thumbnails::Get(size_t index) const { return index < bitmaps_.size() ? bitmaps_[index] : nullptr; }

}  // namespace fw::slide::render
