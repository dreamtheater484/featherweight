#pragma once
// The text being typed into a cell (plan §7.2, s2-plan "Editing in the grid"): Excel's two modes,
// its keys, and point mode, without drawing or the model. The workbook tab draws it in the cell
// and the formula bar, and commits it through the edit API.
//
// Enter mode (typing over a cell): the arrows finish the entry and move, or in a formula after an
// operator insert a reference (point mode). Edit mode (F2, a double-click, the formula bar): the
// arrows move the caret. F2 switches between them.
#include <cstdint>
#include <string>

namespace fw::sheet::view {

class CellEditor {
 public:
  enum class Mode : uint8_t { Enter, Edit };
  // What a key asks of the tab.
  enum class Action : uint8_t {
    None,        // not for the editor: the tab handles it
    Handled,     // the text, caret or mode changed
    Commit,      // finish and move by (dRow, dCol)
    CommitFill,  // Ctrl+Enter: finish into every selected cell, stay
    Cancel,      // Esc: the cell keeps its value
    Point,       // point mode: move the referenced cell by (dRow, dCol), extend with `extend`
  };
  struct Result {
    Action action = Action::None;
    int dRow = 0, dCol = 0;
    bool extend = false;
  };

  bool Active() const { return active_; }
  Mode GetMode() const { return mode_; }
  const std::wstring& Text() const { return text_; }
  int Caret() const { return caret_; }
  int Anchor() const { return anchor_; }
  bool HasSelection() const { return caret_ != anchor_; }
  bool IsFormula() const { return !text_.empty() && (text_[0] == L'=' || text_[0] == L'+' || text_[0] == L'-'); }
  // The cell's text when editing began (what Esc puts back on screen).
  const std::wstring& Original() const { return original_; }
  // Text was typed or deleted since Begin.
  bool Changed() const { return text_ != original_; }

  // Starts editing with `text`: typing replaces the cell (Enter mode, the typed text), F2 edits it
  // (Edit mode, the caret at the end).
  void Begin(const std::wstring& text, Mode mode);
  void End();
  void ToggleMode() { mode_ = mode_ == Mode::Enter ? Mode::Edit : Mode::Enter; point_ = Span{}; }
  void SetMode(Mode m) { mode_ = m; }

  // Keys, as Excel takes them while a cell is being edited. `selectionMulti`: more than one cell
  // is selected (Enter and Tab then move inside the selection; the tab works that out).
  Result OnKey(unsigned vk, bool ctrl, bool shift, bool alt);
  // A typed character (WM_CHAR, input methods, dictation).
  bool OnChar(wchar_t ch);
  void Insert(const std::wstring& s);
  void SetCaret(int caret, bool extend);
  void SelectAll();
  // A double-click: the word at `at` (letters, digits, "_", ".", "$"), or the one character there.
  void SelectWord(int at);

  // Point mode: true when an arrow should insert or move a reference (a formula in Enter mode,
  // the caret right after an operator, a separator, "(" or "=", or on the reference it inserted).
  // `mouse`: a click on a cell, which points in Edit mode too, and before an operator or ")".
  bool CanPoint(bool mouse = false) const;
  // Puts `ref` where the reference being pointed at goes: replaces the one inserted last, or
  // inserts at the caret. The tab formats the reference (A1, A1:B3, Sheet2!A1).
  void SetPointRef(const std::wstring& ref);
  bool Pointing() const { return point_.length > 0; }

 private:
  struct Span {
    int at = 0, length = 0;
  };
  void Replace(int from, int to, const std::wstring& s);
  int WordLeft(int i) const;
  int WordRight(int i) const;
  int LineStart(int i) const;
  int LineEnd(int i) const;

  bool active_ = false;
  Mode mode_ = Mode::Enter;
  std::wstring text_, original_;
  int caret_ = 0, anchor_ = 0;
  Span point_;
};

}  // namespace fw::sheet::view
