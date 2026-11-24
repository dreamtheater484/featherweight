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
#include <shlwapi.h>

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
#include "pkg/XmlDom.h"
#include "slide/Presentation.h"
#include "slide/show/MediaPlayer.h"
#include "slide/show/PresenterView.h"
#include "slide/show/ShowDraw.h"
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
// The first transition plays on a window and swap chain that are brand new, and the compositor takes
// a few refreshes to settle on a new full-screen window (the first frames wait ~16 ms for their
// buffer). So it holds still at its start, which is black, for this long and at least this many
// frames before it moves: every later transition starts on a window that is already presenting.
constexpr double kWarmupMs = 100;
constexpr int kWarmupFrames = 4;
// The bench: a frame starting more than this many refreshes after the previous one skipped a
// refresh slot (the frame queued ahead may still have covered it: missedRefreshes says whether the
// display went without a new frame).
constexpr double kLateStart = 1.8;


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

class Show final : public anim::FrameSource, private PresenterHost {
 public:
  Show(Presentation& deck, SlideRenderer& renderer, ShowOptions options)
      : deck_(deck), renderer_(renderer), opts_(std::move(options)) {}

  bool Create();
  void Stop() {
    if (hwnd_) Close();
  }
  // The slide moves: a transition or a step runs. The frame loop also runs while a video plays (P3).
  bool Moving() const { return transition_ || (cur_ && cur_->animator.Running(0)); }
  bool Animating() const override { return Moving() || VideoPlaying(); }
  void Frame() override;
  // --show-capture: every transition and step at its middle and end, as PNG files.
  int CaptureAll();

