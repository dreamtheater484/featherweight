// The presenter view (P3, p3-present): see PresenterView.h. Drawn with Direct2D on the shared device into
// its own flip-model swap chain, only when something changes (and once a second for the clock).
#include "slide/show/PresenterView.h"

#include <dwrite.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "slide/Presentation.h"
#include "slide/show/SlideRenderer.h"
#include "ui/Gfx.h"
#include "ui/SwapChain.h"

namespace fw::slide::show {

namespace {

constexpr wchar_t kClass[] = L"FeatherweightPresenterView";
constexpr UINT_PTR kTimerClock = 1;

D2D1_COLOR_F Rgb(uint32_t c) {
  return D2D1::ColorF(((c >> 16) & 255) / 255.f, ((c >> 8) & 255) / 255.f, (c & 255) / 255.f, 1.f);
}

bool Inside(const D2D1_RECT_F& r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

// `inner` (w x h) fitted into `box`, centered.
D2D1_RECT_F Fit(const D2D1_RECT_F& box, float w, float h) {
  if (w <= 0 || h <= 0) return box;
  float s = std::min((box.right - box.left) / w, (box.bottom - box.top) / h);
  float x = box.left + ((box.right - box.left) - w * s) / 2, y = box.top + ((box.bottom - box.top) - h * s) / 2;
  return {std::floor(x), std::floor(y), std::floor(x + w * s), std::floor(y + h * s)};
}

}  // namespace

struct PresenterView::Impl {
  Impl(PresenterHost& h, Presentation& d, SlideRenderer& r) : host(h), deck(d), renderer(r) {}

  PresenterHost& host;
  Presentation& deck;
  SlideRenderer& renderer;
  PresenterView* owner = nullptr;
  HWND hwnd = nullptr;
  SwapChain chain;
  ComPtr<ID2D1DeviceContext> offscreen;  // thumbnails
  PresenterState state;
  // The timer: elapsed time since the show started, less the time it was paused.
  ULONGLONG started = GetTickCount64(), pausedAt = 0, pausedTotal = 0;
  bool paused = false;
  float notesPt = 20, notesScroll = 0, notesMax = 0;
  float stripScroll = 0;
  std::vector<ComPtr<ID2D1Bitmap1>> thumbs;
  unsigned thumbGeneration = 0;
  float thumbW = 0, thumbH = 0;
  // Where things were drawn last, for the mouse.
  D2D1_RECT_F current{}, next{}, notes{}, strip{}, pause{}, reset{}, smaller{}, larger{};

  void Paint();
  void RenderThumbs(float w, float h);
  LRESULT Proc(UINT m, WPARAM w, LPARAM l);
  static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l);
  double Scale(float h) const { return std::clamp(h / 1080.0, 0.6, 2.0); }
  ULONGLONG Elapsed() const { return (paused ? pausedAt : GetTickCount64()) - started - pausedTotal; }
};

PresenterView::PresenterView(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) { impl_->owner = this; }

PresenterView::~PresenterView() {
  if (impl_->hwnd) {
    HWND h = impl_->hwnd;
    impl_->hwnd = nullptr;
    SetWindowLongPtrW(h, GWLP_USERDATA, 0);
    DestroyWindow(h);
  }
}

HWND PresenterView::Window() const { return impl_->hwnd; }

std::unique_ptr<PresenterView> PresenterView::Create(PresenterHost& host, Presentation& deck, SlideRenderer& renderer,
                                                     HMONITOR monitor, bool windowed, HWND owner) {
  static bool registered = [] {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = &Impl::WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClass;
    wc.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
    return RegisterClassExW(&wc) != 0;
  }();
  auto impl = std::make_unique<Impl>(host, deck, renderer);
  if (!monitor) return std::unique_ptr<PresenterView>(new PresenterView(std::move(impl)));  // off screen
  if (!registered) return nullptr;
  MONITORINFO mi{sizeof(mi)};
  GetMonitorInfoW(monitor, &mi);
  RECT r = mi.rcMonitor;
  DWORD style = WS_POPUP;
  if (windowed) {
    // The right half of the work area, beside the windowed show.
    const RECT& work = mi.rcWork;
    style = WS_OVERLAPPEDWINDOW;
    r = {work.left + (work.right - work.left) / 2 + 8, work.top + 8, work.right - 8,
         work.top + 8 + static_cast<long>((work.bottom - work.top) * 0.8)};
  }
  impl->hwnd = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, kClass, L"Presenter View", style, r.left, r.top,
                               r.right - r.left, r.bottom - r.top, owner, nullptr, GetModuleHandleW(nullptr), impl.get());
  if (!impl->hwnd) return nullptr;
  ShowWindow(impl->hwnd, SW_SHOWNOACTIVATE);
  SetTimer(impl->hwnd, kTimerClock, 1000, nullptr);  // the clock and the timer, while the show runs
  return std::unique_ptr<PresenterView>(new PresenterView(std::move(impl)));
}

