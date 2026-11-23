// The presentation tab's ribbon (R1, r1-slide; plan §5.6): PowerPoint 2021/365's tabs, groups and
// buttons in PowerPoint's order, each on a command the shortcuts, menus and command search already
// run. Buttons PowerPoint has and Featherweight doesn't are left out; Transitions, Animations, Table
// Design and Chart Design come with the commands that fill them (P3). The table of every PowerPoint
// control and where it went is in r1-slide's state note; docs/r1-plan.md has the decisions.

#include <algorithm>

#include "Commands.h"
#include "core/Layout.h"
#include "core/Strings.h"
#include "slide/show/SlideShow.h"
#include "slide/view/ObjectTool.h"
#include "slide/view/PresentationPanels.h"
#include "slide/view/PresentationText.h"
#include "slide/view/PresentationView.h"
#include "ui/Icons.h"
#include "ui/RibbonModel.h"
#include "ui/Theme.h"

using namespace gfx;

namespace fw::slide::view {

namespace {

// Segoe Fluent Icons (and MDL2 Assets) glyphs that ui/Icons.h doesn't name yet.
constexpr wchar_t kPaste = 0xE77F, kCut = 0xE8C6, kSelectAll = 0xE8B3, kZoom = 0xE71E, kTiles = 0xECA5,
                  kFontIncrease = 0xE8E8, kFontDecrease = 0xE8E7;

using S = RibbonSize;

// The ribbon's menu buttons: their names and the commands their menus list (0: a separator).
struct MenuButton {
  int cmd;
  const wchar_t* label;
};
constexpr MenuButton kMenus[] = {
    {CmdSlideAlignMenu, L"Align"},         {CmdSlideGroupMenu, L"Group"},       {CmdSlideRotateMenu, L"Rotate"},
    {CmdSlideEffectsMenu, L"Shape effects"}, {CmdSlideTableDeleteMenu, L"Delete"}, {CmdSlideSelectMenu, L"Select"},
    {CmdSlideSizeMenu, L"Slide size"},     {CmdSlideColorsMenu, L"Theme colors"}, {CmdSlideSpacingMenu, L"Line spacing"},
    {CmdSlideMonitorMenu, L"Monitor"},     {CmdSlideZoomMenu, L"Zoom"},
};

std::vector<int> MenuOf(int cmd) {
  switch (cmd) {
    case CmdSlideAlignMenu:
      return {CmdObjAlignLeft, CmdObjAlignCenter, CmdObjAlignRight, CmdObjAlignTop, CmdObjAlignMiddle, CmdObjAlignBottom, 0,
              CmdDistributeHorizontally, CmdDistributeVertically, 0, CmdAlignToSlide};
    case CmdSlideGroupMenu: return {CmdGroupObjects, CmdUngroupObjects};
    case CmdSlideRotateMenu: return {CmdRotateRight90, CmdRotateLeft90, CmdFlipVertical, CmdFlipHorizontal};
    case CmdSlideEffectsMenu: return {CmdObjectShadow, CmdSlideNoShadow};
    case CmdSlideTableDeleteMenu: return {CmdTableDeleteColumn, CmdTableDeleteRow, CmdDeleteObject};
    case CmdSlideSelectMenu: return {CmdSelectAll};
    case CmdSlideSizeMenu: return {CmdSlideSize43, CmdSlideSize169, 0, CmdSlideSizePanel};
    case CmdSlideSpacingMenu: return {CmdSpacing1, CmdSpacing15, CmdSpacing2};
    case CmdArrangeMenu:
      return {CmdBringToFront, CmdBringForward, CmdSendBackward, CmdSendToBack, 0, CmdGroupObjects, CmdUngroupObjects, 0,
              CmdObjAlignLeft, CmdObjAlignCenter, CmdObjAlignRight, CmdObjAlignTop, CmdObjAlignMiddle, CmdObjAlignBottom,
              CmdDistributeHorizontally, CmdDistributeVertically, CmdAlignToSlide, 0, CmdRotateRight90, CmdRotateLeft90,
              CmdFlipVertical, CmdFlipHorizontal};
    case CmdSlideZoomMenu: {
      std::vector<int> out;
      for (size_t i = 0; i < fw::ZoomPresets().size(); ++i) out.push_back(CmdZoomPreset + static_cast<int>(i));
      out.insert(out.end(), {0, CmdZoomIn, CmdZoomOut, CmdZoomActual});
      return out;
    }
    default: return {};
  }
}

// Builds the model a tab and a group at a time.
struct Builder {
  RibbonModel& m;
  void Tab(const wchar_t* name, const wchar_t* keyTip, bool contextual = false) {
    RibbonTab t;
    t.name = name;
    t.keyTip = keyTip;
    t.contextual = contextual;
    m.tabs.push_back(std::move(t));
  }
  void Group(const wchar_t* name, int rows = 3, int launcher = 0) {
    RibbonGroup g;
    g.name = name;
    g.rows = rows;
    g.launcher = launcher;
    m.tabs.back().groups.push_back(std::move(g));
  }
  RibbonItem& Add(ToolItem t, S size, const wchar_t* label, const wchar_t* keyTip, S smallest = S::Small) {
    RibbonItem it;
    it.tool = std::move(t);
    it.size = size;
    it.smallest = std::max(size, smallest);  // Large < Medium < Small
    it.label = label;
    it.keyTip = keyTip;
    std::vector<RibbonItem>& items = m.tabs.back().groups.back().items;
    items.push_back(std::move(it));
    return items.back();
  }
};

}  // namespace

bool PresentationView::RibbonMenuState(int cmd, CommandState& s) {
  const MenuButton* b = std::find_if(std::begin(kMenus), std::end(kMenus), [cmd](const MenuButton& x) { return x.cmd == cmd; });
  if (b == std::end(kMenus)) return false;
  s.label = b->label;
  s.enabled = false;
  if (cmd == CmdSlideColorsMenu || cmd == CmdSlideMonitorMenu) s.enabled = Ready() && SlideCount() > 0;
  for (int c : MenuOf(cmd))
    if (c && QueryCommand(c).enabled) s.enabled = true;
  return true;
}

ToolItem PresentationView::RibbonTool(int cmd, wchar_t glyph) {
  // The text's own controls (font, size, colors, the formatting toggles) and the objects' (insert
  // dropdowns, fill and outline colors) as their toolbars made them; every other command a button.
  static const std::vector<int> textOnly = [] {
    std::vector<int> v = TextEditing::CommandIds();
    v.erase(std::remove_if(v.begin(), v.end(), [](int c) { return c == CmdCut || c == CmdCopy || c == CmdPaste || c == CmdPasteMerge || c == CmdSelectAll; }), v.end());
    v.insert(v.end(), {CmdFontBox, CmdSizeBox});
    return v;
  }();
  constexpr int kObjects[] = {CmdInsertPicture, CmdInsertShape, CmdInsertTextBox, CmdInsertTable, CmdInsertChart,
                              CmdObjectFill,    CmdObjectOutline, CmdObjectOutlineWeight, CmdPictureCrop};
  ToolItem t;
  if (std::find(textOnly.begin(), textOnly.end(), cmd) != textOnly.end()) {
    t = text_->Tool(cmd);
  } else if (std::find(std::begin(kObjects), std::end(kObjects), cmd) != std::end(kObjects)) {
    t = Objects().Tool(cmd);
  } else {
    const CommandState q = QueryCommand(cmd);
    t.cmd = cmd;
    t.enabled = q.enabled;
    t.checked = q.checked;
    t.toggle = cmd == CmdNotesPane || cmd == CmdSlideHide;
    t.dropdown = !MenuOf(cmd).empty();
  }
  if (glyph) t.glyph = glyph;
  return t;
}

void PresentationView::BuildRibbon(RibbonModel& m, const ToolbarContext& ctx) {
  if (!Ready()) return;
  PrepareGalleries();  // galleryLayouts_: the current slide's master's layouts
  Builder b{m};
  auto add = [&](int cmd, wchar_t glyph, S size, const wchar_t* label, const wchar_t* keyTip, S smallest = S::Small) -> RibbonItem& {
    RibbonItem& it = b.Add(RibbonTool(cmd, glyph), size, label, keyTip, smallest);
    it.menu = MenuOf(cmd);
    return it;
  };
  // A split button: `cmd` runs, its arrow lists `menu`.
  auto split = [&](int cmd, wchar_t glyph, S size, const wchar_t* label, const wchar_t* keyTip, std::vector<int> menu) -> RibbonItem& {
    RibbonItem& it = add(cmd, glyph, size, label, keyTip);
    it.tool.kind = ToolKind::SplitButton;
    it.menu = std::move(menu);
    return it;
  };
  const int now = deck_ && SlideCount() ? deck_->LayoutOf(current_) : -1;
  int currentLayout = -1;
  std::vector<std::wstring> layoutNames;
  for (size_t k = 0; k < galleryLayouts_.size(); ++k) {
    layoutNames.push_back(galleryLayouts_[k].name);
    if (static_cast<int>(galleryLayouts_[k].index) == now) currentLayout = static_cast<int>(k);
  }
  // The layouts' pictures with their names, opened from New Slide's arrow and from Layout.
  auto layouts = [&](RibbonItem& it, bool insert) {
    const draw::Size size = deck_->SlideSize();
    RibbonGallery g;
    g.choiceWidth = 112;
    g.choiceHeight = 112 * (size.width > 0 ? size.height / size.width : 0.5625f) + 22;
    g.columns = 0;  // a button that opens the grid
    g.popupColumns = 3;
    g.drawChoice = [this](Canvas& c, const D2D1_RECT_F& r, int i, float ui) {
      if (i < 0 || static_cast<size_t>(i) >= galleryLayouts_.size()) return;
      const D2D1_RECT_F pic = RectF(r.left + 4 * ui, r.top + 4 * ui, r.right - 4 * ui, r.bottom - 20 * ui);
      PaintLayoutPicture(c, galleryLayouts_[static_cast<size_t>(i)].index, pic);
      c.Stroke(pic, theme::P().chromeBorder, std::max(1.f, 0.75f * ui));
      c.Text(galleryLayouts_[static_cast<size_t>(i)].name, RectF(r.left, pic.bottom + 2 * ui, r.right, r.bottom), theme::P().text,
             11 * ui, DWRITE_FONT_WEIGHT_REGULAR, Align::Center);
    };
    it.gallery = std::move(g);
    it.tool.dropdown = !insert;
    it.tool.choices = layoutNames;
    it.tool.choice = insert ? -1 : currentLayout;
    it.tool.onChange = [this, insert](const ToolValue& v) {
      if (v.index < 0 || static_cast<size_t>(v.index) >= galleryLayouts_.size()) return;
      const int layout = static_cast<int>(galleryLayouts_[static_cast<size_t>(v.index)].index);
      if (insert) InsertSlide(SlideCount() ? current_ + 1 : 0, layout);
      else SetSlidesLayout(SelectedSlides(), layout);
    };
  };
  auto newSlide = [&] {
    RibbonItem& it = split(CmdSlideNew, 0, S::Large, L"New\nSlide", L"I", {});
    it.smallest = S::Large;
    layouts(it, true);
    return &it;
  };

  // ---- Home
  b.Tab(L"Home", L"H");
  b.Group(L"Clipboard");
  std::vector<int> paste = {CmdPaste};
  if (text_->Editing()) paste.push_back(CmdPasteMerge);
  if (SlidesOnClipboard()) paste.insert(paste.end(), {0, CmdPasteSlidesDestination, CmdPasteSlidesSource});
  split(CmdPaste, kPaste, S::Large, L"Paste", L"V", paste).smallest = S::Large;
  add(CmdCut, kCut, S::Medium, L"Cut", L"X");
  split(CmdCopy, icon::Copy, S::Medium, L"Copy", L"C", {CmdCopy, Objects().Selection().empty() ? CmdSlideDuplicate : CmdDuplicateObjects});
  add(CmdFormatPainter, icon::Brush, S::Medium, L"Format Painter", L"FP");
  b.Group(L"Slides");
  newSlide();
  layouts(add(CmdSlideLayoutMenu, kTiles, S::Medium, L"Layout", L"L"), false);
  b.Group(L"Font", 2);
  add(CmdFontBox, 0, S::Small, L"Font", L"FF");
  add(CmdSizeBox, 0, S::Small, L"Font Size", L"FS").sameRow = true;
  add(CmdGrowFont, kFontIncrease, S::Small, L"Increase Font Size", L"FG").sameRow = true;
  add(CmdShrinkFont, kFontDecrease, S::Small, L"Decrease Font Size", L"FK").sameRow = true;
  add(CmdClearCharFormat, 0, S::Small, L"Clear All Formatting", L"E").sameRow = true;
  add(CmdBold, 0, S::Small, L"Bold", L"1");
  add(CmdItalic, 0, S::Small, L"Italic", L"2").sameRow = true;
  add(CmdUnderline, 0, S::Small, L"Underline", L"3").sameRow = true;
  add(CmdStrike, 0, S::Small, L"Strikethrough", L"4").sameRow = true;
  add(CmdChangeCase, 0, S::Small, L"Change Case", L"7").sameRow = true;
  add(CmdHighlightColor, 0, S::Small, L"Text Highlight Color", L"TC").sameRow = true;
  add(CmdFontColor, 0, S::Small, L"Font Color", L"FC").sameRow = true;
  b.Group(L"Paragraph", 2);
  add(CmdBullets, 0, S::Small, L"Bullets", L"U");
  add(CmdNumbering, 0, S::Small, L"Numbering", L"N").sameRow = true;
  add(CmdOutdent, 0, S::Small, L"Decrease List Level", L"AO").sameRow = true;
  add(CmdIndent, 0, S::Small, L"Increase List Level", L"AI").sameRow = true;
  add(CmdSlideSpacingMenu, 0, S::Small, L"Line Spacing", L"K").sameRow = true;
  add(CmdAlignLeft, 0, S::Small, L"Align Left", L"AL");
  add(CmdAlignCenter, 0, S::Small, L"Center", L"AC").sameRow = true;
  add(CmdAlignRight, 0, S::Small, L"Align Right", L"AR").sameRow = true;
  add(CmdAlignJustify, 0, S::Small, L"Justify", L"AJ").sameRow = true;
  b.Group(L"Drawing");
  add(CmdInsertShape, 0, S::Large, L"Shapes", L"SH");
  add(CmdArrangeMenu, 0, S::Large, L"Arrange", L"G");
  add(CmdObjectFill, 0, S::Medium, L"Shape Fill", L"SF");
  add(CmdObjectOutline, 0, S::Medium, L"Shape Outline", L"SO");
  add(CmdSlideEffectsMenu, 0, S::Medium, L"Shape Effects", L"SE");
  add(CmdObjectOutlineWeight, 0, S::Medium, L"Weight", L"SW").newColumn = true;
  b.Group(L"Editing");
  RibbonItem& find = add(CmdFind, icon::Search, S::Medium, L"Find", L"FD");
  find.tool.enabled = true;
  find.tool.checked = ctx.searchOpen;
  add(CmdReplace, 0, S::Medium, L"Replace", L"R").tool.enabled = CanReplace();
  add(CmdSlideSelectMenu, kSelectAll, S::Medium, L"Select", L"SL");

  // ---- Insert
  b.Tab(L"Insert", L"N");
  b.Group(L"Slides");
  newSlide()->keyTip = L"NS";
  b.Group(L"Tables");
  add(CmdInsertTable, 0, S::Large, L"Table", L"T");
  b.Group(L"Images");
  add(CmdInsertPicture, 0, S::Large, L"Pictures", L"P");
  b.Group(L"Illustrations");
  add(CmdInsertShape, 0, S::Large, L"Shapes", L"SH");
  add(CmdInsertChart, 0, S::Large, L"Chart", L"C");
  b.Group(L"Links");
  add(CmdInsertLink, 0, S::Large, L"Link", L"I");
  b.Group(L"Text");
  add(CmdInsertTextBox, 0, S::Large, L"Text\nBox", L"X");
  add(CmdHeaderFooter, icon::Page, S::Large, L"Header\n& Footer", L"H");
  // Date & Time and Slide Number open Header & footer, as PowerPoint's open its dialog.
  add(CmdHeaderFooter, icon::Calendar, S::Medium, L"Date & Time", L"D").tool.checked = false;
  add(CmdHeaderFooter, 0, S::Medium, L"Slide Number", L"SN").tool.checked = false;

  // ---- Design
  b.Tab(L"Design", L"G");
  b.Group(L"Themes");
  {
    RibbonItem& themes = add(CmdDesignPanel, icon::Color, S::Large, L"Themes", L"H");
    themes.tool.toggle = false;
    themes.tool.checked = false;
    for (int i = 0; i < static_cast<int>(BuiltInThemes().size()); ++i) {
      themes.tool.choices.push_back(BuiltInThemes()[static_cast<size_t>(i)].name);
      if (QueryCommand(CmdApplyTheme + i).checked) themes.tool.choice = i;
    }
    themes.tool.onChange = [this](const ToolValue& v) { Execute(CmdApplyTheme + v.index); };
    RibbonGallery g;
    g.choiceWidth = 96;
    g.choiceHeight = 56;
    g.columns = static_cast<int>(BuiltInThemes().size());
    g.minColumns = 3;
    g.drawChoice = [](Canvas& c, const D2D1_RECT_F& r, int i, float ui) {
      if (i < 0 || static_cast<size_t>(i) >= BuiltInThemes().size()) return;
      const D2D1_RECT_F pic = Inflate(r, -3 * ui, -3 * ui);
      DrawThemePreview(c, i, pic);
      c.Stroke(pic, theme::P().chromeBorder, std::max(1.f, 0.75f * ui));
    };
    g.menu = {CmdDesignPanel};  // the Design panel: themes, colors, background, slide size
    themes.gallery = std::move(g);
  }
  b.Group(L"Variants");
  {
    RibbonItem& colors = add(CmdSlideColorsMenu, icon::Color, S::Large, L"Colors", L"V");
    colors.tool.dropdown = true;
    for (int i = 0; i < static_cast<int>(ColorSchemes().size()); ++i) {
      colors.tool.choices.push_back(ColorSchemes()[static_cast<size_t>(i)].name);
      if (QueryCommand(CmdApplyColors + i).checked) colors.tool.choice = i;
    }
    colors.tool.onChange = [this](const ToolValue& v) { Execute(CmdApplyColors + v.index); };
    RibbonGallery g;
    g.choiceWidth = 260;
    g.choiceHeight = 28;
    g.columns = 0;  // a button that opens the list
    g.popupColumns = 1;
    g.drawChoice = [](Canvas& c, const D2D1_RECT_F& r, int i, float ui) {
      if (i >= 0 && static_cast<size_t>(i) < ColorSchemes().size()) DrawColorsPreview(c, i, r, ui);
    };
    colors.gallery = std::move(g);
  }
  b.Group(L"Customize");
  add(CmdSlideSizeMenu, 0, S::Large, L"Slide\nSize", L"S");
  add(CmdFormatBackground, 0, S::Large, L"Format\nBackground", L"F");

  // ---- Slide Show (Transitions and Animations come with P3's commands)
  b.Tab(L"Slide Show", L"S");
  b.Group(L"Start Slide Show");
  add(CmdSlideShowFromStart, icon::Slideshow, S::Large, L"From\nBeginning", L"B");
  add(CmdSlideShowFromCurrent, 0, S::Large, L"From\nCurrent Slide", L"C");
  b.Group(L"Set Up");
  add(CmdSlideHide, 0, S::Large, L"Hide\nSlide", L"H");
  const std::vector<std::wstring> monitors = fw::slide::show::Monitors();
  if (monitors.size() >= 2) {  // where the show goes
    b.Group(L"Monitors");
    RibbonItem& mon = add(CmdSlideMonitorMenu, 0, S::Small, L"Monitor", L"M");
    mon.tool.kind = ToolKind::Dropdown;
    mon.tool.width = 150;
    mon.tool.choices = {L"Automatic"};
    mon.tool.choices.insert(mon.tool.choices.end(), monitors.begin(), monitors.end());
    mon.tool.choice = showMonitor_ + 1;
    mon.tool.onChange = [this](const ToolValue& v) {
      if (v.index >= 0) showMonitor_ = v.index - 1;
    };
  }

  // ---- Review
  b.Tab(L"Review", L"R");
  b.Group(L"Proofing");
  add(CmdSpelling, 0, S::Large, L"Spelling", L"S");

  // ---- View
  b.Tab(L"View", L"W");
  b.Group(L"Show");
  add(CmdNotesPane, icon::TextDocument, S::Large, L"Notes", L"PN");
  b.Group(L"Zoom");
  add(CmdSlideZoomMenu, kZoom, S::Large, L"Zoom", L"Q");
  add(CmdZoomFitPage, 0, S::Large, L"Fit to\nWindow", L"F");

  // ---- Contextual tabs, while such objects are selected
  const ObjectTool::Kinds kinds = Objects().SelectionKinds();
  auto arrange = [&] {
    b.Group(L"Arrange");
    split(CmdBringForward, 0, S::Medium, L"Bring Forward", L"AF", {CmdBringForward, CmdBringToFront});
    split(CmdSendBackward, 0, S::Medium, L"Send Backward", L"AE", {CmdSendBackward, CmdSendToBack});
    add(CmdSlideAlignMenu, 0, S::Medium, L"Align", L"AA").newColumn = true;
    add(CmdSlideGroupMenu, 0, S::Medium, L"Group", L"G");
    add(CmdSlideRotateMenu, icon::Rotate, S::Medium, L"Rotate", L"AY");
  };
  if (kinds.shapes > 0) {
    b.Tab(L"Shape Format", L"JD", true);
    b.Group(L"Insert Shapes");
    add(CmdInsertShape, 0, S::Medium, L"Shapes", L"SH");
    add(CmdInsertTextBox, 0, S::Medium, L"Text Box", L"X");
    b.Group(L"Shape Styles");
    add(CmdObjectFill, 0, S::Medium, L"Shape Fill", L"SF");
    add(CmdObjectOutline, 0, S::Medium, L"Shape Outline", L"SO");
    add(CmdSlideEffectsMenu, 0, S::Medium, L"Shape Effects", L"SE");
    add(CmdObjectOutlineWeight, 0, S::Medium, L"Weight", L"SW").newColumn = true;
    arrange();
  }
  if (kinds.pictures > 0) {
    b.Tab(L"Picture Format", L"JP", true);
    b.Group(L"Picture Styles");
    add(CmdObjectOutline, 0, S::Medium, L"Picture Border", L"SO");
    add(CmdObjectOutlineWeight, 0, S::Medium, L"Weight", L"SW");
    add(CmdSlideEffectsMenu, 0, S::Medium, L"Picture Effects", L"SE");
    arrange();
    b.Group(L"Size");
    add(CmdPictureCrop, 0, S::Large, L"Crop", L"V");
  }
  if (kinds.tables == 1) {
    b.Tab(L"Table Layout", L"JL", true);
    b.Group(L"Rows & Columns");
    add(CmdSlideTableDeleteMenu, icon::Delete, S::Large, L"Delete", L"D");
    add(CmdTableRowAbove, 0, S::Large, L"Insert\nAbove", L"IA");  // "A" would cut off Arrange's AF, AE, AA, AY
    add(CmdTableRowBelow, 0, S::Medium, L"Insert Below", L"BE");
    add(CmdTableColumnLeft, 0, S::Medium, L"Insert Left", L"L");
    add(CmdTableColumnRight, 0, S::Medium, L"Insert Right", L"R");
    b.Group(L"Merge");
    add(CmdTableMergeCells, 0, S::Large, L"Merge\nCells", L"M");
    add(CmdTableSplitCell, 0, S::Large, L"Split\nCells", L"P");
    arrange();
  }
  // A picture, shape or text box just inserted: its Format tab, as PowerPoint switches to it.
  if (const uint32_t id = Objects().TakeInserted()) m.show = Objects().IsPicture(id) ? L"Picture Format" : L"Shape Format";
}

}  // namespace fw::slide::view
