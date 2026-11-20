// The slide show's window (SlideShow.h): its own flip-model swap chain on the shared device, paced
// by the display (a frame-latency waitable object and Present(1, 0)), so transitions and animations
// run at the refresh rate and the bench can count dropped frames. Between clicks nothing runs.
//
// A slide on screen is a Scene: its timing, its animator, and its pictures. Every animation target
// (a shape, its background, a range of its paragraphs) is a sprite drawn once; while a step runs,
// what doesn't move is drawn into layers between the moving sprites, in z-order, so a frame is a
// few bitmap draws. When nothing moves, the slide is one picture (Scene::frame), which is also what
// a transition starts from.
#include "slide/show/SlideShow.h"

#include <commctrl.h>
#include <d2d1_1helper.h>
#include <dwmapi.h>
#include <dxgi1_3.h>
#include <dwrite.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <memory>

#include "Util.h"
#include "core/FileKinds.h"
#include "core/Strings.h"
#include "pkg/Package.h"
#include "pkg/Snapshot.h"
#include "slide/Presentation.h"
#include "slide/show/SlideRenderer.h"
#include "slide/show/Timing.h"
#include "ui/Anim.h"
#include "ui/Gfx.h"
#include "ui/SwapChain.h"

namespace fw::slide::show {
namespace {

using Microsoft::WRL::ComPtr;
using M = D2D1::Matrix3x2F;

constexpr wchar_t kClass[] = L"FeatherweightSlideShow";
constexpr UINT_PTR kTimerCursor = 1, kTimerAdvance = 2;
constexpr UINT kMsgStop = WM_APP + 1, kMsgPrerender = WM_APP + 2;
constexpr size_t kEnd = static_cast<size_t>(-1);
constexpr double kCursorHideMs = 2000;  // as the PDF reader's presentation mode
constexpr double kBenchHoldMs = 250;    // the bench: how long each slide and step stays

double Smooth(double p) { return p * p * (3 - 2 * p); }

// What a target of the timing is drawn from.
struct Sprite {
  TargetKey key;
  size_t item = 0;
  ComPtr<ID2D1Bitmap1> bitmap;  // drawn on first use
  D2D1_RECT_F rect{};           // where the bitmap sits on the frame, unanimated (pixels)
  D2D1_RECT_F box{};            // the target's own box (pixels): its center, and what wipes show
};

// The sprites of one item of the draw list (-1: none).
struct ItemTargets {
  int whole = -1, background = -1;
  std::vector<int> paragraphs;  // by paragraph
  bool Animated() const {
    return whole >= 0 || background >= 0 || std::any_of(paragraphs.begin(), paragraphs.end(), [](int s) { return s >= 0; });
  }
  bool ParagraphTargeted(int p) const { return p >= 0 && p < static_cast<int>(paragraphs.size()) && paragraphs[p] >= 0; }
};

// The slide in drawing order: an item's parts that don't animate, or a sprite.
struct Piece {
  size_t item = 0;
  int sprite = -1;
};

struct Scene {
  size_t index = kEnd;  // kEnd: the black slide after the last
  Timing timing;
  Transition transition;
  Animator animator;
  std::vector<ItemTargets> targets;
  std::vector<Sprite> sprites;
  std::vector<Piece> pieces;
  std::vector<TargetBox> boxes;  // by sprite: the formulas' ppt_x, ppt_y, ppt_w, ppt_h
  ComPtr<ID2D1Bitmap1> frame;    // the slide as it is while nothing moves
  // While a step runs: the layers between the moving sprites, and those sprites.
  std::vector<ComPtr<ID2D1Bitmap1>> layers;
  std::vector<int> movers;
  std::vector<TargetKey> builtFor;
  bool layered = false;
};

// Static parts of a run of pieces (with the background, for the first layer).
class StaticFilter : public DrawFilter {
 public:
  StaticFilter(const std::vector<ItemTargets>& t, bool bg) : t_(t), bg_(bg), items_(t.size(), 0) {}
  void Add(size_t item) { items_[item] = 1; }
  void Reset() {
    bg_ = false;
    std::fill(items_.begin(), items_.end(), static_cast<char>(0));
  }
  bool Empty() const { return !bg_ && std::none_of(items_.begin(), items_.end(), [](char c) { return c != 0; }); }
  bool Background() const override { return bg_; }
  bool Item(size_t i) const override { return i < items_.size() && items_[i]; }
  bool Body(size_t i) const override { return t_[i].whole < 0 && t_[i].background < 0; }
  bool Paragraph(size_t i, int p) const override { return t_[i].whole < 0 && !t_[i].ParagraphTargeted(p); }

 private:
  const std::vector<ItemTargets>& t_;
  bool bg_;
  std::vector<char> items_;
};

// One sprite alone.
class SpriteFilter : public DrawFilter {
 public:
  SpriteFilter(const Sprite& s, const ItemTargets& t) : s_(s), t_(t) {}
  bool Background() const override { return false; }
  bool Item(size_t i) const override { return i == s_.item; }
  bool Body(size_t) const override { return s_.key.part != TargetKey::Paragraphs; }
  bool Paragraph(size_t, int p) const override {
    if (s_.key.part == TargetKey::Paragraphs) return s_.key.Contains(p);
    return s_.key.part == TargetKey::Whole && !t_.ParagraphTargeted(p);
  }

 private:
  const Sprite& s_;
  const ItemTargets& t_;
};

Pose Combine(const Pose& outer, const Pose& inner) {
  Pose p = inner;
  p.visible = outer.visible && inner.visible;
  p.opacity = outer.opacity * inner.opacity;
  p.dx += outer.dx;
  p.dy += outer.dy;
  p.sx *= outer.sx;
  p.sy *= outer.sy;
  p.rotation += outer.rotation;
  if (p.reveal.kind == Reveal::None) p.reveal = outer.reveal;
  return p;
}

struct MonitorList {
  std::vector<HMONITOR> handles;
  static BOOL CALLBACK Add(HMONITOR m, HDC, LPRECT, LPARAM self) {
    reinterpret_cast<MonitorList*>(self)->handles.push_back(m);
    return TRUE;
  }
  MonitorList() { EnumDisplayMonitors(nullptr, nullptr, &MonitorList::Add, reinterpret_cast<LPARAM>(this)); }
};

// ---------------------------------------------------------------------------------------------

class Show final : public anim::FrameSource {
 public:
  Show(Presentation& deck, SlideRenderer& renderer, ShowOptions options)
      : deck_(deck), renderer_(renderer), opts_(std::move(options)) {}

  bool Create();
  void Stop() {
    if (hwnd_) DestroyWindow(hwnd_);
  }
  bool Animating() const override { return transition_ || (cur_ && cur_->animator.Running(0)); }
  void Frame() override;
  // --show-capture: every transition and step at its middle and end, as PNG files.
  int CaptureAll();

 private:
  double Now() const { return capture_ ? clock_ : anim::Now(); }
  void Step(double t);
  bool Save(const std::wstring& name);
  static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l);
  LRESULT Proc(UINT m, WPARAM w, LPARAM l);
  void PostStop() { PostMessageW(hwnd_, kMsgStop, 0, 0); }

  // Device and swap chain.
  bool EnsureDevice();
  void DropDevice();
  ComPtr<ID2D1Bitmap1> NewBitmap();
  ComPtr<ID2D1Bitmap1> NewBitmap(UINT w, UINT h);
  bool BeginOffscreen(ID2D1Bitmap1* target, bool clear, D2D1_COLOR_F color = {0, 0, 0, 0});
  void EndOffscreen();
  // Sends what was drawn off screen to the GPU and waits (up to `maxMs`) until it's done, so the
  // first frame that shows it doesn't wait for it.
  void FinishGpu(double maxMs);
  // Slide points to the pixels of the show's pictures (frames, layers, sprites), whose (0, 0) is the
  // slide's top left; on screen they sit at (ox_, oy_).
  M SlideToPixels() const { return M::Scale(scale_, scale_); }

  // Scenes.
  std::unique_ptr<Scene> MakeScene(size_t index, bool built, double now);
  static TargetBox BoxOf(void* ctx, const TargetKey& key);
  bool RenderSprite(Scene& s, int i);
  void DrawSprite(Scene& s, int i, const Pose& pose);
  Pose PoseOfSprite(const Scene& s, int i) const;
  void RenderPieces(Scene& s, const std::vector<size_t>& pieces, bool background, ID2D1Bitmap1* target);
  void RenderFrame(Scene& s);
  void EnsureLayers(Scene& s);
  void Settle(Scene& s);
  void Invalidate(Scene& s);

  // Frames.
  void Redraw();
  void Draw(double now);
  void DrawTransition(double p);
  void DrawEnd();
  void AfterSettle(double now);
  void EndTransition(double now);

  // Navigation.
  size_t FirstIndex() const;
  size_t NextIndex(size_t from) const;
  size_t PrevIndex(size_t from) const;
  void GoTo(size_t index, bool transition, bool built);
  void Next(bool fromClick);
  void Previous();
  void Click(int x, int y);
  void Follow(const Hyperlink& link);
  const Hyperlink* LinkAt(int x, int y, uint32_t* trigger);
  // The mouse moved: a shape's mouse-over action runs when the mouse comes onto it.
  void Hover(int x, int y);
  // The item under a point of the window (topmost first, from `below`), with the point in slide
  // points; -1 for none.
  int ItemAt(int x, int y, int below, draw::Point* pt) const;
  // The deck's custom show `id` starts; false when there's no such show.
  bool StartCustom(uint32_t id, size_t returnTo, bool loop);
  void Blank(int kind);
  void Key(UINT vk);
  void ContextMenu(int x, int y);
  void ShowCursorNow();
  // The next slide, with its transition (a custom show past its last slide ends there).
  void Advance();