 private:
  double Now() const { return capture_ ? clock_ : anim::Now(); }
  void Step(double t);
  bool Save(const std::wstring& name, ID2D1Bitmap1* from = nullptr, UINT w = 0, UINT h = 0);
  // --show-capture with --show-presenter: the presenter view as it is now, drawn off screen.
  bool SavePresenter(const std::wstring& name);
  static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l);
  LRESULT Proc(UINT m, WPARAM w, LPARAM l);
  void PostStop() { PostMessageW(hwnd_, kMsgStop, 0, 0); }
  // Destroys the window (and with it the show). Destroying it moves the focus to another program's
  // window: closing_ tells the bench that this focus change is the show's own.
  void Close() {
    closing_ = true;
    DestroyWindow(hwnd_);
  }

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
  // `built`: every step done (coming back to a slide). `steps` >= 0: the main steps up to that one
  // done and settled (the presenter view's next animation).
  std::unique_ptr<Scene> MakeScene(size_t index, bool built, double now, int steps = -1);
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
  // The display queue model: this present shows at the refresh after it, never before the one after
  // the previous present's. `count`: refreshes that go without a new frame count as missed.
  void TrackVblank(bool count, double start, double waited);
  void WriteBench();
  // The bench ignores the mouse and the keyboard (a click or a key changes nothing: Roy, 2026-11-24,
  // "mouse movement or a click in a running program should generally not be a throw away"); only an
  // explicit stop (Esc, Alt+F4, closing it) or another program taking the focus ends it early.
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
  // P3: the presenter view, and the current slide one click on for its "next animation".
  std::unique_ptr<PresenterView> presenter_;
  std::unique_ptr<Scene> built_;
  int builtStep_ = -1;
  // P3: the current slide's videos and audio, opened at their first command, gone when the slide changes.
  struct Media {
    uint32_t spid = 0;
    size_t item = SIZE_MAX;  // in renderer_.Items: where a video's frames go
    std::unique_ptr<MediaPlayer> player;
    ID2D1Bitmap1* frame = nullptr;  // its newest frame (null: the poster shows)
    float crop[4] = {0, 0, 0, 0};   // the picture's a:srcRect (left, top, right, bottom): the video's too
    bool hideWhenStopped = false;   // Hide While Not Playing: its poster is hidden (Timing), the video shown while it plays
    bool fullScreen = false;        // Play Full Screen: the video over the whole slide while it plays
  };
  std::vector<Media> media_;
  // A media that couldn't play (a linked file that isn't on this PC): a line over its shape until the
  // slide changes; its poster stays.
  struct MediaNote {
    uint32_t spid;
    size_t item;
    std::wstring text;
  };
  std::vector<MediaNote> mediaNotes_;
  ComPtr<IDWriteTextFormat> noteFormat_;
  void NoteMedia(uint32_t spid, size_t item, std::wstring text);
  std::string mediaReport_;  // --show-media-report: each video's line
  void ReleaseMedia();       // they stop (the report keeps their frames)
  void TakeMedia();  // runs the media commands the slide's timing fired
  MediaPlayer* OpenMedia(uint32_t spid);
  bool VideoPlaying() const;
  void VideoFrames();  // before a frame's BeginDraw
  void DrawVideos();   // on the slide, in its pixels
  void UpdatePresenter();
  void PresenterKey(UINT vk) override { Key(vk); }
  void PresenterGoTo(size_t slide) override {
    if (slide < deck_.SlideCount()) GoTo(slide, false, false);
  }
  void PresenterClosed() override { PostStop(); }
  bool PresenterBusy() const override { return Moving(); }
  SlideOrder order_;  // hidden slides, the loop, custom shows
  int hoverItem_ = -1;  // the item the mouse is over whose mouse-over action has run
  size_t lastShown_ = 0;

  bool transition_ = false;
  double transitionStart_ = 0;
  bool closing_ = false;  // the window is being destroyed
  // The first transition's warm-up (kWarmupMs): until it's over, the transition holds at its start.
  bool warmedUp_ = false;
  int warmupFrames_ = 0;
  double warmupUntil_ = 0;
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
  // Refreshes with no new frame: each present's vblank interval (on DWM's clock) against the last.
  UINT64 vblank0_ = 0, vblankPeriod_ = 0;
  long long lastVblank_ = -1;
  int vblankMissed_ = 0;
  // The first transition's frames, its warm-up too: what the queue model above made of each present
  // against DXGI's own statistics (which present was on screen at which refresh), to tell a refresh
  // that really had no new frame from the model's guess. Refresh numbers count from vblank0_.
  struct FrameLog {
    double start, wait, draw, present, phase;  // phase: where in its refresh the present returned (0..1)
    long long vblank, shown, syncVblank;
    int missed;
    bool warm;
    UINT lastPresent, presentCount, presentRefresh, syncRefresh;
    HRESULT statsHr;
    int mode;  // DXGI_FRAME_PRESENTATION_MODE (composed 0, overlay 1, none 2, failed 3), -1 unknown
  };
  std::vector<FrameLog> frameLog_;
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
  draw::Size size = renderer_.SlideSize();
  if (size.width <= 0 || size.height <= 0) size = {720, 540};
  DWORD style = WS_POPUP;
  if (opts_.windowed && !capture_) {
    // Half the work area's width, the slide's shape, in the monitor's left half (the presenter view
    // takes the right): a fixed size, so the slide pictures never change size.
    style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    const RECT& work = mi.rcWork;
    const double maxW = (work.right - work.left) / 2.0 - 16, maxH = (work.bottom - work.top) * 0.7;
    const long cw = std::max(320L, static_cast<long>(std::min(maxW, maxH * size.width / size.height)));
    const long ch = std::max(180L, static_cast<long>(std::lround(cw * size.height / size.width)));
    r = {work.left + 8, work.top + 8, work.left + 8 + cw, work.top + 8 + ch};
    AdjustWindowRectEx(&r, style, FALSE, WS_EX_NOREDIRECTIONBITMAP);
    OffsetRect(&r, work.left + 8 - r.left, work.top + 8 - r.top);
  }
  hwnd_ = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, kClass, L"Slide Show", style, r.left, r.top, r.right - r.left,
                          r.bottom - r.top, opts_.owner, nullptr, GetModuleHandleW(nullptr), this);
  if (!hwnd_) return false;
  RECT client{};
  GetClientRect(hwnd_, &client);
  if (capture_) client = {0, 0, r.right - r.left, r.bottom - r.top};
  w_ = static_cast<UINT>(std::max(1L, client.right - client.left));
  h_ = static_cast<UINT>(std::max(1L, client.bottom - client.top));
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
  gfx::DWrite()->CreateTextFormat(gfx::UiFont().c_str(), nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                  DWRITE_FONT_STRETCH_NORMAL, std::max(11.f, h_ / 64.f), L"", &noteFormat_);
  if (noteFormat_) noteFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
  if (!EnsureDevice()) {
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
    return false;
  }
  if (capture_) {
    // CaptureAll drives it; with --show-presenter the presenter view too, off screen (no window).
    if (opts_.presenter) presenter_ = PresenterView::Create(*this, deck_, renderer_, nullptr, false, nullptr);
    return true;
  }
  // A show keeps the display and the PC awake while it runs (the power settings stay as they are).
  SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED);
  awake_ = true;
  ShowWindow(hwnd_, SW_SHOW);
  if (opts_.presenter) {
    // Beside the windowed show, or full screen on another monitor; with one monitor and full screen,
    // none (PowerPoint shows only the slides then).
    HMONITOR where = opts_.windowed ? mon : nullptr;
    for (size_t i = 0; !where && i < monitors.handles.size(); ++i)
      if (monitors.handles[i] != mon) where = monitors.handles[i];
    if (where) presenter_ = PresenterView::Create(*this, deck_, renderer_, where, opts_.windowed, opts_.owner);
  }
  warmupUntil_ = anim::Now() + kWarmupMs;
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

