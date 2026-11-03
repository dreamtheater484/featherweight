// The spin box (Controls.h).
#include "ui/Controls.h"

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <cwctype>

#include "core/Strings.h"
#include "ui/Icons.h"
#include "ui/Theme.h"

using namespace gfx;

namespace {

// The spin box whose button is being held down (one at a time: the pointer is captured).
SpinBox* g_repeating = nullptr;

// Numbers are shown with Windows' decimal separator (plan decision 17); both '.' and ',' are read.
const std::wstring& DecimalSeparator() {
  static const std::wstring sep = [] {
    wchar_t buf[8] = {};
    if (GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_SDECIMAL, buf, 8) > 1) return std::wstring(buf);
    return std::wstring(L".");
  }();
  return sep;
}

void HoverTo(Anim& a, bool on) {
  if (on != (a.Target() > 0.5f)) a.To(on ? 1.f : 0.f, on ? 80.f : 150.f);
}

}  // namespace

SpinBox::SpinBox() {
  box_.trailingInset = 26;
  box_.maxLength = 32;
}

SpinBox::~SpinBox() { StopRepeat(); }

std::wstring SpinBox::Format(double v) const {
  wchar_t buf[64];
  swprintf(buf, 64, L"%.*f", decimals, v);
  std::wstring s = buf;
  if (decimals > 0) {  // 1.50 -> 1.5, 2.00 -> 2
    while (!s.empty() && s.back() == L'0') s.pop_back();
    if (!s.empty() && s.back() == L'.') s.pop_back();
    size_t dot = s.find(L'.');
    if (dot != std::wstring::npos) s.replace(dot, 1, DecimalSeparator());
  }
  if (s == L"-0") s = L"0";
  return s + unit;
}

bool SpinBox::Parse(const std::wstring& text, double& out) const {
  std::wstring s = fw::Trim(text);
  std::wstring u = fw::Trim(unit);
  if (!u.empty() && fw::EndsWithNoCase(s, u)) s.erase(s.size() - u.size());
  s.erase(std::remove(s.begin(), s.end(), L' '), s.end());
  for (wchar_t& ch : s)
    if (ch == L',' || std::wstring(1, ch) == DecimalSeparator()) ch = L'.';
  if (s.empty()) return false;
  wchar_t* end = nullptr;
  double v = wcstod(s.c_str(), &end);
  if (!end || *end != 0 || !std::isfinite(v)) return false;
  out = v;
  return true;
}

void SpinBox::SetValue(double v, bool notify) {
  if (Focused() && !notify) return;  // don't overwrite what is being typed
  const double scale = std::pow(10.0, decimals);
  v = std::round(std::clamp(v, minValue, maxValue) * scale) / scale;
  const bool changed = v != value;
  value = v;
  std::wstring text = Format(v);
  if (text != box_.Text()) box_.SetText(text, Focused());
  if (changed && notify && onChange) onChange(v);
}

void SpinBox::Step(double amount) {
  double base = value, typed = 0;
  if (Parse(box_.Text(), typed)) base = typed;
  // Steps land on multiples of `step`: with steps of 6, 5 goes up to 6 and down to 0.
  const double k = base / step, n = amount / step;
  double target = amount > 0 ? (std::floor(k + 1e-9) + n) * step : (std::ceil(k - 1e-9) + n) * step;
  box_.error = false;
  SetValue(target, true);
  box_.SetText(Format(value), Focused());
}

bool SpinBox::Commit() {
  double v = 0;
  if (!Parse(box_.Text(), v)) {
    bool typed = box_.Text() != Format(value);
    if (typed) box_.ShakeError();
    box_.SetText(Format(value), Focused());
    return !typed;
  }
  box_.error = false;
  SetValue(v, true);
  box_.SetText(Format(value), Focused());
  return true;
}

void SpinBox::StopRepeat() {
  if (g_repeating != this) return;
  if (HWND w = Window()) KillTimer(w, reinterpret_cast<UINT_PTR>(this));
  g_repeating = nullptr;
}

void CALLBACK SpinBox::RepeatProc(HWND hwnd, UINT, UINT_PTR id, DWORD) {
  SpinBox* s = g_repeating;
  if (!s || reinterpret_cast<UINT_PTR>(s) != id || s->pressed_ == 0) {
    KillTimer(hwnd, id);
    return;
  }
  s->Step(s->pressed_ * s->step);
  if (s->repeats_++ == 0) SetTimer(hwnd, id, 60, RepeatProc);
  InvalidateRect(hwnd, nullptr, FALSE);
}

D2D1_RECT_F SpinBox::UpRect() const {
  const float u = Ui();
  float cy = std::round((rc.top + rc.bottom) / 2);
  return RectF(rc.right - 26 * u, rc.top + 3 * u, rc.right - 4 * u, cy);
}

D2D1_RECT_F SpinBox::DownRect() const {
  const float u = Ui();
  float cy = std::round((rc.top + rc.bottom) / 2);
  return RectF(rc.right - 26 * u, cy, rc.right - 4 * u, rc.bottom - 4 * u);
}

void SpinBox::Draw(Canvas& c) {
  const Palette& p = theme::P();
  const float u = Ui();
  box_.rc = rc;
  box_.Draw(c, u, Focused() && CaretOn());
  const D2D1_COLOR_F fg = enabled ? p.textSecondary : p.textDisabled;
  auto button = [&](const D2D1_RECT_F& r, const Anim& hover, const Anim& press, bool up, bool possible) {
    if (enabled && possible) {
      c.FillRound(r, 3 * u, Alpha(p.hover, hover.V()));
      c.FillRound(r, 3 * u, Alpha(p.pressed, press.V()));
    }
    c.Icon(up ? icon::ChevronUp : icon::ChevronDown, r, possible ? fg : p.textDisabled, 8 * u);
  };
  button(UpRect(), upHover_, upPress_, true, value < maxValue);
  button(DownRect(), downHover_, downPress_, false, value > minValue);
}

