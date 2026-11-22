// The notes pane's text box while notes are typed (P2, p2-slides): see NotesEdit.h.

#include "slide/view/NotesEdit.h"

#include <commctrl.h>
#include <uxtheme.h>

#include <cmath>

#include "ui/Theme.h"

namespace fw::slide::view {

namespace {

constexpr wchar_t kHostClass[] = L"FwSlideNotesEdit";
constexpr UINT_PTR kPauseTimer = 1;
constexpr UINT kPauseMs = 1500;           // a pause in typing this long hands the text over
constexpr UINT kLeaveMessage = WM_APP + 1;  // posted on focus loss: the owner closes it outside the edit's handler

COLORREF ToColorRef(D2D1_COLOR_F c) {
  return RGB(static_cast<int>(c.r * 255 + 0.5f), static_cast<int>(c.g * 255 + 0.5f), static_cast<int>(c.b * 255 + 0.5f));
}

COLORREF Background() { return theme::IsDark() ? RGB(0x1F, 0x1F, 0x1F) : RGB(0xFF, 0xFF, 0xFF); }

std::wstring ToEditText(const std::wstring& s) {
  std::wstring out;
  out.reserve(s.size() + 16);
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == L'\r') {
      out += L"\r\n";
      if (i + 1 < s.size() && s[i + 1] == L'\n') ++i;
    } else if (s[i] == L'\n' || s[i] == L'\v') {
      out += L"\r\n";
    } else {
      out += s[i];
    }
  }
  return out;
}

RECT ToRect(const D2D1_RECT_F& r) {
  return {static_cast<LONG>(std::lround(r.left)), static_cast<LONG>(std::lround(r.top)), static_cast<LONG>(std::lround(r.right)),
          static_cast<LONG>(std::lround(r.bottom))};
}

void RegisterHost() {
  static bool done = false;
  if (done) return;
  done = true;
  WNDCLASSW wc{};
  wc.lpfnWndProc = DefWindowProcW;  // set per window below (SetWindowLongPtr), so the class stays plain
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = kHostClass;
  wc.hCursor = LoadCursor(nullptr, IDC_IBEAM);
  RegisterClassW(&wc);
}

}  // namespace

NotesEdit::~NotesEdit() {
  onCommit = nullptr;
  onLeave = nullptr;
  Close();
  if (font_) DeleteObject(font_);
  if (brush_) DeleteObject(brush_);
}

bool NotesEdit::Open(HWND parent, const D2D1_RECT_F& box, const std::wstring& text, float ui) {
  Close();
  RegisterHost();
  parent_ = parent;
  host_ = CreateWindowExW(0, kHostClass, L"", WS_CHILD | WS_CLIPCHILDREN, 0, 0, 10, 10, parent, nullptr,
                          GetModuleHandleW(nullptr), nullptr);
  if (!host_) return false;
  SetWindowLongPtrW(host_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
  SetWindowLongPtrW(host_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&NotesEdit::HostProc));
  edit_ = CreateWindowExW(0, L"EDIT", L"",
                          WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | ES_NOHIDESEL, 0, 0,
                          10, 10, host_, nullptr, GetModuleHandleW(nullptr), nullptr);
  if (!edit_) {
    Close();
    return false;
  }
  SetWindowSubclass(edit_, &NotesEdit::EditProc, 1, reinterpret_cast<DWORD_PTR>(this));
  if (theme::IsDark()) SetWindowTheme(edit_, L"DarkMode_Explorer", nullptr);
  SendMessageW(edit_, EM_SETLIMITTEXT, 1u << 20, 0);
  committed_ = text;
  SetWindowTextW(edit_, ToEditText(text).c_str());
  placed_ = {};
  Place(box, ui);
  ShowWindow(host_, SW_SHOWNA);
  SetFocus(edit_);
  const int end = GetWindowTextLengthW(edit_);
  SendMessageW(edit_, EM_SETSEL, end, end);
  SendMessageW(edit_, EM_SCROLLCARET, 0, 0);
  return true;
}