std::unique_ptr<Scene> Show::MakeScene(size_t index, bool built, double now, int steps) {
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
  if (built) {
    s->animator.FinishAll(now);
  } else if (steps >= 0) {
    s->animator.Start(now);
    s->animator.Finish();
    while (s->animator.StepIndex() < steps && s->animator.Click(now)) s->animator.Finish();
  } else {
    s->animator.Start(now, false);
  }
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
  DrawPosed(dc_.Get(), sp.bitmap.Get(), sp.rect, sp.box, pose, slideW_, slideH_);  // ShowDraw (shared with Preview)
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
  TakeMedia();
  if (cur_ && !cur_->frame && !cur_->layered) RenderFrame(*cur_);
  if (cur_ && cur_->layered && cur_->layers.empty()) EnsureLayers(*cur_);
  VideoFrames();
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
    if (!transition_) DrawVideos();
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
  // ShowDraw (shared with the editor's Preview): the old slide (or black) to the new one.
  DrawTransitionFrame(dc_.Get(), cur_->transition, from_.Get(), cur_->frame.Get(), static_cast<float>(fw_),
                      static_cast<float>(fh_), p);
}

void Show::EndTransition(double now) {
  transition_ = false;
  from_.Reset();
  visibleAt_ = now;
  if (cur_) cur_->animator.BeginAuto(now);
}

void Show::Frame() {
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
  const bool wasMoving = Moving();
  bool warming = false;
  if (!warmedUp_) {
    if (transition_ && (warmupFrames_ < kWarmupFrames || now < warmupUntil_)) {
      ++warmupFrames_;
      transitionStart_ = now;  // held at its start (black): the clock starts once the window has settled
      warming = true;
    } else {
      warmedUp_ = true;
    }
  }
  bool inTransition = transition_ && !warming;
  if (transition_ && cur_ && now - transitionStart_ >= cur_->transition.durationMs) EndTransition(now);
  if (!transition_ && cur_) {
    cur_->animator.Update(now);
    if (cur_->animator.Running(now)) EnsureLayers(*cur_);
    else if (cur_->layered) Settle(*cur_);
  }
  Draw(now);
  if (!warming) BenchFrame(start, Now() - start, inTransition, waited);  // the warm-up is no part of any motion
  // Its frames still fill the display queue the first counted frames join: the model sees them too.
  else if (bench_) TrackVblank(false, start, waited);
  if (wasMoving && !Moving()) AfterSettle(Now());
  else if (!Animating()) PostMessageW(hwnd_, kMsgPrerender, 0, 0);  // a video stopped: the next slide now
}

void Show::AfterSettle(double now) {
  lastFrame_ = -1;
  if (boosted_) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
    boosted_ = false;
  }
  UpdatePresenter();  // only once nothing moves: the presenter view never costs a transition a frame
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
  ReleaseMedia();  // the slide's videos and audio stop
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
    if (!Moving()) AfterSettle(now);
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
    if (!Moving()) AfterSettle(now);
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
    if (!Moving()) {
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
  UpdatePresenter();
}

// ---- Media (P3) -----------------------------------------------------------------------------

namespace {

// A media shape of the slide by its id, in groups too.
const Shape* FindMedia(const std::vector<Shape>& shapes, uint32_t spid) {
  for (const Shape& s : shapes) {
    if (s.id == spid && s.media) return &s;
    if (const Shape* in = FindMedia(s.children, spid)) return in;
  }
  return nullptr;
}

std::string RelAttr(pugi::xml_node n, std::string_view local) {
  for (pugi::xml_attribute a : n.attributes())
    if (pkg::LocalNameOf(a) == local && std::string_view(a.name()).find(':') != std::string_view::npos) return a.value();
  return {};
}

std::wstring ExtensionOf(const std::wstring& name) {
  const size_t dot = name.find_last_of(L'.');
  const size_t slash = name.find_last_of(L"\\/");
  return dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash) ? std::wstring() : name.substr(dot);
}

}  // namespace

