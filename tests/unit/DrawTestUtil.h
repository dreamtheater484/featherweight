#pragma once

// Rendering helpers for the fwdraw tests: Direct2D's software renderer into a WIC bitmap, so
// results are the same on every PC.
#include <windows.h>

#include <d2d1_1.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdint>

namespace drawtest {

// An off-screen surface drawn by Direct2D's software renderer, so results are the same on every PC.
struct Surface {
  Microsoft::WRL::ComPtr<ID2D1Factory1> factory;
  Microsoft::WRL::ComPtr<IWICImagingFactory> wic;
  Microsoft::WRL::ComPtr<IWICBitmap> bitmap;
  Microsoft::WRL::ComPtr<ID2D1RenderTarget> target;
  Microsoft::WRL::ComPtr<ID2D1DeviceContext> dc;
  int w, h;

  Surface(int width, int height) : w(width), h(height) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    D2D1_FACTORY_OPTIONS opts{};
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &opts,
                      reinterpret_cast<void**>(factory.GetAddressOf()));
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    wic->CreateBitmap(w, h, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap);
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    factory->CreateWicBitmapRenderTarget(bitmap.Get(), props, &target);
    target.As(&dc);
    dc->BeginDraw();
    dc->Clear(D2D1::ColorF(1, 1, 1, 1));
  }

  // Ends drawing; true if Direct2D accepted everything (pushes and pops matched).
  bool End() { return SUCCEEDED(dc->EndDraw()); }

  // The pixel as 0xRRGGBB (the surface is opaque).
  uint32_t At(int x, int y) {
    WICRect r{x, y, 1, 1};
    Microsoft::WRL::ComPtr<IWICBitmapLock> lock;
    bitmap->Lock(&r, WICBitmapLockRead, &lock);
    UINT size = 0;
    BYTE* p = nullptr;
    lock->GetDataPointer(&size, &p);
    return (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[1]) << 8) | p[0];
  }
};

inline int Red(uint32_t c) { return (c >> 16) & 0xFF; }
inline int Green(uint32_t c) { return (c >> 8) & 0xFF; }
inline int Blue(uint32_t c) { return c & 0xFF; }

}  // namespace drawtest