  // The bench.
  void BenchFrame(double start, double cpuMs, bool inTransition, double waitMs);
  void WriteBench();
  // The bench takes the screen of someone who may come back: any input since it started (anywhere,
  // not only in the show) or another program taking the focus stops it at once.
  bool BenchInterrupted() const;
  // Ends the show and deletes it: the caller returns at once, touching nothing of it.
  void AbortBench(const char* why);

  Presentation& deck_;
  SlideRenderer& renderer_;
  ShowOptions opts_;
  HWND hwnd_ = nullptr;
  UINT w_ = 1, h_ = 1;
  float scale_ = 1, ox_ = 0, oy_ = 0, slideW_ = 1, slideH_ = 1;  // slide placement, pixels
  UINT fw_ = 1, fh_ = 1;  // the slide's pictures: the slide's size in pixels

  ComPtr<IDXGISwapChain2> swap_;
  HANDLE waitable_ = nullptr;
  ComPtr<ID2D1DeviceContext> dc_;
  ComPtr<ID2D1Bitmap1> target_;
  unsigned generation_ = 0;
  ComPtr<IDWriteTextFormat> endFormat_;

  std::unique_ptr<Scene> cur_, next_;
  std::vector<size_t> history_;  // slides shown, for "last viewed"
  SlideOrder order_;  // hidden slides, the loop, custom shows
  int hoverItem_ = -1;  // the item the mouse is over whose mouse-over action has run
  size_t lastShown_ = 0;

  bool transition_ = false;
  double transitionStart_ = 0;
  ComPtr<ID2D1Bitmap1> from_;  // what the transition starts from (null: black)
  double visibleAt_ = 0;       // when the slide's transition ended: its advance time counts from here
  bool boosted_ = false;  // the thread runs above normal while something moves

  int blank_ = 0;  // 1: black, 2: white
  int digits_ = -1;
  bool cursorHidden_ = false;
  std::wstring fileToOpen_;

  // The bench's numbers.
  struct BenchTransition {
    size_t slide;
    TransitionKind kind;
    int frames = 0, dropped = 0, vblankMissed = 0;
    double worstMs = 0;
  };
  bool bench_ = false;
  const char* benchAborted_ = nullptr;  // why the bench stopped early, or null
  DWORD benchInput_ = 0;                // GetLastInputInfo's time when the bench started
  POINT benchCursor_{};
  bool awake_ = false;  // the display and the PC kept on while the show runs
  bool capture_ = false;
  double clock_ = 1000;  // capture_: the show's own time
  double periodMs_ = 1000.0 / 60;
  double refreshHz_ = 60;
  double lastFrame_ = -1;
  std::vector<double> cpuMs_;
  std::vector<BenchTransition> benchTransitions_;
  int animationFrames_ = 0, animationDropped_ = 0;
  double drawMs_ = 0, presentMs_ = 0;  // the last frame's
  struct SlowFrame {
    size_t slide;
    double interval, wait, cpu, draw, present;
  };
  std::vector<SlowFrame> slow_;  // late frames in transitions, the first 40
  double waitMax_ = 0, drawMax_ = 0, presentMax_ = 0;
  // DXGI's frame statistics, sampled after every present during transitions: refreshes against
  // presents that reached the screen.
  DXGI_FRAME_STATISTICS dxgiLast_{};
  bool dxgiHave_ = false;
  int dxgiSamples_ = 0, dxgiMissed_ = 0, dxgiFailures_ = 0;
  // Refreshes with no new frame: each present's vblank interval (on DWM's clock) against the last.
  UINT64 vblank0_ = 0, vblankPeriod_ = 0;
  long long lastVblank_ = -1;
  int vblankMissed_ = 0;
};

Show* g_show = nullptr;
bool g_benchPassed = false;
bool g_benchAborted = false;

bool Show::Create() {
  static bool registered = [] {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = &Show::WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClass;
    wc.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
    return RegisterClassExW(&wc) != 0;
  }();
  if (!registered) return false;
  MonitorList monitors;
  HMONITOR mon = nullptr;
  if (opts_.monitor >= 0 && opts_.monitor < static_cast<int>(monitors.handles.size()))
    mon = monitors.handles[opts_.monitor];
  if (!mon) mon = MonitorFromWindow(opts_.owner, MONITOR_DEFAULTTOPRIMARY);
  MONITORINFOEXW mi{};
  mi.cbSize = sizeof(mi);
  GetMonitorInfoW(mon, &mi);
  DEVMODEW dm{};
  dm.dmSize = sizeof(dm);
  if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1) {
    refreshHz_ = dm.dmDisplayFrequency == 59 ? 59.94 : dm.dmDisplayFrequency;
    periodMs_ = 1000.0 / refreshHz_;
  }
  RECT r = mi.rcMonitor;
  capture_ = !opts_.captureDir.empty();
  if (capture_) r = {0, 0, 960, 540};  // off screen: never shown
  hwnd_ = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, kClass, L"Slide show", WS_POPUP, r.left, r.top, r.right - r.left,
                          r.bottom - r.top, opts_.owner, nullptr, GetModuleHandleW(nullptr), this);
  if (!hwnd_) return false;
  w_ = static_cast<UINT>(std::max(1L, r.right - r.left));
  h_ = static_cast<UINT>(std::max(1L, r.bottom - r.top));
  draw::Size size = renderer_.SlideSize();
  if (size.width <= 0 || size.height <= 0) size = {720, 540};
  scale_ = std::min(w_ / size.width, h_ / size.height);
  slideW_ = size.width * scale_;
  slideH_ = size.height * scale_;
  ox_ = std::floor((w_ - slideW_) / 2);
  oy_ = std::floor((h_ - slideH_) / 2);
  fw_ = static_cast<UINT>(std::max(1.f, std::ceil(slideW_)));
  fh_ = static_cast<UINT>(std::max(1.f, std::ceil(slideH_)));
  bench_ = !opts_.benchReport.empty();
  Presentation* deck = &deck_;
  order_ = SlideOrder(
      deck_.SlideCount(),
      [deck](size_t i) {
        const Slide* s = deck->GetSlide(i);
        return s && s->hidden;
      },
      deck_.Show().loop && !bench_);
  gfx::DWrite()->CreateTextFormat(gfx::UiFont().c_str(), nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                  DWRITE_FONT_STRETCH_NORMAL, std::max(12.f, h_ / 54.f), L"", &endFormat_);
  if (endFormat_) endFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
  if (!EnsureDevice()) {
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
    return false;
  }
  if (capture_) return true;  // CaptureAll drives it
  // A show keeps the display and the PC awake while it runs (the power settings stay as they are).
  SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED);
  awake_ = true;
  if (bench_) {
    LASTINPUTINFO li{sizeof(li)};
    GetLastInputInfo(&li);
    benchInput_ = li.dwTime;
    GetCursorPos(&benchCursor_);
  }
  ShowWindow(hwnd_, SW_SHOW);
  SetForegroundWindow(hwnd_);
  SetFocus(hwnd_);
  ShowCursorNow();
  // The first slide's transition plays from black, as in PowerPoint.
  size_t first = std::min(opts_.first, deck_.SlideCount() ? deck_.SlideCount() - 1 : 0);
  if (opts_.first == 0) {
    // From the beginning: the deck's own custom show if it has one (Set Up Slide Show), else the
    // first slide that isn't hidden.
    if (deck_.Show().hasCustomShow) StartCustom(deck_.Show().customShow, kEnd, deck_.Show().loop && !bench_);
    first = FirstIndex();
  }
  if (deck_.SlideCount() == 0) first = kEnd;
  GoTo(first, true, false);
  anim::SetFrameSource(this);
  return true;
}

// ---- Device ---------------------------------------------------------------------------------

bool Show::EnsureDevice() {
  if (dc_ && target_ && generation_ == gpu::Generation()) return true;
  DropDevice();
  ID3D11Device* d3d = gpu::D3DDevice();
  if (!d3d) return false;
  if (capture_) {
    dc_ = gpu::CreateContext();
    target_ = NewBitmap(w_, h_);
    if (!dc_ || !target_) return false;
    dc_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    generation_ = gpu::Generation();
    return true;
  }
  ComPtr<IDXGIDevice1> dxgi;
  ComPtr<IDXGIAdapter> adapter;
  ComPtr<IDXGIFactory2> factory;
  if (FAILED(d3d->QueryInterface(IID_PPV_ARGS(&dxgi))) || FAILED(dxgi->GetAdapter(&adapter)) ||
      FAILED(adapter->GetParent(IID_PPV_ARGS(&factory))))
    return false;
  DXGI_SWAP_CHAIN_DESC1 desc{};
  desc.Width = w_;
  desc.Height = h_;
  desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  desc.BufferCount = 3;  // one frame queued ahead: a late wake-up of the thread or the compositor costs no refresh
  desc.Scaling = DXGI_SCALING_NONE;
  desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
  desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
  ComPtr<IDXGISwapChain1> swap1;
  if (FAILED(factory->CreateSwapChainForHwnd(d3d, hwnd_, &desc, nullptr, nullptr, &swap1)) || FAILED(swap1.As(&swap_)))
    return false;
  factory->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
  swap_->SetMaximumFrameLatency(2);
  waitable_ = swap_->GetFrameLatencyWaitableObject();
  dc_ = gpu::CreateContext();
  ComPtr<IDXGISurface> surface;
  if (!dc_ || FAILED(swap_->GetBuffer(0, IID_PPV_ARGS(&surface)))) {
    DropDevice();
    return false;
  }
  D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
      D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.f, 96.f);
  if (FAILED(dc_->CreateBitmapFromDxgiSurface(surface.Get(), &props, &target_))) {
    DropDevice();
    return false;
  }
  // Projectors and transparent layers: grayscale text everywhere, so a shape looks the same in a
  // sprite as in a layer.
  dc_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
  generation_ = gpu::Generation();
  if (cur_) Invalidate(*cur_);
  next_.reset();
  from_.Reset();
  return true;
}

