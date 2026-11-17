#pragma once

// Defined names in the workbook tab (S3 plan "Functions, dynamic arrays while editing, names",
// s3-calc): the Name Manager (Ctrl+F3) with Define Name, Create from Selection (Ctrl+Shift+F3) and
// Paste Names (F3), as one side panel in three modes. Names change through the edit API
// (Editor::SetName and DeleteName), each change one undo step.

#include <string>
#include <utility>
#include <vector>

#include "sheet/calc/NameManager.h"
#include "sheet/view/WorkbookFormat.h"
#include "sheet/view/WorkbookPanels.h"

namespace fw::sheet::view {

class WorkbookView::NamesPanel : public PanelContent {
 public:
  enum class Mode { Manager, Create, Paste };
  explicit NamesPanel(WorkbookView& view);
  std::wstring Title() const override;
  std::vector<Control*> Layout(const D2D1_RECT_F& body, float ui) override;
  void Draw(gfx::Canvas& c) override;
  // `define`: the manager with a new name for the selection (Define Name).
  void Open(Mode mode, bool define = false);
  // Scripts (snapshot runs, the torture and fixture scripts) through the panel's own paths:
  // name:Rate=Sheet1!$B$2 (Define Name, Save; the refers-to in the region's form), name:-Rate
  // (Delete), names:create=top,left (Create from Selection). False with `why` when refused.
  bool Script(const std::wstring& op, const std::wstring& arg, std::wstring& why);

 private:
  void Refresh(const std::string& keep = {}, int keepScope = -2);
  void Pick(int i);  // a row of the list into the fields
  void NewName();
  void Save();
  void Delete();
  void Create();
  void Paste(int i);
  void PasteList();
  std::string FullName(const calc::NameRow& r) const { return r.builtIn ? "_xlnm." + r.name : r.name; }

  WorkbookView& v_;
  Mode mode_ = Mode::Manager;
  std::vector<calc::NameRow> rows_;  // all of them; shown_ indexes the filtered ones
  std::vector<size_t> shown_;
  int editing_ = -1;  // the row in rows_ the fields show, or -1 for a new name
  Dropdown filter_, scope_;
  ListBox list_;
  FocusComboBox name_, refersTo_;
  PanelButton new_, save_, delete_, top_, left_, bottom_, right_, create_, pasteList_;
  std::wstring note_;
  std::vector<std::pair<std::wstring, D2D1_RECT_F>> labels_;
  D2D1_RECT_F noteRc_{};
  float ui_ = 1;
  bool focusWanted_ = false;
};

}  // namespace fw::sheet::view
