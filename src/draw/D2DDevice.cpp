#include "draw/D2DDevice.h"

#include <dxgi.h>

#include <iterator>

using Microsoft::WRL::ComPtr;

namespace draw {

HRESULT CreateD2DDevice(ID2D1Factory1* factory, bool warp, ComPtr<ID3D11Device>& d3d, ComPtr<ID2D1Device>& d2d) {
  static const D3D_FEATURE_LEVEL kLevels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                              D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_9_3,  D3D_FEATURE_LEVEL_9_2,
                                              D3D_FEATURE_LEVEL_9_1};
  d3d.Reset();
  d2d.Reset();
  HRESULT hr = D3D11CreateDevice(nullptr, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                 D3D11_CREATE_DEVICE_BGRA_SUPPORT, kLevels, static_cast<UINT>(std::size(kLevels)),
                                 D3D11_SDK_VERSION, &d3d, nullptr, nullptr);
  ComPtr<IDXGIDevice> dxgi;
  if (SUCCEEDED(hr)) hr = d3d.As(&dxgi);
  if (SUCCEEDED(hr)) hr = factory->CreateDevice(dxgi.Get(), &d2d);
  if (FAILED(hr)) {
    d3d.Reset();
    d2d.Reset();
  }
  return hr;
}

}  // namespace draw