MediaPlayer* Show::OpenMedia(uint32_t spid) {
  const Slide* slide = cur_ && cur_->index != kEnd ? deck_.GetSlide(cur_->index) : nullptr;
  const Shape* sh = slide ? FindMedia(slide->shapes, spid) : nullptr;
  if (!sh) return nullptr;
  // Its file: the embedded copy (p14:media r:embed), else what a:videoFile or a:audioFile names, embedded
  // or a local file.
  std::string embed, link;
  bool video = true;
  for (pugi::xml_node nv : sh->node.children()) {
    if (pkg::LocalNameOf(nv).substr(0, 2) != "nv") continue;
    for (pugi::xml_node pr : nv.children()) {
      if (pkg::LocalNameOf(pr) != "nvPr") continue;
      for (pugi::xml_node m : pr.children()) {
        const std::string_view k = pkg::LocalNameOf(m);
        if (k == "videoFile" || k == "quickTimeFile" || k == "audioFile" || k == "wavAudioFile") {
          video = k == "videoFile" || k == "quickTimeFile";
          link = RelAttr(m, "link");
          if (link.empty()) link = RelAttr(m, "embed");
        } else if (k == "extLst") {
          for (pugi::xml_node ext : m.children())
            for (pugi::xml_node x : ext.children())
              if (pkg::LocalNameOf(x) == "media") embed = RelAttr(x, "embed");
        }
      }
    }
    break;
  }
  size_t item = SIZE_MAX;
  const std::vector<ItemInfo>& items = renderer_.Items(cur_->index);
  for (size_t i = 0; i < items.size(); ++i)
    if (items[i].spid == spid) item = i;
  const pkg::RelationshipSet* rels = deck_.RelationshipsOf(slide->part);
  MediaPlayer::Source src;
  src.video = video;
  std::string missing;  // a linked file that isn't on this PC (or not local: a web address, a share)
  auto resolve = [&](const std::string& id) {
    const pkg::Relationship* r = rels && !id.empty() ? rels->Find(id) : nullptr;
    if (!r) return false;
    if (r->external) {
      src.path = LocalMediaPath(r->target, opts_.deckPath);  // a local file only: nothing is fetched
      src.extension = ExtensionOf(src.path);
      if (src.path.empty()) missing = r->target;
      return !src.path.empty();
    }
    src.extension = ExtensionOf(Utf8ToWide(r->targetPart));
    return PlayableMediaExtension(src.extension) && deck_.ReadPart(r->targetPart, src.bytes);
  };
  if (!resolve(embed) && !resolve(link)) {
    if (!missing.empty()) {
      // Its name only (file:///C:/My%20clips/a.mp4 -> a.mp4), never the whole path.
      std::wstring name = Utf8ToWide(missing);
      const size_t slash = name.find_last_of(L"\\/");
      if (slash != std::wstring::npos) name.erase(0, slash + 1);
      if (!name.empty()) {
        std::vector<wchar_t> buf(name.begin(), name.end());
        buf.push_back(0);
        if (SUCCEEDED(UrlUnescapeW(buf.data(), nullptr, nullptr, URL_UNESCAPE_INPLACE))) name = buf.data();
      }
      NoteMedia(spid, item, L"The linked file isn't on this PC: " + name);
    } else {
      NoteMedia(spid, item, L"This media can't be played.");
    }
    return nullptr;
  }
  MediaPlayer::Settings settings;
  bool hide = false, full = false;
  for (const MediaNode& n : cur_->timing.media)
    if (n.spid == spid) {
      settings.volume = n.volume;
      settings.mute = n.mute;
      settings.loop = n.loop;
      hide = !n.showWhenStopped;
      full = n.fullScreen;
    }
  std::unique_ptr<MediaPlayer> player = MediaPlayer::Open(std::move(src), settings);
  if (!player) {
    NoteMedia(spid, item, L"This media can't be played.");
    return nullptr;
  }
  Media m;
  m.spid = spid;
  m.item = item;
  m.hideWhenStopped = hide;
  m.fullScreen = full && video;
  // The poster's crop crops the video too, as in PowerPoint (in 1/1000 %).
  for (pugi::xml_node fill : sh->node.children()) {
    if (pkg::LocalNameOf(fill) != "blipFill") continue;
    for (pugi::xml_node c : fill.children()) {
      if (pkg::LocalNameOf(c) != "srcRect") continue;
      const char* names[] = {"l", "t", "r", "b"};
      for (int k = 0; k < 4; ++k)
        m.crop[k] = std::clamp(c.attribute(names[k]).as_float(0) / 100000.f, -1.f, 1.f);
    }
  }
  m.player = std::move(player);
  media_.push_back(std::move(m));
  return media_.back().player.get();
}

