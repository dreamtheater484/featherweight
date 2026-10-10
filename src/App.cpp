#include "App.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <imm.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>

#include "Commands.h"
#include "FileAssoc.h"
#include "Icons.h"
#include "Print.h"
#include "Theme.h"
#include "Util.h"
#include "Version.h"
#include "core/Strings.h"
#include "resource.h"

#pragma comment(lib, "imm32.lib")

using namespace gfx;

namespace {

constexpr UINT_PTR kTimerTooltip = 1;
constexpr UINT_PTR kTimerCaret = 2;
constexpr UINT_PTR kTimerModal = 3;
constexpr UINT_PTR kTimerCursorHide = 4;
constexpr float kTitleH = 40;
constexpr float kToolbarH = 48;

std::wstring ZoomLabel(DocumentView* v) {
  if (!v || !v->Ready()) return L"";
  return fw::FormatPercent(v->Zoom());
}

void AppendItem(HMENU m, UINT id, const std::wstring& text, bool enabled = true, bool checked = false,
                bool radio = false) {
  MENUITEMINFOW mi{sizeof(mi)};
  mi.fMask = MIIM_ID | MIIM_STRING | MIIM_STATE | MIIM_FTYPE;
  mi.fType = MFT_STRING | (radio ? MFT_RADIOCHECK : 0);
  mi.fState = (enabled ? MFS_ENABLED : MFS_DISABLED) | (checked ? MFS_CHECKED : 0);
  mi.wID = id;
  mi.dwTypeData = const_cast<wchar_t*>(text.c_str());
  InsertMenuItemW(m, GetMenuItemCount(m), TRUE, &mi);
}

void AppendSeparator(HMENU m) { AppendMenuW(m, MF_SEPARATOR, 0, nullptr); }

void AppendSubmenu(HMENU m, HMENU sub, const std::wstring& text, bool enabled = true) {
  AppendMenuW(m, MF_POPUP | (enabled ? 0 : MF_GRAYED), reinterpret_cast<UINT_PTR>(sub), text.c_str());
}

}  // namespace

App::App() = default;

App::~App() {
  if (dropTarget_) dropTarget_->Release();
}

// ---------------------------------------------------------------------------
// Startup and message loop

int App::Run(const AppOptions& opts) {
  if (!gfx::InitFactories()) {
    MessageBoxW(nullptr, L"Featherweight couldn’t start its graphics engine (Direct2D).", L"Featherweight",
                MB_ICONERROR);
    return 1;
  }
  snapshot_ = !opts.snapshotPath.empty();
  settings_.Load();
  ThemeSetting themeSetting = settings_.theme;
  if (snapshot_) {
    settings_ = Settings();  // snapshots never use or change real settings
    if (opts.snapshotTheme == L"dark") themeSetting = ThemeSetting::Dark;
    else if (opts.snapshotTheme == L"light") themeSetting = ThemeSetting::Light;
    settings_.invert = opts.snapshotInvert;
  }
  theme::Init(themeSetting);
  if (!snapshot_) Updater::CleanupOldFiles();

  if (!CreateMainWindow(opts)) return 1;
  if (snapshot_) return RunSnapshot(opts);

  OpenFiles(opts.files);
  StartupNotices();
  Layout();
  int show = settings_.maximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
  if (opts.showCommand == SW_SHOWMINIMIZED || opts.showCommand == SW_MINIMIZE ||
      opts.showCommand == SW_SHOWMINNOACTIVE)
    show = opts.showCommand;
  ShowWindow(hwnd_, show);
  UpdateWindow(hwnd_);
  MaybeCheckForUpdates(false);
  MessageLoop();
  return 0;
}

bool App::CreateMainWindow(const AppOptions& opts) {
  WNDCLASSEXW wc{sizeof(wc)};
  wc.style = CS_DBLCLKS;
  wc.lpfnWndProc = WndProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(IDI_APP));
  wc.hIconSm = static_cast<HICON>(LoadImageW(wc.hInstance, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                             GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.lpszClassName = kWindowClass;
  RegisterClassExW(&wc);

  // Window position: last used, if it is still on a screen; otherwise a comfortable default.
  RECT r{};
  bool havePos = false;
  if (settings_.hasWindowRect && !snapshot_) {
    r = settings_.windowRect;
    havePos = MonitorFromRect(&r, MONITOR_DEFAULTTONULL) != nullptr;
  }
  if (!havePos) {
    HMONITOR mon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    int ww = mi.rcWork.right - mi.rcWork.left, wh = mi.rcWork.bottom - mi.rcWork.top;
    int h = static_cast<int>(wh * 0.86), w = std::min(ww - 80, static_cast<int>(h * 1.05));
    w = std::max(w, std::min(ww, 900));
    r = RECT{mi.rcWork.left + (ww - w) / 2, mi.rcWork.top + (wh - h) / 2, 0, 0};
    r.right = r.left + w;
    r.bottom = r.top + h;
  }
  if (snapshot_) r = RECT{0, 0, opts.snapshotW, opts.snapshotH};

  hwnd_ = CreateWindowExW(0, kWindowClass, L"Featherweight", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, r.left, r.top,
                          r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, this);
  if (!hwnd_) return false;
  ui_ = snapshot_ ? opts.snapshotScale : util::DpiScale(hwnd_);
  theme::ApplyToWindow(hwnd_);
  // Make the frame change take effect (custom title bar).
  SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
  if (!snapshot_) {
    tooltip_.Create(hwnd_);
    dropTarget_ = new DropTarget(this);
    RegisterDragDrop(hwnd_, dropTarget_);
  }

  notes_.Create(hwnd_);
  notes_.onAction = [this](NoteEditor::Action a) { FinishNote(a); };

  // Search box: results update as you type.
  TextBox& sb = search_.Box();
  sb.onChange = [this] {
    if (DocumentView* v = Active()) v->StartSearch(search_.Box().Text(), search_.MatchCase());
  };
  sb.onEnter = [this](bool shift) { Execute(shift ? CmdFindPrev : CmdFindNext); };
  sb.onEscape = [this] { CloseSearch(); };

  // Page box: type a number and press Enter; Up/Down step through pages.
  TextBox& pb = toolbar_.PageBox();
  pb.onEnter = [this](bool) {
    DocumentView* v = Active();
    int page = 0;
    if (v && fw::ParseInt(toolbar_.PageBox().Text(), page)) v->GoToPage(std::clamp(page, 1, v->PageCount()) - 1);
    FocusBox(nullptr);
  };
  pb.onEscape = [this] { FocusBox(nullptr); };
  pb.onKey = [this](UINT vk, bool) {
    DocumentView* v = Active();
    if (!v || (vk != VK_UP && vk != VK_DOWN)) return false;
    vk == VK_UP ? v->PrevPage() : v->NextPage();
    toolbar_.PageBox().SetText(std::to_wstring(v->CurrentPage() + 1), true);
    return true;
  };
  return true;
}

void App::MessageLoop() {
  MSG msg;
  for (;;) {
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) return;
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    if (Idle()) continue;
    DWORD timeout = INFINITE;
    if (anim::Running()) {
      double next = lastFrame_ + 15 - anim::Now();
      timeout = next > 0 ? static_cast<DWORD>(next) : 0;
    }
    double hideIn = banner_.MsUntilHide();
    if (hideIn >= 0) timeout = std::min<DWORD>(timeout, static_cast<DWORD>(hideIn) + 5);
    MsgWaitForMultipleObjectsEx(0, nullptr, timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
  }
}

// Does a slice of background work. Returns true if more work is waiting.
bool App::Idle() {
  if (!hwnd_) return false;
  bool more = false;
  if (anim::Running() && anim::Now() - lastFrame_ >= 14) InvalidateRect(hwnd_, nullptr, FALSE);
  if (banner_.Tick()) InvalidateRect(hwnd_, nullptr, FALSE);
  DocumentView* v = Active();
  if (v && v->HasIdleWork() && (dc_ || EnsureTarget())) {
    if (v->IdleWork(dc_.Get(), anim::Now() + 8)) InvalidateRect(hwnd_, nullptr, FALSE);
    more = v->HasIdleWork();
  }
  return more;
}

// ---------------------------------------------------------------------------
// Rendering

bool App::EnsureTarget() {
  if (dc_) return true;
  if (snapshot_) return false;
  RECT rc;
  GetClientRect(hwnd_, &rc);
  D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
      D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.f,
      96.f);
  D2D1_HWND_RENDER_TARGET_PROPERTIES hp = D2D1::HwndRenderTargetProperties(
      hwnd_, D2D1::SizeU(std::max(1L, rc.right), std::max(1L, rc.bottom)), D2D1_PRESENT_OPTIONS_IMMEDIATELY);
  if (FAILED(D2D()->CreateHwndRenderTarget(props, hp, &target_))) return false;
  target_.As(&dc_);
  if (!dc_) {
    target_.Reset();
    return false;
  }
  return true;
}

void App::DiscardDeviceResources() {
  for (auto& t : tabs_) t->ReleaseDeviceResources();
  dc_.Reset();
  target_.Reset();
}

void App::Paint() {
  if (!EnsureTarget()) return;
  RECT rc;
  GetClientRect(hwnd_, &rc);
  D2D1_SIZE_U size = target_->GetPixelSize();
  if (size.width != static_cast<UINT32>(rc.right) || size.height != static_cast<UINT32>(rc.bottom))
    target_->Resize(D2D1::SizeU(std::max(1L, rc.right), std::max(1L, rc.bottom)));
  target_->BeginDraw();
  RenderFrame(dc_.Get());
  HRESULT hr = target_->EndDraw();
  lastFrame_ = anim::Now();
  notes_.RepaintEdit();
  if (hr == D2DERR_RECREATE_TARGET) {
    DiscardDeviceResources();
    InvalidateRect(hwnd_, nullptr, FALSE);
  }
}

int App::TitleHeight() const { return (fullscreen_ || presentation_) ? 0 : static_cast<int>(std::lround(kTitleH * ui_)); }

int App::ToolbarHeight() const {
  if (presentation_) return 0;
  return static_cast<int>(std::lround(kToolbarH * ui_));
}

RECT App::DocRect() const {
  RECT rc;
  GetClientRect(hwnd_, &rc);
  int top = TitleHeight() + (fullscreen_ || presentation_ ? 0 : ToolbarHeight());
  return RECT{0, top, rc.right, rc.bottom};
}

ToolbarState App::CurrentToolbarState() {
  ToolbarState s;
  DocumentView* v = Active();
  s.hasDoc = v != nullptr;
  s.ready = v && v->Ready();
  s.inverted = settings_.invert;
  s.searchOpen = search_.IsOpen();
  s.fullscreen = fullscreen_;
  if (s.ready) {
    s.modified = v->Modified();
    s.canPrint = v->CanPrint();
    s.page = v->CurrentPage();
    s.pageCount = v->PageCount();
    s.zoomText = ZoomLabel(v);
    s.viewMode = v->ViewMode();
  }
  return s;
}

void App::Layout() {
  if (!hwnd_) return;
  RECT rc;
  GetClientRect(hwnd_, &rc);
  const int titleH = TitleHeight();
  const int toolH = ToolbarHeight();
  title_.Layout(RECT{0, 0, rc.right, titleH}, ui_, static_cast<int>(tabs_.size()));
  int toolTop = titleH;
  if (fullscreen_) toolTop = static_cast<int>(std::lround((toolbarReveal_.V() - 1.0f) * toolH));
  toolbar_.Layout(RECT{0, toolTop, rc.right, toolTop + toolH}, ui_, CurrentToolbarState());
  RECT doc = DocRect();
  search_.Layout(doc, ui_);
  banner_.Layout(doc, ui_);
  empty_.Layout(doc, ui_, settings_.recent);
  notes_.Layout(doc, ui_);
  if (DocumentView* v = Active()) v->SetViewport(doc, ui_);
}

