// The cell editor's keys, as Excel takes them while a cell is being edited (s2-plan "Editing in
// the grid"): Enter and Edit mode, the arrows, point mode, Alt+Enter, Esc, Ctrl+Enter.
#include <string>

#include "TestHarness.h"
#include "sheet/view/CellEditor.h"

using fw::sheet::view::CellEditor;
using Action = CellEditor::Action;

namespace {

constexpr unsigned kBack = 0x08, kTab = 0x09, kReturn = 0x0D, kEscape = 0x1B, kEnd = 0x23, kHome = 0x24,
                   kLeft = 0x25, kUp = 0x26, kRight = 0x27, kDown = 0x28, kDelete = 0x2E, kF2 = 0x71;

void Type(CellEditor& e, const std::wstring& s) {
  for (wchar_t c : s) e.OnChar(c);
}

}  // namespace

TEST(SheetGridEdit_TypingReplacesAndArrowsCommit) {
  CellEditor e;
  e.Begin(L"", CellEditor::Mode::Enter);
  Type(e, L"12,5");
  CHECK(e.Text() == L"12,5");
  CHECK(e.Changed());
  // Enter mode: an arrow finishes the entry and moves.
  CellEditor::Result r = e.OnKey(kRight, false, false, false);
  CHECK(r.action == Action::Commit && r.dCol == 1 && r.dRow == 0);
  r = e.OnKey(kReturn, false, true, false);
  CHECK(r.action == Action::Commit && r.dRow == -1);
  r = e.OnKey(kTab, false, true, false);
  CHECK(r.action == Action::Commit && r.dCol == -1);
  CHECK(e.OnKey(kReturn, true, false, false).action == Action::CommitFill);
  CHECK(e.OnKey(kEscape, false, false, false).action == Action::Cancel);
}

TEST(SheetGridEdit_EditModeMovesTheCaret) {
  CellEditor e;
  e.Begin(L"Hello world", CellEditor::Mode::Edit);
  CHECK(e.Caret() == 11);
  CHECK(e.OnKey(kLeft, false, false, false).action == Action::Handled);
  CHECK(e.Caret() == 10);
  e.OnKey(kLeft, true, false, false);  // Ctrl+Left: a word
  CHECK(e.Caret() == 6);
  e.OnKey(kHome, false, true, false);  // Shift+Home selects to the start
  CHECK(e.Caret() == 0 && e.Anchor() == 6);
  Type(e, L"Big ");
  CHECK(e.Text() == L"Big world");
  e.OnKey(kEnd, false, false, false);
  e.OnKey(kBack, false, false, false);
  CHECK(e.Text() == L"Big worl");
  e.OnKey(kHome, false, false, false);
  e.OnKey(kDelete, true, false, false);  // Ctrl+Delete: to the end of the line
  CHECK(e.Text().empty());
  // F2 switches to Enter mode: arrows commit again.
  e.OnKey(kF2, false, false, false);
  CHECK(e.GetMode() == CellEditor::Mode::Enter);
  CHECK(e.OnKey(kDown, false, false, false).action == Action::Commit);
}

TEST(SheetGridEdit_AltEnterAndLines) {
  CellEditor e;
  e.Begin(L"", CellEditor::Mode::Enter);
  Type(e, L"one");
  CHECK(e.OnKey(kReturn, false, false, true).action == Action::Handled);
  Type(e, L"two");
  CHECK(e.Text() == L"one\ntwo");
  e.SetMode(CellEditor::Mode::Edit);
  e.OnKey(kUp, false, false, false);
  CHECK(e.Caret() == 3);
  e.OnKey(kDown, false, false, false);
  CHECK(e.Caret() == 7);
}

TEST(SheetGridEdit_SelectWord) {
  CellEditor e;
  e.Begin(L"=SUM(Sheet1!$A$1, B2)", CellEditor::Mode::Edit);
  e.SelectWord(2);
  CHECK(e.Anchor() == 1 && e.Caret() == 4);  // SUM
  e.SelectWord(14);
  CHECK(e.Anchor() == 12 && e.Caret() == 16);  // $A$1
  e.SelectWord(4);
  CHECK(e.Anchor() == 1 && e.Caret() == 4);  // the word ending there
  e.SelectWord(5);
  CHECK(e.Anchor() == 5 && e.Caret() == 11);  // Sheet1
  e.SelectWord(17);
  CHECK(e.Anchor() == 17 && e.Caret() == 18);  // a space: just it
}

TEST(SheetGridEdit_PointMode) {
  CellEditor e;
  e.Begin(L"", CellEditor::Mode::Enter);
  Type(e, L"=SUM(");
  CHECK(e.CanPoint());
  CellEditor::Result r = e.OnKey(kDown, false, false, false);
  CHECK(r.action == Action::Point && r.dRow == 1);
  e.SetPointRef(L"A2");
  CHECK(e.Text() == L"=SUM(A2");
  // Moving again replaces the reference it inserted.
  CHECK(e.OnKey(kRight, false, true, false).action == Action::Point);
  e.SetPointRef(L"A2:B2");
  CHECK(e.Text() == L"=SUM(A2:B2");
  // Typing ends pointing: the next arrow commits.
  Type(e, L")");
  CHECK(!e.CanPoint());
  CHECK(e.OnKey(kDown, false, false, false).action == Action::Commit);
  // Not a formula: arrows commit.
  e.Begin(L"", CellEditor::Mode::Enter);
  Type(e, L"a+");
  CHECK(!e.CanPoint());
}