bool SpinBox::OnMouseMove(float x, float y) {
  if (selecting_ && GetKeyState(VK_LBUTTON) < 0) {
    box_.OnMouseDrag(x, y, Ui());
    return true;
  }
  bool up = Contains(UpRect(), x, y), down = Contains(DownRect(), x, y);
  bool changed = up != (upHover_.Target() > 0.5f) || down != (downHover_.Target() > 0.5f);
  HoverTo(upHover_, up);
  HoverTo(downHover_, down);
  return changed;
}

void SpinBox::OnMouseLeave() {
  HoverTo(upHover_, false);
  HoverTo(downHover_, false);
}

void SpinBox::OnMouseDown(float x, float y, bool dbl, bool shift) {
  const bool first = justFocused_;
  justFocused_ = false;
  const bool up = Contains(UpRect(), x, y);
  if (up || Contains(DownRect(), x, y)) {
    pressed_ = up ? 1 : -1;
    (up ? upPress_ : downPress_).To(1, 40);
    Step(pressed_ * step);
    // Holding the button keeps stepping.
    StopRepeat();
    g_repeating = this;
    repeats_ = 0;
    if (HWND w = Window()) SetTimer(w, reinterpret_cast<UINT_PTR>(this), 400, RepeatProc);
    return;
  }
  if (first && !shift && !dbl) {
    box_.SelectAll();
    return;
  }
  selecting_ = true;
  if (dbl) box_.OnDoubleClick();
  else box_.OnMouseDown(x, y, shift, Ui());
}

void SpinBox::OnMouseUp(float, float) {
  selecting_ = false;
  pressed_ = 0;
  upPress_.To(0, 120);
  downPress_.To(0, 120);
  StopRepeat();
}

bool SpinBox::OnWheel(int delta, float, float) {
  if (!Focused()) return false;
  Step(delta > 0 ? step : -step);
  return true;
}

bool SpinBox::OnKeyDown(UINT vk, bool ctrl, bool shift, bool alt) {
  if (!enabled || alt) return false;
  switch (vk) {
    case VK_UP: Step(step); return true;
    case VK_DOWN: Step(-step); return true;
    case VK_PRIOR: Step(step * 10); return true;
    case VK_NEXT: Step(-step * 10); return true;
    case VK_RETURN: {
      // Applied: false lets a toolbar hand the keyboard back. Refused text keeps it.
      bool ok = Commit();
      box_.SelectAll();
      return !ok;
    }
    case VK_ESCAPE:
      box_.SetText(Format(value), true);
      box_.error = false;
      return false;
    case VK_TAB: Commit(); return false;
    default: break;
  }
  if (box_.OnKeyDown(vk, ctrl, shift)) {
    box_.error = false;
    return true;
  }
  return false;
}

bool SpinBox::OnChar(wchar_t ch) {
  if (!enabled) return false;
  if (ch < 32 || ch == 127) return box_.OnChar(ch);
  const bool unitChar = std::any_of(unit.begin(), unit.end(), [&](wchar_t u) { return towlower(u) == towlower(ch); });
  if (!(iswdigit(ch) || ch == L'.' || ch == L',' || (ch == L'-' && minValue < 0) || ch == L' ' || unitChar))
    return true;  // not part of a number: ignored
  box_.error = false;
  return box_.OnChar(ch);
}

void SpinBox::OnFocus(bool focused) {
  box_.Focus(focused);
  if (focused) {
    box_.SelectAll();
    justFocused_ = true;
    return;
  }
  justFocused_ = false;
  StopRepeat();
  Commit();
}

bool SpinBox::OverText(float x, float y) const {
  return Hit(x, y) && !Contains(UpRect(), x, y) && !Contains(DownRect(), x, y);
}

void SpinBox::Describe(uia::Tree& t, int parent) {
  const float u = Ui();
  int i = t.Add(parent, id, UIA_SpinnerControlTypeId, Name(), rc);
  {
    uia::Node& n = t[i];
    n.accelerator = uia::SplitShortcut(tooltip).second;
    n.enabled = enabled;
    n.focusable = true;
    n.focus = [this] { TakeFocus(); };
    n.hasRange = true;
    n.rangeMin = minValue;
    n.rangeMax = maxValue;
    n.rangeValue = value;
    n.rangeSmall = step;
    n.rangeLarge = step * 10;
    n.setRange = [this](double v) { SetValue(v, true); };
  }
  int e = t.Add(i, 1, UIA_EditControlTypeId, Name(), RectF(rc.left, rc.top, rc.right - 26 * u, rc.bottom));
  {
    uia::Node& n = t[e];
    n.enabled = enabled;
    n.focusable = true;
    n.focused = Focused();
    n.focus = [this] { TakeFocus(); };
    n.hasValue = true;
    n.value = box_.Text();
    n.setValue = [this](const std::wstring& v) {
      box_.SetText(v);
      Commit();
    };
  }
  int up = t.Add(i, 2, UIA_ButtonControlTypeId, L"Increase", UpRect());
  t[up].enabled = enabled && value < maxValue;
  t[up].invoke = [this] { Step(step); };
  int down = t.Add(i, 3, UIA_ButtonControlTypeId, L"Decrease", DownRect());
  t[down].enabled = enabled && value > minValue;
  t[down].invoke = [this] { Step(-step); };
}