void Show::DropDevice() {
  if (dc_) dc_->SetTarget(nullptr);
  target_.Reset();
  dc_.Reset();
  if (waitable_) CloseHandle(waitable_);
  waitable_ = nullptr;
  swap_.Reset();
  renderer_.ReleaseDevice();
  if (cur_) Invalidate(*cur_);
  next_.reset();
  from_.Reset();
}

void Show::Invalidate(Scene& s) {
  s.frame.Reset();
  s.layers.clear();
  s.movers.clear();
  s.builtFor.clear();
  s.layered = false;
  for (Sprite& sp : s.sprites) sp.bitmap.Reset();
}

ComPtr<ID2D1Bitmap1> Show::NewBitmap() { return NewBitmap(fw_, fh_); }

ComPtr<ID2D1Bitmap1> Show::NewBitmap(UINT w, UINT h) {
  ComPtr<ID2D1Bitmap1> b;
  if (!dc_) return b;
  D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
      D2D1_BITMAP_OPTIONS_TARGET, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f);
  dc_->CreateBitmap(D2D1::SizeU(std::max(1u, w), std::max(1u, h)), nullptr, 0, &props, &b);
  return b;
}

bool Show::BeginOffscreen(ID2D1Bitmap1* target, bool clear, D2D1_COLOR_F color) {
  if (!dc_ || !target) return false;
  dc_->SetTarget(target);
  dc_->BeginDraw();
  dc_->SetTransform(M::Identity());
  if (clear) dc_->Clear(color);
  return true;
}

void Show::FinishGpu(double maxMs) {
  ID3D11Device* d3d = gpu::D3DDevice();
  if (!d3d || capture_) return;
  ComPtr<ID3D11DeviceContext> ctx;
  d3d->GetImmediateContext(&ctx);
  D3D11_QUERY_DESC desc{D3D11_QUERY_EVENT, 0};
  ComPtr<ID3D11Query> done;
  if (!ctx || FAILED(d3d->CreateQuery(&desc, &done))) return;
  ctx->End(done.Get());
  ctx->Flush();
  const double until = anim::Now() + maxMs;
  BOOL finished = FALSE;
  while (ctx->GetData(done.Get(), &finished, sizeof(finished), 0) == S_FALSE && anim::Now() < until) Sleep(1);
}

void Show::EndOffscreen() {
  HRESULT hr = dc_->EndDraw();
  dc_->SetTarget(nullptr);
  if (gpu::IsDeviceLoss(hr)) gpu::DeviceLost();
}

// ---- Scenes ---------------------------------------------------------------------------------

TargetBox Show::BoxOf(void* ctx, const TargetKey& key) {
  auto* s = static_cast<Scene*>(ctx);
  for (size_t i = 0; i < s->sprites.size(); ++i)
    if (s->sprites[i].key == key) return s->boxes[i];
  // A paragraph range a sprite covers (the timing can name one inside another).
  for (size_t i = 0; i < s->sprites.size(); ++i)
    if (s->sprites[i].key.spid == key.spid) return s->boxes[i];
  return {};
}

std::unique_ptr<Scene> Show::MakeScene(size_t index, bool built, double now) {
  auto s = std::make_unique<Scene>();
  s->index = index;
  if (index == kEnd) return s;
  const Slide* slide = deck_.GetSlide(index);
  if (slide) {
    s->transition = ParseTransition(slide->transition);
    if (deck_.Show().animations) s->timing = ParseTiming(slide->timing);
  }
  const std::vector<ItemInfo>& items = renderer_.Items(index);
  s->targets.assign(items.size(), {});
  const M toPx = SlideToPixels();
  auto px = [&](const draw::Rect& r) {
    D2D1_POINT_2F a = toPx.TransformPoint({r.left, r.top}), b = toPx.TransformPoint({r.right, r.bottom});
    return D2D1::RectF(a.x, a.y, b.x, b.y);
  };
  draw::Size size = renderer_.SlideSize();
  for (const TargetKey& k : s->timing.targets) {
    size_t i = 0;
    while (i < items.size() && items[i].spid != k.spid) ++i;
    if (i == items.size() || !k.spid) continue;  // a shape the slide doesn't draw
    ItemTargets& t = s->targets[i];
    Sprite sp;
    sp.key = k;
    sp.item = i;
    draw::Rect box = items[i].bounds, ink = items[i].ink;
    int idx = static_cast<int>(s->sprites.size());
    if (k.part == TargetKey::Whole) {
      if (t.whole >= 0) continue;
      t.whole = idx;
    } else if (k.part == TargetKey::Background) {
      if (t.background >= 0) continue;
      t.background = idx;
    } else {
      int last = std::min(k.last, items[i].paragraphs - 1);
      if (k.first > last) continue;
      t.paragraphs.resize(static_cast<size_t>(items[i].paragraphs), -1);
      bool taken = false;
      for (int p = k.first; p <= last; ++p) taken |= t.paragraphs[p] >= 0;
      if (taken) continue;  // overlapping ranges: the first wins
      for (int p = k.first; p <= last; ++p) t.paragraphs[p] = idx;
      box = renderer_.ParagraphBox(index, i, k.first);
      for (int p = k.first + 1; p <= last; ++p) {
        draw::Rect b = renderer_.ParagraphBox(index, i, p);
        box = {std::min(box.left, b.left), std::min(box.top, b.top), std::max(box.right, b.right), std::max(box.bottom, b.bottom)};
      }
      ink = box;
    }
    sp.box = px(box);
    D2D1_RECT_F r = px({std::min(box.left, ink.left), std::min(box.top, ink.top), std::max(box.right, ink.right),
                        std::max(box.bottom, ink.bottom)});
    sp.rect = {std::floor(r.left) - 2, std::floor(r.top) - 2, std::ceil(r.right) + 2, std::ceil(r.bottom) + 2};
    TargetBox tb;
    if (size.width > 0 && size.height > 0) {
      tb.x = (box.left + box.right) / 2 / size.width;
      tb.y = (box.top + box.bottom) / 2 / size.height;
      tb.w = box.Width() / size.width;
      tb.h = box.Height() / size.height;
    }
    s->sprites.push_back(std::move(sp));
    s->boxes.push_back(tb);
  }
  // Pieces in drawing order: an item's static parts, its background or whole sprite, then its paragraphs'.
  for (size_t i = 0; i < items.size(); ++i) {
    const ItemTargets& t = s->targets[i];
    if (t.whole >= 0) s->pieces.push_back({i, t.whole});
    else if (t.background >= 0) s->pieces.push_back({i, t.background});
    if (t.whole < 0) s->pieces.push_back({i, -1});
    int lastSprite = -1;
    for (int sp : t.paragraphs)
      if (sp >= 0 && sp != lastSprite) {
        s->pieces.push_back({i, sp});
        lastSprite = sp;
      }
  }
  s->animator = Animator(&s->timing, &Show::BoxOf, s.get());
  if (built) s->animator.FinishAll(now);
  else s->animator.Start(now, false);
  RenderFrame(*s);
  FinishGpu(250);
  return s;
}

bool Show::RenderSprite(Scene& s, int i) {
  Sprite& sp = s.sprites[static_cast<size_t>(i)];
  if (sp.bitmap) return true;
  UINT w = static_cast<UINT>(std::max(1.f, sp.rect.right - sp.rect.left));
  UINT h = static_cast<UINT>(std::max(1.f, sp.rect.bottom - sp.rect.top));
  sp.bitmap = NewBitmap(w, h);
  if (!BeginOffscreen(sp.bitmap.Get(), true)) return false;
  SpriteFilter filter(sp, s.targets[sp.item]);
  renderer_.Draw(dc_.Get(), s.index, SlideToPixels() * M::Translation(-sp.rect.left, -sp.rect.top), &filter);
  EndOffscreen();
  return true;
}

Pose Show::PoseOfSprite(const Scene& s, int i) const {
  const Sprite& sp = s.sprites[static_cast<size_t>(i)];
  Pose p = s.animator.PoseOf(sp.key);
  // A paragraph moves with its shape when the shape animates too.
  const ItemTargets& t = s.targets[sp.item];
  if (sp.key.part == TargetKey::Paragraphs && t.whole >= 0)
    p = Combine(s.animator.PoseOf(s.sprites[static_cast<size_t>(t.whole)].key), p);
  return p;
}

