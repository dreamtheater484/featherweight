#pragma once

// Internal to the edit layer: Editor::RefreshPivots's work (Pivots.cpp), called by Edit.cpp once
// the edit is guarded and recorded.
#include "sheet/edit/Edit.h"

namespace fw::sheet::edit {

Result RefreshPivotsIn(Editor& ed, Workbook& book, Editor::PivotRefresh which, int sheet, CellAddr cell);
Result InsertPivotIn(Editor& ed, Workbook& book, const Editor::NewPivotTable& p);

}  // namespace fw::sheet::edit