void NotesEdit::UpdateFont(float ui) {
  if (font_ && fontUi_ == ui) return;
  if (font_) DeleteObject(font_);
  fontUi_ = ui;
  // As the pane draws the notes: Segoe UI 13 DIPs.
  font_ = CreateFontW(-static_cast<int>(std::lround(13 * ui)), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
  SendMessageW(edit_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
}

void NotesEdit::Place(const D2D1_RECT_F& box, float ui) {
  if (!host_) return;
  UpdateFont(ui);
  const RECT r = ToRect(box);
  if (EqualRect(&r, &placed_)) return;
  placed_ = r;
  const int w = std::max<int>(1, r.right - r.left), h = std::max<int>(1, r.bottom - r.top);
  SetWindowPos(host_, HWND_TOP, r.left, r.top, w, h, SWP_NOACTIVATE);
  const int pad = static_cast<int>(std::lround(4 * ui));
  SetWindowPos(edit_, nullptr, pad, pad, std::max(1, w - 2 * pad), std::max(1, h - 2 * pad), SWP_NOACTIVATE | SWP_NOZORDER);
}

std::wstring NotesEdit::Text() const {
  if (!edit_) return committed_;
  std::wstring raw(static_cast<size_t>(GetWindowTextLengthW(edit_)) + 1, L'\0');
  raw.resize(static_cast<size_t>(GetWindowTextW(edit_, raw.data(), static_cast<int>(raw.size()))));
  std::wstring out;
  out.reserve(raw.size());
  for (size_t i = 0; i < raw.size(); ++i) {
    if (raw[i] == L'\r') {
      out += L'\n';
      if (i + 1 < raw.size() && raw[i + 1] == L'\n') ++i;
    } else {
      out += raw[i];
    }
  }
  return out;
}

void NotesEdit::SetText(const std::wstring& text) {
  if (edit_) SetWindowTextW(edit_, ToEditText(text).c_str());
}

void NotesEdit::Commit() {
  if (host_) KillTimer(host_, kPauseTimer);
  const std::wstring text = Text();
  if (text == committed_) return;
  committed_ = text;
  if (onCommit) onCommit(text);
}

void NotesEdit::Close() {
  if (!host_) return;
  HWND host = host_;
  const bool focused = edit_ && GetFocus() == edit_;
  host_ = edit_ = nullptr;  // first: the focus change below mustn't come back here
  KillTimer(host, kPauseTimer);
  if (focused && parent_) SetFocus(parent_);
  DestroyWindow(host);
}

LRESULT CALLBACK NotesEdit::HostProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  auto* self = reinterpret_cast<NotesEdit*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (!self || self->host_ != hwnd) return DefWindowProcW(hwnd, msg, wp, lp);
  switch (msg) {
    case WM_CTLCOLOREDIT: {
      const COLORREF bg = Background();
      if (!self->brush_ || self->brushColor_ != bg) {
        if (self->brush_) DeleteObject(self->brush_);
        self->brush_ = CreateSolidBrush(bg);
        self->brushColor_ = bg;
      }
      SetTextColor(reinterpret_cast<HDC>(wp), ToColorRef(theme::P().text));
      SetBkColor(reinterpret_cast<HDC>(wp), bg);
      return reinterpret_cast<LRESULT>(self->brush_);
    }
    case WM_ERASEBKGND: {
      RECT rc;
      GetClientRect(hwnd, &rc);
      HBRUSH b = CreateSolidBrush(Background());
      FillRect(reinterpret_cast<HDC>(wp), &rc, b);
      DeleteObject(b);
      return 1;
    }
    case WM_COMMAND:
      if (HIWORD(wp) == EN_CHANGE) SetTimer(hwnd, kPauseTimer, kPauseMs, nullptr);
      return 0;
    case WM_TIMER:
      if (wp == kPauseTimer) self->Commit();
      return 0;
    case kLeaveMessage:
      if (self->onLeave) self->onLeave();
      return 0;
    default: return DefWindowProcW(hwnd, msg, wp, lp);
  }
}

LRESULT CALLBACK NotesEdit::EditProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
  auto* self = reinterpret_cast<NotesEdit*>(data);
  if (msg == WM_KEYDOWN && wp == VK_ESCAPE) {
    if (self->host_) PostMessageW(self->host_, kLeaveMessage, 0, 0);
    return 0;
  }
  if (msg == WM_KEYDOWN) {
    // The app's shortcuts (Ctrl+S, Ctrl+P, F5...) still work while notes are typed: the text goes
    // to the slide first. The edit keeps its own: copy, paste, cut, undo, select all.
    const bool ctrl = GetKeyState(VK_CONTROL) < 0;
    const bool own = wp == 'A' || wp == 'C' || wp == 'V' || wp == 'X' || wp == 'Z' || wp == 'Y';
    if ((ctrl && !own && wp >= 'B' && wp <= 'W') || (wp >= VK_F1 && wp <= VK_F12)) {
      self->swallowChar_ = ctrl;
      self->Commit();
      if (self->parent_) PostMessageW(self->parent_, WM_KEYDOWN, wp, lp);
      return 0;
    }
  }
  if (msg == WM_CHAR && (wp == 27 || self->swallowChar_)) {  // no beep for Esc or a forwarded shortcut
    self->swallowChar_ = false;
    return 0;
  }
  if (msg == WM_KILLFOCUS && self->host_) {
    self->Commit();
    PostMessageW(self->host_, kLeaveMessage, 0, 0);
  }
  if (msg == WM_NCDESTROY) RemoveWindowSubclass(hwnd, &NotesEdit::EditProc, 1);
  return DefSubclassProc(hwnd, msg, wp, lp);
}

}  // namespace fw::slide::view