void Show::DrawSprite(Scene& s, int i, const Pose& pose) {
  const Sprite& sp = s.sprites[static_cast<size_t>(i)];
  if (!pose.visible || pose.opacity <= 0.001f || !sp.bitmap) return;  // drawn ahead (one BeginDraw at a time)
  float cx = (sp.box.left + sp.box.right) / 2, cy = (sp.box.top + sp.box.bottom) / 2;
  D2D1_MATRIX_3X2_F m = M::Translation(-cx, -cy) * M::Scale(pose.sx, pose.sy) * M::Rotation(pose.rotation) *
                        M::Translation(cx + pose.dx * slideW_, cy + pose.dy * slideH_);
  D2D1_MATRIX_3X2_F saved;
  dc_->GetTransform(&saved);
  dc_->SetTransform(m * saved);
  D2D1_RECT_F clips[2];
  int n = 0;
  const Reveal& rv = pose.reveal;
  if (rv.kind != Reveal::None) {
    const D2D1_RECT_F& b = sp.box;
    D2D1_RECT_F r = sp.rect;
    float f = std::clamp(rv.shown, 0.f, 1.f);
    if (rv.kind == Reveal::Wipe) {
      switch (rv.side) {
        case Side::Up: r.bottom = b.top + (b.bottom - b.top) * f; break;     // from the top
        case Side::Left: r.right = b.left + (b.right - b.left) * f; break;   // from the left
        case Side::Right: r.left = b.right - (b.right - b.left) * f; break;  // from the right
        default: r.top = b.bottom - (b.bottom - b.top) * f; break;           // from the bottom
      }
      clips[n++] = r;
    } else if (rv.outward) {
      if (rv.vertical) {
        float half = (b.right - b.left) * f / 2, c = (b.left + b.right) / 2;
        r.left = c - half;
        r.right = c + half;
      } else {
        float half = (b.bottom - b.top) * f / 2, c = (b.top + b.bottom) / 2;
        r.top = c - half;
        r.bottom = c + half;
      }
      clips[n++] = r;
    } else {
      D2D1_RECT_F a = r, z = r;
      if (rv.vertical) {
        a.right = b.left + (b.right - b.left) * f / 2;
        z.left = b.right - (b.right - b.left) * f / 2;
      } else {
        a.bottom = b.top + (b.bottom - b.top) * f / 2;
        z.top = b.bottom - (b.bottom - b.top) * f / 2;
      }
      clips[n++] = a;
      clips[n++] = z;
    }
  }
  for (int k = 0; k < std::max(1, n); ++k) {
    if (n) dc_->PushAxisAlignedClip(clips[k], D2D1_ANTIALIAS_MODE_ALIASED);
    dc_->DrawBitmap(sp.bitmap.Get(), sp.rect, pose.opacity, D2D1_INTERPOLATION_MODE_LINEAR, nullptr, nullptr);
    if (n) dc_->PopAxisAlignedClip();
  }
  dc_->SetTransform(saved);
}

// Draws pieces into `target` (cleared first): runs of static parts through the renderer, sprites
// at their poses.
void Show::RenderPieces(Scene& s, const std::vector<size_t>& pieces, bool background, ID2D1Bitmap1* target) {
  // Sprites the run needs, drawn before the run's own drawing starts (one BeginDraw at a time).
  for (size_t k : pieces)
    if (s.pieces[k].sprite >= 0 && PoseOfSprite(s, s.pieces[k].sprite).visible) RenderSprite(s, s.pieces[k].sprite);
  if (!BeginOffscreen(target, true, background ? D2D1_COLOR_F{0, 0, 0, 1} : D2D1_COLOR_F{0, 0, 0, 0})) return;
  const D2D1_MATRIX_3X2_F toPx = SlideToPixels();
  if (background) {
    // The slide's own area: white under a background that doesn't fill it.
    ComPtr<ID2D1SolidColorBrush> white;
    dc_->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &white);
    if (white) dc_->FillRectangle(D2D1::RectF(0, 0, slideW_, slideH_), white.Get());
  }
  StaticFilter run(s.targets, background);
  auto flush = [&] {
    if (run.Empty()) return;
    dc_->PushAxisAlignedClip(D2D1::RectF(0, 0, slideW_, slideH_), D2D1_ANTIALIAS_MODE_ALIASED);
    renderer_.Draw(dc_.Get(), s.index, toPx, &run);
    dc_->PopAxisAlignedClip();
    run.Reset();
  };
  for (size_t k : pieces) {
    const Piece& p = s.pieces[k];
    if (p.sprite < 0) {
      run.Add(p.item);
      continue;
    }
    flush();
    dc_->PushAxisAlignedClip(D2D1::RectF(0, 0, slideW_, slideH_), D2D1_ANTIALIAS_MODE_ALIASED);
    DrawSprite(s, p.sprite, PoseOfSprite(s, p.sprite));
    dc_->PopAxisAlignedClip();
  }
  flush();
  EndOffscreen();
}

void Show::RenderFrame(Scene& s) {
  if (!s.frame) s.frame = NewBitmap();
  if (s.index == kEnd) {
    if (BeginOffscreen(s.frame.Get(), true, {0, 0, 0, 1})) EndOffscreen();
    return;
  }
  std::vector<size_t> all(s.pieces.size());
  for (size_t k = 0; k < all.size(); ++k) all[k] = k;
  RenderPieces(s, all, true, s.frame.Get());
}

void Show::EnsureLayers(Scene& s) {
  const std::vector<TargetKey>& moving = s.animator.Moving();
  if (s.layered && s.builtFor == moving) return;
  s.builtFor = moving;
  s.layered = true;
  s.movers.clear();
  std::vector<std::vector<size_t>> segments(1);
  auto movingSprite = [&](int sp) {
    const Sprite& sprite = s.sprites[static_cast<size_t>(sp)];
    if (std::find(moving.begin(), moving.end(), sprite.key) != moving.end()) return true;
    const ItemTargets& t = s.targets[sprite.item];
    return sprite.key.part == TargetKey::Paragraphs && t.whole >= 0 &&
           std::find(moving.begin(), moving.end(), s.sprites[static_cast<size_t>(t.whole)].key) != moving.end();
  };
  for (size_t k = 0; k < s.pieces.size(); ++k) {
    int sp = s.pieces[k].sprite;
    if (sp >= 0 && movingSprite(sp)) {
      s.movers.push_back(sp);
      segments.emplace_back();
    } else {
      segments.back().push_back(k);
    }
  }
  s.layers.resize(segments.size());
  for (size_t l = 0; l < segments.size(); ++l) {
    if (l > 0 && segments[l].empty()) {
      s.layers[l].Reset();
      continue;
    }
    if (!s.layers[l]) s.layers[l] = NewBitmap();
    RenderPieces(s, segments[l], l == 0, s.layers[l].Get());
  }
  for (int sp : s.movers) RenderSprite(s, sp);
}

void Show::Settle(Scene& s) {
  s.layered = false;
  s.builtFor.clear();
  s.movers.clear();
  s.layers.clear();  // their memory back: only the slide's picture stays
  RenderFrame(s);
}

// ---- Frames ---------------------------------------------------------------------------------

void Show::Redraw() { Draw(Now()); }

void Show::Draw(double now) {
  if (!EnsureDevice()) return;
  if (cur_ && !cur_->frame && !cur_->layered) RenderFrame(*cur_);
  if (cur_ && cur_->layered && cur_->layers.empty()) EnsureLayers(*cur_);
  const double t0 = anim::Now();
  dc_->SetTarget(target_.Get());
  dc_->BeginDraw();
  dc_->SetTransform(M::Identity());
  if (blank_) {
    dc_->Clear(blank_ == 2 ? D2D1_COLOR_F{1, 1, 1, 1} : D2D1_COLOR_F{0, 0, 0, 1});
  } else if (!cur_ || cur_->index == kEnd) {
    DrawEnd();
  } else {
    // The slide's pictures in its place, black around it.
    dc_->Clear({0, 0, 0, 1});
    dc_->SetTransform(M::Translation(ox_, oy_));
    dc_->PushAxisAlignedClip(D2D1::RectF(0, 0, slideW_, slideH_), D2D1_ANTIALIAS_MODE_ALIASED);
    if (transition_) {
      double dur = std::max(1.0, cur_->transition.durationMs);
      DrawTransition(std::clamp((now - transitionStart_) / dur, 0.0, 1.0));
    } else if (cur_->layered) {
      for (size_t l = 0; l < cur_->layers.size(); ++l) {
        if (cur_->layers[l]) dc_->DrawBitmap(cur_->layers[l].Get());
        if (l < cur_->movers.size()) DrawSprite(*cur_, cur_->movers[l], PoseOfSprite(*cur_, cur_->movers[l]));
      }
    } else if (cur_->frame) {
      dc_->DrawBitmap(cur_->frame.Get());
    }
    dc_->PopAxisAlignedClip();
    dc_->SetTransform(M::Identity());
  }
  HRESULT hr = dc_->EndDraw();
  dc_->SetTarget(nullptr);
  const double t1 = anim::Now();
  if (SUCCEEDED(hr) && swap_) hr = swap_->Present(1, 0);
  if (hr == DXGI_STATUS_OCCLUDED) hr = S_OK;
  drawMs_ = t1 - t0;
  presentMs_ = anim::Now() - t1;
  if (gpu::IsDeviceLoss(hr)) {
    gpu::DeviceLost();
    DropDevice();
    InvalidateRect(hwnd_, nullptr, FALSE);
  }
}

void Show::DrawEnd() {
  dc_->Clear({0, 0, 0, 1});
  ComPtr<ID2D1SolidColorBrush> gray;
  dc_->CreateSolidColorBrush(D2D1::ColorF(0.8f, 0.8f, 0.8f, 1), &gray);
  static const wchar_t kText[] = L"End of slide show, click to exit.";
  if (gray && endFormat_)
    dc_->DrawTextW(kText, static_cast<UINT32>(wcslen(kText)), endFormat_.Get(),
                   D2D1::RectF(0, h_ / 40.f, static_cast<float>(w_), h_ / 8.f), gray.Get());
}

