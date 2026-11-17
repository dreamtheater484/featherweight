#include "sheet/view/CellEditor.h"

#include <algorithm>
#include <cwchar>
#include <cwctype>

namespace fw::sheet::view {

namespace {

// Virtual-key codes (WinUser.h), so this stays free of Windows headers.
constexpr unsigned kBack = 0x08, kTab = 0x09, kReturn = 0x0D, kEscape = 0x1B, kEnd = 0x23, kHome = 0x24,
                   kLeft = 0x25, kUp = 0x26, kRight = 0x27, kDown = 0x28, kDelete = 0x2E, kF2 = 0x71, kA = 0x41;

bool WordChar(wchar_t c) { return std::iswalnum(c) || c == L'_' || c == L'.' || c == L'$'; }

}  // namespace

void CellEditor::Begin(const std::wstring& text, Mode mode) {
  active_ = true;
  mode_ = mode;
  text_ = original_ = text;
  caret_ = anchor_ = static_cast<int>(text_.size());
  point_ = Span{};
}

void CellEditor::End() {
  active_ = false;
  text_.clear();
  original_.clear();
  caret_ = anchor_ = 0;
  point_ = Span{};
}

void CellEditor::Replace(int from, int to, const std::wstring& s) {
  from = std::clamp(from, 0, static_cast<int>(text_.size()));
  to = std::clamp(to, from, static_cast<int>(text_.size()));
  text_.replace(static_cast<size_t>(from), static_cast<size_t>(to - from), s);
  caret_ = anchor_ = from + static_cast<int>(s.size());
}

void CellEditor::Insert(const std::wstring& s) {
  Replace(std::min(caret_, anchor_), std::max(caret_, anchor_), s);
  point_ = Span{};
}

bool CellEditor::OnChar(wchar_t ch) {
  if (!active_ || (ch < 0x20 && ch != L'\n') || ch == 0x7F) return false;
  Insert(std::wstring(1, ch));
  return true;
}

void CellEditor::SetCaret(int caret, bool extend) {
  caret_ = std::clamp(caret, 0, static_cast<int>(text_.size()));
  if (!extend) anchor_ = caret_;
  point_ = Span{};
}

void CellEditor::SetText(const std::wstring& text, int anchor, int caret) {
  text_ = text;
  const int n = static_cast<int>(text_.size());
  anchor_ = std::clamp(anchor, 0, n);
  caret_ = std::clamp(caret, 0, n);
  point_ = Span{};
}

void CellEditor::SelectAll() {
  anchor_ = 0;
  caret_ = static_cast<int>(text_.size());
}

void CellEditor::ReplaceSpan(int from, int to, const std::wstring& s) {
  const int n = static_cast<int>(text_.size());
  from = std::clamp(from, 0, n);
  to = std::clamp(to, from, n);
  const int delta = static_cast<int>(s.size()) - (to - from);
  auto keep = [&](int i) { return i >= to ? i + delta : i > from ? from + static_cast<int>(s.size()) : i; };
  const int caret = keep(caret_), anchor = keep(anchor_);
  text_.replace(static_cast<size_t>(from), static_cast<size_t>(to - from), s);
  caret_ = caret;
  anchor_ = anchor;
  point_ = Span{};
}

void CellEditor::SelectWord(int at) {
  const int n = static_cast<int>(text_.size());
  at = std::clamp(at, 0, n);
  int from = at, to = at;
  while (from > 0 && WordChar(text_[static_cast<size_t>(from) - 1])) --from;
  while (to < n && WordChar(text_[static_cast<size_t>(to)])) ++to;
  if (from == to && to < n && text_[static_cast<size_t>(to)] != L'\n') ++to;
  anchor_ = from;
  caret_ = to;
  point_ = Span{};
}

int CellEditor::WordLeft(int i) const {
  while (i > 0 && !WordChar(text_[static_cast<size_t>(i) - 1])) --i;
  while (i > 0 && WordChar(text_[static_cast<size_t>(i) - 1])) --i;
  return i;
}

int CellEditor::WordRight(int i) const {
  const int n = static_cast<int>(text_.size());
  while (i < n && WordChar(text_[static_cast<size_t>(i)])) ++i;
  while (i < n && !WordChar(text_[static_cast<size_t>(i)])) ++i;
  return i;
}

int CellEditor::LineStart(int i) const {
  while (i > 0 && text_[static_cast<size_t>(i) - 1] != L'\n') --i;
  return i;
}

int CellEditor::LineEnd(int i) const {
  const int n = static_cast<int>(text_.size());
  while (i < n && text_[static_cast<size_t>(i)] != L'\n') ++i;
  return i;
}

bool CellEditor::CanPoint(bool mouse) const {
  if (!active_ || !IsFormula() || HasSelection() || (mode_ != Mode::Enter && !mouse)) return false;
  if (point_.length > 0) return caret_ == point_.at + point_.length;
  if (pointRule_) return pointRule_(text_, static_cast<size_t>(caret_));
  if (caret_ <= 0) return false;
  // Right after an operator, a separator or an opening parenthesis (Excel's "Point" in the status
  // bar), at the end of what's typed; a click also before an operator or a closing parenthesis.
  static constexpr wchar_t kAfter[] = L"=+-*/^&(,;:<>";
  if (!std::wcschr(kAfter, text_[static_cast<size_t>(caret_) - 1])) return false;
  if (caret_ == static_cast<int>(text_.size())) return true;
  static constexpr wchar_t kBefore[] = L")+-*/^&,;:<> ";
  return mouse && std::wcschr(kBefore, text_[static_cast<size_t>(caret_)]) != nullptr;
}

void CellEditor::SetPointRef(const std::wstring& ref) {
  if (point_.length > 0) {
    Replace(point_.at, point_.at + point_.length, ref);
  } else {
    point_.at = caret_;
    Replace(caret_, caret_, ref);
  }
  point_.length = static_cast<int>(ref.size());
}

CellEditor::Result CellEditor::OnKey(unsigned vk, bool ctrl, bool shift, bool alt) {
  Result r;
  if (!active_) return r;
  auto handled = [&] {
    r.action = Action::Handled;
    return r;
  };
  switch (vk) {
    case kEscape: r.action = Action::Cancel; return r;
    case kReturn:
      if (alt) {
        Insert(L"\n");
        return handled();
      }
      if (ctrl) {
        r.action = Action::CommitFill;
        return r;
      }
      r.action = Action::Commit;
      r.dRow = shift ? -1 : 1;
      return r;
    case kTab:
      if (ctrl || alt) return r;
      r.action = Action::Commit;
      r.dCol = shift ? -1 : 1;
      return r;
    case kF2:
      if (ctrl || alt || shift) return r;
      ToggleMode();
      return handled();
    case kLeft:
    case kRight:
    case kUp:
    case kDown: {
      const int dRow = vk == kUp ? -1 : vk == kDown ? 1 : 0, dCol = vk == kLeft ? -1 : vk == kRight ? 1 : 0;
      if (mode_ == Mode::Enter) {
        if (CanPoint() && !alt) {
          r.action = Action::Point;
          r.dRow = dRow;
          r.dCol = dCol;
          r.extend = shift;
          return r;
        }
        if (ctrl && dCol == 0) return handled();  // Excel ignores Ctrl+Up/Down while typing
        r.action = Action::Commit;
        r.dRow = dRow;
        r.dCol = dCol;
        return r;
      }
      if (dCol != 0) {
        const bool collapse = !shift && HasSelection();
        int to = caret_;
        if (collapse) to = dCol < 0 ? std::min(caret_, anchor_) : std::max(caret_, anchor_);
        else if (ctrl) to = dCol < 0 ? WordLeft(caret_) : WordRight(caret_);
        else to = caret_ + dCol;
        SetCaret(to, shift);
        return handled();
      }
      // Up and Down move a line, keeping the column as far as the line allows.
      const int start = LineStart(caret_), column = caret_ - start;
      if (dRow < 0) {
        if (start == 0) return handled();
        const int prev = LineStart(start - 1);
        SetCaret(std::min(prev + column, start - 1), shift);
      } else {
        const int end = LineEnd(caret_);
        if (end >= static_cast<int>(text_.size())) return handled();
        SetCaret(std::min(end + 1 + column, LineEnd(end + 1)), shift);
      }
      return handled();
    }
    case kHome:
      SetCaret(ctrl ? 0 : LineStart(caret_), shift);
      return handled();
    case kEnd:
      SetCaret(ctrl ? static_cast<int>(text_.size()) : LineEnd(caret_), shift);
      return handled();
    case kBack:
      if (HasSelection()) Insert(L"");
      else if (caret_ > 0) {
        Replace(ctrl ? WordLeft(caret_) : caret_ - 1, caret_, L"");
        point_ = Span{};
      }
      return handled();
    case kDelete:
      if (HasSelection()) Insert(L"");
      else if (caret_ < static_cast<int>(text_.size())) {
        // Ctrl+Delete deletes to the end of the line, as in Excel.
        Replace(caret_, ctrl ? LineEnd(caret_) : caret_ + 1, L"");
        point_ = Span{};
      }
      return handled();
    case kA:
      if (ctrl && !shift && !alt) {
        SelectAll();
        return handled();
      }
      return r;
    default: return r;
  }
}

}  // namespace fw::sheet::view
