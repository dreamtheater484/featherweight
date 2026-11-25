#pragma once

// Families documents name that Windows doesn't install (Office's own fonts, the fonts Office
// downloads for documents that ask for them, macOS's), with the Windows families drawn instead,
// best first (decision 11). Featherweight reads no font file of the families it replaces and takes
// no metrics from them (F1): a substitute's width scale and line height are measured on Word's own
// renders of the corpus, so that lines break and pages end where Word's do.
// fwtext's layout and fwdraw's outlined text both use the table.
#include <span>
#include <string>
#include <string_view>

namespace draw {

struct FontSubstitute {
  const wchar_t* family;
  // The substitute's widths, as w:w scales a run's: 1 as designed, 0.95 for 5 % narrower.
  float scale = 1;
  // Its line height in ems (ascent and descent, as Word's single spacing uses them): 0 for the
  // substitute's own. The ascent and descent keep their proportion.
  float height = 0;
};

// The substitutes for `family`, best first; empty when it isn't in the table. Case doesn't matter.
std::span<const FontSubstitute> BuiltInSubstitutes(std::wstring_view family);

// For measuring substitutes (fwtool, FW_SUBSTITUTES), never the app: replaces or adds table rows,
// "Aptos=Segoe UI:0.97:1.2,Arial;Open Sans=Segoe UI:1.02". Before any lookup; not thread-safe.
void SetSubstituteTuning(std::wstring_view spec);

}  // namespace draw