void Show::DrawTransition(double p) {
  const Transition& t = cur_->transition;
  ID2D1Bitmap1* b = cur_->frame.Get();
  ID2D1Bitmap1* a = from_.Get();
  const float W = static_cast<float>(fw_), H = static_cast<float>(fh_);
  dc_->Clear({0, 0, 0, 1});
  auto put = [&](ID2D1Bitmap1* bmp, float x, float y, float opacity = 1) {
    if (bmp) dc_->DrawBitmap(bmp, D2D1::RectF(x, y, x + W, y + H), opacity, D2D1_INTERPOLATION_MODE_LINEAR);
  };
  auto clipped = [&](ID2D1Bitmap1* bmp, D2D1_RECT_F clip) {
    dc_->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_ALIASED);
    put(bmp, 0, 0);
    dc_->PopAxisAlignedClip();
  };
  float sx = 0, sy = 0;  // the direction of motion
  switch (t.side) {
    case Side::Left: sx = -1; break;
    case Side::Up: sy = -1; break;
    case Side::Right: sx = 1; break;
    case Side::Down: sy = 1; break;
    case Side::LeftUp: sx = -1, sy = -1; break;
    case Side::RightUp: sx = 1, sy = -1; break;
    case Side::LeftDown: sx = -1, sy = 1; break;
    case Side::RightDown: sx = 1, sy = 1; break;
  }
  const float e = static_cast<float>(Smooth(p)), f = static_cast<float>(p);
  switch (t.kind) {
    case TransitionKind::Fade:
      if (t.throughBlack) {
        if (f < 0.5f) put(a, 0, 0, 1 - 2 * f);
        else put(b, 0, 0, 2 * f - 1);
      } else {
        put(a, 0, 0);
        put(b, 0, 0, f);
      }
      break;
    case TransitionKind::Push:
      put(a, sx * e * W, sy * e * H);
      put(b, sx * (e - 1) * W, sy * (e - 1) * H);
      break;
    case TransitionKind::Cover:
      put(a, 0, 0);
      put(b, sx * (e - 1) * W, sy * (e - 1) * H);
      break;
    case TransitionKind::Uncover:
      put(b, 0, 0);
      put(a, sx * e * W, sy * e * H);
      break;
    case TransitionKind::Wipe: {
      // dir is where the edge moves: "u" wipes from the bottom up.
      put(a, 0, 0);
      D2D1_RECT_F r = D2D1::RectF(0, 0, W, H);
      if (sx < 0) r.left = W * (1 - f);
      else if (sx > 0) r.right = W * f;
      else if (sy < 0) r.top = H * (1 - f);
      else r.bottom = H * f;
      clipped(b, r);
      break;
    }
    case TransitionKind::Split: {
      put(a, 0, 0);
      if (!t.in) {
        clipped(b, t.vertical ? D2D1::RectF(W * (1 - f) / 2, 0, W * (1 + f) / 2, H)
                              : D2D1::RectF(0, H * (1 - f) / 2, W, H * (1 + f) / 2));
      } else if (t.vertical) {
        clipped(b, D2D1::RectF(0, 0, W * f / 2, H));
        clipped(b, D2D1::RectF(W - W * f / 2, 0, W, H));
      } else {
        clipped(b, D2D1::RectF(0, 0, W, H * f / 2));
        clipped(b, D2D1::RectF(0, H - H * f / 2, W, H));
      }
      break;
    }
    case TransitionKind::Zoom: {
      // In: the new slide grows from the middle; out: the old one shrinks into it.
      D2D1_MATRIX_3X2_F saved;
      dc_->GetTransform(&saved);
      if (t.in) {
        put(a, 0, 0);
        float s = 0.25f + 0.75f * e;
        dc_->SetTransform(M::Scale(s, s, D2D1::Point2F(W / 2, H / 2)) * saved);
        put(b, 0, 0, f);
      } else {
        put(b, 0, 0);
        float s = 1 - 0.75f * e;
        dc_->SetTransform(M::Scale(s, s, D2D1::Point2F(W / 2, H / 2)) * saved);
        put(a, 0, 0, 1 - f);
      }
      dc_->SetTransform(saved);
      break;
    }
    case TransitionKind::None: put(b, 0, 0); break;
  }
}

void Show::EndTransition(double now) {
  transition_ = false;
  from_.Reset();
  visibleAt_ = now;
  if (cur_) cur_->animator.BeginAuto(now);
}

void Show::Frame() {
  if (BenchInterrupted()) {
    AbortBench("input");
    return;
  }
  if (!EnsureDevice()) return;
  // While something moves, the show's frames come before other programs' work (a build, a sync):
  // a frame takes well under a millisecond, so this costs them nothing.
  if (!boosted_ && !capture_) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    boosted_ = true;
  }
  const double before = anim::Now();
  if (waitable_) WaitForSingleObjectEx(waitable_, 100, TRUE);
  const double waited = anim::Now() - before;
  const double start = Now();
  double now = start;
  bool inTransition = transition_;
  if (transition_ && cur_ && now - transitionStart_ >= cur_->transition.durationMs) EndTransition(now);
  if (!transition_ && cur_) {
    cur_->animator.Update(now);
    if (cur_->animator.Running(now)) EnsureLayers(*cur_);
    else if (cur_->layered) Settle(*cur_);
  }
  Draw(now);
  BenchFrame(start, Now() - start, inTransition, waited);
  if (!Animating()) AfterSettle(Now());
}

void Show::AfterSettle(double now) {
  lastFrame_ = -1;
  if (boosted_) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
    boosted_ = false;
  }
  if (!cur_ || cur_->index == kEnd) {
    if (bench_) SetTimer(hwnd_, kTimerAdvance, static_cast<UINT>(kBenchHoldMs), nullptr);
    return;
  }
  PostMessageW(hwnd_, kMsgPrerender, 0, 0);
  if (bench_) {
    SetTimer(hwnd_, kTimerAdvance, static_cast<UINT>(kBenchHoldMs), nullptr);
    return;
  }
  const Transition& t = cur_->transition;
  if (t.advanceAfterMs >= 0 && deck_.Show().useTimings) {
    double wait = std::max(0.0, visibleAt_ + t.advanceAfterMs - now);
    SetTimer(hwnd_, kTimerAdvance, static_cast<UINT>(std::min(wait, 86400000.0)) + 1, nullptr);
  }
}

// ---- Navigation -----------------------------------------------------------------------------

size_t Show::FirstIndex() const { return order_.First(); }
size_t Show::NextIndex(size_t from) const { return order_.Next(from); }
size_t Show::PrevIndex(size_t from) const { return order_.Prev(from); }

bool Show::StartCustom(uint32_t id, size_t returnTo, bool loop) {
  for (const CustomShow& cs : deck_.CustomShows())
    if (cs.id == id) return order_.StartCustom(cs.slides, returnTo, loop);
  return false;
}

void Show::GoTo(size_t index, bool transition, bool built) {
  const double now = Now();
  KillTimer(hwnd_, kTimerAdvance);
  if (index != kEnd && index >= deck_.SlideCount()) return;
  order_.Moved(index);
  hoverItem_ = -1;
  if (cur_ && cur_->animator.Running(now)) {
    cur_->animator.Finish();
    Settle(*cur_);
  }
  if (transition_) EndTransition(now);
  ComPtr<ID2D1Bitmap1> from = cur_ ? cur_->frame : nullptr;
  if (cur_ && cur_->index != kEnd) {
    history_.push_back(cur_->index);
    lastShown_ = cur_->index;
  }
  std::unique_ptr<Scene> scene;
  if (next_ && next_->index == index && !built) scene = std::move(next_);
  else scene = MakeScene(index, built, now);
  next_.reset();
  cur_ = std::move(scene);
  if (cur_->index != kEnd) lastShown_ = cur_->index;
  digits_ = -1;
  const Transition& t = cur_->transition;
  if (transition && index != kEnd && t.kind != TransitionKind::None && t.durationMs > 0) {
    transition_ = true;
    transitionStart_ = Now();
    from_ = from;
    if (bench_) {
      benchTransitions_.push_back({index, t.kind});
      lastFrame_ = -1;
      lastVblank_ = -1;
    }
  } else {
    visibleAt_ = now;
    if (!built) cur_->animator.BeginAuto(now);
    Redraw();
    if (!Animating()) AfterSettle(now);
  }
}

void Show::Next(bool fromClick) {
  const double now = Now();
  if (blank_) {
    Blank(0);
    return;
  }
  if (transition_) {
    // A click during a transition ends it.
    EndTransition(now);
    Redraw();
    if (!Animating()) AfterSettle(now);
    return;
  }
  if (!cur_) return;
  if (cur_->index == kEnd) {
    PostStop();
    return;
  }
  if (fromClick && !cur_->transition.advanceOnClick && !bench_) return;
  KillTimer(hwnd_, kTimerAdvance);
  if (cur_->animator.Running(now) && !cur_->animator.HasNextStep()) {
    cur_->animator.Finish();
    Settle(*cur_);
    Redraw();
    AfterSettle(now);
    return;
  }
  if (cur_->animator.Click(now)) {
    if (!Animating()) {
      Settle(*cur_);
      Redraw();
      AfterSettle(now);
    }
    return;
  }
  Advance();
}

void Show::Advance() { GoTo(order_.Advance(cur_->index), true, false); }

void Show::Previous() {
  if (blank_) {
    Blank(0);
    return;
  }
  const double now = Now();
  if (transition_) EndTransition(now);
  if (!cur_) return;
  if (cur_->index != kEnd && cur_->animator.Back()) {
    KillTimer(hwnd_, kTimerAdvance);
    Settle(*cur_);
    Redraw();
    return;
  }
  size_t prev = cur_->index == kEnd ? lastShown_ : order_.Back(cur_->index);
  if (prev == kEnd) return;
  GoTo(prev, false, true);
}

int Show::ItemAt(int x, int y, int below, draw::Point* pt) const {
  if (!cur_ || cur_->index == kEnd || transition_ || blank_) return -1;
  *pt = {(x - ox_) / scale_, (y - oy_) / scale_};
  const std::vector<ItemInfo>& items = renderer_.Items(cur_->index);
  draw::Size size = renderer_.SlideSize();
  for (size_t i = std::min(items.size(), static_cast<size_t>(below)); i-- > 0;) {
    const ItemInfo& it = items[i];
    draw::Rect b = it.bounds;
    const ItemTargets& t = cur_->targets[i];
    if (t.whole >= 0) {
      Pose p = cur_->animator.PoseOf(cur_->sprites[static_cast<size_t>(t.whole)].key);
      if (!p.visible) continue;
      b = {b.left + p.dx * size.width, b.top + p.dy * size.height, b.right + p.dx * size.width,
           b.bottom + p.dy * size.height};
    }
    if (pt->x >= b.left && pt->x <= b.right && pt->y >= b.top && pt->y <= b.bottom) return static_cast<int>(i);
  }
  return -1;
}