std::wstring App::SearchStatus() {
  DocumentView* v = Active();
  if (!v || search_.Box().Text().empty()) return L"";
  int n = v->SearchCount();
  if (n == 0) return v->SearchDone() ? L"No results" : L"Searching…";
  int cur = v->SearchCurrent();
  return std::to_wstring(cur + 1) + L" of " + std::to_wstring(n) + (v->SearchDone() ? L"" : L"+");
}

void App::RenderFrame(ID2D1DeviceContext* dc) {
  const Palette& p = theme::P();
  canvas_.Begin(dc);
  dc->SetTransform(D2D1::IdentityMatrix());
  dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_DEFAULT);
  Layout();
  RECT rc;
  GetClientRect(hwnd_, &rc);
  RECT doc = DocRect();
  bool caretOn = static_cast<int>((anim::Now() - caretStart_) / 530) % 2 == 0;

  DocumentView* v = Active();
  if (v) {
    v->Draw(canvas_, dc);
  } else {
    canvas_.Fill(RectF(doc), p.docBg);
    empty_.Draw(canvas_, settings_.recent);
  }
  if (search_.Visible()) {
    bool none = v && !search_.Box().Text().empty() && v->SearchDone() && v->SearchCount() == 0;
    search_.Draw(canvas_, SearchStatus(), caretOn && focusBox_ == &search_.Box(), none);
  }
  banner_.Draw(canvas_);
  notes_.Draw(canvas_);
  if (snapshot_) DrawChildControlsForSnapshot(dc);

  // Drag-and-drop overlay.
  float dv = dropAnim_.V();
  if (dv > 0.01f) {
    D2D1_RECT_F all = RectF(rc);
    canvas_.Fill(all, Alpha(p.dark ? Rgb(0x000000) : Rgb(0xFFFFFF), 0.55f * dv));
    D2D1_RECT_F inner = Inflate(RectF(doc), -16 * ui_, -16 * ui_);
    canvas_.FillRound(inner, 12 * ui_, Alpha(p.accent, 0.10f * dv));
    ComPtr<ID2D1StrokeStyle> dash;
    D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties();
    sp.dashStyle = D2D1_DASH_STYLE_DASH;
    D2D()->CreateStrokeStyle(sp, nullptr, 0, &dash);
    dc->DrawRoundedRectangle(D2D1::RoundedRect(inner, 12 * ui_, 12 * ui_), canvas_.Brush(Alpha(p.accent, dv)),
                             2 * ui_, dash.Get());
    float cy = (inner.top + inner.bottom) / 2;
    canvas_.Icon(icon::Download, RectF(inner.left, cy - 60 * ui_, inner.right, cy - 12 * ui_), Alpha(p.accentText, dv),
                 32 * ui_);
    canvas_.Text(L"Drop to open", RectF(inner.left, cy, inner.right, cy + 32 * ui_), Alpha(p.text, dv), 20 * ui_,
                 DWRITE_FONT_WEIGHT_SEMI_BOLD, Align::Center);
  }

  // Chrome on top.
  if (!presentation_) {
    if (!fullscreen_ || toolbarReveal_.V() > 0.01f) {
      ToolbarState ts = CurrentToolbarState();
      toolbar_.Draw(canvas_, ts, caretOn && focusBox_ == &toolbar_.PageBox());
    }
    if (!fullscreen_) {
      std::vector<TabInfo> tabs;
      for (auto& t : tabs_) tabs.push_back({t->Title(), t->Path(), t->Modified()});
      title_.Draw(canvas_, tabs, active_, windowActive_, IsZoomed(hwnd_) != FALSE);
    }
  }
  canvas_.End();
}

// ---------------------------------------------------------------------------
// Window procedure