void PresenterView::Update(const PresenterState& state) {
  if (state.slide != impl_->state.slide) impl_->notesScroll = 0;
  impl_->state = state;
  if (impl_->hwnd) InvalidateRect(impl_->hwnd, nullptr, FALSE);
}

// ---- Drawing ------------------------------------------------------------------------------------

void PresenterView::Impl::RenderThumbs(float w, float h) {
  // Thumbnails of the slides the strip shows, made once (the device's generation kept).
  if (thumbGeneration != gpu::Generation() || std::abs(thumbW - w) > 0.5f || std::abs(thumbH - h) > 0.5f) {
    thumbs.assign(renderer.SlideCount(), nullptr);
    thumbGeneration = gpu::Generation();
    thumbW = w;
    thumbH = h;
    offscreen.Reset();
  }
  if (!offscreen) offscreen = gpu::CreateContext();
  if (!offscreen || w < 1 || h < 1) return;
  const float gap = std::floor(h * 0.12f);
  draw::Size size = renderer.SlideSize();
  for (size_t i = 0; i < thumbs.size(); ++i) {
    float x = strip.left + i * (w + gap) - stripScroll;
    if (x + w < strip.left || x > strip.right || thumbs[i]) continue;
    ComPtr<ID2D1Bitmap1> b;
    D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(offscreen->CreateBitmap(D2D1::SizeU(static_cast<UINT>(w), static_cast<UINT>(h)), nullptr, 0, props, &b))) continue;
    offscreen->SetTarget(b.Get());
    offscreen->BeginDraw();
    offscreen->Clear(D2D1::ColorF(D2D1::ColorF::White));
    renderer.Draw(offscreen.Get(), i, D2D1::Matrix3x2F::Scale(w / size.width, h / size.height), nullptr);
    if (SUCCEEDED(offscreen->EndDraw())) thumbs[i] = b;
    offscreen->SetTarget(nullptr);
  }
}

void PresenterView::Prepare(float w, float h) {
  Impl& v = *impl_;
  const float s = static_cast<float>(v.Scale(h)), pad = std::floor(16 * s), stripH = std::floor(110 * s);
  v.strip = {pad, h - stripH, w - pad, h - pad / 2};
  draw::Size size = v.renderer.SlideSize();
  const float th = std::floor((v.strip.bottom - v.strip.top) * 0.86f);
  v.RenderThumbs(std::floor(th * size.width / size.height), th);
}