void Show::Hover(int x, int y) {
  draw::Point pt;
  int found = -1;
  for (int i = ItemAt(x, y, INT_MAX, &pt); i >= 0; i = ItemAt(x, y, i, &pt)) {
    const Hyperlink* h = renderer_.Items(cur_->index)[static_cast<size_t>(i)].hover;
    if (h && h->action != Hyperlink::Action::None) {
      found = i;
      break;
    }
  }
  if (found == hoverItem_) return;
  hoverItem_ = found;
  if (found >= 0) Follow(*renderer_.Items(cur_->index)[static_cast<size_t>(found)].hover);
}

const Hyperlink* Show::LinkAt(int x, int y, uint32_t* trigger) {
  if (trigger) *trigger = 0;
  draw::Point pt;
  const std::vector<ItemInfo>* all = cur_ && cur_->index != kEnd ? &renderer_.Items(cur_->index) : nullptr;
  for (int found = ItemAt(x, y, INT_MAX, &pt); found >= 0; found = ItemAt(x, y, found, &pt)) {
    const size_t i = static_cast<size_t>(found);
    const ItemInfo& it = (*all)[i];
    if (it.spid && trigger) {
      for (const Sequence& seq : cur_->timing.triggers)
        if (seq.triggerSpid == it.spid) {
          *trigger = it.spid;
          return nullptr;
        }
    }
    if (it.click && it.click->action != Hyperlink::Action::None) return it.click;
    if (const Hyperlink* l = renderer_.TextLinkAt(cur_->index, i, pt)) return l;
  }
  return nullptr;
}

void Show::Click(int x, int y) {
  if (blank_) {
    Blank(0);
    return;
  }
  uint32_t trigger = 0;
  if (const Hyperlink* link = LinkAt(x, y, &trigger)) {
    Follow(*link);
    return;
  }
  if (trigger && cur_->animator.Trigger(trigger, Now())) {
    if (!Animating()) Redraw();
    return;
  }
  Next(true);
}

void Show::Follow(const Hyperlink& link) {
  using A = Hyperlink::Action;
  auto message = [&](const wchar_t* text) {
    TaskDialog(hwnd_, nullptr, L"Featherweight", text, nullptr, TDCBF_OK_BUTTON, TD_INFORMATION_ICON, nullptr);
  };
  auto confirm = [&](const std::wstring& text, const wchar_t* ok) {
    TASKDIALOG_BUTTON buttons[] = {{IDOK, ok}};
    TASKDIALOGCONFIG cfg{sizeof(cfg)};
    cfg.hwndParent = hwnd_;
    cfg.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
    cfg.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    cfg.pszWindowTitle = L"Featherweight";
    cfg.pszMainInstruction = text.c_str();
    cfg.pButtons = buttons;
    cfg.cButtons = 1;
    cfg.nDefaultButton = IDOK;
    int pressed = IDCANCEL;
    ShowCursorNow();
    return SUCCEEDED(TaskDialogIndirect(&cfg, &pressed, nullptr, nullptr)) && pressed == IDOK;
  };
  switch (link.action) {
    case A::NextSlide: Advance(); break;
    case A::PreviousSlide: {
      size_t p = order_.Back(cur_->index);
      if (p != kEnd) GoTo(p, true, false);
      break;
    }
    case A::FirstSlide:
      if (FirstIndex() != kEnd) GoTo(FirstIndex(), true, false);
      break;
    case A::LastSlide: {
      size_t last = PrevIndex(kEnd);
      if (last != kEnd) GoTo(last, true, false);
      break;
    }
    case A::Slide:
      if (link.slide >= 0 && static_cast<size_t>(link.slide) < deck_.SlideCount())
        GoTo(static_cast<size_t>(link.slide), true, false);
      break;
    case A::CustomShow: {
      // Its slides, then back to this slide (Show and return) or on to the end of the show.
      if (StartCustom(link.customShow, link.returns ? cur_->index : kEnd, false)) GoTo(FirstIndex(), true, false);
      break;
    }
    case A::LastViewed:
      if (!history_.empty()) {
        size_t back = history_.back();
        history_.pop_back();
        GoTo(back, true, true);  // the slide left is now the last viewed: pressing again comes back
      }
      break;
    case A::EndShow: PostStop(); break;
    case A::Url: {
      std::wstring url = fw::Utf8ToWide(link.url);
      if (!util::IsSafeExternalLink(url)) {
        message(L"This link was blocked because it doesn’t point to a website.");
        break;
      }
      if (confirm(L"Open " + url + L"?", L"Open") && !util::OpenExternalLink(url))
        message(L"Windows couldn’t open this link.");
      break;
    }
    case A::File: {
      std::wstring name = fw::Utf8ToWide(link.url);
      if (name.rfind(L"file:///", 0) == 0) name = name.substr(8);
      for (wchar_t& c : name)
        if (c == L'/') c = L'\\';
      std::wstring path = name;
      if (path.size() < 2 || (path[1] != L':' && path.rfind(L"\\\\", 0) != 0)) {
        size_t slash = opts_.deckPath.find_last_of(L"\\/");
        path = (slash == std::wstring::npos ? L"" : opts_.deckPath.substr(0, slash + 1)) + name;
      }
      if (!opts_.openFile || !fw::Opens(path)) {
        message(L"For your safety, Featherweight only opens the kinds of file it reads from links in a slide show.");
        break;
      }
      if (confirm(L"End the slide show and open " + name + L"?", L"Open")) {
        fileToOpen_ = path;
        PostStop();
      }
      break;
    }
    case A::Program:
    case A::Macro:
    case A::Ole: message(L"For your safety, Featherweight doesn’t run programs, macros or embedded objects from links."); break;
    default: break;
  }
}

void Show::Blank(int kind) {
  blank_ = blank_ == kind ? 0 : kind;
  Redraw();
}

void Show::Key(UINT vk) {
  if (blank_ && vk != 'B' && vk != 'W' && vk != VK_OEM_PERIOD && vk != VK_OEM_COMMA && vk != VK_ESCAPE) {
    Blank(0);  // any key brings the slide back
    return;
  }
  if ((vk >= '0' && vk <= '9') || (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9)) {
    int d = static_cast<int>(vk >= VK_NUMPAD0 ? vk - VK_NUMPAD0 : vk - '0');
    digits_ = std::min(100000, std::max(0, digits_) * 10 + d);
    return;
  }
  switch (vk) {
    case VK_RETURN:
      if (digits_ > 0) {
        size_t target = static_cast<size_t>(digits_ - 1);
        digits_ = -1;
        if (target < deck_.SlideCount()) GoTo(target, false, false);
        return;
      }
      Next(false);
      return;
    case VK_RIGHT: case VK_DOWN: case VK_NEXT: case VK_SPACE: case 'N': Next(false); return;
    case VK_LEFT: case VK_UP: case VK_PRIOR: case VK_BACK: case 'P': Previous(); return;
    case VK_HOME:
      if (FirstIndex() != kEnd) GoTo(FirstIndex(), false, false);
      return;
    case VK_END: {
      size_t last = PrevIndex(kEnd);
      if (last != kEnd) GoTo(last, false, false);
      return;
    }
    case 'B': case VK_OEM_PERIOD: Blank(1); return;
    case 'W': case VK_OEM_COMMA: Blank(2); return;
    case VK_ESCAPE: case VK_OEM_MINUS: case VK_SUBTRACT: PostStop(); return;
    default: break;
  }
  digits_ = -1;
}

void Show::ContextMenu(int x, int y) {
  enum { kNext = 1, kPrev, kBlack, kWhite, kEndShow };
  HMENU m = CreatePopupMenu();
  AppendMenuW(m, MF_STRING, kNext, L"Next");
  AppendMenuW(m, MF_STRING, kPrev, L"Previous");
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING | (blank_ == 1 ? MF_CHECKED : 0), kBlack, L"Black screen\tB");
  AppendMenuW(m, MF_STRING | (blank_ == 2 ? MF_CHECKED : 0), kWhite, L"White screen\tW");
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING, kEndShow, L"End show\tEsc");
  POINT pt{x, y};
  ClientToScreen(hwnd_, &pt);
  ShowCursorNow();
  int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_, nullptr);
  DestroyMenu(m);
  switch (cmd) {
    case kNext: Next(false); break;
    case kPrev: Previous(); break;
    case kBlack: Blank(1); break;
    case kWhite: Blank(2); break;
    case kEndShow: PostStop(); break;
    default: break;
  }
}

void Show::ShowCursorNow() {
  cursorHidden_ = false;
  SetTimer(hwnd_, kTimerCursor, static_cast<UINT>(kCursorHideMs), nullptr);  // once: see WM_TIMER
}

// ---- Capture --------------------------------------------------------------------------------

void Show::Step(double t) {
  clock_ = t;
  if (transition_ && cur_ && clock_ - transitionStart_ >= cur_->transition.durationMs) EndTransition(clock_);
  if (!transition_ && cur_) {
    cur_->animator.Update(clock_);
    if (cur_->animator.Running(clock_)) EnsureLayers(*cur_);
    else if (cur_->layered) Settle(*cur_);
  }
  Draw(clock_);
}

