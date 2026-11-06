#pragma once

#include <d2d1_1.h>
#include <d3d11.h>
#include <wrl/client.h>

namespace draw {

// A Direct3D 11 device with Direct2D support, on the hardware adapter or on WARP (the software
// rasterizer), and the Direct2D device on top of it.
HRESULT CreateD2DDevice(ID2D1Factory1* factory, bool warp, Microsoft::WRL::ComPtr<ID3D11Device>& d3d,
                        Microsoft::WRL::ComPtr<ID2D1Device>& d2d);

}  // namespace draw