LRESULT CALLBACK App::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  App* self = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (msg == WM_NCCREATE) {
    self = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    self->hwnd_ = hwnd;
  }
  if (self) return self->Handle(msg, wp, lp);
  return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT App::HitTestNC(int sx, int sy) {
  POINT pt{sx, sy};
  ScreenToClient(hwnd_, &pt);
  RECT rc;
  GetClientRect(hwnd_, &rc);
  if (fullscreen_ || presentation_) return HTCLIENT;
  const bool maximized = IsZoomed(hwnd_) != FALSE;
  const UINT dpi = GetDpiForWindow(hwnd_);
  const int frame = GetSystemMetricsForDpi(SM_CYFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
  if (!maximized && pt.y < frame) {
    if (pt.x < frame * 2) return HTTOPLEFT;
    if (pt.x >= rc.right - frame * 2) return HTTOPRIGHT;
    return HTTOP;
  }
  if (pt.y < TitleHeight()) {
    TitleBar::Hit h = title_.HitTest(pt.x, pt.y);
    switch (h.part) {
      case TitleBar::Part::Caption: return HTCAPTION;
      case TitleBar::Part::Maximize: return HTMAXBUTTON;  // enables Windows 11 Snap Layouts
      default: return HTCLIENT;
    }
  }
  return HTCLIENT;
}

LRESULT App::Handle(UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_NCCALCSIZE: {
      if (!wp) break;
      auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lp);
      if (fullscreen_ || presentation_) return 0;
      const LONG top = params->rgrc[0].top;
      DefWindowProcW(hwnd_, msg, wp, lp);  // keeps the left/right/bottom resize borders
      params->rgrc[0].top = top;           // and gives the title bar area to the client
      if (IsZoomed(hwnd_)) {
        const UINT dpi = GetDpiForWindow(hwnd_);
        params->rgrc[0].top += GetSystemMetricsForDpi(SM_CYFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
      }
      return 0;
    }
    case WM_NCHITTEST: {
      LRESULT def = DefWindowProcW(hwnd_, msg, wp, lp);
      if (def != HTCLIENT) return def;
      return HitTestNC(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
    }
    case WM_NCMOUSEMOVE: {
      if (wp == HTMAXBUTTON) {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd_, &pt);
        if (title_.OnMouseMove(pt.x, pt.y)) InvalidateRect(hwnd_, nullptr, FALSE);
        if (!ncMouseTracked_) {
          TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE | TME_NONCLIENT, hwnd_, 0};
          TrackMouseEvent(&t);
          ncMouseTracked_ = true;
        }
        UpdateTooltip(pt.x, pt.y);
      } else {
        title_.OnMouseLeave();
        InvalidateRect(hwnd_, nullptr, FALSE);
        HideTooltip();
      }
      break;
    }
    case WM_NCMOUSELEAVE:
      ncMouseTracked_ = false;
      title_.OnMouseLeave();
      HideTooltip();
      InvalidateRect(hwnd_, nullptr, FALSE);
      break;
    case WM_NCLBUTTONDOWN:
    case WM_NCLBUTTONDBLCLK:
      if (wp == HTMAXBUTTON) {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd_, &pt);
        title_.OnMouseDown(pt.x, pt.y, 0);
        HideTooltip();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
      }
      HideTooltip();
      break;
    case WM_NCLBUTTONUP:
      if (wp == HTMAXBUTTON) {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd_, &pt);
        TitleBar::Action a = title_.OnMouseUp(pt.x, pt.y, 0);
        if (a.part == TitleBar::Part::Maximize) ShowWindow(hwnd_, IsZoomed(hwnd_) ? SW_RESTORE : SW_MAXIMIZE);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
      }
      break;
    case WM_ACTIVATE:
      windowActive_ = LOWORD(wp) != WA_INACTIVE;
      if (!windowActive_) HideTooltip();
      InvalidateRect(hwnd_, nullptr, FALSE);
      break;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      BeginPaint(hwnd_, &ps);
      Paint();
      EndPaint(hwnd_, &ps);
      return 0;
    }
    case WM_ERASEBKGND:
      return 1;
    case WM_CTLCOLOREDIT:
      if (reinterpret_cast<HWND>(lp) == notes_.Edit())
        return reinterpret_cast<LRESULT>(notes_.OnCtlColor(reinterpret_cast<HDC>(wp)));
      break;
    case WM_SIZE:
      if (wp != SIZE_MINIMIZED) {
        if (target_) target_->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp)));
        Layout();
        InvalidateRect(hwnd_, nullptr, FALSE);
        if (inSizeMove_) {
          Idle();
          Paint();
          ValidateRect(hwnd_, nullptr);
        }
      }
      return 0;
    case WM_ENTERSIZEMOVE:
    case WM_ENTERMENULOOP:
      inSizeMove_ = msg == WM_ENTERSIZEMOVE;
      SetTimer(hwnd_, kTimerModal, 16, nullptr);
      break;
    case WM_EXITSIZEMOVE:
    case WM_EXITMENULOOP:
      inSizeMove_ = false;
      KillTimer(hwnd_, kTimerModal);
      break;
    case WM_GETMINMAXINFO: {
      auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
      mmi->ptMinTrackSize.x = static_cast<LONG>(480 * ui_);
      mmi->ptMinTrackSize.y = static_cast<LONG>(360 * ui_);
      return 0;
    }
    case WM_DPICHANGED: {
      ui_ = HIWORD(wp) / 96.0f;
      auto* r = reinterpret_cast<RECT*>(lp);
      SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                   SWP_NOZORDER | SWP_NOACTIVATE);
      Layout();
      InvalidateRect(hwnd_, nullptr, FALSE);
      return 0;
    }
    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
    case WM_SYSCOLORCHANGE:
      if (theme::Refresh()) {
        theme::ApplyToWindow(hwnd_);
        InvalidateRect(hwnd_, nullptr, FALSE);
      }
      break;
    case WM_MOUSEMOVE: {
      int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
      // (Snapshot windows are never visible, so "mouse left" would fire at once and undo every hover.)
      if (!mouseTracked_ && !snapshot_) {
        TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, hwnd_, 0};
        TrackMouseEvent(&t);
        mouseTracked_ = true;
      }
      if (x == lastMouse_.x && y == lastMouse_.y) return 0;
      lastMouse_ = {x, y};
      lastMouseMove_ = anim::Now();
      OnMouseMove(x, y);
      return 0;
    }
    case WM_MOUSELEAVE:
      mouseTracked_ = false;
      lastMouse_ = {-1, -1};
      OnMouseLeave();
      return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
      OnMouseDown(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), 0, msg == WM_LBUTTONDBLCLK);
      return 0;
    case WM_RBUTTONDOWN:
      OnMouseDown(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), 1, false);
      return 0;
    case WM_MBUTTONDOWN:
      OnMouseDown(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), 2, false);
      return 0;
    case WM_LBUTTONUP:
      OnMouseUp(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), 0);
      return 0;
    case WM_RBUTTONUP: {
      int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
      OnMouseUp(x, y, 1);
      if (RegionAt(x, y) == Region::Doc && Active() && Active()->Ready()) {
        POINT pt{x, y};
        ClientToScreen(hwnd_, &pt);
        ShowContextMenu(pt.x, pt.y);
      }
      return 0;
    }
    case WM_MBUTTONUP:
      OnMouseUp(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), 2);
      return 0;
    case WM_CAPTURECHANGED:
      if (reinterpret_cast<HWND>(lp) != hwnd_ && capture_ != Region::None) capture_ = Region::None;
      break;
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL: {
      HideTooltip();
      POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      ScreenToClient(hwnd_, &pt);
      UINT keys = GET_KEYSTATE_WPARAM(wp);
      UINT mods = (keys & MK_CONTROL ? kModCtrl : 0) | (keys & MK_SHIFT ? kModShift : 0);
      if (DocumentView* v = Active()) {
        v->OnWheel(GET_WHEEL_DELTA_WPARAM(wp), msg == WM_MOUSEHWHEEL, mods, pt.x, pt.y);
        InvalidateRect(hwnd_, nullptr, FALSE);
      }
      return 0;
    }
    case WM_SETCURSOR:
      if (LOWORD(lp) == HTCLIENT && OnSetCursor()) return TRUE;
      break;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
      if (msg == WM_SYSKEYDOWN && wp != VK_F10 && !(wp >= VK_F1 && wp <= VK_F24)) {
        if (wp == VK_LEFT || wp == VK_RIGHT) {  // Alt+Left/Right: previous/next page
          if (DocumentView* v = Active()) wp == VK_LEFT ? v->PrevPage() : v->NextPage();
          return 0;
        }
        break;  // Alt+F4, Alt+Space etc.
      }
      OnKeyDown(static_cast<UINT>(wp));
      return 0;
    case WM_CHAR:
      OnChar(static_cast<wchar_t>(wp));
      return 0;
    case WM_TIMER:
      switch (wp) {
        case kTimerTooltip: {
          KillTimer(hwnd_, kTimerTooltip);
          POINT cur;
          GetCursorPos(&cur);
          POINT client = cur;
          ScreenToClient(hwnd_, &client);
          if (!tipText_.empty() && capture_ == Region::None && windowActive_) {
            RECT anchor{cur.x - 4, cur.y - 4, cur.x + 4, cur.y + static_cast<LONG>(18 * ui_)};
            tooltip_.Show(tipText_, anchor, ui_);
          }
          break;
        }
        case kTimerCaret:
          if (focusBox_) InvalidateRect(hwnd_, nullptr, FALSE);
          break;
        case kTimerModal:
          Idle();
          if (anim::Running()) InvalidateRect(hwnd_, nullptr, FALSE);
          break;
        case kTimerCursorHide:
          if (presentation_ && anim::Now() - lastMouseMove_ > 2000) SetCursor(nullptr);
          break;
      }
      return 0;
    case WM_COPYDATA: {
      auto* cds = reinterpret_cast<COPYDATASTRUCT*>(lp);
      if (cds && cds->dwData == kCopyDataOpen && cds->lpData) {
        std::wstring all(static_cast<const wchar_t*>(cds->lpData), cds->cbData / sizeof(wchar_t));
        std::vector<std::wstring> files;
        for (auto& f : fw::Split(all, L'\n'))
          if (!fw::Trim(f).empty()) files.push_back(fw::Trim(f));
        if (IsIconic(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
        SetForegroundWindow(hwnd_);
        OpenFiles(files);
        return TRUE;
      }
      return FALSE;
    }
    case Updater::kDoneMessage:
      OnUpdaterDone();
      return 0;
    case WM_COMMAND:
      if (HIWORD(wp) == 0 && lp == 0) {
        Execute(LOWORD(wp));
        return 0;
      }
      break;
    case WM_CLOSE: {
      if (notes_.IsOpen()) FinishNote(NoteEditor::kSave);
      for (int i = 0; i < static_cast<int>(tabs_.size()); ++i) {
        if (tabs_[i]->Modified()) {
          ActivateTab(i);
          if (!ConfirmClose(tabs_[i].get())) return 0;
        }
      }
      for (auto& t : tabs_) RememberState(t.get());
      if (fullscreen_ || presentation_) {
        settings_.maximized = savedPlacement_.showCmd == SW_SHOWMAXIMIZED;
        settings_.windowRect = savedPlacement_.rcNormalPosition;
        settings_.hasWindowRect = true;
      } else {
        WINDOWPLACEMENT wpl{sizeof(wpl)};
        GetWindowPlacement(hwnd_, &wpl);
        settings_.maximized = wpl.showCmd == SW_SHOWMAXIMIZED;
        settings_.windowRect = wpl.rcNormalPosition;
        settings_.hasWindowRect = true;
      }
      if (!snapshot_) settings_.Save();
      HideTooltip();
      tooltip_.Destroy();
      tabs_.clear();
      DestroyWindow(hwnd_);
      return 0;
    }
    case WM_DESTROY:
      if (dropTarget_) RevokeDragDrop(hwnd_);
      updater_.WaitForSwap();
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(hwnd_, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// Input

UINT App::Mods() const {
  if (simulatedMods_ >= 0) return static_cast<UINT>(simulatedMods_);
  UINT m = 0;
  if (GetKeyState(VK_SHIFT) < 0) m |= kModShift;
  if (GetKeyState(VK_CONTROL) < 0) m |= kModCtrl;
  if (GetKeyState(VK_MENU) < 0) m |= kModAlt;
  return m;
}

App::Region App::RegionAt(int x, int y) const {
  RECT rc;
  GetClientRect(hwnd_, &rc);
  if (x < 0 || y < 0 || x >= rc.right || y >= rc.bottom) return Region::None;
  const int titleH = TitleHeight();
  if (y < titleH) return Region::Title;
  if (!presentation_) {
    int toolTop = titleH, toolH = ToolbarHeight();
    if (fullscreen_) {
      if (toolbarReveal_.Target() > 0.5f && y < toolH) return Region::Toolbar;
    } else if (y < toolTop + toolH) {
      return Region::Toolbar;
    }
  }
  if (notes_.Hit(x, y)) return Region::Note;
  if (search_.Hit(x, y)) return Region::Search;
  if (banner_.Hit(x, y)) return Region::Banner;
  return Region::Doc;
}

void App::OnMouseDown(int x, int y, int button, bool dbl) {
  HideTooltip();
  Region r = RegionAt(x, y);
  // Clicking anywhere outside the note card keeps what was written and closes it.
  if (notes_.IsOpen() && r != Region::Note) FinishNote(NoteEditor::kSave);
  if (r != Region::Note) SetFocus(hwnd_);
  capture_ = r;
  SetCapture(hwnd_);
  const bool shift = (Mods() & kModShift) != 0;

  // Clicking anywhere else takes focus away from a text box.
  TextBox* clickedBox = nullptr;
  if (r == Region::Toolbar && toolbar_.HitPageBox(x, y)) clickedBox = &toolbar_.PageBox();
  if (r == Region::Search && Contains(search_.Box().rc, (float)x, (float)y)) clickedBox = &search_.Box();
  if (r == Region::Doc && Active() && Active()->PasswordBox() &&
      Contains(Active()->PasswordBox()->rc, (float)x, (float)y))
    clickedBox = Active()->PasswordBox();
  if (focusBox_ != clickedBox) FocusBox(clickedBox);

  switch (r) {
    case Region::Title: {
      title_.OnMouseDown(x, y, button);
      TitleBar::Hit h = title_.HitTest(x, y);
      if (button == 0 && h.part == TitleBar::Part::Tab) ActivateTab(h.index);
      break;
    }
    case Region::Toolbar:
      if (clickedBox) {
        if (dbl) clickedBox->OnDoubleClick();
        else if (!clickedBox->Text().empty() && !shift) clickedBox->SelectAll();
        else clickedBox->OnMouseDown((float)x, (float)y, shift, ui_);
      } else if (button == 0) {
        toolbar_.OnMouseDown(x, y);
      }
      break;
    case Region::Search:
      if (button == 0) search_.OnMouseDown(x, y, shift, dbl);
      break;
    case Region::Banner:
      if (button == 0) banner_.OnMouseDown(x, y);
      break;
    case Region::Note:
      if (button == 0) notes_.OnMouseDown(x, y);
      break;
    case Region::Doc:
      if (DocumentView* v = Active()) {
        v->OnMouseDown(x, y, button, Mods(), dbl);
        if (v->HasFieldFocus() || v->Dragging()) caretStart_ = anim::Now();
      } else if (button == 0) {
        empty_.OnMouseDown(x, y);
      }
      break;
    default: break;
  }
  if (presentation_) lastMouseMove_ = anim::Now();
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::OnMouseMove(int x, int y) {
  if (fullscreen_ && !presentation_) {
    float target = toolbarReveal_.Target();
    if (y <= 2 && target < 0.5f) toolbarReveal_.To(1, 160);
    else if (y > ToolbarHeight() + 24 * ui_ && target > 0.5f && capture_ == Region::None) toolbarReveal_.To(0, 200);
    if (toolbarReveal_.Target() != target) InvalidateRect(hwnd_, nullptr, FALSE);
  }
  if (presentation_) SetCursor(LoadCursor(nullptr, IDC_ARROW));

  if (capture_ != Region::None) {
    switch (capture_) {
      case Region::Doc:
        if (DocumentView* v = Active()) v->OnMouseMove(x, y, Mods());
        break;
      case Region::Toolbar:
        if (focusBox_ == &toolbar_.PageBox() && GetKeyState(VK_LBUTTON) < 0) toolbar_.PageBox().OnMouseDrag((float)x, (float)y, ui_);
        toolbar_.OnMouseMove(x, y);
        break;
      case Region::Search:
        if (focusBox_ == &search_.Box() && GetKeyState(VK_LBUTTON) < 0) search_.OnMouseDrag(x, y);
        search_.OnMouseMove(x, y);
        break;
      case Region::Title: title_.OnMouseMove(x, y); break;
      case Region::Banner: banner_.OnMouseMove(x, y); break;
      case Region::Note: notes_.OnMouseMove(x, y); break;
      default: break;
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
    return;
  }

  Region r = RegionAt(x, y);
  if (r != hover_) {
    switch (hover_) {
      case Region::Title: title_.OnMouseLeave(); break;
      case Region::Toolbar: toolbar_.OnMouseLeave(); break;
      case Region::Search: search_.OnMouseLeave(); break;
      case Region::Banner: banner_.OnMouseLeave(); break;
      case Region::Note: notes_.OnMouseLeave(); break;
      case Region::Doc:
        if (DocumentView* v = Active()) v->OnMouseLeave();
        else empty_.OnMouseLeave();
        break;
      default: break;
    }
    hover_ = r;
    InvalidateRect(hwnd_, nullptr, FALSE);
  }
  bool changed = false;
  switch (r) {
    case Region::Title: changed = title_.OnMouseMove(x, y); break;
    case Region::Toolbar: changed = toolbar_.OnMouseMove(x, y); break;
    case Region::Search: changed = search_.OnMouseMove(x, y); break;
    case Region::Banner: changed = banner_.OnMouseMove(x, y); break;
    case Region::Note: changed = notes_.OnMouseMove(x, y); break;
    case Region::Doc:
      if (DocumentView* v = Active()) v->OnMouseMove(x, y, Mods());
      else changed = empty_.OnMouseMove(x, y);
      break;
    default: break;
  }
  if (changed) InvalidateRect(hwnd_, nullptr, FALSE);
  UpdateTooltip(x, y);
}

void App::OnMouseUp(int x, int y, int button) {
  Region r = capture_;
  capture_ = Region::None;
  if (GetCapture() == hwnd_) ReleaseCapture();
  switch (r) {
    case Region::Title: {
      TitleBar::Action a = title_.OnMouseUp(x, y, button);
      switch (a.part) {
        case TitleBar::Part::Minimize: ShowWindow(hwnd_, SW_MINIMIZE); break;
        case TitleBar::Part::Maximize: ShowWindow(hwnd_, IsZoomed(hwnd_) ? SW_RESTORE : SW_MAXIMIZE); break;
        case TitleBar::Part::Close: PostMessageW(hwnd_, WM_CLOSE, 0, 0); break;
        case TitleBar::Part::NewTab: Execute(CmdOpen); break;
        case TitleBar::Part::TabClose: CloseTab(a.index); break;
        default: break;
      }
      break;
    }
    case Region::Toolbar:
      if (button == 0) {
        int cmd = toolbar_.OnMouseUp(x, y);
        if (cmd) Execute(cmd);
      }
      break;
    case Region::Search:
      if (button == 0) {
        int cmd = search_.OnMouseUp(x, y);
        if (cmd) Execute(cmd);
      }
      break;
    case Region::Banner: {
      int id = banner_.OnMouseUp(x, y);
      if (id > 0) Execute(CmdBannerAction + id);
      break;
    }
    case Region::Note:
      if (button == 0) {
        NoteEditor::Action a = notes_.OnMouseUp(x, y);
        if (a != NoteEditor::kNone) FinishNote(a);
      }
      break;
    case Region::Doc:
      if (DocumentView* v = Active()) {
        v->OnMouseUp(x, y, button, Mods());
      } else if (button == 0) {
        EmptyState::Result res = empty_.OnMouseUp(x, y);
        if (res.kind == EmptyState::Result::Kind::Open) Execute(CmdOpen);
        else if (res.kind == EmptyState::Result::Kind::OpenRecent) Execute(CmdRecent + res.index);
        else if (res.kind == EmptyState::Result::Kind::RemoveRecent && res.index < static_cast<int>(settings_.recent.size())) {
          settings_.RemoveRecent(settings_.recent[res.index].path);
          settings_.Save();
          Layout();
        }
      }
      break;
    default: break;
  }
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::OnMouseLeave() {
  HideTooltip();
  title_.OnMouseLeave();
  toolbar_.OnMouseLeave();
  search_.OnMouseLeave();
  banner_.OnMouseLeave();
  empty_.OnMouseLeave();
  if (DocumentView* v = Active()) v->OnMouseLeave();
  hover_ = Region::None;
  InvalidateRect(hwnd_, nullptr, FALSE);
}

bool App::OnSetCursor() {
  if (presentation_ && anim::Now() - lastMouseMove_ > 2000) {
    SetCursor(nullptr);
    return true;
  }
  POINT pt;
  GetCursorPos(&pt);
  ScreenToClient(hwnd_, &pt);
  static HCURSOR arrow = LoadCursor(nullptr, IDC_ARROW), ibeam = LoadCursor(nullptr, IDC_IBEAM),
                 hand = LoadCursor(nullptr, IDC_HAND);
  HCURSOR c = arrow;
  Region r = capture_ != Region::None ? capture_ : RegionAt(pt.x, pt.y);
  switch (r) {
    case Region::Toolbar:
      if (toolbar_.HitPageBox(pt.x, pt.y)) c = ibeam;
      break;
    case Region::Search:
      if (Contains(search_.Box().rc, (float)pt.x, (float)pt.y)) c = ibeam;
      break;
    case Region::Doc:
      if (DocumentView* v = Active()) c = v->CursorAt(pt.x, pt.y);
      else if (empty_.OverClickable(pt.x, pt.y)) c = hand;
      break;
    default: break;
  }
  SetCursor(c);
  return true;
}

void App::UpdateTooltip(int x, int y) {
  std::wstring text;
  switch (RegionAt(x, y)) {
    case Region::Title: {
      std::vector<TabInfo> tabs;
      for (auto& t : tabs_) tabs.push_back({t->Title(), t->Path(), t->Modified()});
      text = title_.TooltipAt(x, y, tabs);
      break;
    }
    case Region::Toolbar: text = toolbar_.TooltipAt(x, y); break;
    case Region::Search: text = search_.TooltipAt(x, y); break;
    case Region::Note: text = notes_.TooltipAt(x, y); break;
    case Region::Doc:
      if (DocumentView* v = Active()) text = v->TooltipAt(x, y);
      else text = empty_.TooltipAt(x, y, settings_.recent);
      break;
    default: break;
  }
  if (text == tipText_) return;
  bool wasVisible = tooltip_.Visible();
  tooltip_.Hide();
  tipText_ = text;
  if (!text.empty()) SetTimer(hwnd_, kTimerTooltip, wasVisible ? 120 : 650, nullptr);
  else KillTimer(hwnd_, kTimerTooltip);
}

void App::HideTooltip() {
  KillTimer(hwnd_, kTimerTooltip);
  tooltip_.Hide();
  tipText_.clear();
}

void App::FocusBox(TextBox* box) {
  if (focusBox_ == box) return;
  if (focusBox_) {
    focusBox_->Focus(false);
    if (focusBox_ == &toolbar_.PageBox()) Layout();  // restores the current page number
  }
  focusBox_ = box;
  if (box) {
    box->Focus(true);
    caretStart_ = anim::Now();
    SetTimer(hwnd_, kTimerCaret, 265, nullptr);
    if (HIMC imc = ImmGetContext(hwnd_)) {
      D2D1_POINT_2F p = box->CaretPos(ui_);
      COMPOSITIONFORM cf{CFS_POINT, POINT{static_cast<LONG>(p.x), static_cast<LONG>(p.y)}, {}};
      ImmSetCompositionWindow(imc, &cf);
      ImmReleaseContext(hwnd_, imc);
    }
  } else {
    KillTimer(hwnd_, kTimerCaret);
  }
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::OnKeyDown(UINT vk) {
  HideTooltip();
  const UINT mods = Mods();
  const bool ctrl = (mods & kModCtrl) != 0, shift = (mods & kModShift) != 0;
  DocumentView* v = Active();
  caretStart_ = anim::Now();

  // Shortcuts that work everywhere, even while typing in a box or a form field.
  auto global = [&]() -> int {
    if (ctrl) {
      switch (vk) {
        case 'O': return CmdOpen;
        case 'S': return shift ? CmdSaveAs : CmdSave;
        case 'P': return CmdPrint;
        case 'W':
        case VK_F4: return CmdCloseTab;
        case VK_TAB: return shift ? CmdPrevTab : CmdNextTab;
        case VK_NEXT: return CmdNextTab;
        case VK_PRIOR: return CmdPrevTab;
        case 'F': return CmdFind;
        case 'G': return CmdGoToPage;
        case 'I': return CmdToggleInvert;
        case VK_OEM_PLUS:
        case VK_ADD: return shift ? CmdRotateRight : CmdZoomIn;
        case VK_OEM_MINUS:
        case VK_SUBTRACT: return shift ? CmdRotateLeft : CmdZoomOut;
        case '0':
        case VK_NUMPAD0: return CmdZoomAuto;
        case '1': return CmdZoomActual;
        case '2': return CmdZoomFitPage;
        case '3': return CmdZoomFitWidth;
        case '6': return CmdViewSingle;
        case '7': return CmdViewContinuous;
        case '8': return CmdViewFacing;
      }
    }
    switch (vk) {
      case VK_F11: return CmdFullScreen;
      case VK_F5: return CmdPresentation;
      case VK_F3: return shift ? CmdFindPrev : CmdFindNext;
    }
    return 0;
  };

  if (focusBox_) {
    if (int cmd = global(); cmd && !(ctrl && (vk == 'A' || vk == 'C' || vk == 'V' || vk == 'X' || vk == 'Z'))) {
      Execute(cmd);
      return;
    }
    TextBox* box = focusBox_;
    if (box->OnKeyDown(vk, ctrl, shift)) {
      InvalidateRect(hwnd_, nullptr, FALSE);
      return;
    }
    if (vk == VK_TAB && box == &search_.Box()) {
      FocusBox(nullptr);
      return;
    }
    return;
  }

  if (v && v->HasFieldFocus()) {
    int cmd = global();
    if (cmd) {
      Execute(cmd);
      return;
    }
    if (v->OnKeyDown(vk, mods)) {
      InvalidateRect(hwnd_, nullptr, FALSE);
      return;
    }
  }

  if (int cmd = global()) {
    Execute(cmd);
    return;
  }
  if (ctrl && vk == 'C') {
    Execute(CmdCopy);
    return;
  }
  if (ctrl && vk == 'A') {
    Execute(CmdSelectAll);
    return;
  }
  if (vk == VK_ESCAPE) {
    if (dragOver_) return;
    if (presentation_) {
      SetPresentation(false);
      return;
    }
    if (search_.IsOpen()) {
      CloseSearch();
      return;
    }
    if (v && v->OnKeyDown(vk, mods)) {
      InvalidateRect(hwnd_, nullptr, FALSE);
      return;
    }
    if (fullscreen_) ToggleFullScreen();
    if (banner_.IsShowing()) banner_.Hide();
    InvalidateRect(hwnd_, nullptr, FALSE);
    return;
  }
  if (v && v->OnKeyDown(vk, mods)) InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::OnChar(wchar_t ch) {
  if (focusBox_) {
    if (focusBox_->OnChar(ch)) InvalidateRect(hwnd_, nullptr, FALSE);
    return;
  }
  if (DocumentView* v = Active()) {
    if (v->OnChar(ch, Mods())) InvalidateRect(hwnd_, nullptr, FALSE);
  }
}

// ---------------------------------------------------------------------------
// Commands

void App::Execute(int cmd) {
  DocumentView* v = Active();
  const bool ready = v && v->Ready();
  HideTooltip();

  if (cmd >= CmdRecent && cmd < CmdRecent + Settings::kMaxRecent) {
    int i = cmd - CmdRecent;
    if (i < static_cast<int>(settings_.recent.size())) OpenFiles({settings_.recent[i].path});
    return;
  }
  if (cmd >= CmdZoomPreset && cmd < CmdZoomPreset + 50) {
    const auto& presets = fw::ZoomPresets();
    int i = cmd - CmdZoomPreset;
    if (ready && i < static_cast<int>(presets.size())) v->SetZoom(presets[i]);
    InvalidateRect(hwnd_, nullptr, FALSE);
    return;
  }
  if (cmd > CmdBannerAction && cmd < CmdBannerAction + 50) {
    switch (cmd - CmdBannerAction) {
      case kActUpdate:
        manualUpdateCheck_ = true;
        banner_.Show(BannerKind::Info, L"Downloading the update…");
        updater_.Start(hwnd_, true);
        break;
      case kActDownload: util::OpenExternalLink(bannerUpdateUrl_); break;
      case kActSkip:
        settings_.skippedVersion = bannerUpdateVersion_;
        settings_.Save();
        break;
      case kActMakeDefault: Execute(CmdMakeDefault); break;
      case kActSaveAs: SaveActive(true); break;
      default: break;
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
    return;
  }

  switch (cmd) {
    case CmdOpen: OpenDialog(); break;
    case CmdSave: SaveActive(false); break;
    case CmdSaveAs: SaveActive(true); break;
    case CmdPrint: PrintActive(); break;
    case CmdCloseTab:
      if (active_ >= 0) CloseTab(active_);
      break;
    case CmdNextTab:
      if (tabs_.size() > 1) ActivateTab((active_ + 1) % static_cast<int>(tabs_.size()));
      break;
    case CmdPrevTab:
      if (tabs_.size() > 1) ActivateTab((active_ + static_cast<int>(tabs_.size()) - 1) % static_cast<int>(tabs_.size()));
      break;
    case CmdFind: OpenSearch(); break;
    case CmdFindNext:
    case CmdFindPrev:
      if (!search_.IsOpen() || search_.Box().Text().empty()) OpenSearch();
      else if (ready) v->SearchStep(cmd == CmdFindNext);
      break;
    case CmdCloseFind: CloseSearch(); break;
    case CmdSearchCase:
      search_.ToggleMatchCase();
      if (ready) v->StartSearch(search_.Box().Text(), search_.MatchCase());
      break;
    case CmdCopy:
      if (ready) v->Copy();
      break;
    case CmdSelectAll:
      if (ready) v->SelectAll();
      break;
    case CmdSelectAllOnPage:
      if (ready) v->SelectAllOnPage();
      break;
    case CmdGoToPage:
      if (ready && !presentation_) {
        if (fullscreen_) toolbarReveal_.To(1, 160);
        FocusBox(&toolbar_.PageBox());
        toolbar_.PageBox().SelectAll();
      }
      break;
    case CmdNextPage:
      if (ready) v->NextPage();
      break;
    case CmdPrevPage:
      if (ready) v->PrevPage();
      break;
    case CmdFirstPage:
      if (ready) v->GoToPage(0);
      break;
    case CmdLastPage:
      if (ready) v->GoToPage(v->PageCount() - 1);
      break;
    case CmdZoomIn:
      if (ready) v->ZoomStep(true);
      break;
    case CmdZoomOut:
      if (ready) v->ZoomStep(false);
      break;
    case CmdZoomAuto:
      if (ready) v->SetZoomMode(fw::ZoomMode::Automatic);
      break;
    case CmdZoomActual:
      if (ready) v->SetZoomMode(fw::ZoomMode::ActualSize);
      break;
    case CmdZoomFitPage:
      if (ready) v->SetZoomMode(fw::ZoomMode::FitPage);
      break;
    case CmdZoomFitWidth:
      if (ready) v->SetZoomMode(fw::ZoomMode::FitWidth);
      break;
    case CmdZoomMenu:
    case CmdViewMenu:
    case CmdSaveMenu:
    case CmdMoreMenu: ShowMenu(cmd); break;
    case CmdHighlight:
      if (ready) v->HighlightSelection();
      break;
    case CmdAddNote: {
      NoteTarget note;
      if (ready && v->NewNoteAt(contextPt_.x, contextPt_.y, note)) ViewOpenNote(v, note);
      break;
    }
    case CmdOpenNote:
      if (ready && contextAnnot_.kind == AnnotHit::Kind::Note)
        ViewOpenNote(v, NoteTarget{contextPage_, contextAnnot_.index, contextAnnot_.rect.left, contextAnnot_.rect.top});
      break;
    case CmdRemoveAnnot:
      if (ready && contextAnnot_.kind != AnnotHit::Kind::None) v->RemoveAnnot(contextPage_, contextAnnot_.index);
      break;
    case CmdViewSingle:
      if (ready) v->SetViewMode(fw::ViewMode::Single);
      break;
    case CmdViewContinuous:
      if (ready) v->SetViewMode(fw::ViewMode::Continuous);
      break;
    case CmdViewFacing:
      if (ready) v->SetViewMode(fw::ViewMode::Facing);
      break;
    case CmdToggleCover:
      settings_.coverAlone = !settings_.coverAlone;
      for (auto& t : tabs_) t->SettingsChanged();
      break;
    case CmdRotateLeft:
      if (ready) v->Rotate(false);
      break;
    case CmdRotateRight:
      if (ready) v->Rotate(true);
      break;
    case CmdToggleInvert:
      settings_.invert = !settings_.invert;
      banner_.Show(BannerKind::Info, settings_.invert ? L"Night reading on: pages are dark" : L"Night reading off", {},
                   1800);
      break;
    case CmdFullScreen:
      if (presentation_) SetPresentation(false);
      else ToggleFullScreen();
      break;
    case CmdPresentation:
      if (ready) SetPresentation(!presentation_);
      break;
    case CmdThemeSystem: SetTheme(ThemeSetting::System); break;
    case CmdThemeLight: SetTheme(ThemeSetting::Light); break;
    case CmdThemeDark: SetTheme(ThemeSetting::Dark); break;
    case CmdToggleHighlight:
      settings_.highlightFields = !settings_.highlightFields;
      for (auto& t : tabs_) t->SettingsChanged();
      break;
    case CmdToggleAutoUpdate:
      settings_.autoUpdate = !settings_.autoUpdate;
      settings_.Save();
      break;
    case CmdCheckUpdates: MaybeCheckForUpdates(true); break;
    case CmdMakeDefault:
      if (fileassoc::Register(util::ExePath())) {
        fileassoc::OpenDefaultAppsSettings();
        banner_.Show(BannerKind::Info, L"In the Settings window that just opened, choose Featherweight for .pdf files.",
                     {}, 9000);
      } else {
        banner_.Show(BannerKind::Error, L"Featherweight couldn’t register itself as a PDF app.");
      }
      break;
    case CmdAbout: ShowAbout(); break;
    case CmdClearRecent:
      settings_.ClearRecent();
      settings_.Save();
      Layout();
      break;
    case CmdExit: PostMessageW(hwnd_, WM_CLOSE, 0, 0); break;
    default: break;
  }
  InvalidateRect(hwnd_, nullptr, FALSE);
}

int App::TrackMenu(HMENU menu, const D2D1_RECT_F& anchor, bool alignRight) {
  HideTooltip();
  POINT pt{static_cast<LONG>(alignRight ? anchor.right : anchor.left), static_cast<LONG>(anchor.bottom + 2 * ui_)};
  ClientToScreen(hwnd_, &pt);
  theme::ApplyToMenus();
  UINT flags = TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_TOPALIGN | (alignRight ? TPM_RIGHTALIGN : TPM_LEFTALIGN);
  return TrackPopupMenuEx(menu, flags, pt.x, pt.y, hwnd_, nullptr);
}

void App::ShowMenu(int which) {
  DocumentView* v = Active();
  const bool ready = v && v->Ready();
  HMENU m = CreatePopupMenu();
  bool alignRight = false;
  if (which == CmdZoomMenu && ready) {
    auto mode = v->ZoomMode();
    AppendItem(m, CmdZoomAuto, L"Automatic\tCtrl+0", true, mode == fw::ZoomMode::Automatic, true);
    AppendItem(m, CmdZoomFitWidth, L"Fit width\tCtrl+3", true, mode == fw::ZoomMode::FitWidth, true);
    AppendItem(m, CmdZoomFitPage, L"Fit page\tCtrl+2", true, mode == fw::ZoomMode::FitPage, true);
    AppendItem(m, CmdZoomActual, L"Actual size\tCtrl+1", true, mode == fw::ZoomMode::ActualSize, true);
    AppendSeparator(m);
    const auto& presets = fw::ZoomPresets();
    for (int i = 0; i < static_cast<int>(presets.size()); ++i) {
      bool current = mode == fw::ZoomMode::Custom && std::abs(v->Zoom() - presets[i]) < 0.005;
      AppendItem(m, CmdZoomPreset + i, fw::FormatPercent(presets[i]), true, current, true);
    }
  } else if (which == CmdViewMenu && ready) {
    auto mode = v->ViewMode();
    AppendItem(m, CmdViewSingle, L"Single page\tCtrl+6", true, mode == fw::ViewMode::Single, true);
    AppendItem(m, CmdViewContinuous, L"Continuous\tCtrl+7", true, mode == fw::ViewMode::Continuous, true);
    AppendItem(m, CmdViewFacing, L"Two pages\tCtrl+8", true, mode == fw::ViewMode::Facing, true);
    AppendSeparator(m);
    AppendItem(m, CmdToggleCover, L"Show cover page on its own", mode == fw::ViewMode::Facing, settings_.coverAlone);
    AppendSeparator(m);
    AppendItem(m, CmdRotateLeft, L"Rotate counterclockwise\tCtrl+Shift+−");
    AppendItem(m, CmdRotateRight, L"Rotate clockwise\tCtrl+Shift+=");
  } else if (which == CmdSaveMenu && ready) {
    AppendItem(m, CmdSave, L"Save\tCtrl+S", v->Modified());
    AppendItem(m, CmdSaveAs, L"Save as\u2026\tCtrl+Shift+S");
  } else if (which == CmdMoreMenu) {
    alignRight = true;
    AppendItem(m, CmdOpen, L"Open…\tCtrl+O");
    HMENU recent = CreatePopupMenu();
    for (int i = 0; i < static_cast<int>(settings_.recent.size()); ++i) {
      std::wstring label = fw::FileNameOf(settings_.recent[i].path);
      if (i < 9) label = L"&" + std::to_wstring(i + 1) + L"  " + label;
      AppendItem(recent, CmdRecent + i, label);
    }
    if (!settings_.recent.empty()) {
      AppendSeparator(recent);
      AppendItem(recent, CmdClearRecent, L"Clear list");
    }
    AppendSubmenu(m, recent, L"Open recent", !settings_.recent.empty());
    AppendItem(m, CmdSaveAs, L"Save as…\tCtrl+Shift+S", ready);
    AppendItem(m, CmdPrint, L"Print…\tCtrl+P", ready && v->CanPrint());
    AppendSeparator(m);
    AppendItem(m, CmdPresentation, L"Present\tF5", ready);
    AppendItem(m, CmdFullScreen, fullscreen_ ? L"Exit full screen\tF11" : L"Full screen\tF11");
    AppendItem(m, CmdToggleInvert, L"Night reading (dark pages)\tCtrl+I", true, settings_.invert);
    HMENU themeMenu = CreatePopupMenu();
    AppendItem(themeMenu, CmdThemeSystem, L"Use Windows setting", true, settings_.theme == ThemeSetting::System, true);
    AppendItem(themeMenu, CmdThemeLight, L"Light", true, settings_.theme == ThemeSetting::Light, true);
    AppendItem(themeMenu, CmdThemeDark, L"Dark", true, settings_.theme == ThemeSetting::Dark, true);
    AppendSubmenu(m, themeMenu, L"App theme");
    AppendItem(m, CmdToggleHighlight, L"Highlight form fields", true, settings_.highlightFields);
    AppendSeparator(m);
    AppendItem(m, CmdToggleAutoUpdate, L"Update automatically", true, settings_.autoUpdate);
    AppendItem(m, CmdCheckUpdates, L"Check for updates now");
    if (!fileassoc::IsDefault()) AppendItem(m, CmdMakeDefault, L"Make Featherweight the default PDF app");
    AppendSeparator(m);
    AppendItem(m, CmdAbout, L"About Featherweight");
  }
  if (GetMenuItemCount(m) > 0) {
    int cmd = TrackMenu(m, toolbar_.ButtonRect(which == CmdSaveMenu ? CmdSave : which), alignRight);
    if (cmd) Execute(cmd);
  }
  DestroyMenu(m);
}

void App::ShowContextMenu(int sx, int sy) {
  DocumentView* v = Active();
  if (!v || !v->Ready()) return;
  HMENU m = CreatePopupMenu();
  if (v->HasFieldFocus()) {
    AppendItem(m, 1, L"Cut\tCtrl+X");
    AppendItem(m, 2, L"Copy\tCtrl+C");
    AppendItem(m, 3, L"Paste\tCtrl+V");
    AppendSeparator(m);
    AppendItem(m, 4, L"Select all\tCtrl+A");
  } else {
    POINT cp{sx, sy};
    ScreenToClient(hwnd_, &cp);
    contextPt_ = cp;
    contextAnnot_ = v->AnnotAtPoint(cp.x, cp.y, contextPage_);
    if (v->HasSelection()) {
      AppendItem(m, CmdHighlight, L"Highlight");
      AppendItem(m, CmdCopy, L"Copy\tCtrl+C");
    }
    if (contextAnnot_.kind == AnnotHit::Kind::Highlight) {
      AppendItem(m, CmdRemoveAnnot, L"Remove highlight");
    } else if (contextAnnot_.kind == AnnotHit::Kind::Note) {
      AppendItem(m, CmdOpenNote, L"Open note");
      AppendItem(m, CmdRemoveAnnot, L"Delete note");
    } else if (contextPage_ >= 0) {
      AppendItem(m, CmdAddNote, L"Add note here");
    }
    AppendSeparator(m);
    AppendItem(m, CmdSelectAllOnPage, L"Select all text on this page");
    AppendSeparator(m);
    AppendItem(m, CmdZoomIn, L"Zoom in\tCtrl+=");
    AppendItem(m, CmdZoomOut, L"Zoom out\tCtrl+−");
    AppendItem(m, CmdZoomFitWidth, L"Fit width\tCtrl+3");
    AppendItem(m, CmdZoomFitPage, L"Fit page\tCtrl+2");
    AppendSeparator(m);
    AppendItem(m, CmdRotateRight, L"Rotate clockwise\tCtrl+Shift+=");
    AppendItem(m, CmdRotateLeft, L"Rotate counterclockwise\tCtrl+Shift+−");
    AppendSeparator(m);
    AppendItem(m, CmdFind, L"Find…\tCtrl+F");
    AppendItem(m, CmdPrint, L"Print…\tCtrl+P", v->CanPrint());
  }
  theme::ApplyToMenus();
  HideTooltip();
  int cmd = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, sx, sy, hwnd_, nullptr);
  DestroyMenu(m);
  if (v->HasFieldFocus() && cmd >= 1 && cmd <= 4) {
    const UINT keys[] = {0, 'X', 'C', 'V', 'A'};
    v->OnKeyDown(keys[cmd], kModCtrl);
    InvalidateRect(hwnd_, nullptr, FALSE);
    return;
  }
  if (cmd) Execute(cmd);
}

// ---------------------------------------------------------------------------
// Documents and tabs

DocumentView* App::Active() const {
  return active_ >= 0 && active_ < static_cast<int>(tabs_.size()) ? tabs_[active_].get() : nullptr;
}

void App::OpenFiles(const std::vector<std::wstring>& files) {
  int toActivate = -1;
  std::vector<std::wstring> errors;
  for (const auto& raw : files) {
    std::wstring path = util::FullPath(raw);
    // Already open? Just switch to it.
    bool found = false;
    for (int i = 0; i < static_cast<int>(tabs_.size()); ++i) {
      if (fw::EqualsNoCase(tabs_[i]->Path(), path)) {
        toActivate = i;
        found = true;
        break;
      }
    }
    if (found) continue;
    auto view = std::make_unique<DocumentView>(*this, path);
    view->SetViewport(DocRect(), ui_);
    std::wstring error;
    OpenResult r = view->Load(L"", error);
    if (r == OpenResult::Failed) {
      errors.push_back(error);
      if (!util::FileExists(path)) settings_.RemoveRecent(path);
      continue;
    }
    if (r == OpenResult::Ok) {
      if (auto st = settings_.GetState(path)) view->ApplyState(*st);
      else view->ApplyState(FileState{});
    }
    if (!snapshot_) settings_.AddRecent(path);
    tabs_.push_back(std::move(view));
    toActivate = static_cast<int>(tabs_.size()) - 1;
  }
  if (errors.size() == 1) {
    banner_.Show(BannerKind::Error, errors.front(), {}, 8000);
  } else if (errors.size() > 1) {
    banner_.Show(BannerKind::Error,
                 std::to_wstring(errors.size()) + L" files couldn’t be opened. They may be damaged or not be PDFs.",
                 {}, 8000);
  }
  if (toActivate >= 0) ActivateTab(toActivate);
  if (!snapshot_) settings_.Save();
  Layout();
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::OpenDialog() {
  ComPtr<IFileOpenDialog> dlg;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
  COMDLG_FILTERSPEC filters[] = {{L"PDF documents", L"*.pdf"}, {L"All files", L"*.*"}};
  dlg->SetFileTypes(2, filters);
  DWORD opts = 0;
  dlg->GetOptions(&opts);
  dlg->SetOptions(opts | FOS_ALLOWMULTISELECT | FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM);
  if (DocumentView* v = Active()) {
    ComPtr<IShellItem> folder;
    if (SUCCEEDED(SHCreateItemFromParsingName(fw::DirOf(v->Path()).c_str(), nullptr, IID_PPV_ARGS(&folder))))
      dlg->SetFolder(folder.Get());
  }
  HideTooltip();
  if (FAILED(dlg->Show(hwnd_))) return;
  ComPtr<IShellItemArray> items;
  if (FAILED(dlg->GetResults(&items))) return;
  DWORD count = 0;
  items->GetCount(&count);
  std::vector<std::wstring> files;
  for (DWORD i = 0; i < count; ++i) {
    ComPtr<IShellItem> item;
    PWSTR path = nullptr;
    if (SUCCEEDED(items->GetItemAt(i, &item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
      files.push_back(path);
      CoTaskMemFree(path);
    }
  }
  OpenFiles(files);
}

std::wstring App::SaveDialog(const std::wstring& suggested) {
  ComPtr<IFileSaveDialog> dlg;
  if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return {};
  COMDLG_FILTERSPEC filters[] = {{L"PDF document", L"*.pdf"}};
  dlg->SetFileTypes(1, filters);
  dlg->SetDefaultExtension(L"pdf");
  dlg->SetFileName(fw::FileNameOf(suggested).c_str());
  ComPtr<IShellItem> folder;
  std::wstring dir = fw::DirOf(suggested);
  if (!util::DirectoryWritable(dir)) {
    PWSTR docs = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) {
      dir = docs;
      CoTaskMemFree(docs);
    }
  }
  if (SUCCEEDED(SHCreateItemFromParsingName(dir.c_str(), nullptr, IID_PPV_ARGS(&folder)))) dlg->SetFolder(folder.Get());
  DWORD opts = 0;
  dlg->GetOptions(&opts);
  dlg->SetOptions(opts | FOS_OVERWRITEPROMPT | FOS_FORCEFILESYSTEM);
  HideTooltip();
  if (FAILED(dlg->Show(hwnd_))) return {};
  ComPtr<IShellItem> item;
  PWSTR path = nullptr;
  std::wstring result;
  if (SUCCEEDED(dlg->GetResult(&item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
    result = path;
    CoTaskMemFree(path);
  }
  return result;
}

void App::ActivateTab(int index) {
  if (index < 0 || index >= static_cast<int>(tabs_.size())) return;
  if (index != active_ && search_.IsOpen()) CloseSearch();
  if (index != active_ && notes_.IsOpen()) FinishNote(NoteEditor::kSave);
  if (index != active_ && Active()) Active()->OnDeactivated();
  active_ = index;
  DocumentView* v = Active();
  FocusBox(v->PasswordBox());
  Layout();
  std::wstring title = v->Title() + L" – Featherweight";
  SetWindowTextW(hwnd_, title.c_str());
  InvalidateRect(hwnd_, nullptr, FALSE);
}

bool App::ConfirmClose(DocumentView* view) {
  if (!view->Modified()) return true;
  std::wstring heading = L"Save changes to “" + view->Title() + L"”?";
  constexpr int kSaveAs = 100;
  TASKDIALOG_BUTTON buttons[] = {{IDYES, L"Save"}, {kSaveAs, L"Save as\u2026"}, {IDNO, L"Don’t save"}};
  TASKDIALOGCONFIG cfg{sizeof(cfg)};
  cfg.hwndParent = hwnd_;
  cfg.dwFlags = TDF_POSITION_RELATIVE_TO_WINDOW | TDF_ALLOW_DIALOG_CANCELLATION;
  cfg.dwCommonButtons = TDCBF_CANCEL_BUTTON;
  cfg.pszWindowTitle = L"Featherweight";
  cfg.pszMainInstruction = heading.c_str();
  cfg.pszContent = L"If you don’t save, your changes will be lost.";
  cfg.pButtons = buttons;
  cfg.cButtons = 3;
  cfg.nDefaultButton = IDYES;
  int pressed = IDCANCEL;
  HideTooltip();
  if (FAILED(TaskDialogIndirect(&cfg, &pressed, nullptr, nullptr))) return false;
  if (pressed == IDNO) return true;
  if (pressed != IDYES && pressed != kSaveAs) return false;
  for (int i = 0; i < static_cast<int>(tabs_.size()); ++i)
    if (tabs_[i].get() == view) ActivateTab(i);
  return SaveActive(pressed == kSaveAs);
}

bool App::CloseTab(int index) {
  if (index < 0 || index >= static_cast<int>(tabs_.size())) return false;
  DocumentView* v = tabs_[index].get();
  if (notes_.IsOpen() && notes_.View() == v) FinishNote(NoteEditor::kSave);
  if (v->Modified()) {
    ActivateTab(index);
    if (!ConfirmClose(v)) return false;
  }
  RememberState(v);
  if (!snapshot_) settings_.Save();
  if (focusBox_ && focusBox_ == v->PasswordBox()) FocusBox(nullptr);
  if (index == active_ && search_.IsOpen()) CloseSearch();
  tabs_.erase(tabs_.begin() + index);
  if (tabs_.empty()) {
    active_ = -1;
    SetWindowTextW(hwnd_, L"Featherweight");
    FocusBox(nullptr);
  } else {
    int next = active_ > index ? active_ - 1 : std::min(active_, static_cast<int>(tabs_.size()) - 1);
    active_ = -1;
    ActivateTab(next);
  }
  Layout();
  InvalidateRect(hwnd_, nullptr, FALSE);
  return true;
}

void App::RememberState(DocumentView* view) {
  if (view && view->Ready() && !snapshot_) settings_.SetState(view->Path(), view->State());
}

bool App::SaveActive(bool saveAs) {
  DocumentView* v = Active();
  if (!v || !v->Ready()) return false;
  if (!saveAs && !v->Modified()) return true;
  std::wstring path = v->Path();
  if (saveAs) {
    path = SaveDialog(v->Path());
    if (path.empty()) return false;
  }
  std::wstring error;
  if (v->Save(path, error)) {
    if (!snapshot_) {
      settings_.AddRecent(v->Path());
      settings_.Save();
    }
    banner_.Show(BannerKind::Success, L"Saved", {}, 1600);
    ActivateTab(active_);
    return true;
  }
  if (!saveAs) {
    // Can't write here: offer another place right away.
    banner_.Show(BannerKind::Warning, error + L" Choose another place to save it.");
    InvalidateRect(hwnd_, nullptr, FALSE);
    UpdateWindow(hwnd_);
    path = SaveDialog(v->Path());
    if (!path.empty() && v->Save(path, error)) {
      settings_.AddRecent(v->Path());
      settings_.Save();
      banner_.Show(BannerKind::Success, L"Saved", {}, 1600);
      ActivateTab(active_);
      return true;
    }
    if (path.empty()) return false;
  }
  banner_.Show(BannerKind::Error, error);
  return false;
}

void App::PrintActive() {
  DocumentView* v = Active();
  if (!v || !v->Ready()) return;
  std::wstring message;
  HideTooltip();
  print::Outcome outcome = print::PrintDocument(
      hwnd_, *v->Doc(), v->CurrentPage(), v->Title(),
      [this](int done, int total) {
        banner_.Show(BannerKind::Info,
                     L"Printing… page " + std::to_wstring(done + 1) + L" of " + std::to_wstring(total));
        Layout();
        RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
      },
      message);
  switch (outcome) {
    case print::Outcome::Printed: banner_.Show(BannerKind::Success, message, {}, 3000); break;
    case print::Outcome::NotAllowed: banner_.Show(BannerKind::Warning, message, {}, 6000); break;
    case print::Outcome::Failed: banner_.Show(BannerKind::Error, message); break;
    case print::Outcome::Cancelled: banner_.Hide(); break;
  }
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::OpenSearch() {
  DocumentView* v = Active();
  if (!v || !v->Ready()) return;
  if (presentation_) return;
  if (!search_.IsOpen()) search_.Open();
  // Start from the selected text, if any (a short, single line).
  std::wstring selected = fw::Trim(v->SelectedText());
  if (!selected.empty() && selected.size() <= 80 && selected.find_first_of(L"\r\n") == std::wstring::npos)
    search_.Box().SetText(selected);
  FocusBox(&search_.Box());
  search_.Box().SelectAll();
  if (!search_.Box().Text().empty()) v->StartSearch(search_.Box().Text(), search_.MatchCase());
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::CloseSearch() {
  search_.Close();
  if (focusBox_ == &search_.Box()) FocusBox(nullptr);
  if (DocumentView* v = Active()) v->ClearSearch();
  InvalidateRect(hwnd_, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
// Window modes and theme

void App::ToggleFullScreen() {
  if (!fullscreen_) {
    GetWindowPlacement(hwnd_, &savedPlacement_);
    savedStyle_ = GetWindowLongPtrW(hwnd_, GWL_STYLE);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi);
    fullscreen_ = true;
    toolbarReveal_.Snap(0);
    if (snapshot_) {  // keep the off-screen size
      Layout();
      return;
    }
    SetWindowLongPtrW(hwnd_, GWL_STYLE, (savedStyle_ & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
    SetWindowPos(hwnd_, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                 mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    banner_.Show(BannerKind::Info, L"Full screen. Press Esc or F11 to exit.", {}, 2500);
  } else {
    fullscreen_ = false;
    if (snapshot_) {
      Layout();
      return;
    }
    SetWindowLongPtrW(hwnd_, GWL_STYLE, savedStyle_);
    SetWindowPlacement(hwnd_, &savedPlacement_);
    SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER);
  }
  Layout();
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::SetPresentation(bool on) {
  DocumentView* v = Active();
  if (on == presentation_ || !v || !v->Ready()) return;
  if (on) {
    if (search_.IsOpen()) CloseSearch();
    FocusBox(nullptr);
    presentFromFullscreen_ = fullscreen_;
    if (!fullscreen_) ToggleFullScreen();
    presentation_ = true;
    banner_.Hide();
    lastMouseMove_ = anim::Now();
    SetTimer(hwnd_, kTimerCursorHide, 500, nullptr);
    Layout();
    v->SetPresentation(true);
  } else {
    presentation_ = false;
    KillTimer(hwnd_, kTimerCursorHide);
    v->SetPresentation(false);
    if (fullscreen_ && !presentFromFullscreen_) ToggleFullScreen();
  }
  Layout();
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::SetTheme(ThemeSetting t) {
  settings_.theme = t;
  theme::SetSetting(t);
  theme::ApplyToWindow(hwnd_);
  settings_.Save();
  InvalidateRect(hwnd_, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
// ViewHost / DropHost

void App::ViewInvalidate() {
  if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::ViewStateChanged() {
  if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::ViewMessage(BannerKind kind, const std::wstring& text) {
  banner_.Show(kind, text, {}, kind == BannerKind::Success ? 1400 : 5000);
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::ViewRequestPrint() { PostMessageW(hwnd_, WM_COMMAND, CmdPrint, 0); }

void App::ViewFocusTextBox(TextBox* box) { FocusBox(box); }

void App::ViewPasswordAccepted(DocumentView* view) {
  view->SetViewport(DocRect(), ui_);
  if (auto st = settings_.GetState(view->Path())) view->ApplyState(*st);
  else view->ApplyState(FileState{});
  if (focusBox_ == view->PasswordBox() || (focusBox_ && !view->NeedsPassword())) FocusBox(nullptr);
  Layout();
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::ViewOpenNote(DocumentView* view, const NoteTarget& note) {
  if (notes_.IsOpen()) FinishNote(NoteEditor::kSave);
  HideTooltip();
  FocusBox(nullptr);
  notes_.Open(view, note, view->NoteText(note), view->NoteScreenPos(note));
  Layout();
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::FinishNote(NoteEditor::Action action) {
  DocumentView* v = notes_.View();
  if (!v) return;
  bool stillOpen = false;
  for (auto& t : tabs_)
    if (t.get() == v) stillOpen = true;
  NoteTarget target = notes_.Target();
  std::wstring text = notes_.Text();
  notes_.Close();
  if (stillOpen) {
    if (action == NoteEditor::kSave) v->SaveNote(target, text);
    else if (action == NoteEditor::kDelete) v->SaveNote(target, L"");
  }
  SetFocus(hwnd_);
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::OnDragEnter(bool acceptable) {
  dragOver_ = acceptable;
  if (acceptable) dropAnim_.To(1, 140);
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::OnDragLeave() {
  dragOver_ = false;
  dropAnim_.To(0, 160);
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::OnDropFiles(const std::vector<std::wstring>& files) {
  std::vector<std::wstring> pdfs;
  for (const auto& f : files)
    if (fw::EndsWithNoCase(f, L".pdf")) pdfs.push_back(f);
  if (pdfs.empty()) {
    banner_.Show(BannerKind::Warning, L"Only PDF files can be opened.", {}, 4000);
  } else {
    SetForegroundWindow(hwnd_);
    OpenFiles(pdfs);
  }
  InvalidateRect(hwnd_, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
// Startup notices and updates

void App::StartupNotices() {
  const std::wstring current = FW_VERSION_WSTRING;
  const std::wstring exe = util::ExePath();
  bool updated = !settings_.lastRunVersion.empty() && settings_.lastRunVersion != current;
  bool firstRun = settings_.lastRunVersion.empty();
  settings_.lastRunVersion = current;

  // If Featherweight was registered from a folder that no longer exists (moved or deleted),
  // point "Open with" at this copy. A registration of another existing copy is left alone.
  if (fileassoc::IsRegistered() && !fileassoc::IsRegisteredAt(exe)) {
    std::wstring registered = fileassoc::RegisteredExe();
    if (registered.empty() || !util::FileExists(registered)) fileassoc::Register(exe);
  }

  if (settings_.IsInstalled()) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Featherweight_is1",
                      0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
      RegSetValueExW(key, L"DisplayVersion", 0, REG_SZ, reinterpret_cast<const BYTE*>(current.c_str()),
                     static_cast<DWORD>((current.size() + 1) * sizeof(wchar_t)));
      RegCloseKey(key);
    }
  }

  if (updated) {
    banner_.Show(BannerKind::Success, L"Featherweight was updated to " + current, {}, 6000);
  } else if (!settings_.offeredDefaultApp && !firstRun && !fileassoc::IsDefault()) {
    // Offered once, on the second start, so the very first impression is just the app.
    settings_.offeredDefaultApp = true;
    banner_.Show(BannerKind::Info, L"Open PDFs with Featherweight by default?",
                 {{kActMakeDefault, L"Make default", true}, {kActNotNow, L"Not now"}});
  }
  settings_.Save();
}

void App::MaybeCheckForUpdates(bool manual) {
  if (snapshot_ || updater_.Busy()) return;
  int64_t now = util::UnixNow();
  if (!manual && now - settings_.lastUpdateCheck < 24 * 3600) return;
  settings_.lastUpdateCheck = now;
  settings_.Save();
  manualUpdateCheck_ = manual;
  if (manual) banner_.Show(BannerKind::Info, L"Checking for updates…");
  updater_.Start(hwnd_, settings_.autoUpdate);
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::OnUpdaterDone() {
  Updater::Result r = updater_.Get();
  bannerUpdateUrl_ = r.pageUrl;
  bannerUpdateVersion_ = r.version;
  switch (r.status) {
    case Updater::Status::UpToDate:
      if (manualUpdateCheck_) banner_.Show(BannerKind::Success, L"Featherweight is up to date.", {}, 3000);
      break;
    case Updater::Status::Installed:
      if (manualUpdateCheck_)
        banner_.Show(BannerKind::Success,
                     L"Featherweight " + r.version + L" is ready. It will be used the next time you start Featherweight.",
                     {}, 8000);
      break;
    case Updater::Status::Available:
      if (manualUpdateCheck_ || r.version != settings_.skippedVersion) {
        std::vector<Banner::Action> actions;
        if (Updater::CanSelfUpdate()) actions.push_back({kActUpdate, L"Update", true});
        else actions.push_back({kActDownload, L"Download", true});
        actions.push_back({kActSkip, L"Skip this version"});
        banner_.Show(BannerKind::Info, L"Featherweight " + r.version + L" is available.", actions);
      }
      break;
    case Updater::Status::Failed:
      if (manualUpdateCheck_) banner_.Show(BannerKind::Error, L"Update check failed: " + r.error, {}, 6000);
      break;
    default: break;
  }
  manualUpdateCheck_ = false;
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::ShowAbout() {
  std::wstring content = L"Version " FW_VERSION_WSTRING L"\n\nA light, fast PDF reader. No bloat, no subscriptions, no cloud.\n\n"
                         L"PDF engine: PDFium (build " + fw::Utf8ToWide(FW_PDFIUM_BUILD) +
                         L")\nMIT License © 2026 Roy Clerx\n\n"
                         L"<a href=\"https://github.com/dreamtheater484/featherweight\">github.com/dreamtheater484/featherweight</a>";
  TASKDIALOGCONFIG cfg{sizeof(cfg)};
  cfg.hwndParent = hwnd_;
  cfg.hInstance = GetModuleHandleW(nullptr);
  cfg.dwFlags = TDF_POSITION_RELATIVE_TO_WINDOW | TDF_ENABLE_HYPERLINKS | TDF_ALLOW_DIALOG_CANCELLATION;
  cfg.dwCommonButtons = TDCBF_CLOSE_BUTTON;
  cfg.pszWindowTitle = L"About Featherweight";
  cfg.pszMainIcon = MAKEINTRESOURCEW(IDI_APP);
  cfg.pszMainInstruction = L"Featherweight";
  cfg.pszContent = content.c_str();
  cfg.pfCallback = [](HWND, UINT msg, WPARAM, LPARAM lp, LONG_PTR) -> HRESULT {
    if (msg == TDN_HYPERLINK_CLICKED) util::OpenExternalLink(reinterpret_cast<const wchar_t*>(lp));
    return S_OK;
  };
  HideTooltip();
  TaskDialogIndirect(&cfg, nullptr, nullptr, nullptr);
}

// ---------------------------------------------------------------------------
// Snapshot mode: render off-screen to a PNG (used for automated visual checks)

void App::PumpUntilIdle(double maxMs) {
  double end = anim::Now() + maxMs;
  MSG msg;
  while (anim::Now() < end) {
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    // Hidden windows get no WM_PAINT; draw a frame so views register what they need.
    if (dc_) {
      wicTarget_->BeginDraw();
      RenderFrame(dc_.Get());
      wicTarget_->EndDraw();
    }
    bool more = Idle();
    if (!more && !anim::Running()) {
      // One more frame to pick up anything that just became visible.
      wicTarget_->BeginDraw();
      RenderFrame(dc_.Get());
      wicTarget_->EndDraw();
      if (!Idle() && !anim::Running()) break;
    }
    if (!more) Sleep(5);
  }
}

void App::RunScript(const std::wstring& script) {
  for (const std::wstring& rawStep : fw::Split(script, L';')) {
    std::wstring step = fw::Trim(rawStep);
    if (step.empty()) continue;
    size_t colon = step.find(L':');
    std::wstring op = fw::ToLower(step.substr(0, colon));
    std::wstring arg = colon == std::wstring::npos ? L"" : step.substr(colon + 1);
    auto xy = [&](int& x, int& y) {
      auto parts = fw::Split(arg, L',');
      x = parts.size() > 0 ? _wtoi(parts[0].c_str()) : 0;
      y = parts.size() > 1 ? _wtoi(parts[1].c_str()) : 0;
    };
    if (op == L"click" || op == L"dblclick" || op == L"rclick") {
      int x, y;
      xy(x, y);
      LPARAM lp = MAKELPARAM(x, y);
      SendMessageW(hwnd_, WM_MOUSEMOVE, 0, lp);
      if (op == L"rclick") {
        OnMouseDown(x, y, 1, false);
        OnMouseUp(x, y, 1);
      } else {
        SendMessageW(hwnd_, WM_LBUTTONDOWN, MK_LBUTTON, lp);
        SendMessageW(hwnd_, WM_LBUTTONUP, 0, lp);
        if (op == L"dblclick") {
          SendMessageW(hwnd_, WM_LBUTTONDBLCLK, MK_LBUTTON, lp);
          SendMessageW(hwnd_, WM_LBUTTONUP, 0, lp);
        }
      }
    } else if (op == L"drag") {
      auto p = fw::Split(arg, L',');
      if (p.size() == 4) {
        int x1 = _wtoi(p[0].c_str()), y1 = _wtoi(p[1].c_str()), x2 = _wtoi(p[2].c_str()), y2 = _wtoi(p[3].c_str());
        SendMessageW(hwnd_, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x1, y1));
        for (int i = 1; i <= 8; ++i)
          SendMessageW(hwnd_, WM_MOUSEMOVE, MK_LBUTTON,
                       MAKELPARAM(x1 + (x2 - x1) * i / 8, y1 + (y2 - y1) * i / 8));
        SendMessageW(hwnd_, WM_LBUTTONUP, 0, MAKELPARAM(x2, y2));
      }
    } else if (op == L"move") {
      int x, y;
      xy(x, y);
      SendMessageW(hwnd_, WM_MOUSEMOVE, 0, MAKELPARAM(x, y));
    } else if (op == L"type") {
      HWND target = notes_.IsOpen() ? notes_.Edit() : hwnd_;
      for (wchar_t ch : arg) {
        if (ch == L'|') {  // '|' types a line break
          SendMessageW(target, WM_KEYDOWN, VK_RETURN, 0);
          SendMessageW(target, WM_CHAR, L'\r', 0);
        } else {
          SendMessageW(target, WM_CHAR, ch, 0);
        }
      }
    } else if (op == L"key") {
      int mods = 0;
      UINT vk = 0;
      for (auto& part : fw::Split(fw::ToLower(arg), L'+')) {
        if (part == L"ctrl") mods |= kModCtrl;
        else if (part == L"shift") mods |= kModShift;
        else if (part == L"alt") mods |= kModAlt;
        else if (part == L"tab") vk = VK_TAB;
        else if (part == L"enter") vk = VK_RETURN;
        else if (part == L"esc") vk = VK_ESCAPE;
        else if (part == L"pgdn") vk = VK_NEXT;
        else if (part == L"pgup") vk = VK_PRIOR;
        else if (part == L"home") vk = VK_HOME;
        else if (part == L"end") vk = VK_END;
        else if (part == L"up") vk = VK_UP;
        else if (part == L"down") vk = VK_DOWN;
        else if (part == L"left") vk = VK_LEFT;
        else if (part == L"right") vk = VK_RIGHT;
        else if (part == L"back") vk = VK_BACK;
        else if (part == L"del") vk = VK_DELETE;
        else if (part == L"plus") vk = VK_OEM_PLUS;
        else if (part == L"minus") vk = VK_OEM_MINUS;
        else if (part.size() > 1 && part[0] == L'f') vk = VK_F1 + _wtoi(part.c_str() + 1) - 1;
        else if (part.size() == 1) vk = towupper(part[0]);
      }
      if (notes_.IsOpen() && (vk == VK_ESCAPE || (vk == VK_RETURN && (mods & kModCtrl)))) {
        FinishNote(vk == VK_ESCAPE ? NoteEditor::kCancel : NoteEditor::kSave);
        PumpUntilIdle(500);
        continue;
      }
      simulatedMods_ = mods;
      OnKeyDown(vk);
      if (vk == VK_BACK || vk == VK_RETURN) OnChar(vk == VK_BACK ? L'\b' : L'\r');
      simulatedMods_ = -1;
    } else if (op == L"wheel") {
      int x, y;
      auto p = fw::Split(arg, L',');
      int delta = p.size() > 0 ? _wtoi(p[0].c_str()) : -120;
      x = p.size() > 1 ? _wtoi(p[1].c_str()) : 400;
      y = p.size() > 2 ? _wtoi(p[2].c_str()) : 400;
      if (DocumentView* v = Active()) v->OnWheel(delta, false, 0, x, y);
    } else if (op == L"zoom") {
      if (DocumentView* v = Active()) v->SetZoom(_wtoi(arg.c_str()) / 100.0);
    } else if (op == L"page") {
      if (DocumentView* v = Active()) v->GoToPage(_wtoi(arg.c_str()) - 1);
    } else if (op == L"view") {
      if (DocumentView* v = Active())
        v->SetViewMode(arg == L"single" ? fw::ViewMode::Single : arg == L"facing" ? fw::ViewMode::Facing : fw::ViewMode::Continuous);
    } else if (op == L"save") {
      if (DocumentView* v = Active()) {
        std::wstring err;
        if (!v->Save(arg, err)) fwprintf(stderr, L"save failed: %s\n", err.c_str());
      }
    } else if (op == L"cmd") {
      static const std::pair<const wchar_t*, int> names[] = {
          {L"find", CmdFind}, {L"next", CmdFindNext}, {L"invert", CmdToggleInvert}, {L"present", CmdPresentation},
          {L"fit-page", CmdZoomFitPage}, {L"fit-width", CmdZoomFitWidth}, {L"rotate", CmdRotateRight},
          {L"close", CmdCloseTab}, {L"selectall", CmdSelectAll}, {L"nexttab", CmdNextTab}};
      for (const auto& [name, id] : names)
        if (arg == name) Execute(id);
    } else if (op == L"updatecheck") {
      if (!arg.empty()) Updater::SetReleaseUrlForTesting(arg);
      updater_.Start(hwnd_, false);
      for (int i = 0; i < 300 && updater_.Busy(); ++i) Sleep(50);
      Updater::Result r = updater_.Get();
      fwprintf(stderr, L"updater: status=%d version=%s page=%s error=%s\n", static_cast<int>(r.status),
               r.version.c_str(), r.pageUrl.c_str(), r.error.c_str());
    } else if (op == L"highlight") {
      if (DocumentView* v = Active()) v->HighlightSelection();
    } else if (op == L"dumpedit") {
      wchar_t buf[512] = {};
      GetWindowTextW(notes_.Edit(), buf, 512);
      for (wchar_t* c = buf; *c; ++c) fwprintf(stderr, L"%04x ", static_cast<unsigned>(*c));
      fwprintf(stderr, L"\n");
    } else if (op == L"dumpnote") {
      int x, y, page;
      xy(x, y);
      if (DocumentView* v = Active()) {
        AnnotHit hit = v->AnnotAtPoint(x, y, page);
        std::wstring t = hit.kind == AnnotHit::Kind::Note ? v->NoteText(NoteTarget{page, hit.index}) : L"(no note)";
        for (wchar_t ch : t) fwprintf(stderr, L"%04x ", static_cast<unsigned>(ch));
        fwprintf(stderr, L"\n");
      }
    } else if (op == L"removeannot") {
      int x, y, page;
      xy(x, y);
      if (DocumentView* v = Active()) {
        AnnotHit hit = v->AnnotAtPoint(x, y, page);
        if (hit.kind != AnnotHit::Kind::None) v->RemoveAnnot(page, hit.index);
      }
    } else if (op == L"note" || op == L"opennote") {
      auto p = fw::Split(arg, L',');
      DocumentView* v = Active();
      NoteTarget note;
      if (v && p.size() >= 2 && v->NewNoteAt(_wtoi(p[0].c_str()), _wtoi(p[1].c_str()), note)) {
        std::wstring text = p.size() > 2 ? p[2] : L"Note";
        std::replace(text.begin(), text.end(), L'|', L'\r');
        if (op == L"note") v->SaveNote(note, text);
        else ViewOpenNote(v, note);
      }
    } else if (op == L"recent") {
      settings_.AddRecent(arg);
      Layout();
    } else if (op == L"banner") {
      banner_.Show(BannerKind::Info, arg, {{kActUpdate, L"Update", true}, {kActSkip, L"Skip this version"}});
    } else if (op == L"wait") {
      PumpUntilIdle(_wtoi(arg.c_str()));
      continue;
    }
    PumpUntilIdle(1500);
  }
}

void App::DrawChildControlsForSnapshot(ID2D1DeviceContext* dc) {
  if (!notes_.IsOpen()) return;
  HWND edit = notes_.Edit();
  RECT r;
  GetWindowRect(edit, &r);
  MapWindowPoints(nullptr, hwnd_, reinterpret_cast<POINT*>(&r), 2);
  int w = r.right - r.left, h = r.bottom - r.top;
  if (w <= 0 || h <= 0) return;
  HDC screen = GetDC(hwnd_);
  HDC mem = CreateCompatibleDC(screen);
  HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
  HGDIOBJ old = SelectObject(mem, bmp);
  // Paint the background the way the control would, then let it draw its text into our bitmap.
  HBRUSH brush = reinterpret_cast<HBRUSH>(SendMessageW(hwnd_, WM_CTLCOLOREDIT, reinterpret_cast<WPARAM>(mem),
                                                       reinterpret_cast<LPARAM>(edit)));
  RECT all{0, 0, w, h};
  FillRect(mem, &all, brush);
  SendMessageW(edit, WM_PRINT, reinterpret_cast<WPARAM>(mem), PRF_CLIENT | PRF_ERASEBKGND);
  SelectObject(mem, old);
  ComPtr<IWICBitmap> wic;
  ComPtr<ID2D1Bitmap> d2d;
  if (SUCCEEDED(WIC()->CreateBitmapFromHBITMAP(bmp, nullptr, WICBitmapIgnoreAlpha, &wic)) &&
      SUCCEEDED(dc->CreateBitmapFromWicBitmap(wic.Get(), &d2d))) {
    dc->DrawBitmap(d2d.Get(), RectF((float)r.left, (float)r.top, (float)r.right, (float)r.bottom));
  }
  DeleteObject(bmp);
  DeleteDC(mem);
  ReleaseDC(hwnd_, screen);
}

int App::RunSnapshot(const AppOptions& opts) {
  RECT rc;
  GetClientRect(hwnd_, &rc);
  UINT w = std::max(1L, rc.right), h = std::max(1L, rc.bottom);
  if (FAILED(WIC()->CreateBitmap(w, h, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &wicBitmap_))) return 2;
  D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
      D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f,
      96.f);
  if (FAILED(D2D()->CreateWicBitmapRenderTarget(wicBitmap_.Get(), props, &wicTarget_))) return 3;
  wicTarget_.As(&dc_);
  if (!dc_) return 4;

  OpenFiles(opts.files);
  Layout();
  PumpUntilIdle(5000);
  if (!opts.script.empty()) RunScript(opts.script);
  PumpUntilIdle(5000);

  wicTarget_->BeginDraw();
  RenderFrame(dc_.Get());
  wicTarget_->EndDraw();

  // Encode as PNG.
  ComPtr<IWICStream> stream;
  ComPtr<IWICBitmapEncoder> encoder;
  ComPtr<IWICBitmapFrameEncode> frame;
  if (FAILED(WIC()->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(opts.snapshotPath.c_str(), GENERIC_WRITE)) ||
      FAILED(WIC()->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
      FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
      FAILED(encoder->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr)) ||
      FAILED(frame->WriteSource(wicBitmap_.Get(), nullptr)) || FAILED(frame->Commit()) || FAILED(encoder->Commit())) {
    return 5;
  }
  tabs_.clear();
  dc_.Reset();
  wicTarget_.Reset();
  DestroyWindow(hwnd_);
  return 0;
}
