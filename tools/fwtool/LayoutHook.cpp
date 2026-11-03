// The hook for D1's layout. When pagination lands, LayOutDocument becomes:
//   1. OpenPackage (Common.h), marking OpenTimings::Read after the snapshot and Package after it
//   2. the .docx model, then Model
//   3. pagination of the first page, then FirstPage; the remaining pages, then Paginated
//   4. one fw::LayoutLine per line drawn, in reading order per story, into `dump`
// and HaveLayout returns true. Nothing else in fwtool needs to change.
#include "LayoutHook.h"

namespace fwtool {

bool HaveLayout() { return false; }

bool LayOutDocument(const std::filesystem::path&, fw::LayoutDump*, fw::OpenTimings*, std::string& error) {
  error = "Featherweight can't lay out Word documents yet (milestone D1)";
  return false;
}

}  // namespace fwtool