bool Show::Save(const std::wstring& name) {
  if (!target_ || !dc_) return false;
  D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
      D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f);
  ComPtr<ID2D1Bitmap1> cpu;
  D2D1_MAPPED_RECT mapped{};
  if (FAILED(dc_->CreateBitmap(D2D1::SizeU(w_, h_), nullptr, 0, &props, &cpu)) ||
      FAILED(cpu->CopyFromBitmap(nullptr, target_.Get(), nullptr)) || FAILED(cpu->Map(D2D1_MAP_OPTIONS_READ, &mapped)))
    return false;
  ComPtr<IWICBitmap> image;
  HRESULT hr = gfx::WIC()->CreateBitmapFromMemory(w_, h_, GUID_WICPixelFormat32bppPBGRA, mapped.pitch,
                                                  mapped.pitch * h_, mapped.bits, &image);
  cpu->Unmap();
  std::wstring path = opts_.captureDir + L"\\" + name + L".png";
  ComPtr<IWICStream> stream;
  ComPtr<IWICBitmapEncoder> encoder;
  ComPtr<IWICBitmapFrameEncode> frame;
  return SUCCEEDED(hr) && SUCCEEDED(gfx::WIC()->CreateStream(&stream)) &&
         SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
         SUCCEEDED(gfx::WIC()->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
         SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
         SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
         SUCCEEDED(frame->WriteSource(image.Get(), nullptr)) && SUCCEEDED(frame->Commit()) &&
         SUCCEEDED(encoder->Commit());
}

// Every slide in show order: its transition at the middle, steps that start by themselves, each
// trigger, then each click's step at its middle and end.
int Show::CaptureAll() {
  CreateDirectoryW(opts_.captureDir.c_str(), nullptr);
  int saved = 0;
  auto save = [&](const wchar_t* fmt, size_t slide, int k) {
    wchar_t name[64];
    swprintf(name, 64, fmt, slide + 1, k);
    if (Save(name)) ++saved;
  };
  // The middle of whatever runs now, and its end.
  auto playOut = [&](size_t slide, const wchar_t* mid, const wchar_t* end, int k, double length) {
    double t0 = clock_;
    if (length > 0) {
      Step(t0 + length / 2);
      save(mid, slide, k);
    }
    for (int guard = 0; guard < 1000 && Animating(); ++guard) Step(clock_ + 500);
    Step(clock_ + 1);
    save(end, slide, k);
  };
  GoTo(FirstIndex() == kEnd ? kEnd : std::min(opts_.first, deck_.SlideCount() - 1), true, false);
  for (int guard = 0; guard < 5000 && cur_ && cur_->index != kEnd; ++guard) {
    const size_t slide = cur_->index;
    if (transition_) {
      double dur = cur_->transition.durationMs;
      Step(transitionStart_ + dur * 0.5);
      save(L"s%02zu-0-transition-mid", slide, 0);
      Step(transitionStart_ + dur + 1);
    }
    const auto& steps = cur_->timing.main.steps;
    if (!steps.empty() && steps[0].start != StepStart::Click)
      playOut(slide, L"s%02zu-1-auto-mid", L"s%02zu-1-auto-end", 0, steps[0].length);
    else
      save(L"s%02zu-1-start", slide, 0);
    // Triggers: a click on each trigger shape.
    for (size_t t = 0; t < cur_->timing.triggers.size(); ++t) {
      const Sequence& seq = cur_->timing.triggers[t];
      if (!cur_->animator.Trigger(seq.triggerSpid, clock_)) continue;
      playOut(slide, L"s%02zu-2-trigger%d-mid", L"s%02zu-2-trigger%d-end", static_cast<int>(t + 1),
              seq.steps.empty() ? 0 : seq.steps[0].length);
    }
    for (int k = 1; cur_ && cur_->index == slide && cur_->animator.HasNextStep(); ++k) {
      int step = cur_->animator.StepIndex();
      double length = step < static_cast<int>(steps.size()) ? steps[static_cast<size_t>(step)].length : 0;
      Next(false);
      playOut(slide, L"s%02zu-3-step%02d-mid", L"s%02zu-3-step%02d-end", k, length);
    }
    Next(false);
  }
  if (cur_ && cur_->index == kEnd) {
    Draw(clock_);
    save(L"s%02zu-end", deck_.SlideCount(), 0);
  }
  return saved;
}

// ---- Window ---------------------------------------------------------------------------------

LRESULT CALLBACK Show::WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_NCCREATE) {
    auto* self = static_cast<Show*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
    SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    self->hwnd_ = h;
  }
  auto* self = reinterpret_cast<Show*>(GetWindowLongPtrW(h, GWLP_USERDATA));
  if (!self) return DefWindowProcW(h, m, w, l);
  if (m == WM_NCDESTROY) {
    // The show is over: the window goes, then the show; nothing of it is touched after this.
    SetWindowLongPtrW(h, GWLP_USERDATA, 0);
    anim::SetFrameSource(nullptr);
    if (self->bench_) self->WriteBench();
    auto ended = std::move(self->opts_.ended);
    auto open = std::move(self->opts_.openFile);
    size_t last = self->cur_ && self->cur_->index != kEnd ? self->cur_->index : self->lastShown_;
    std::wstring file = std::move(self->fileToOpen_);
    if (self->boosted_) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
    if (self->awake_) SetThreadExecutionState(ES_CONTINUOUS);
    self->hwnd_ = nullptr;
    self->DropDevice();
    if (g_show == self) g_show = nullptr;
    delete self;
    if (ended) ended(last);
    if (!file.empty() && open) open(file);
    return DefWindowProcW(h, m, w, l);
  }
  return self->Proc(m, w, l);
}

LRESULT Show::Proc(UINT m, WPARAM w, LPARAM l) {
  if (bench_) {
    switch (m) {
      case WM_KEYDOWN:
      case WM_SYSKEYDOWN:
      case WM_LBUTTONDOWN:
      case WM_RBUTTONDOWN:
      case WM_MBUTTONDOWN:
      case WM_XBUTTONDOWN:
      case WM_MOUSEWHEEL:
      case WM_MOUSEHWHEEL: AbortBench("input"); return 0;
      case WM_MOUSEMOVE: {  // also sent when nothing moved (the window appearing under the cursor)
        POINT pt;
        GetCursorPos(&pt);
        if (pt.x != benchCursor_.x || pt.y != benchCursor_.y) AbortBench("input");
        return 0;
      }
      case WM_ACTIVATE: {
        DWORD pid = 0;
        if (l) GetWindowThreadProcessId(reinterpret_cast<HWND>(l), &pid);
        if (LOWORD(w) == WA_INACTIVE && pid != GetCurrentProcessId()) {
          AbortBench("another window took the focus");
          return 0;
        }
        break;
      }
      case WM_TIMER:
      case kMsgPrerender:
        if (BenchInterrupted()) {
          AbortBench("input");
          return 0;
        }
        break;
      default: break;
    }
  }
  switch (m) {
    case WM_PAINT: {
      PAINTSTRUCT ps;
      BeginPaint(hwnd_, &ps);
      EndPaint(hwnd_, &ps);
      if (!Animating()) Redraw();
      return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_LBUTTONDOWN:
      ShowCursorNow();
      Click(static_cast<short>(LOWORD(l)), static_cast<short>(HIWORD(l)));
      return 0;
    case WM_RBUTTONUP: ContextMenu(static_cast<short>(LOWORD(l)), static_cast<short>(HIWORD(l))); return 0;
    case WM_MOUSEWHEEL:
      if (GET_WHEEL_DELTA_WPARAM(w) < 0) Next(false);
      else Previous();
      return 0;
    case WM_MOUSEMOVE: {
      static LPARAM lastPos = -1;  // WM_MOUSEMOVE also comes when nothing moved (a window shown)
      if (l != lastPos) {
        lastPos = l;
        ShowCursorNow();
        Hover(static_cast<short>(LOWORD(l)), static_cast<short>(HIWORD(l)));
      }
      return 0;
    }
    case WM_SETCURSOR:
      if (LOWORD(l) == HTCLIENT) {
        if (cursorHidden_) {
          SetCursor(nullptr);
          return TRUE;
        }
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(hwnd_, &pt);
        uint32_t trigger = 0;
        bool link = LinkAt(pt.x, pt.y, &trigger) || trigger;
        SetCursor(LoadCursorW(nullptr, link ? IDC_HAND : IDC_ARROW));
        return TRUE;
      }
      break;
    case WM_TIMER:
      KillTimer(hwnd_, w);  // both are one-shot: nothing ticks while the show is idle
      if (w == kTimerCursor) {
        cursorHidden_ = true;
        SetCursor(nullptr);
      } else if (w == kTimerAdvance && !Animating()) {
        Next(false);
      }
      return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
      if (w == VK_F4 && (GetKeyState(VK_MENU) < 0)) break;
      Key(static_cast<UINT>(w));
      return 0;
    case kMsgPrerender:
      if (cur_ && cur_->index != kEnd && !Animating()) {
        size_t n = NextIndex(cur_->index);
        if (n != kEnd && (!next_ || next_->index != n)) next_ = MakeScene(n, false, Now());
      }
      return 0;
    case kMsgStop: DestroyWindow(hwnd_); return 0;
    case WM_CLOSE: DestroyWindow(hwnd_); return 0;
    case WM_ACTIVATE:
      if (LOWORD(w) != WA_INACTIVE) SetFocus(hwnd_);
      return 0;
    default: break;
  }
  return DefWindowProcW(hwnd_, m, w, l);
}

// ---- The bench ------------------------------------------------------------------------------

void Show::BenchFrame(double start, double cpuMs, bool inTransition, double waitMs) {
  if (!bench_) return;
  cpuMs_.push_back(cpuMs);
  drawMax_ = std::max(drawMax_, drawMs_);
  presentMax_ = std::max(presentMax_, presentMs_);
  if (lastFrame_ >= 0) {
    waitMax_ = std::max(waitMax_, waitMs);
    // Frames start when the display takes one: a gap of two refreshes or more is a dropped frame.
    double interval = start - lastFrame_;
    int missed = interval > 1.5 * periodMs_ ? static_cast<int>(std::lround(interval / periodMs_)) - 1 : 0;
    if (inTransition && !benchTransitions_.empty()) {
      BenchTransition& t = benchTransitions_.back();
      ++t.frames;
      t.dropped += missed;
      t.worstMs = std::max(t.worstMs, interval);
      if (missed && slow_.size() < 40) slow_.push_back({t.slide, interval, waitMs, cpuMs, drawMs_, presentMs_});
    } else {
      ++animationFrames_;
      animationDropped_ += missed;
    }
  } else if (inTransition && !benchTransitions_.empty()) {
    ++benchTransitions_.back().frames;
  }
  lastFrame_ = start;
  if (inTransition) {
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    if (lastVblank_ < 0 || !vblankPeriod_) {
      DWM_TIMING_INFO ti{};
      ti.cbSize = sizeof(ti);
      if (SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &ti)) && ti.qpcRefreshPeriod) {
        vblank0_ = ti.qpcVBlank;
        vblankPeriod_ = ti.qpcRefreshPeriod;
      }
    }
    if (vblankPeriod_ && static_cast<UINT64>(qpc.QuadPart) >= vblank0_) {
      long long index = static_cast<long long>((static_cast<UINT64>(qpc.QuadPart) - vblank0_) / vblankPeriod_);
      // The display queue: a frame shows at the refresh after its present, and never before the one
      // after the previous frame's (frames wait their turn). A refresh between two frames shown is
      // one with no new frame.
      long long shown = lastVblank_ >= 0 ? std::max(lastVblank_ + 1, index + 1) : index + 1;
      if (lastVblank_ >= 0 && shown > lastVblank_ + 1) {
        int missed = static_cast<int>(shown - lastVblank_ - 1);
        vblankMissed_ += missed;
        if (!benchTransitions_.empty()) benchTransitions_.back().vblankMissed += missed;
      }
      lastVblank_ = shown;
    }
  } else {
    lastVblank_ = -1;
  }
  // DXGI: over consecutive samples within a transition, every refresh should have had a new frame.
  DXGI_FRAME_STATISTICS st{};
  if (!swap_ || FAILED(swap_->GetFrameStatistics(&st))) {
    ++dxgiFailures_;
    dxgiHave_ = false;
    return;
  }
  if (dxgiHave_ && inTransition && st.PresentCount > dxgiLast_.PresentCount &&
      st.SyncRefreshCount >= dxgiLast_.SyncRefreshCount) {
    UINT presents = st.PresentCount - dxgiLast_.PresentCount;
    UINT refreshes = st.SyncRefreshCount - dxgiLast_.SyncRefreshCount;
    if (refreshes > presents) dxgiMissed_ += static_cast<int>(refreshes - presents);
    ++dxgiSamples_;
  }
  dxgiLast_ = st;
  dxgiHave_ = inTransition;
}

