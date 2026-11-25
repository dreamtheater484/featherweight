#include "sheet/view/WorkbookNames.h"

#include <algorithm>

#include "Commands.h"
#include "core/Lang.h"
#include "core/Strings.h"
#include "sheet/calc/Bind.h"
#include "ui/Theme.h"

using namespace gfx;

namespace fw::sheet::view {

namespace {

const UiText kFilters[] = {N_(L"All names"), N_(L"Scoped to the workbook"), N_(L"Scoped to this sheet"),
                                   N_(L"Names with errors"), N_(L"Names without errors")};

bool IsError(const calc::NameRow& r) { return !r.value.empty() && r.value[0] == '#'; }

}  // namespace

WorkbookView::NamesPanel::NamesPanel(WorkbookView& view) : v_(view) {
  for (const wchar_t* f : kFilters) filter_.items.push_back(f);
  filter_.selected = 0;
  filter_.onChange = [this](int) { Refresh(); };
  list_.rowHeight = 28;
  list_.onSelect = [this](int i) {
    if (mode_ == Mode::Manager) Pick(i);
  };
  list_.onActivate = [this](int i) {
    if (mode_ == Mode::Paste) Paste(i);
  };
  name_.autoComplete = refersTo_.autoComplete = false;
  name_.label = T(L"Name");
  refersTo_.label = T(L"Refers to");
  refersTo_.tooltip = L"A range, a constant or a formula, as you'd type it in a cell (=Sheet1!$A$1:$A$9)";
  refersTo_.onEnter = [this] { Save(); };
  scope_.label = T(L"Scope");
  new_.text = T(L"New");
  new_.onInvoke = [this] { NewName(); };
  save_.text = T(L"Save");
  save_.onInvoke = [this] { Save(); };
  delete_.text = T(L"Delete");
  delete_.onInvoke = [this] { Delete(); };
  const std::pair<PanelButton*, const wchar_t*> lines[] = {
      {&top_, T(L"Top row")}, {&left_, T(L"Left column")}, {&bottom_, T(L"Bottom row")}, {&right_, T(L"Right column")}};
  for (const auto& [b, text] : lines) {
    b->text = text;
    b->toggle = true;
  }
  create_.text = T(L"Create names");
  create_.onInvoke = [this] { Create(); };
  pasteList_.text = T(L"Paste list");
  pasteList_.tooltip = T(L"Writes every name and what it refers to from the active cell down");
  pasteList_.onInvoke = [this] { PasteList(); };
}

std::wstring WorkbookView::NamesPanel::Title() const {
  switch (mode_) {
    case Mode::Create: return T(L"Create names from selection");
    case Mode::Paste: return T(L"Paste name");
    default: return T(L"Name Manager");
  }
}

std::vector<Control*> WorkbookView::NamesPanel::Layout(const D2D1_RECT_F& body, float ui) {
  ui_ = ui;
  labels_.clear();
  const float left = body.left + 16 * ui, right = body.right - 16 * ui, gap = 8 * ui;
  float y = body.top + 12 * ui;
  auto label = [&](const wchar_t* text) {
    labels_.push_back({text, RectF(left, y, right, y + 16 * ui)});
    y += 20 * ui;
  };
  if (mode_ == Mode::Create) {
    label(T(L"Create names from values in the"));
    for (PanelButton* b : {&top_, &left_, &bottom_, &right_}) {
      b->rc = RectF(left, y, right, y + 30 * ui);
      y += 38 * ui;
    }
    create_.rc = RectF(left, y + 4 * ui, right, y + 34 * ui);
    noteRc_ = RectF(left, create_.rc.bottom + 12 * ui, right, create_.rc.bottom + 60 * ui);
    return {&top_, &left_, &bottom_, &right_, &create_};
  }
  if (mode_ == Mode::Paste) {
    label(T(L"Names (Enter or double-click pastes one)"));
    const float bottom = std::max(y + 120 * ui, body.bottom - 60 * ui);
    list_.rc = RectF(left, y, right, bottom);
    pasteList_.rc = RectF(left, bottom + 12 * ui, right, bottom + 42 * ui);
    noteRc_ = RectF(left, pasteList_.rc.bottom + 8 * ui, right, pasteList_.rc.bottom + 40 * ui);
    return {&list_, &pasteList_};
  }
  filter_.rc = RectF(left, y, right, y + 30 * ui);
  y += 40 * ui;
  const float listBottom = std::max(y + 120 * ui, body.bottom - 300 * ui);
  list_.rc = RectF(left, y, right, listBottom);
  y = listBottom + 12 * ui;
  label(T(L"Name"));
  name_.rc = RectF(left, y, right, y + 30 * ui);
  y += 40 * ui;
  label(T(L"Scope"));
  scope_.rc = RectF(left, y, right, y + 30 * ui);
  y += 40 * ui;
  label(T(L"Refers to"));
  refersTo_.rc = RectF(left, y, right, y + 30 * ui);
  y += 42 * ui;
  const float w = (right - left - 2 * gap) / 3;
  new_.rc = RectF(left, y, left + w, y + 30 * ui);
  save_.rc = RectF(left + w + gap, y, left + 2 * w + gap, y + 30 * ui);
  delete_.rc = RectF(right - w, y, right, y + 30 * ui);
  noteRc_ = RectF(left, y + 40 * ui, right, y + 80 * ui);
  return {&filter_, &list_, &name_, &scope_, &refersTo_, &new_, &save_, &delete_};
}

void WorkbookView::NamesPanel::Draw(Canvas& c) {
  const Palette& p = theme::P();
  for (const auto& [text, rc] : labels_) c.Text(text, rc, p.textSecondary, 12 * ui_);
  std::wstring note = note_;
  if (note.empty() && mode_ == Mode::Manager && editing_ >= 0 && static_cast<size_t>(editing_) < rows_.size())
    note = T(L"Value: ") + Utf8ToWide(rows_[static_cast<size_t>(editing_)].value);
  if (!note.empty()) c.TextWrapped(note, noteRc_, note_.empty() ? p.textSecondary : p.text, 12 * ui_, 3);
  if (focusWanted_) {
    if (mode_ == Mode::Manager) name_.TakeFocus();
    focusWanted_ = false;
  }
}

void WorkbookView::NamesPanel::Open(Mode mode, bool define) {
  mode_ = mode;
  note_.clear();
  if (mode == Mode::Create) {
    bool top = false, left = false;
    calc::GuessLabels(*v_.book_, static_cast<int>(v_.sheet_), v_.Selected(), top, left);
    top_.checked = top, left_.checked = left, bottom_.checked = right_.checked = false;
    return;
  }
  scope_.items = {T(L"Workbook")};
  for (size_t i = 0; i < v_.book_->SheetCount(); ++i) scope_.items.push_back(Utf8ToWide(v_.book_->GetSheet(i).Name()));
  Refresh();
  if (mode == Mode::Manager) {
    if (define || rows_.empty()) NewName();
    else Pick(list_.Selected());
  }
}

void WorkbookView::NamesPanel::Refresh(const std::string& keep, int keepScope) {
  rows_ = calc::ListNames(*v_.book_, v_.Calc(), v_.Region());
  const int sheet = static_cast<int>(v_.sheet_);
  shown_.clear();
  list_.items.clear();
  for (size_t i = 0; i < rows_.size(); ++i) {
    const calc::NameRow& r = rows_[i];
    if (r.hidden) continue;
    if (mode_ == Mode::Paste) {
      if (r.builtIn || (r.scope >= 0 && r.scope != sheet)) continue;
    } else {
      switch (filter_.selected) {
        case 1: if (r.scope >= 0) continue; break;
        case 2: if (r.scope != sheet) continue; break;
        case 3: if (!IsError(r)) continue; break;
        case 4: if (IsError(r)) continue; break;
        default: break;
      }
    }
    shown_.push_back(i);
    std::wstring item = Utf8ToWide(r.name);
    if (mode_ == Mode::Manager) {
      item += L"   " + Utf8ToWide(r.refersTo);
      if (r.scope >= 0 && static_cast<size_t>(r.scope) < v_.book_->SheetCount())
        item += L"   (" + Utf8ToWide(v_.book_->GetSheet(static_cast<size_t>(r.scope)).Name()) + L")";
    }
    list_.items.push_back(item);
  }
  int select = shown_.empty() ? -1 : 0;
  if (!keep.empty())
    for (size_t k = 0; k < shown_.size(); ++k) {
      const calc::NameRow& r = rows_[shown_[k]];
      if (FullName(r) == keep && (keepScope == -2 || r.scope == keepScope)) select = static_cast<int>(k);
    }
  list_.SetSelected(select);
  if (mode_ == Mode::Manager) {
    if (select >= 0) Pick(select);
    else editing_ = -1;
  }
  v_.host_.ViewInvalidate();
}

void WorkbookView::NamesPanel::Pick(int i) {
  if (i < 0 || static_cast<size_t>(i) >= shown_.size()) return;
  editing_ = static_cast<int>(shown_[static_cast<size_t>(i)]);
  const calc::NameRow& r = rows_[static_cast<size_t>(editing_)];
  name_.SetText(Utf8ToWide(r.name));
  scope_.selected = r.scope + 1;
  refersTo_.SetText(Utf8ToWide(r.refersTo));
  note_.clear();
  v_.host_.ViewInvalidate();
}

// A new name for the selection, named after the label left of it or above it when there's one.
void WorkbookView::NamesPanel::NewName() {
  editing_ = -1;
  list_.SetSelected(-1);
  const Range sel = v_.Expand(v_.Selected());
  std::string label;
  CellInfo cell;
  const Sheet& s = v_.Current();
  if (sel.firstCol > 0 && s.GetCell(sel.firstRow, sel.firstCol - 1, cell) && cell.value.type == ValueType::String)
    label = std::string(cell.value.text);
  else if (sel.firstRow > 0 && s.GetCell(sel.firstRow - 1, sel.firstCol, cell) && cell.value.type == ValueType::String)
    label = std::string(cell.value.text);
  name_.SetText(Utf8ToWide(calc::NameFromLabel(label)));
  scope_.selected = 0;
  refersTo_.SetText(L"=" + Utf8ToWide(calc::RangeFormula(*v_.book_, static_cast<int>(v_.sheet_), sel)));
  note_.clear();
  focusWanted_ = true;
  v_.host_.ViewInvalidate();
}

void WorkbookView::NamesPanel::Save() {
  const calc::NameRow* old = editing_ >= 0 ? &rows_[static_cast<size_t>(editing_)] : nullptr;
  const int scope = std::max(0, scope_.selected) - 1;
  std::string name = WideToUtf8(name_.Text());
  while (!name.empty() && name.back() == ' ') name.pop_back();
  while (!name.empty() && name.front() == ' ') name.erase(name.begin());
  const bool builtIn = old && old->builtIn;
  if (builtIn) {
    name = FullName(*old);  // Print_Area and the like keep their names
  } else {
    const std::u16string bad =
        calc::CheckName(*v_.book_, name, scope, old && old->scope == scope ? std::string_view(old->name) : std::string_view());
    if (!bad.empty()) {
      note_ = std::wstring(bad.begin(), bad.end());
      v_.host_.ViewInvalidate();
      return;
    }
  }
  calc::CompileOptions at;
  at.sheet = static_cast<int>(v_.sheet_);
  at.row = static_cast<int32_t>(v_.active_.row);
  at.col = static_cast<int32_t>(v_.active_.col);
  std::string formula, message;
  if (!calc::ReadRefersTo(WideToUtf8(refersTo_.Text()), v_.Region(), at, v_.Calc().engine(), formula, message)) {
    note_ = Utf8ToWide(message);
    v_.host_.ViewInvalidate();
    return;
  }
  DefinedName d;
  d.name = name;
  d.sheet = scope;
  d.formula = formula;
  d.hidden = old && old->hidden;
  edit::EditGroup group(v_.Ed(), old ? T(u"Edit Name") : T(u"Define Name"));
  edit::Result r;
  if (old && (FullName(*old) != name || old->scope != scope)) r = v_.Ed().DeleteName(FullName(*old), old->scope);
  if (r) r = v_.Ed().SetName(d);
  if (v_.EditRefused(r)) return;
  group.done = true;
  note_.clear();
  Refresh(name, scope);
}

void WorkbookView::NamesPanel::Delete() {
  if (editing_ < 0) return;
  const calc::NameRow& r = rows_[static_cast<size_t>(editing_)];
  if (v_.EditRefused(v_.Ed().DeleteName(FullName(r), r.scope))) return;
  note_.clear();
  Refresh();
}

void WorkbookView::NamesPanel::Create() {
  const Range sel = v_.Expand(v_.Selected());
  const std::vector<calc::LabelName> made = calc::NamesFromLabels(
      *v_.book_, static_cast<int>(v_.sheet_), sel, top_.checked, left_.checked, bottom_.checked, right_.checked);
  if (made.empty()) {
    note_ = T(L"No names were made: the chosen rows and columns have no text to name the cells by.");
    v_.host_.ViewInvalidate();
    return;
  }
  edit::EditGroup group(v_.Ed(), T(u"Create Names"));
  for (const calc::LabelName& m : made) {
    DefinedName d;
    d.name = m.name;
    d.formula = m.formula;
    if (v_.EditRefused(v_.Ed().SetName(d))) return;
  }
  group.done = true;
  v_.host_.ViewShowPanel(nullptr);
  v_.host_.ViewFocusPage();
}

// A name into the formula being typed (or a new one: =Name), as Excel's Paste Name.
void WorkbookView::NamesPanel::Paste(int i) {
  if (i < 0 || static_cast<size_t>(i) >= shown_.size()) return;
  const std::wstring name = Utf8ToWide(rows_[shown_[static_cast<size_t>(i)]].name);
  v_.host_.ViewShowPanel(nullptr);
  if (v_.editor_.Active()) v_.editor_.Insert(name);
  else v_.BeginEdit(CellEditor::Mode::Enter, L"=" + name);
  v_.host_.ViewFocusPage();
  v_.host_.ViewInvalidate();
}

// Every name and what it refers to, in two columns from the active cell down (Excel's Paste List).
void WorkbookView::NamesPanel::PasteList() {
  std::vector<std::pair<CellAddr, edit::CellInput>> cells;
  uint32_t row = v_.active_.row;
  const uint32_t col = v_.active_.col;
  for (size_t i : shown_) {
    const calc::NameRow& r = rows_[i];
    cells.push_back({{row, col}, edit::CellInput::Text(r.name)});
    cells.push_back({{row, col + 1}, edit::CellInput::Text(r.refersTo)});
    ++row;
  }
  if (cells.empty()) return;
  edit::EditGroup group(v_.Ed(), T(u"Paste List"));
  if (v_.EditRefused(v_.Ed().SetCells(static_cast<int>(v_.sheet_), cells))) return;
  group.done = true;
  v_.host_.ViewShowPanel(nullptr);
  v_.host_.ViewFocusPage();
  v_.host_.ViewInvalidate();
}

bool WorkbookView::NamesPanel::Script(const std::wstring& op, const std::wstring& arg, std::wstring& why) {
  note_.clear();
  if (op == L"names") {
    if (arg.rfind(L"create=", 0) != 0) {
      why = L"names: takes create=top,left,bottom,right";
      return false;
    }
    Open(Mode::Create);
    const std::wstring lines = L"," + arg.substr(7) + L",";
    top_.checked = lines.find(L",top,") != std::wstring::npos;
    left_.checked = lines.find(L",left,") != std::wstring::npos;
    bottom_.checked = lines.find(L",bottom,") != std::wstring::npos;
    right_.checked = lines.find(L",right,") != std::wstring::npos;
    Create();
  } else if (!arg.empty() && arg[0] == L'-') {
    Open(Mode::Manager);
    const std::string name = WideToUtf8(arg.substr(1));
    int found = -1;
    for (size_t k = 0; k < shown_.size(); ++k)
      if (NameEqualsNoCase(rows_[shown_[k]].name, name)) found = static_cast<int>(k);
    if (found < 0) {
      why = T(L"name: there's no name ") + arg.substr(1);
      return false;
    }
    list_.SetSelected(found);
    Pick(found);
    Delete();
  } else {
    const size_t eq = arg.find(L'=');
    if (eq == std::wstring::npos || eq == 0) {
      why = L"name: takes Name=refers to, or -Name";
      return false;
    }
    Open(Mode::Manager, true);
    name_.SetText(arg.substr(0, eq));
    std::wstring refers = arg.substr(eq + 1);
    if (refers.empty() || refers[0] != L'=') refers.insert(refers.begin(), L'=');
    refersTo_.SetText(refers);
    Save();
  }
  why = note_;
  return note_.empty();
}

// ---------------------------------------------------------------------------
// The view's side

bool WorkbookView::NamesScriptOp(const std::wstring& op, const std::wstring& arg) {
  if (!Ready() || preview_ || ChartSheet()) return true;
  if (editor_.Active() && !CommitEdit(false)) return true;
  if (!namesPanel_) namesPanel_ = std::make_unique<NamesPanel>(*this);
  std::wstring why;
  if (!namesPanel_->Script(op, arg, why)) host_.ViewMessage(BannerKind::Warning, why);
  host_.ViewShowPanel(nullptr);
  host_.ViewInvalidate();
  return true;
}

void WorkbookView::ShowNamesPanel(int mode, bool define) {
  if (!Ready() || preview_ || ChartSheet()) return;
  if (!namesPanel_) namesPanel_ = std::make_unique<NamesPanel>(*this);
  namesPanel_->Open(static_cast<NamesPanel::Mode>(mode), define);
  host_.ViewShowPanel(namesPanel_.get());
}

bool WorkbookView::NamesCommandState(int cmd, CommandState& s) {
  const bool cells = Ready() && !preview_ && !ChartSheet();
  switch (cmd) {
    case CmdNameManager:
      s.label = T(L"Name Manager");
      s.shortcut = L"Ctrl+F3";
      s.enabled = cells && !editor_.Active();
      return true;
    case CmdDefineName: s.label = T(L"Define name"); s.enabled = cells && !editor_.Active(); return true;
    case CmdCreateNames:
      s.label = T(L"Create names from selection");
      s.shortcut = L"Ctrl+Shift+F3";
      s.enabled = cells && !editor_.Active();
      return true;
    case CmdPasteNames: {
      s.label = T(L"Paste name");
      s.shortcut = L"F3";
      bool any = false;
      if (cells)
        for (const DefinedName& n : book_->Names())
          any = any || (!n.hidden && n.name.rfind("_xlnm.", 0) != 0 && (n.sheet < 0 || n.sheet == static_cast<int>(sheet_)));
      s.enabled = any;  // Excel's F3 does nothing without names
      return true;
    }
  }
  return false;
}

bool WorkbookView::NamesCommand(int cmd) {
  CommandState s;
  if (!NamesCommandState(cmd, s)) return false;
  if (!s.enabled) return true;
  switch (cmd) {
    case CmdNameManager: ShowNamesPanel(static_cast<int>(NamesPanel::Mode::Manager), false); break;
    case CmdDefineName: ShowNamesPanel(static_cast<int>(NamesPanel::Mode::Manager), true); break;
    case CmdCreateNames: ShowNamesPanel(static_cast<int>(NamesPanel::Mode::Create), false); break;
    case CmdPasteNames: ShowNamesPanel(static_cast<int>(NamesPanel::Mode::Paste), false); break;
  }
  return true;
}

// The Name Box: a new name typed there names the selection (workbook scope), as in Excel.
bool WorkbookView::DefineFromNameBox(const std::wstring& typed) {
  if (!Ready() || preview_ || ChartSheet()) return false;
  const std::string name = WideToUtf8(typed);
  if (!calc::CheckName(*book_, name, -1).empty()) return false;
  DefinedName d;
  d.name = name;
  d.formula = calc::RangeFormula(*book_, static_cast<int>(sheet_), Expand(Selected()));
  return !EditRefused(Ed().SetName(d));
}

// The name of the selection when a name refers to exactly it, else the active cell's address.
std::wstring WorkbookView::SelectionName() const {
  const Range sel = Expand(Selected());
  const std::string want = calc::RangeFormula(*book_, static_cast<int>(sheet_), sel);
  for (const DefinedName& n : book_->Names())
    if (!n.hidden && n.name.rfind("_xlnm.", 0) != 0 && (n.sheet < 0 || n.sheet == static_cast<int>(sheet_)) &&
        n.formula.size() == want.size() && NameEqualsNoCase(n.formula, want))
      return Utf8ToWide(n.name);
  return {};
}

}  // namespace fw::sheet::view