void Show::TakeMedia() {
  if (!cur_ || cur_->index == kEnd) return;
  std::vector<MediaEvent> events = cur_->animator.TakeMediaEvents();
  if (capture_ || bench_) return;  // checks and benches play nothing
  for (const MediaEvent& e : events) {
    MediaPlayer* p = nullptr;
    for (Media& m : media_)
      if (m.spid == e.spid) p = m.player.get();
    bool noted = false;
    for (const MediaNote& n : mediaNotes_) noted = noted || n.spid == e.spid;
    if (!p && !noted &&
        (e.command == MediaCommand::Play || e.command == MediaCommand::TogglePause || e.command == MediaCommand::Resume))
      p = OpenMedia(e.spid);
    if (!p) continue;
    switch (e.command) {
      case MediaCommand::Play: p->Play(e.fromMs); break;
      case MediaCommand::TogglePause: p->TogglePause(); break;
      case MediaCommand::Pause: p->Pause(); break;
      case MediaCommand::Resume: p->Resume(); break;
      case MediaCommand::Stop: p->Stop(); break;
      default: break;
    }
  }
}

void Show::ReleaseMedia() {
  for (const Media& m : media_) {
    if (opts_.mediaReport.empty() || !m.player->IsVideo()) continue;
    const MediaPlayer::Stats s = m.player->VideoStats();
    char line[384];
    snprintf(line, sizeof(line),
             "%s\n    {\"slide\": %zu, \"spid\": %u, \"frames\": %llu, \"skipped\": %llu, \"frameMs\": %.3f, "
             "\"coveredMs\": %.1f, \"paused\": %s, \"ended\": %s, \"error\": %d, \"hresult\": \"0x%08lX\"}",
             mediaReport_.empty() ? "" : ",", cur_ ? cur_->index + 1 : 0, m.spid,
             static_cast<unsigned long long>(s.frames), static_cast<unsigned long long>(s.skipped), s.frameMs,
             s.coveredMs, s.paused ? "true" : "false", s.ended ? "true" : "false", s.error,
             static_cast<unsigned long>(s.extended));
    mediaReport_ += line;
  }
  media_.clear();
  mediaNotes_.clear();
}

void Show::NoteMedia(uint32_t spid, size_t item, std::wstring text) {
  for (const MediaNote& n : mediaNotes_)
    if (n.spid == spid) return;
  mediaNotes_.push_back({spid, item, std::move(text)});
}

bool Show::VideoPlaying() const {
  for (const Media& m : media_)
    if (m.player->IsVideo() && m.player->Playing()) return true;
  return false;
}

void Show::VideoFrames() {
  for (Media& m : media_)
    if (m.player->IsVideo()) m.frame = m.player->Frame(dc_.Get(), nullptr);
}

void Show::DrawVideos() {
  if (!cur_ || cur_->index == kEnd || (media_.empty() && mediaNotes_.empty())) return;
  const std::vector<ItemInfo>& items = renderer_.Items(cur_->index);
  const M toPx = SlideToPixels();
  // Why a media didn't play: a line on a dark band at the foot of its shape (below a small one).
  for (const MediaNote& n : mediaNotes_) {
    if (n.item >= items.size() || !noteFormat_) continue;
    const draw::Rect& b = items[n.item].bounds;
    const D2D1_POINT_2F a = toPx.TransformPoint({b.left, b.top}), c = toPx.TransformPoint({b.right, b.bottom});
    const float pad = noteFormat_->GetFontSize() * 0.4f;
    const float w = std::min(slideW_, std::max(c.x - a.x, noteFormat_->GetFontSize() * 22));
    ComPtr<IDWriteTextLayout> layout;
    DWRITE_TEXT_METRICS tm{};
    if (FAILED(gfx::DWrite()->CreateTextLayout(n.text.c_str(), static_cast<UINT32>(n.text.size()), noteFormat_.Get(),
                                               w - 2 * pad, slideH_, &layout)) ||
        FAILED(layout->GetMetrics(&tm)))
      continue;
    const float h = tm.height + 2 * pad;
    const float left = std::clamp((a.x + c.x - w) / 2, 0.f, std::max(0.f, slideW_ - w));
    float top = c.y - a.y >= 3 * h ? c.y - h : (c.y + h <= slideH_ ? c.y : a.y - h);
    top = std::clamp(top, 0.f, std::max(0.f, slideH_ - h));
    ComPtr<ID2D1SolidColorBrush> band, ink;
    dc_->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.75f), &band);
    dc_->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1), &ink);
    if (!band || !ink) continue;
    dc_->FillRectangle(D2D1::RectF(left, top, left + w, top + h), band.Get());
    dc_->DrawTextLayout({left + pad, top + pad}, layout.Get(), ink.Get());
  }
  for (const Media& m : media_) {
    if (!m.frame || m.item >= items.size()) continue;
    const bool shown = m.player->Shown();
    if (m.hideWhenStopped) {
      if (!shown) continue;  // its poster is hidden too
    } else if (m.item < cur_->targets.size()) {  // hidden by its animations: not shown
      const int whole = cur_->targets[m.item].whole;
      if (whole >= 0 && !cur_->animator.PoseOf(cur_->sprites[static_cast<size_t>(whole)].key).visible) continue;
    }
    const draw::Rect& b = items[m.item].bounds;
    D2D1_POINT_2F a = toPx.TransformPoint({b.left, b.top}), c = toPx.TransformPoint({b.right, b.bottom});
    const D2D1_SIZE_U px = m.frame->GetPixelSize();
    if (m.fullScreen && shown && px.width && px.height) {
      // The whole slide, black around the video's shape.
      ComPtr<ID2D1SolidColorBrush> black;
      dc_->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &black);
      if (black) dc_->FillRectangle(D2D1::RectF(0, 0, slideW_, slideH_), black.Get());
      const float s = std::min(slideW_ / px.width, slideH_ / px.height);
      a = {(slideW_ - px.width * s) / 2, (slideH_ - px.height * s) / 2};
      c = {a.x + px.width * s, a.y + px.height * s};
    }
    const float l = std::max(0.f, m.crop[0]), t = std::max(0.f, m.crop[1]);
    const float r = std::max(l + 0.01f, 1 - std::max(0.f, m.crop[2])), bt = std::max(t + 0.01f, 1 - std::max(0.f, m.crop[3]));
    const D2D1_RECT_F src = D2D1::RectF(l * px.width, t * px.height, r * px.width, bt * px.height);
    dc_->DrawBitmap(m.frame, D2D1::RectF(a.x, a.y, c.x, c.y), 1.f, D2D1_INTERPOLATION_MODE_LINEAR, &src);
  }
}

