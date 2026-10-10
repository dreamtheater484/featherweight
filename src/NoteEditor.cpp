#include "NoteEditor.h"

#include <commctrl.h>

#include <algorithm>
#include <cmath>

#include "Icons.h"
#include "Theme.h"

using namespace gfx;

namespace {

constexpr float kCardW = 320;
constexpr float kCardH = 212;

COLORREF ToColorRef(D2D1_COLOR_F c) {
  return RGB(static_cast<int>(c.r * 255 + 0.5f), static_cast<int>(c.g * 255 + 0.5f), static_cast<int>(c.b * 255 + 0.5f));
}

// PDF notes usually separate lines with CR; the edit control wants CR LF.
std::wstring ToEditText(const std::wstring& s) {
  std::wstring out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == L'\r') {
      out += L"\r\n";
      if (i + 1 < s.size() && s[i + 1] == L'\n') ++i;
    } else if (s[i] == L'\n') {
      out += L"\r\n";
    } else {
      out += s[i];
    }
  }
  return out;
}

}  // namespace

NoteEditor::~NoteEditor() {
  if (font_) DeleteObject(font_);
  if (brush_) DeleteObject(brush_);
}

void NoteEditor::Create(HWND parent) {
  parent_ = parent;
  edit_ = CreateWindowExW(0, L"EDIT", L"",
                          WS_CHILD | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | ES_NOHIDESEL, 0, 0, 10, 10,
                          parent, nullptr, GetModuleHandleW(nullptr), nullptr);
  SetWindowSubclass(edit_, EditProc, 1, reinterpret_cast<DWORD_PTR>(this));
  SendMessageW(edit_, EM_SETLIMITTEXT, 8000, 0);
  save_.id = kSave;
  save_.text = L"Save";
  save_.style = ButtonStyle::Primary;
  save_.tooltip = L"Save note (Ctrl+Enter)";
  cancel_.id = kCancel;
  cancel_.text = L"Cancel";
  cancel_.style = ButtonStyle::Standard;
  cancel_.tooltip = L"Close without saving (Esc)";
  delete_.id = kDelete;
  delete_.glyph = icon::Delete;
  delete_.glyphSize = 14;
  delete_.tooltip = L"Delete note";
  set_.Add(&save_);
  set_.Add(&cancel_);
  set_.Add(&delete_);
}

void NoteEditor::Open(DocumentView* view, const NoteTarget& note, const std::wstring& text, D2D1_POINT_2F anchor) {
  view_ = view;
  note_ = note;
  anchor_ = anchor;
  delete_.visible = note.index >= 0;
  set_.Reset();
  SetWindowTextW(edit_, ToEditText(text).c_str());
  int len = GetWindowTextLengthW(edit_);
  SendMessageW(edit_, EM_SETSEL, len, len);  // cursor at the end, ready to add to the note
  SendMessageW(edit_, EM_SCROLLCARET, 0, 0);
  ShowWindow(edit_, SW_SHOW);
  SetFocus(edit_);
}

void NoteEditor::Close() {
  view_ = nullptr;
  if (edit_) {
    if (GetFocus() == edit_) SetFocus(parent_);
    ShowWindow(edit_, SW_HIDE);
  }
}

std::wstring NoteEditor::Text() const {
  int n = GetWindowTextLengthW(edit_);
  std::wstring s(static_cast<size_t>(n) + 1, L'\0');
  GetWindowTextW(edit_, s.data(), n + 1);
  s.resize(n);
  // Store lines the way PDF readers expect (CR), and trim trailing blank space.
  std::wstring out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == L'\r' && i + 1 < s.size() && s[i + 1] == L'\n') {
      out += L'\r';
      ++i;
    } else {
      out += s[i];
    }
  }
  while (!out.empty() && (out.back() == L' ' || out.back() == L'\r' || out.back() == L'\n' || out.back() == L'\t'))
    out.pop_back();
  return out;
}

COLORREF NoteEditor::EditBackground() const { return theme::IsDark() ? RGB(0x1F, 0x1F, 0x1F) : RGB(0xFF, 0xFF, 0xFF); }

