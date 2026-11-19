// Pivot tables in the grid (S4 s4-pivot): Refresh (Alt+F5) and Refresh All (Ctrl+Alt+F5), through
// the edit layer (Editor::RefreshPivots), so each is one undo step.
#include <string_view>
#include <vector>

#include "Commands.h"
#include "pkg/Package.h"
#include "sheet/edit/Edit.h"
#include "sheet/view/WorkbookView.h"

namespace fw::sheet::view {

bool WorkbookView::PivotsCommandState(int cmd, CommandState& s) {
  const bool cells = Ready() && !preview_ && !ChartSheet() && !Current().PartName().empty();
  switch (cmd) {
    case CmdPivotRefresh:
      s.label = L"Refresh PivotTable";
      s.shortcut = L"Alt+F5";
      s.enabled = cells;
      return true;
    case CmdPivotRefreshAll:
      s.label = L"Refresh all PivotTables";
      s.shortcut = L"Ctrl+Alt+F5";
      s.enabled = Ready() && !preview_;
      return true;
  }
  return false;
}

bool WorkbookView::PivotsCommand(int cmd) {
  switch (cmd) {
    case CmdPivotRefresh:
      if (Ready() && !preview_)
        EditRefused(Ed().RefreshPivots(edit::Editor::PivotRefresh::AtCell, static_cast<int>(sheet_), active_));
      return true;
    case CmdPivotRefreshAll:
      if (Ready() && !preview_) EditRefused(Ed().RefreshPivots(edit::Editor::PivotRefresh::All));
      return true;
  }
  return false;
}

void WorkbookView::RefreshPivotsOnOpen() {
  const pkg::Package* package = book_ ? book_->GetPackage() : nullptr;
  if (!package) return;
  // Only a workbook with such a cache gets an editor here (most have no pivots at all).
  bool asks = false;
  for (const pkg::Part& part : package->Parts()) {
    if (part.deleted || !package->ContentTypeOf(part.name).ends_with(".pivotCacheDefinition+xml")) continue;
    std::vector<uint8_t> bytes;
    if (!package->ReadPart(part.name, bytes).Ok()) continue;
    const std::string_view xml(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (xml.find("refreshOnLoad=\"1\"") != std::string_view::npos || xml.find("refreshOnLoad=\"true\"") != std::string_view::npos) {
      asks = true;
      break;
    }
  }
  // A pivot Featherweight keeps but doesn't refresh keeps its values, as the file has them.
  if (asks) (void)Ed().RefreshPivots(edit::Editor::PivotRefresh::OnLoad);
}

}  // namespace fw::sheet::view