void Show::UpdatePresenter() {
  if (!presenter_ || !cur_) return;
  PresenterState s;
  s.slide = cur_->index;
  s.current = cur_->index == kEnd ? nullptr : cur_->frame.Get();  // the end's frame is black: the view says so
  s.blank = blank_;
  if (cur_->index != kEnd && cur_->animator.HasNextStep()) {
    // Clicks left on this slide: the preview is the slide as the next click leaves it.
    const int step = cur_->animator.StepIndex() + 1;
    if (!built_ || built_->index != cur_->index || builtStep_ != step) {
      built_ = MakeScene(cur_->index, false, Now(), step);
      builtStep_ = step;
    }
    s.next = built_ ? built_->frame.Get() : nullptr;
    s.stepsLeft = std::max(1, static_cast<int>(cur_->timing.main.steps.size()) - cur_->animator.StepIndex());
  } else {
    const size_t n = cur_->index == kEnd ? kEnd : NextIndex(cur_->index);
    s.next = n != kEnd && next_ && next_->index == n ? next_->frame.Get() : nullptr;
    if (n != kEnd && !s.next) return;  // the next slide is still being made: kMsgPrerender updates again
  }
  presenter_->Update(s);
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

bool Show::Save(const std::wstring& name, ID2D1Bitmap1* from, UINT w, UINT h) {
  if (!from) {
    from = target_.Get();
    w = w_;
    h = h_;
  }
  if (!from || !dc_) return false;
  D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
      D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f);
  ComPtr<ID2D1Bitmap1> cpu;
  D2D1_MAPPED_RECT mapped{};
  if (FAILED(dc_->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0, &props, &cpu)) ||
      FAILED(cpu->CopyFromBitmap(nullptr, from, nullptr)) || FAILED(cpu->Map(D2D1_MAP_OPTIONS_READ, &mapped)))
    return false;
  ComPtr<IWICBitmap> image;
  HRESULT hr = gfx::WIC()->CreateBitmapFromMemory(w, h, GUID_WICPixelFormat32bppPBGRA, mapped.pitch,
                                                  mapped.pitch * h, mapped.bits, &image);
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