bool Show::BenchInterrupted() const {
  if (!bench_) return false;
  LASTINPUTINFO li{sizeof(li)};
  return GetLastInputInfo(&li) && li.dwTime != benchInput_;
}

void Show::AbortBench(const char* why) {
  benchAborted_ = why;
  DestroyWindow(hwnd_);  // writes the report and deletes the show
}

void Show::WriteBench() {
  int frames = 0, dropped = 0;
  double worst = 0;
  for (const BenchTransition& t : benchTransitions_) {
    frames += t.frames;
    dropped += t.dropped;
    worst = std::max(worst, t.worstMs);
  }
  std::vector<double> cpu = cpuMs_;
  std::sort(cpu.begin(), cpu.end());
  auto pct = [&](double q) { return cpu.empty() ? 0.0 : cpu[std::min(cpu.size() - 1, static_cast<size_t>(q * cpu.size()))]; };
  char buf[1024];
  std::string json = "{\n";
  snprintf(buf, sizeof(buf),
           "  \"refreshHz\": %.2f,\n  \"width\": %u,\n  \"height\": %u,\n  \"slides\": %zu,\n  \"transitions\": %zu,\n"
           "  \"transitionFrames\": %d,\n  \"missedRefreshes\": %d,\n  \"lateFrameStarts\": %d,\n"
           "  \"dxgiMissedRefreshes\": %d,\n"
           "  \"dxgiSamples\": %d,\n  \"dxgiFailures\": %d,\n  \"waitMsMax\": %.2f,\n  \"drawMsMax\": %.2f,\n"
           "  \"presentMsMax\": %.2f,\n"
           "  \"worstIntervalMs\": %.2f,\n  \"animationFrames\": %d,\n  \"animationDropped\": %d,\n"
           "  \"cpuMsMedian\": %.3f,\n  \"cpuMs99\": %.3f,\n  \"cpuMsMax\": %.3f,\n  \"warp\": %s,\n  \"pass\": %s,\n"
           "  \"aborted\": %s,\n  \"abortReason\": \"%s\",\n",
           refreshHz_, w_, h_, deck_.SlideCount(), benchTransitions_.size(), frames, vblankMissed_, dropped,
           dxgiMissed_, dxgiSamples_, dxgiFailures_, waitMax_, drawMax_, presentMax_, worst, animationFrames_, animationDropped_, pct(0.5),
           pct(0.99), cpu.empty() ? 0.0 : cpu.back(), gpu::IsWarp() ? "true" : "false",
           !benchAborted_ && vblankMissed_ == 0 && frames > 0 ? "true" : "false", benchAborted_ ? "true" : "false",
           benchAborted_ ? benchAborted_ : "");
  json += buf;
  json += "  \"lateFrames\": [\n";
  for (size_t i = 0; i < slow_.size(); ++i) {
    const SlowFrame& f = slow_[i];
    snprintf(buf, sizeof(buf),
             "    {\"slide\": %zu, \"intervalMs\": %.2f, \"waitMs\": %.2f, \"cpuMs\": %.2f, \"drawMs\": %.2f, "
             "\"presentMs\": %.2f}%s\n",
             f.slide + 1, f.interval, f.wait, f.cpu, f.draw, f.present, i + 1 < slow_.size() ? "," : "");
    json += buf;
  }
  json += "  ],\n";
  json += "  \"perTransition\": [\n";
  for (size_t i = 0; i < benchTransitions_.size(); ++i) {
    const BenchTransition& t = benchTransitions_[i];
    snprintf(buf, sizeof(buf),
             "    {\"slide\": %zu, \"kind\": %d, \"frames\": %d, \"missed\": %d, \"lateStarts\": %d, \"worstMs\": %.2f}%s\n",
             t.slide + 1, static_cast<int>(t.kind), t.frames, t.vblankMissed, t.dropped, t.worstMs,
             i + 1 < benchTransitions_.size() ? "," : "");
    json += buf;
  }
  json += "  ]\n}\n";
  util::WriteFileAtomic(opts_.benchReport, json);
  g_benchPassed = !benchAborted_ && vblankMissed_ == 0 && frames > 0;
  g_benchAborted = benchAborted_ != nullptr;
}

}  // namespace

bool Start(Presentation& deck, SlideRenderer& renderer, const ShowOptions& options) {
  if (g_show) return false;
  auto* show = new Show(deck, renderer, options);
  g_show = show;
  if (!show->Create()) {
    if (g_show == show) g_show = nullptr;
    delete show;
    return false;
  }
  return true;
}

bool Running() { return g_show != nullptr; }

void Stop() {
  if (g_show) g_show->Stop();
}

std::vector<std::wstring> Monitors() {
  MonitorList list;
  std::vector<std::wstring> names;
  for (size_t i = 0; i < list.handles.size(); ++i) {
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(list.handles[i], &mi);
    wchar_t buf[96];
    swprintf(buf, 96, L"%zu: %ld × %ld%s", i + 1, mi.rcMonitor.right - mi.rcMonitor.left,
             mi.rcMonitor.bottom - mi.rcMonitor.top, (mi.dwFlags & MONITORINFOF_PRIMARY) ? L" (main)" : L"");
    names.push_back(buf);
  }
  return names;
}

int RunStandalone(const std::wstring& path, const std::wstring& benchReport, const std::wstring& captureDir) {
  fw::pkg::Status st;
  fw::pkg::Snapshot::Options so;
  auto snapshot = fw::pkg::Snapshot::Open(path, so, st);
  std::unique_ptr<fw::pkg::Package> package =
      snapshot ? fw::pkg::Package::Open(std::move(snapshot), fw::pkg::Limits::Default(), st) : nullptr;
  std::unique_ptr<Presentation> deck = package ? Read(std::move(package), st) : nullptr;
  if (!deck) {
    MessageBoxW(nullptr, (L"Featherweight couldn’t open " + path + L".").c_str(), L"Featherweight", MB_ICONERROR);
    return 1;
  }
  std::unique_ptr<SlideRenderer> renderer = MakeRenderer(*deck);
  ShowOptions o;
  o.deckPath = path;
  o.benchReport = benchReport;
  o.captureDir = captureDir;
  if (!Start(*deck, *renderer, o)) return 1;
  if (!captureDir.empty()) {
    int saved = g_show->CaptureAll();
    g_show->Stop();
    return saved > 0 ? 0 : 1;
  }
  MSG msg;
  while (g_show) {
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    if (!g_show) break;
    if (g_show->Animating()) g_show->Frame();
    else MsgWaitForMultipleObjectsEx(0, nullptr, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
  }
  if (g_benchAborted) return 3;  // stopped on someone's input: no result
  return benchReport.empty() || g_benchPassed ? 0 : 2;
}

}  // namespace fw::slide::show
