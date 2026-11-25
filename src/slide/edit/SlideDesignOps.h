#pragma once

#include <string>
#include <vector>

#include "core/Lang.h"
#include "slide/edit/Edit.h"

// Design operations of the presentation tab (P2, p2-slides), on top of the Editor: a built-in theme
// or theme colors on a master (its theme part changed in place, so its effects and East Asian fonts
// stay), and hiding the master's and layout's graphics on slides. Each is one undo step.
namespace fw::slide::edit {

struct ThemeDesign {
  std::u16string step = N_(u"Theme");
  std::string name;            // the theme's name (a:theme name); empty: kept
  std::string colorScheme;     // an a:clrScheme element declaring its prefixes; empty: kept
  std::string major, minor;    // the heading and body Latin typefaces; empty: kept
  int dark = -1;               // the master's p:clrMap: 1 light text on dark, 0 the usual; -1 kept
  std::string background;      // the master's p:bg declaring its prefixes; empty: kept
};
// On master `master` (an index into Presentation::Masters()).
Result ApplyThemeDesign(Editor& ed, size_t master, const ThemeDesign& design);
// p:sld showMasterSp on `slides`: false hides the master's and layout's own shapes there.
Result SetShowMasterShapes(Editor& ed, const std::vector<size_t>& slides, bool show);

}  // namespace fw::slide::edit
