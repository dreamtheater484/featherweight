#pragma once

#include <windows.h>

#include <d3d11.h>
#include <dxgi1_2.h>

#include "ui/Gfx.h"

// The Direct3D 11 device that every window and off-screen surface draws with, shared by
// Direct2D (plan §4.2, decision 20). It's created on first use: the hardware adapter, or WARP
// when there is none or it fails.
namespace gpu {

ID2D1Device* D2DDevice();
ID3D11Device* D3DDevice();
bool IsWarp();
// Use WARP even when a hardware adapter works (for tests and troubleshooting). Call it before
// anything draws.
void ForceWarp();

// Call this when drawing reports a lost device (D2DERR_RECREATE_TARGET, DXGI_ERROR_DEVICE_REMOVED
// or DXGI_ERROR_DEVICE_RESET). The device is recreated on next use and Generation() changes, so
// every owner of device-bound resources (bitmaps, effects, brushes) knows to drop them.
void DeviceLost();
unsigned Generation();
bool IsDeviceLoss(HRESULT hr);

// A new device context, for drawing off-screen.
ComPtr<ID2D1DeviceContext> CreateContext();

}  // namespace gpu

// A window's flip-model swap chain and the Direct2D device context that draws into it.
//
//   if (!chain.DC() && !chain.Create(hwnd)) return;
//   chain.Resize(w, h);
//   chain.DC()->BeginDraw(); ...; HRESULT hr = chain.DC()->EndDraw();
//   if (SUCCEEDED(hr)) hr = chain.Present();
//   if (gpu::IsDeviceLoss(hr)) { gpu::DeviceLost(); chain.Reset(); }
class SwapChain {
 public:
  // Creates the swap chain at the window's client size. False if no device can be used.
  bool Create(HWND hwnd);
  void Reset();
  // Resizes the buffers (a no-op at the current size). Call it before drawing, and from
  // WM_SIZE so a frame drawn during a live resize isn't stretched.
  void Resize(UINT w, UINT h);
  HRESULT Present();

  ID2D1DeviceContext* DC() const { return dc_.Get(); }
  // The device generation this swap chain was made with (see gpu::Generation).
  unsigned Generation() const { return generation_; }

 private:
  bool CreateTargetBitmap();

  ComPtr<IDXGISwapChain1> swap_;
  ComPtr<ID2D1DeviceContext> dc_;
  ComPtr<ID2D1Bitmap1> target_;
  UINT w_ = 0, h_ = 0;
  unsigned generation_ = 0;
  HRESULT lost_ = S_OK;
};