void PresenterView::Draw(ID2D1DeviceContext* dc, float w, float h) {
  Impl& v = *impl_;
  const float s = static_cast<float>(v.Scale(h));
  const float pad = std::floor(16 * s), bar = std::floor(56 * s), stripH = std::floor(110 * s);
  ComPtr<ID2D1SolidColorBrush> text, dim, box, accent;
  dc->CreateSolidColorBrush(Rgb(0xE8E8E8), &text);
  dc->CreateSolidColorBrush(Rgb(0x9A9A9A), &dim);
  dc->CreateSolidColorBrush(Rgb(0x3A3A3A), &box);
  dc->CreateSolidColorBrush(Rgb(0xE07A2E), &accent);
  dc->SetTransform(D2D1::Matrix3x2F::Identity());
  dc->Clear(Rgb(0x1E1E1E));
  if (!text || !dim || !box || !accent) return;
  IDWriteFactory* dw = gfx::DWrite();
  ComPtr<IDWriteTextFormat> big, label, notesFmt;
  dw->CreateTextFormat(gfx::UiFont().c_str(), nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                       DWRITE_FONT_STRETCH_NORMAL, 26 * s, L"", &big);
  dw->CreateTextFormat(gfx::UiFont().c_str(), nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                       DWRITE_FONT_STRETCH_NORMAL, 15 * s, L"", &label);
  dw->CreateTextFormat(gfx::UiFont().c_str(), nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                       DWRITE_FONT_STRETCH_NORMAL, v.notesPt * s, L"", &notesFmt);
  if (!big || !label || !notesFmt) return;
  for (IDWriteTextFormat* f : {big.Get(), label.Get()}) f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
  auto say = [&](const std::wstring& t, IDWriteTextFormat* f, D2D1_RECT_F r, ID2D1Brush* b,
                 DWRITE_TEXT_ALIGNMENT a = DWRITE_TEXT_ALIGNMENT_LEADING) {
    f->SetTextAlignment(a);
    dc->DrawText(t.c_str(), static_cast<UINT32>(t.size()), f, r, b, D2D1_DRAW_TEXT_OPTIONS_CLIP);
  };
  auto button = [&](D2D1_RECT_F r, const wchar_t* t) {
    dc->FillRoundedRectangle(D2D1::RoundedRect(r, 4 * s, 4 * s), box.Get());
    say(t, label.Get(), r, text.Get(), DWRITE_TEXT_ALIGNMENT_CENTER);
  };

  // The bar: the timer with pause and reset, the slide number, the clock.
  ULONGLONG e = v.Elapsed() / 1000;
  wchar_t buf[64];
  swprintf(buf, 64, L"%llu:%02llu:%02llu", e / 3600, e / 60 % 60, e % 60);
  say(buf, big.Get(), {pad, 0, pad + 150 * s, bar}, v.paused ? dim.Get() : text.Get());
  v.pause = {pad + 160 * s, bar * 0.2f, pad + 250 * s, bar * 0.8f};
  v.reset = {pad + 260 * s, bar * 0.2f, pad + 350 * s, bar * 0.8f};
  button(v.pause, v.paused ? L"Resume" : L"Pause");
  button(v.reset, L"Reset");
  const size_t count = v.renderer.SlideCount();
  if (v.state.slide != PresenterState::kEnd)
    swprintf(buf, 64, L"Slide %zu of %zu", v.state.slide + 1, count);
  else
    swprintf(buf, 64, L"End of slide show");
  say(buf, label.Get(), {pad + 370 * s, 0, w / 2 + 200 * s, bar}, dim.Get());
  wchar_t clock[32] = L"";
  GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, nullptr, nullptr, clock, 32);
  say(clock, big.Get(), {w / 2, 0, w - pad, bar}, text.Get(), DWRITE_TEXT_ALIGNMENT_TRAILING);

  // The current slide on the left, the next one and the notes on the right.
  draw::Size size = v.renderer.SlideSize();
  const float top = bar, bottom = h - stripH - pad;
  const float split = std::floor(w * 0.6f);
  v.current = Fit({pad, top, split - pad / 2, bottom}, size.width, size.height);
  auto slide = [&](ID2D1Bitmap1* b, D2D1_RECT_F r, int blank, const wchar_t* none) {
    if (blank == 2) dc->FillRectangle(r, text.Get());
    if (blank == 1 || (!b && blank == 0)) {
      ComPtr<ID2D1SolidColorBrush> black;
      dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &black);
      if (black) dc->FillRectangle(r, black.Get());
    }
    if (b && blank == 0) dc->DrawBitmap(b, r, 1.f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
    if (!b && none) say(none, label.Get(), r, dim.Get(), DWRITE_TEXT_ALIGNMENT_CENTER);
    dc->DrawRectangle(r, box.Get(), 1);
  };
  const bool atEnd = v.state.slide == PresenterState::kEnd;
  slide(v.state.current, v.current, v.state.blank, atEnd ? L"End of slide show, click to exit." : nullptr);
  const float right = split + pad / 2, nextH = std::floor((bottom - top) * 0.42f);
  say(v.state.stepsLeft > 0 ? L"Next animation" : L"Next slide", label.Get(), {right, top, w - pad, top + 28 * s},
      dim.Get());
  v.next = Fit({right, top + 28 * s, w - pad, top + 28 * s + nextH}, size.width, size.height);
  slide(v.state.next, v.next, 0, L"End of slide show");

  // The notes, scrolled and zoomed; A- and A+ beside their heading.
  const float notesTop = v.next.bottom + pad;
  say(L"Notes", label.Get(), {right, notesTop, w - pad, notesTop + 28 * s}, dim.Get());
  v.smaller = {w - pad - 96 * s, notesTop, w - pad - 52 * s, notesTop + 28 * s};
  v.larger = {w - pad - 44 * s, notesTop, w - pad, notesTop + 28 * s};
  button(v.smaller, L"A−");
  button(v.larger, L"A+");
  v.notes = {right, notesTop + 36 * s, w - pad, bottom};
  std::wstring notes;
  if (v.state.slide != PresenterState::kEnd && v.state.slide < count)
    if (const Slide* sl = v.deck.GetSlide(v.state.slide)) notes = sl->NotesText();
  if (notes.empty() && !atEnd) notes = L"No notes on this slide.";
  ComPtr<IDWriteTextLayout> layout;
  const float nw = std::max(1.f, v.notes.right - v.notes.left), nh = std::max(1.f, v.notes.bottom - v.notes.top);
  if (SUCCEEDED(dw->CreateTextLayout(notes.c_str(), static_cast<UINT32>(notes.size()), notesFmt.Get(), nw, 1e6f, &layout))) {
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    v.notesMax = std::max(0.f, m.height - nh);
    v.notesScroll = std::clamp(v.notesScroll, 0.f, v.notesMax);
    dc->PushAxisAlignedClip(v.notes, D2D1_ANTIALIAS_MODE_ALIASED);
    dc->DrawTextLayout({v.notes.left, v.notes.top - v.notesScroll}, layout.Get(), text.Get());
    dc->PopAxisAlignedClip();
  }

  // The strip of slides; the current one marked.
  v.strip = {pad, h - stripH, w - pad, h - pad / 2};
  const float th = std::floor((v.strip.bottom - v.strip.top) * 0.86f), tw = std::floor(th * size.width / size.height);
  const float gap = std::floor(th * 0.12f);
  const float total = count * (tw + gap);
  v.stripScroll = std::clamp(v.stripScroll, 0.f, std::max(0.f, total - (v.strip.right - v.strip.left)));
  if (v.state.slide < count && v.thumbW == tw) {
    // Keep the current slide in view.
    float x = v.state.slide * (tw + gap);
    if (x < v.stripScroll) v.stripScroll = x;
    if (x + tw > v.stripScroll + (v.strip.right - v.strip.left)) v.stripScroll = x + tw - (v.strip.right - v.strip.left);
  }
  dc->PushAxisAlignedClip(v.strip, D2D1_ANTIALIAS_MODE_ALIASED);
  for (size_t i = 0; i < count; ++i) {
    float x = v.strip.left + i * (tw + gap) - v.stripScroll;
    if (x + tw < v.strip.left || x > v.strip.right) continue;
    D2D1_RECT_F r{x, v.strip.top, x + tw, v.strip.top + th};
    if (i < v.thumbs.size() && v.thumbs[i]) dc->DrawBitmap(v.thumbs[i].Get(), r);
    else dc->FillRectangle(r, box.Get());
    dc->DrawRectangle(r, i == v.state.slide ? accent.Get() : box.Get(), i == v.state.slide ? 3 * s : 1);
  }
  dc->PopAxisAlignedClip();
  (void)gap;
}