void NoteEditor::UpdateFont() {
  if (font_ && fontUi_ == ui_) return;
  if (font_) DeleteObject(font_);
  fontUi_ = ui_;
  font_ = CreateFontW(-static_cast<int>(std::lround(14 * ui_)), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
  SendMessageW(edit_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
}

void NoteEditor::Layout(const RECT& area, float ui) {
  area_ = area;
  ui_ = ui;
  if (!IsOpen()) return;
  const float u = ui;
  float w = kCardW * u, h = kCardH * u;
  // Next to the note, kept inside the document area.
  float x = anchor_.x + 28 * u, y = anchor_.y - 8 * u;
  if (x + w > area.right - 12 * u) x = anchor_.x - w - 12 * u;
  x = std::clamp(x, area.left + 12 * u, std::max(area.left + 12 * u, area.right - w - 12 * u));
  y = std::clamp(y, area.top + 12 * u, std::max(area.top + 12 * u, area.bottom - h - 12 * u));
  card_ = RectF(std::round(x), std::round(y), std::round(x + w), std::round(y + h));

  box_ = RectF(card_.left + 12 * u, card_.top + 40 * u, card_.right - 12 * u, card_.bottom - 56 * u);
  float by = card_.bottom - 44 * u;
  save_.rc = RectF(card_.right - 12 * u - 84 * u, by, card_.right - 12 * u, by + 32 * u);
  cancel_.rc = RectF(save_.rc.left - 8 * u - 84 * u, by, save_.rc.left - 8 * u, by + 32 * u);
  delete_.rc = RectF(card_.left + 12 * u, by, card_.left + 12 * u + 36 * u, by + 32 * u);

  UpdateFont();
  int pad = static_cast<int>(std::lround(8 * u));
  SetWindowPos(edit_, HWND_TOP, static_cast<int>(box_.left) + pad, static_cast<int>(box_.top) + pad / 2 + 2,
               static_cast<int>(Width(box_)) - 2 * pad, static_cast<int>(Height(box_)) - pad - 4,
               SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void NoteEditor::Draw(Canvas& c) {
  if (!IsOpen()) return;
  const Palette& p = theme::P();
  const float u = ui_;
  c.SoftShadow(card_, 8 * u, 16 * u, p.popupShadow, 4 * u);
  c.FillRound(card_, 8 * u, p.popupBg);
  c.StrokeRound(card_, 8 * u, p.popupBorder, 1);
  D2D1_RECT_F badge = RectF(card_.left + 12 * u, card_.top + 11 * u, card_.left + 32 * u, card_.top + 31 * u);
  c.FillRound(badge, 5 * u, Rgb(0xFFB900));
  c.Icon(icon::Comment, badge, Rgb(0x1B1B1B), 11 * u);
  c.Text(note_.index >= 0 ? L"Note" : L"New note",
         RectF(card_.left + 38 * u, card_.top + 8 * u, card_.right - 12 * u, card_.top + 34 * u), p.text, 14 * u,
         DWRITE_FONT_WEIGHT_SEMI_BOLD);
  // Text area: same colors as the edit control inside it, with the accent line of a focused field.
  COLORREF bg = EditBackground();
  c.FillRound(box_, 4 * u, Rgb((GetRValue(bg) << 16) | (GetGValue(bg) << 8) | GetBValue(bg)));
  c.StrokeRound(box_, 4 * u, p.inputBorder, 1);
  c.PushClip(RectF(box_.left, box_.bottom - 2 * u, box_.right, box_.bottom));
  c.FillRound(box_, 4 * u, p.accent);
  c.PopClip();
  DrawButton(c, save_, u);
  DrawButton(c, cancel_, u);
  DrawButton(c, delete_, u);
}

void NoteEditor::RepaintEdit() {
  if (IsOpen()) RedrawWindow(edit_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
}

HBRUSH NoteEditor::OnCtlColor(HDC dc) {
  COLORREF bg = EditBackground();
  if (!brush_ || brushColor_ != bg) {
    if (brush_) DeleteObject(brush_);
    brush_ = CreateSolidBrush(bg);
    brushColor_ = bg;
  }
  SetTextColor(dc, ToColorRef(theme::P().text));
  SetBkColor(dc, bg);
  return brush_;
}

bool NoteEditor::Hit(int x, int y) const { return IsOpen() && Contains(card_, (float)x, (float)y); }

bool NoteEditor::OnMouseMove(int x, int y) { return set_.OnMouseMove((float)x, (float)y); }
void NoteEditor::OnMouseLeave() { set_.OnMouseLeave(); }
void NoteEditor::OnMouseDown(int x, int y) {
  if (Contains(box_, (float)x, (float)y)) {
    SetFocus(edit_);
    return;
  }
  set_.OnMouseDown((float)x, (float)y);
}

NoteEditor::Action NoteEditor::OnMouseUp(int x, int y) { return static_cast<Action>(set_.OnMouseUp((float)x, (float)y)); }

std::wstring NoteEditor::TooltipAt(int x, int y) const {
  for (const Button* b : {&save_, &cancel_, &delete_})
    if (b->visible && Contains(b->rc, (float)x, (float)y)) return b->tooltip;
  return {};
}

LRESULT CALLBACK NoteEditor::EditProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
  auto* self = reinterpret_cast<NoteEditor*>(data);
  if (msg == WM_KEYDOWN) {
    if (wp == VK_ESCAPE) {
      if (self->onAction) self->onAction(kCancel);
      return 0;
    }
    if (wp == VK_RETURN && GetKeyState(VK_CONTROL) < 0) {
      if (self->onAction) self->onAction(kSave);
      return 0;
    }
    if (wp == 'A' && GetKeyState(VK_CONTROL) < 0) {
      SendMessageW(hwnd, EM_SETSEL, 0, -1);
      return 0;
    }
  }
  if (msg == WM_CHAR && (wp == 27 || (wp == 10 && GetKeyState(VK_CONTROL) < 0) || wp == 1)) return 0;  // no beeps
  return DefSubclassProc(hwnd, msg, wp, lp);
}
