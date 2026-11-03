#include "ui/SwapChain.h"

#include <algorithm>

#include "draw/D2DDevice.h"

namespace gpu {
namespace {

ComPtr<ID3D11Device> g_d3d;
ComPtr<ID2D1Device> g_device;
bool g_warp = false;
bool g_forceWarp = false;
unsigned g_generation = 1;

bool Ensure() {
  if (g_device) return true;
  g_warp = g_forceWarp || FAILED(draw::CreateD2DDevice(gfx::D2D(), false, g_d3d, g_device));
  if (g_warp && FAILED(draw::CreateD2DDevice(gfx::D2D(), true, g_d3d, g_device))) return false;
  // Present no more than one frame ahead, so typing and scrolling show up on the next frame.
  ComPtr<IDXGIDevice1> dxgi;
  if (SUCCEEDED(g_d3d.As(&dxgi))) dxgi->SetMaximumFrameLatency(1);
  return true;
}

}  // namespace

ID2D1Device* D2DDevice() { return Ensure() ? g_device.Get() : nullptr; }
ID3D11Device* D3DDevice() { return Ensure() ? g_d3d.Get() : nullptr; }
bool IsWarp() { return Ensure() && g_warp; }
void ForceWarp() { g_forceWarp = true; }

void DeviceLost() {
  g_device.Reset();
  g_d3d.Reset();
  ++g_generation;
}

unsigned Generation() { return g_generation; }

bool IsDeviceLoss(HRESULT hr) {
  return hr == D2DERR_RECREATE_TARGET || hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET;
}

ComPtr<ID2D1DeviceContext> CreateContext() {
  ComPtr<ID2D1DeviceContext> dc;
  if (ID2D1Device* device = D2DDevice()) device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc);
  return dc;
}

}  // namespace gpu

bool SwapChain::Create(HWND hwnd) {
  Reset();
  ID3D11Device* d3d = gpu::D3DDevice();
  if (!d3d) return false;
  ComPtr<IDXGIDevice1> dxgi;
  ComPtr<IDXGIAdapter> adapter;
  ComPtr<IDXGIFactory2> factory;
  if (FAILED(d3d->QueryInterface(IID_PPV_ARGS(&dxgi))) || FAILED(dxgi->GetAdapter(&adapter)) ||
      FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
    return false;
  }
  RECT rc;
  GetClientRect(hwnd, &rc);
  w_ = static_cast<UINT>(std::max(1L, rc.right));
  h_ = static_cast<UINT>(std::max(1L, rc.bottom));
  DXGI_SWAP_CHAIN_DESC1 desc{};
  desc.Width = w_;
  desc.Height = h_;
  desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  desc.BufferCount = 2;
  // Not stretched while a live resize waits for the next frame: the old frame stays in place.
  desc.Scaling = DXGI_SCALING_NONE;
  desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
  if (FAILED(factory->CreateSwapChainForHwnd(d3d, hwnd, &desc, nullptr, nullptr, &swap_))) return false;
  // DXGI must not watch the window's messages or switch it to exclusive full screen on Alt+Enter.
  factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
  dc_ = gpu::CreateContext();
  if (!dc_ || !CreateTargetBitmap()) {
    Reset();
    return false;
  }
  generation_ = gpu::Generation();
  return true;
}

void SwapChain::Reset() {
  if (dc_) dc_->SetTarget(nullptr);
  target_.Reset();
  dc_.Reset();
  swap_.Reset();
  w_ = h_ = 0;
  lost_ = S_OK;
}

bool SwapChain::CreateTargetBitmap() {
  ComPtr<IDXGISurface> surface;
  if (FAILED(swap_->GetBuffer(0, IID_PPV_ARGS(&surface)))) return false;
  // Opaque, like the window: Direct2D can use ClearType for text.
  D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
      D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.f, 96.f);
  if (FAILED(dc_->CreateBitmapFromDxgiSurface(surface.Get(), &props, &target_))) return false;
  dc_->SetTarget(target_.Get());
  return true;
}

void SwapChain::Resize(UINT w, UINT h) {
  w = std::max(1u, w);
  h = std::max(1u, h);
  if (!swap_ || (w == w_ && h == h_)) return;
  // The buffers can't be resized while the device context still points at one.
  dc_->SetTarget(nullptr);
  target_.Reset();
  HRESULT hr = swap_->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);
  if (FAILED(hr) || !CreateTargetBitmap()) {
    lost_ = gpu::IsDeviceLoss(hr) ? hr : DXGI_ERROR_DEVICE_RESET;
    return;
  }
  w_ = w;
  h_ = h;
}

HRESULT SwapChain::Present() {
  if (!swap_) return E_FAIL;
  if (FAILED(lost_)) return lost_;
  HRESULT hr = swap_->Present(0, 0);
  // Minimized or covered: nothing to show, and nothing wrong.
  if (hr == DXGI_STATUS_OCCLUDED) hr = S_OK;
  return hr;
}