void PresenterView::Impl::Paint() {
  RECT rc{};
  GetClientRect(hwnd, &rc);
  const UINT w = static_cast<UINT>(std::max(1L, rc.right)), h = static_cast<UINT>(std::max(1L, rc.bottom));
  if ((!chain.DC() || chain.Generation() != gpu::Generation()) && !chain.Create(hwnd)) return;
  chain.Resize(w, h);
  owner->Prepare(static_cast<float>(w), static_cast<float>(h));  // thumbnails first (off screen), then the view
  ID2D1DeviceContext* dc = chain.DC();
  dc->BeginDraw();
  owner->Draw(dc, static_cast<float>(w), static_cast<float>(h));
  HRESULT hr = dc->EndDraw();
  if (SUCCEEDED(hr)) hr = chain.Present();
  if (gpu::IsDeviceLoss(hr)) {
    gpu::DeviceLost();
    chain.Reset();
  }
}

// ---- Input ----------------------------------------------------------------------------------------

LRESULT CALLBACK PresenterView::Impl::WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_NCCREATE) {
    auto* self = static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
    SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    self->hwnd = h;
  }
  auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(h, GWLP_USERDATA));
  if (!self) return DefWindowProcW(h, m, w, l);
  return self->Proc(m, w, l);
}

LRESULT PresenterView::Impl::Proc(UINT m, WPARAM w, LPARAM l) {
  switch (m) {
    case WM_PAINT: {
      PAINTSTRUCT ps;
      BeginPaint(hwnd, &ps);
      EndPaint(hwnd, &ps);
      Paint();
      return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_SIZE:
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    case WM_TIMER:
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
      if (w == VK_F4 && GetKeyState(VK_MENU) < 0) break;  // Alt+F4: closes it, and with it the show
      host.PresenterKey(static_cast<UINT>(w));
      return 0;
    case WM_LBUTTONDOWN: {
      const float x = static_cast<short>(LOWORD(l)), y = static_cast<short>(HIWORD(l));
      if (Inside(pause, x, y)) {
        if (paused) pausedTotal += GetTickCount64() - pausedAt;
        else pausedAt = GetTickCount64();
        paused = !paused;
      } else if (Inside(reset, x, y)) {
        started = GetTickCount64();
        pausedTotal = 0;
        pausedAt = started;
      } else if (Inside(smaller, x, y) || Inside(larger, x, y)) {
        notesPt = std::clamp(notesPt + (Inside(larger, x, y) ? 2.f : -2.f), 10.f, 48.f);
      } else if (Inside(strip, x, y) && thumbW > 0) {
        const float gap = std::floor(thumbH * 0.12f);
        const float at = x - strip.left + stripScroll;
        size_t i = static_cast<size_t>(at / (thumbW + gap));
        if (at - i * (thumbW + gap) <= thumbW && i < renderer.SlideCount()) host.PresenterGoTo(i);
      } else if (Inside(current, x, y) || Inside(next, x, y)) {
        host.PresenterKey(VK_RIGHT);  // a click on the slides: on, as in the show
      }
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    }
    case WM_MOUSEWHEEL: {
      POINT pt{static_cast<short>(LOWORD(l)), static_cast<short>(HIWORD(l))};
      ScreenToClient(hwnd, &pt);
      const float d = GET_WHEEL_DELTA_WPARAM(w) / 120.f;
      if (Inside(notes, static_cast<float>(pt.x), static_cast<float>(pt.y))) {
        notesScroll = std::clamp(notesScroll - d * notesPt * 3, 0.f, notesMax);
      } else if (Inside(strip, static_cast<float>(pt.x), static_cast<float>(pt.y))) {
        stripScroll = std::max(0.f, stripScroll - d * (thumbW + thumbH * 0.12f));
      } else {
        host.PresenterKey(d < 0 ? VK_RIGHT : VK_LEFT);
      }
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    }
    case WM_CLOSE: host.PresenterClosed(); return 0;
    case WM_NCDESTROY: {
      HWND h = hwnd;
      hwnd = nullptr;
      chain.Reset();
      return DefWindowProcW(h, m, w, l);
    }
    default: break;
  }
  return DefWindowProcW(hwnd, m, w, l);
}

}  // namespace fw::slide::show
