#include "Tooltip.h"

#include <dwmapi.h>

#include <algorithm>
#include <cmath>

#include "Theme.h"

using namespace gfx;

namespace {
constexpr wchar_t kClass[] = L"FeatherweightTooltip";
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
}  // namespace

bool Tooltip::Create(HWND owner) {
  owner_ = owner;
  WNDCLASSEXW wc{sizeof(wc)};
  wc.lpfnWndProc = WndProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = kClass;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.style = CS_DROPSHADOW;
  RegisterClassExW(&wc);
  hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT, kClass, L"", WS_POPUP, 0, 0, 10, 10,
                          owner, nullptr, wc.hInstance, this);
  if (!hwnd_) return false;
  int corner = 3;  // DWMWCP_ROUNDSMALL
  DwmSetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
  return true;
}

void Tooltip::Destroy() {
  target_.Reset();
  if (hwnd_) DestroyWindow(hwnd_);
  hwnd_ = nullptr;
}

void Tooltip::Show(const std::wstring& text, const RECT& anchor, float ui) {
  if (!hwnd_ || text.empty()) {
    Hide();
    return;
  }
  ui_ = ui;
  text_ = text;
  IDWriteTextFormat* fmt = Canvas::Format(12 * ui, DWRITE_FONT_WEIGHT_REGULAR, false);
  ComPtr<IDWriteTextLayout> layout;
  DWrite()->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), fmt, 640 * ui, 100, &layout);
  DWRITE_TEXT_METRICS m{};
  if (layout) layout->GetMetrics(&m);
  int w = static_cast<int>(std::ceil(std::min(m.widthIncludingTrailingWhitespace, 640 * ui) + 18 * ui));
  int h = static_cast<int>(std::ceil(28 * ui));

  HMONITOR mon = MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi{sizeof(mi)};
  GetMonitorInfoW(mon, &mi);
  int x = (anchor.left + anchor.right) / 2 - w / 2;
  int y = anchor.bottom + static_cast<int>(6 * ui);
  if (y + h > mi.rcWork.bottom) y = anchor.top - h - static_cast<int>(6 * ui);
  x = std::clamp(x, static_cast<int>(mi.rcWork.left + 4), static_cast<int>(mi.rcWork.right - w - 4));

  BOOL dark = theme::IsDark();
  DwmSetWindowAttribute(hwnd_, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
  SetWindowPos(hwnd_, HWND_TOP, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
  visible_ = true;
  InvalidateRect(hwnd_, nullptr, FALSE);
  UpdateWindow(hwnd_);
}

void Tooltip::Hide() {
  if (!hwnd_ || !visible_) return;
  ShowWindow(hwnd_, SW_HIDE);
  visible_ = false;
  text_.clear();
}

void Tooltip::Paint() {
  RECT rc;
  GetClientRect(hwnd_, &rc);
  if (!target_) {
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties();
    props.dpiX = props.dpiY = 96;
    D2D()->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(hwnd_, D2D1::SizeU(rc.right, rc.bottom)),
                                  &target_);
  } else {
    target_->Resize(D2D1::SizeU(rc.right, rc.bottom));
  }
  if (!target_) return;
  ComPtr<ID2D1DeviceContext> dc;
  target_.As(&dc);
  if (!dc) return;
  const Palette& p = theme::P();
  Canvas c;
  c.Begin(dc.Get());
  target_->BeginDraw();
  target_->Clear(p.popupBg);
  c.Stroke(RectF(rc), p.dark ? Rgb(0x484848) : Rgb(0xE0E0E0), 1);
  c.Text(text_, RectF(9 * ui_, 0, rc.right - 6 * ui_, static_cast<float>(rc.bottom)), p.text, 12 * ui_);
  if (target_->EndDraw() == D2DERR_RECREATE_TARGET) target_.Reset();
}

LRESULT CALLBACK Tooltip::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  auto* self = reinterpret_cast<Tooltip*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  switch (msg) {
    case WM_NCCREATE: {
      auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
      break;
    }
    case WM_PAINT: {
      PAINTSTRUCT ps;
      BeginPaint(hwnd, &ps);
      if (self) self->Paint();
      EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_MOUSEACTIVATE:
      return MA_NOACTIVATE;
    case WM_NCHITTEST:
      return HTTRANSPARENT;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}