bool Show::SavePresenter(const std::wstring& name) {
  if (!presenter_ || !cur_) return false;
  // The next slide as kMsgPrerender makes it in the show (a capture has no message loop).
  const size_t n = cur_->index == kEnd ? kEnd : NextIndex(cur_->index);
  if (n != kEnd && (!next_ || next_->index != n)) next_ = MakeScene(n, false, Now());
  UpdatePresenter();
  constexpr UINT w = 1600, h = 900;
  ComPtr<ID2D1Bitmap1> b = NewBitmap(w, h);
  presenter_->Prepare(w, h);
  if (!BeginOffscreen(b.Get(), false)) return false;
  presenter_->Draw(dc_.Get(), w, h);
  EndOffscreen();
  return Save(name, b.Get(), w, h);
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
  // With --show-presenter, the presenter view once each slide or step has settled.
  auto savePresenter = [&](const wchar_t* fmt, size_t slide, int k) {
    if (!presenter_) return;
    wchar_t name[64];
    swprintf(name, 64, fmt, slide + 1, k);
    if (SavePresenter(name)) ++saved;
  };
  // The middle of whatever runs now, and its end.
  auto playOut = [&](size_t slide, const wchar_t* mid, const wchar_t* end, int k, double length) {
    double t0 = clock_;
    if (length > 0) {
      Step(t0 + length / 2);
      save(mid, slide, k);
    }
    for (int guard = 0; guard < 1000 && Moving(); ++guard) Step(clock_ + 500);
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
    savePresenter(L"p%02zu-1-start", slide, 0);
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
      savePresenter(L"p%02zu-3-step%02d", slide, k);
    }
    Next(false);
  }
  if (cur_ && cur_->index == kEnd) {
    Draw(clock_);
    save(L"s%02zu-end", deck_.SlideCount(), 0);
    savePresenter(L"p%02zu-end", deck_.SlideCount(), 0);
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
    self->presenter_.reset();
    self->ReleaseMedia();  // the sound stops with the show
    if (!self->opts_.mediaReport.empty()) {
      FILE* f = nullptr;
      if (_wfopen_s(&f, self->opts_.mediaReport.c_str(), L"wb") == 0 && f) {
        fprintf(f, "{\n  \"refreshHz\": %.2f,\n  \"videos\": [%s\n  ]\n}\n", self->refreshHz_,
                self->mediaReport_.c_str());
        fclose(f);
      }
    }
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
        if (w == VK_ESCAPE) AbortBench("Esc");
        else if (w == VK_F4 && GetKeyState(VK_MENU) < 0) AbortBench("closed");
        return 0;  // other keys change nothing
      case WM_CLOSE: AbortBench("closed"); return 0;
      case WM_LBUTTONDOWN:
      case WM_LBUTTONUP:
      case WM_RBUTTONDOWN:
      case WM_RBUTTONUP:
      case WM_MBUTTONDOWN:
      case WM_XBUTTONDOWN:
      case WM_MOUSEWHEEL:
      case WM_MOUSEHWHEEL: return 0;  // clicks and the wheel change nothing
      case WM_MOUSEMOVE: ShowCursorNow(); return 0;  // the cursor shows; no mouse-over actions
      case WM_ACTIVATE: {
        DWORD pid = 0;
        if (l) GetWindowThreadProcessId(reinterpret_cast<HWND>(l), &pid);
        // Not once the show is over (its end slide) or going: then the focus change is its own.
        if (LOWORD(w) == WA_INACTIVE && pid != GetCurrentProcessId() && !closing_ && cur_ && cur_->index != kEnd) {
          AbortBench("another window took the focus");
          return 0;
        }
        break;
      }
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
      } else if (w == kTimerAdvance && !Moving()) {  // a playing video doesn't hold the slide
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
        UpdatePresenter();  // the next slide is ready now
      }
      return 0;
    case kMsgStop: Close(); return 0;
    case WM_CLOSE: Close(); return 0;
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
    // A frame starts when the display takes one from the queue: a start more than kLateStart
    // refreshes after the previous one skipped a refresh slot.
    double interval = start - lastFrame_;
    int missed = interval > kLateStart * periodMs_ ? std::max(1, static_cast<int>(std::lround(interval / periodMs_)) - 1) : 0;
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
  if (inTransition) TrackVblank(true, start, waitMs);
  else lastVblank_ = -1;
}

void Show::TrackVblank(bool count, double start, double waited) {
  {
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
      int missed = 0;
      if (count && lastVblank_ >= 0 && shown > lastVblank_ + 1) {
        missed = static_cast<int>(shown - lastVblank_ - 1);
        vblankMissed_ += missed;
        if (!benchTransitions_.empty()) benchTransitions_.back().vblankMissed += missed;
      }
      lastVblank_ = shown;
      if (benchTransitions_.size() == 1 && frameLog_.size() < 600 && swap_) {
        FrameLog f{};
        f.start = start;
        f.wait = waited;
        f.draw = drawMs_;
        f.present = presentMs_;
        f.phase = static_cast<double>((static_cast<UINT64>(qpc.QuadPart) - vblank0_) % vblankPeriod_) / vblankPeriod_;
        f.vblank = index;
        f.shown = shown;
        f.missed = missed;
        f.warm = !count;
        f.mode = -1;
        swap_->GetLastPresentCount(&f.lastPresent);
        LARGE_INTEGER sync{};
        ComPtr<IDXGISwapChainMedia> media;
        if (SUCCEEDED(swap_.As(&media))) {
          DXGI_FRAME_STATISTICS_MEDIA s{};
          f.statsHr = media->GetFrameStatisticsMedia(&s);
          if (SUCCEEDED(f.statsHr)) {
            f.presentCount = s.PresentCount;
            f.presentRefresh = s.PresentRefreshCount;
            f.syncRefresh = s.SyncRefreshCount;
            sync = s.SyncQPCTime;
            f.mode = static_cast<int>(s.CompositionMode);
          }
        } else {
          DXGI_FRAME_STATISTICS s{};
          f.statsHr = swap_->GetFrameStatistics(&s);
          if (SUCCEEDED(f.statsHr)) {
            f.presentCount = s.PresentCount;
            f.presentRefresh = s.PresentRefreshCount;
            f.syncRefresh = s.SyncRefreshCount;
            sync = s.SyncQPCTime;
          }
        }
        f.syncVblank = sync.QuadPart ? std::llround(static_cast<double>(sync.QuadPart - static_cast<long long>(vblank0_)) /
                                                    static_cast<double>(vblankPeriod_))
                                     : LLONG_MIN;
        frameLog_.push_back(f);
      }
    }
  }
}

void Show::AbortBench(const char* why) {
  benchAborted_ = why;
  Close();  // writes the report and deletes the show
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
           "  \"lateStartThresholdMs\": %.2f,\n  \"warmupFrames\": %d,\n"
           "  \"waitMsMax\": %.2f,\n  \"drawMsMax\": %.2f,\n"
           "  \"presentMsMax\": %.2f,\n"
           "  \"worstIntervalMs\": %.2f,\n  \"animationFrames\": %d,\n  \"animationDropped\": %d,\n"
           "  \"cpuMsMedian\": %.3f,\n  \"cpuMs99\": %.3f,\n  \"cpuMsMax\": %.3f,\n  \"warp\": %s,\n  \"pass\": %s,\n"
           "  \"aborted\": %s,\n  \"abortReason\": \"%s\",\n",
           refreshHz_, w_, h_, deck_.SlideCount(), benchTransitions_.size(), frames, vblankMissed_, dropped,
           kLateStart * periodMs_, warmupFrames_, waitMax_, drawMax_, presentMax_, worst, animationFrames_, animationDropped_, pct(0.5),
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
  json += "  ],\n";
  // Times in ms from the first frame; vblank, shown and syncVblank: refreshes from the first
  // frame's (syncVblank null: DXGI gave no time). pc/prc/src: DXGI's PresentCount,
  // PresentRefreshCount and SyncRefreshCount; last: GetLastPresentCount.
  json += "  \"firstTransitionFrames\": [\n";
  for (size_t i = 0; i < frameLog_.size(); ++i) {
    const FrameLog& f = frameLog_[i];
    char sync[24] = "null";
    if (f.syncVblank != LLONG_MIN) snprintf(sync, sizeof(sync), "%lld", f.syncVblank);
    snprintf(buf, sizeof(buf),
             "    {\"t\": %.3f, \"warm\": %s, \"wait\": %.3f, \"draw\": %.3f, \"present\": %.3f, \"vblank\": %lld, "
             "\"phase\": %.2f, \"shown\": %lld, \"missed\": %d, \"last\": %u, \"hr\": \"0x%08lx\", \"mode\": %d, "
             "\"pc\": %u, \"prc\": %u, \"src\": %u, \"syncVblank\": %s}%s\n",
             f.start - frameLog_[0].start, f.warm ? "true" : "false", f.wait, f.draw, f.present, f.vblank, f.phase,
             f.shown, f.missed, f.lastPresent, static_cast<unsigned long>(f.statsHr), f.mode, f.presentCount,
             f.presentRefresh, f.syncRefresh, sync, i + 1 < frameLog_.size() ? "," : "");
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

int RunStandalone(const std::wstring& path, const std::wstring& benchReport, const std::wstring& captureDir, bool windowed,
                  bool presenter, const std::wstring& mediaReport) {
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
  o.windowed = windowed;
  o.presenter = presenter;
  o.mediaReport = mediaReport;
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
  if (g_benchAborted) return 3;  // stopped early (Esc, closed, the focus taken): no result
  return benchReport.empty() || g_benchPassed ? 0 : 2;
}

}  // namespace fw::slide::show
