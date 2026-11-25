// The presentation tab's ribbon (R1, r1-slide; plan §5.6): PowerPoint 2021/365's tabs, groups and
// buttons in PowerPoint's order, each on a command the shortcuts, menus and command search already
// run. Buttons PowerPoint has and Featherweight doesn't are left out; Table Design and Chart Design
// come with the commands that fill them. Transitions and Animations are p3-anim's (PresentationAnim.cpp). The table of every PowerPoint
// control and where it went is in r1-slide's state note; docs/r1-plan.md has the decisions.

#include <algorithm>
#include <cmath>

#include "Commands.h"
#include "core/Lang.h"
#include "core/Layout.h"
#include "core/Strings.h"
#include "draw/D2DPainter.h"
#include "draw/Geometry.h"
#include "slide/edit/Edit.h"
#include "slide/objects/ObjectXml.h"
#include "slide/show/SlideShow.h"
#include "slide/view/ObjectTool.h"
#include "slide/view/PresentationFile.h"
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
// The rest are ui/Icons.h's drawn icons.
constexpr wchar_t kPaste = 0xE77F, kCut = 0xE8C6, kSelectAll = 0xE8B3, kZoom = 0xE71E, kTiles = 0xECA5,
                  kFontIncrease = 0xE8E8, kFontDecrease = 0xE8E7;

using S = RibbonSize;

// The ribbon's menu buttons: their names and the commands their menus list (0: a separator).
struct MenuButton {
  int cmd;
  UiText label;
};
constexpr MenuButton kMenus[] = {
    {CmdSlideAlignMenu, N_(L"Align")},         {CmdSlideGroupMenu, N_(L"Group")},       {CmdSlideRotateMenu, N_(L"Rotate")},
    {CmdSlideEffectsMenu, N_(L"Shape effects")}, {CmdSlideTableDeleteMenu, N_(L"Delete")}, {CmdSlideSelectMenu, N_(L"Select")},
    {CmdSlideSizeMenu, N_(L"Slide size")},     {CmdSlideColorsMenu, N_(L"Theme colors")}, {CmdSlideSpacingMenu, N_(L"Line spacing")},
    {CmdSlideMonitorMenu, N_(L"Monitor")},     {CmdSlideZoomMenu, N_(L"Zoom")},
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

// Shape `i` of the Shapes gallery as an outline in `r`, as PowerPoint's gallery shows its shapes.
void DrawShapeChoice(Canvas& c, const D2D1_RECT_F& r, int i, float ui) {
  const auto& gallery = fw::slide::objects::ShapeGallery();
  if (i < 0 || static_cast<size_t>(i) >= gallery.size()) return;
  const std::string_view preset = gallery[static_cast<size_t>(i)].preset;
  const bool line = fw::slide::objects::IsLinePreset(preset);
  const float pad = 5 * ui, w = Width(r) - 2 * pad, h = Height(r) - 2 * pad;
  const std::shared_ptr<const draw::Geometry> g = draw::Geometry::Preset(line ? "line" : preset);
  if (!g || w <= 0 || h <= 0) return;
  const D2D1_COLOR_F ink = theme::P().text;
  draw::Pen pen;
  pen.brush = draw::Brush::Solid({ink.r, ink.g, ink.b, ink.a});
  pen.width = std::max(1.f, ui);
  draw::D2DPainter painter(c.DC());
  painter.Transform(draw::Matrix::Translate(r.left + pad, r.top + pad));
  for (const draw::Geometry::Figure& f : g->Evaluate({w, h}).figures)
    if (f.stroke) painter.StrokePath(f.path, pen);
  if (preset == "straightConnector1") {  // the arrow's head at the end of the line
    const float k = 5 * ui, len = std::max(1.f, std::hypot(w, h)), ux = w / len, uy = h / len;
    const float x = r.left + pad + w, y = r.top + pad + h;
    c.Line(x, y, x - k * (ux + uy * 0.6f), y - k * (uy - ux * 0.6f), ink, pen.width);
    c.Line(x, y, x - k * (ux - uy * 0.6f), y - k * (uy + ux * 0.6f), ink, pen.width);
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
  // `shrinkFirst`: higher shrinks sooner as the window narrows (PowerPoint folds Editing, then
  // Drawing, then Paragraph, and keeps Font and the Clipboard longest).
  void Group(const wchar_t* name, int rows = 3, int shrinkFirst = 0) {
    RibbonGroup g;
    g.name = name;
    g.rows = rows;
    g.shrinkFirst = shrinkFirst;
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
  constexpr int kObjects[] = {CmdInsertPicture, CmdInsertShape,   CmdInsertTextBox,       CmdInsertTable,
                              CmdInsertChart,   CmdObjectFill,    CmdObjectOutline,       CmdObjectOutlineWeight,
                              CmdPictureCrop,   CmdInsertVideo,   CmdInsertAudio};
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
    t.toggle = cmd == CmdNotesPane || cmd == CmdSlideHide || cmd == CmdUsePresenterView ||
               (cmd >= CmdMediaLoop && cmd <= CmdMediaAcrossSlides);
    t.dropdown = !MenuOf(cmd).empty();
  }
  if (glyph) t.glyph = glyph;
  if (cmd == CmdObjectOutlineWeight) {  // a list button: the weights, then the dashes (shadow is Shape Effects')
    t.kind = ToolKind::Button;
    t.dropdown = true;
    t.choices.resize(t.choices.size() - 2);
  }
  return t;
}

void PresentationView::BuildRibbon(RibbonModel& m, const ToolbarContext& ctx) {
  if (!Ready()) return;
  galleryLayouts_ = deck_->LayoutsFor(current_);  // New Slide and Layout show these
  // Quick access: From Beginning after Save, Undo and Redo, as PowerPoint's.
  ToolItem& fromStart = m.quick.emplace_back();
  fromStart.cmd = CmdSlideShowFromStart;
  fromStart.glyph = icon::Slideshow;
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
  // The shapes as outlines in a grid, opened from Shapes (it inserts the one picked).
  auto shapes = [&](RibbonItem& it) {
    it.tool.kind = ToolKind::Button;
    it.tool.dropdown = true;
    RibbonGallery g;
    g.choiceWidth = g.choiceHeight = 30;
    g.columns = 0;
    g.popupColumns = 8;
    g.drawChoice = DrawShapeChoice;
    it.gallery = std::move(g);
  };
  auto newSlide = [&] {
    RibbonItem& it = split(CmdSlideNewMenu, icon::NewSlide, S::Large, T(L"New\nSlide"), L"I", {});
    it.smallest = S::Large;
    layouts(it, true);
    return &it;
  };

  // ---- Home
  b.Tab(T(L"Home"), L"H");
  b.Group(T(L"Clipboard"));
  std::vector<int> paste = {CmdPaste};
  if (text_->Editing()) paste.push_back(CmdPasteMerge);
  if (SlidesOnClipboard()) paste.insert(paste.end(), {0, CmdPasteSlidesDestination, CmdPasteSlidesSource});
  split(CmdPaste, kPaste, S::Large, T(L"Paste"), L"V", paste).smallest = S::Large;
  add(CmdCut, kCut, S::Medium, T(L"Cut"), L"X");
  split(CmdCopy, icon::Copy, S::Medium, T(L"Copy"), L"C", {CmdCopy, Objects().Selection().empty() ? CmdSlideDuplicate : CmdDuplicateObjects});
  add(CmdFormatPainter, icon::Brush, S::Medium, T(L"Format Painter"), L"FP");
  b.Group(T(L"Slides"));
  newSlide();
  layouts(add(CmdSlideLayoutMenu, kTiles, S::Medium, T(L"Layout"), L"L"), false);
  b.Group(T(L"Font"), 2);
  add(CmdFontBox, 0, S::Small, T(L"Font"), L"FF");
  add(CmdSizeBox, 0, S::Small, T(L"Font Size"), L"FS").sameRow = true;
  add(CmdGrowFont, kFontIncrease, S::Small, T(L"Increase Font Size"), L"FG").sameRow = true;
  add(CmdShrinkFont, kFontDecrease, S::Small, T(L"Decrease Font Size"), L"FK").sameRow = true;
  add(CmdClearCharFormat, 0, S::Small, T(L"Clear All Formatting"), L"E").sameRow = true;
  add(CmdBold, 0, S::Small, T(L"Bold"), L"1");
  add(CmdItalic, 0, S::Small, T(L"Italic"), L"2").sameRow = true;
  add(CmdUnderline, 0, S::Small, T(L"Underline"), L"3").sameRow = true;
  add(CmdStrike, 0, S::Small, T(L"Strikethrough"), L"4").sameRow = true;
  add(CmdChangeCase, 0, S::Small, T(L"Change Case"), L"7").sameRow = true;
  add(CmdHighlightColor, 0, S::Small, T(L"Text Highlight Color"), L"TC").sameRow = true;
  add(CmdFontColor, 0, S::Small, T(L"Font Color"), L"FC").sameRow = true;
  b.Group(T(L"Paragraph"), 2, 1);
  add(CmdBullets, 0, S::Small, T(L"Bullets"), L"U");
  add(CmdNumbering, 0, S::Small, T(L"Numbering"), L"N").sameRow = true;
  add(CmdOutdent, 0, S::Small, T(L"Decrease List Level"), L"AO").sameRow = true;
  add(CmdIndent, 0, S::Small, T(L"Increase List Level"), L"AI").sameRow = true;
  add(CmdSlideSpacingMenu, icon::LineSpacing, S::Small, T(L"Line Spacing"), L"K").sameRow = true;
  add(CmdAlignLeft, 0, S::Small, T(L"Align Left"), L"AL");
  add(CmdAlignCenter, 0, S::Small, T(L"Center"), L"AC").sameRow = true;
  add(CmdAlignRight, 0, S::Small, T(L"Align Right"), L"AR").sameRow = true;
  add(CmdAlignJustify, 0, S::Small, T(L"Justify"), L"AJ").sameRow = true;
  b.Group(T(L"Drawing"), 3, 2);
  shapes(add(CmdInsertShape, icon::Shapes, S::Large, T(L"Shapes"), L"SH"));
  add(CmdArrangeMenu, icon::Arrange, S::Large, T(L"Arrange"), L"G");
  add(CmdObjectFill, 0, S::Medium, T(L"Shape Fill"), L"SF");
  add(CmdObjectOutline, 0, S::Medium, T(L"Shape Outline"), L"SO");
  add(CmdSlideEffectsMenu, icon::ShapeEffects, S::Medium, T(L"Shape Effects"), L"SE");
  add(CmdObjectOutlineWeight, 0, S::Medium, T(L"Weight"), L"SW").newColumn = true;
  b.Group(T(L"Editing"), 3, 3);
  RibbonItem& find = add(CmdFind, icon::Search, S::Medium, T(L"Find"), L"FD");
  find.tool.enabled = true;
  find.tool.checked = ctx.searchOpen;
  add(CmdReplace, icon::Replace, S::Medium, T(L"Replace"), L"R").tool.enabled = CanReplace();
  add(CmdSlideSelectMenu, kSelectAll, S::Medium, T(L"Select"), L"SL");

  // ---- Insert
  b.Tab(T(L"Insert"), L"N");
  b.Group(T(L"Slides"));
  newSlide()->keyTip = L"NS";
  b.Group(T(L"Tables"));
  add(CmdInsertTable, 0, S::Large, T(L"Table"), L"T");
  b.Group(T(L"Images"));
  add(CmdInsertPicture, 0, S::Large, T(L"Pictures"), L"P");
  b.Group(T(L"Illustrations"));
  shapes(add(CmdInsertShape, icon::Shapes, S::Large, T(L"Shapes"), L"SH"));
  add(CmdInsertChart, 0, S::Large, T(L"Chart"), L"C");
  b.Group(T(L"Links"));
  add(CmdInsertLink, 0, S::Large, T(L"Link"), L"I");
  b.Group(T(L"Text"));
  add(CmdInsertTextBox, icon::TextBox, S::Large, T(L"Text\nBox"), L"X");
  add(CmdHeaderFooter, icon::Header, S::Large, T(L"Header\n& Footer"), L"H");
  // Date & Time and Slide Number open Header & footer, as PowerPoint's open its dialog.
  add(CmdHeaderFooter, icon::Calendar, S::Medium, T(L"Date & Time"), L"D").tool.checked = false;
  add(CmdHeaderFooter, icon::PageNumber, S::Medium, T(L"Slide Number"), L"SN").tool.checked = false;
  b.Group(T(L"Media"));  // P3: Insert's last group, as PowerPoint's
  add(CmdInsertVideo, 0, S::Large, T(L"Video"), L"V");
  add(CmdInsertAudio, 0, S::Large, T(L"Audio"), L"O");

  // ---- Design
  b.Tab(T(L"Design"), L"G");
  b.Group(T(L"Themes"), 3, 1);  // the gallery narrows before Variants and Customize
  {
    RibbonItem& themes = add(CmdDesignPanel, icon::Color, S::Large, T(L"Themes"), L"H");
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
  b.Group(T(L"Variants"));
  {
    RibbonItem& colors = add(CmdSlideColorsMenu, icon::Color, S::Large, T(L"Colors"), L"V", S::Large);
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
  b.Group(T(L"Customize"));
  add(CmdSlideSizeMenu, icon::SlideSize, S::Large, T(L"Slide\nSize"), L"S", S::Large);
  add(CmdFormatBackground, icon::FormatBackground, S::Large, T(L"Format\nBackground"), L"F", S::Large);

  // ---- Transitions and Animations (p3-anim, PresentationAnim.cpp)
  BuildAnimTabs(m);

  // ---- Slide Show
  b.Tab(T(L"Slide Show"), L"S");
  b.Group(T(L"Start Slide Show"));
  add(CmdSlideShowFromStart, icon::Slideshow, S::Large, T(L"From\nBeginning"), L"B");
  add(CmdSlideShowFromCurrent, icon::FromCurrentSlide, S::Large, T(L"From\nCurrent Slide"), L"C");
  b.Group(T(L"Set Up"));
  add(CmdSlideHide, icon::HideSlide, S::Large, T(L"Hide\nSlide"), L"H");
  // Monitors: Use Presenter View always (as PowerPoint); where the show goes with two monitors or more.
  b.Group(T(L"Monitors"));
  const std::vector<std::wstring> monitors = fw::slide::show::Monitors();
  if (monitors.size() >= 2) {
    RibbonItem& mon = add(CmdSlideMonitorMenu, 0, S::Small, T(L"Monitor"), L"M");
    mon.tool.kind = ToolKind::Dropdown;
    mon.tool.width = 150;
    mon.tool.choices = {T(L"Automatic")};
    mon.tool.choices.insert(mon.tool.choices.end(), monitors.begin(), monitors.end());
    mon.tool.choice = showMonitor_ + 1;
    mon.tool.onChange = [this](const ToolValue& v) {
      if (v.index >= 0) showMonitor_ = v.index - 1;
    };
  }
  add(CmdUsePresenterView, 0, S::Medium, T(L"Use Presenter View"), L"V", S::Medium);  // P3: a check box

  // ---- Review
  b.Tab(T(L"Review"), L"R");
  b.Group(T(L"Proofing"));
  add(CmdSpelling, icon::Spelling, S::Large, T(L"Spelling"), L"S");
  // p3-review: PowerPoint 365's Comments group.
  b.Group(T(L"Comments"));
  add(CmdNewComment, icon::Comment, S::Large, T(L"New\nComment"), L"C");
  split(CmdDeleteThread, icon::Delete, S::Large, T(L"Delete"), L"D", {CmdDeleteThread, CmdDeleteSlideComments, CmdDeleteAllComments});
  add(CmdPrevComment, icon::ChevronUp, S::Large, T(L"Previous"), L"V");
  add(CmdNextComment, icon::ChevronDown, S::Large, T(L"Next"), L"N");
  split(CmdReviewPane, icon::Comment, S::Large, T(L"Show\nComments"), L"H", {CmdReviewPane, CmdShowCommentMarkup});

  // ---- View
  b.Tab(T(L"View"), L"W");
  b.Group(T(L"Show"));
  add(CmdNotesPane, icon::TextDocument, S::Large, T(L"Notes"), L"PN");
  b.Group(T(L"Zoom"));
  add(CmdSlideZoomMenu, kZoom, S::Large, T(L"Zoom"), L"Q");
  add(CmdZoomFitPage, icon::FitToWindow, S::Large, T(L"Fit to\nWindow"), L"F");

  // ---- Contextual tabs, while such objects are selected
  const ObjectTool::Kinds kinds = Objects().SelectionKinds();
  auto arrange = [&] {
    b.Group(T(L"Arrange"));
    split(CmdBringForward, icon::BringForward, S::Medium, T(L"Bring Forward"), L"AF", {CmdBringForward, CmdBringToFront});
    split(CmdSendBackward, icon::SendBackward, S::Medium, T(L"Send Backward"), L"AE", {CmdSendBackward, CmdSendToBack});
    add(CmdSlideAlignMenu, icon::AlignObjects, S::Medium, T(L"Align"), L"AA").newColumn = true;
    add(CmdSlideGroupMenu, icon::Group, S::Medium, T(L"Group"), L"G");
    add(CmdSlideRotateMenu, icon::Rotate, S::Medium, T(L"Rotate"), L"AY");
  };
  if (kinds.shapes > 0) {
    b.Tab(T(L"Shape Format"), L"JD", true);
    b.Group(T(L"Insert Shapes"));
    shapes(add(CmdInsertShape, icon::Shapes, S::Medium, T(L"Shapes"), L"SH"));
    add(CmdInsertTextBox, icon::TextBox, S::Medium, T(L"Text Box"), L"X");
    b.Group(T(L"Shape Styles"));
    add(CmdObjectFill, 0, S::Medium, T(L"Shape Fill"), L"SF");
    add(CmdObjectOutline, 0, S::Medium, T(L"Shape Outline"), L"SO");
    add(CmdSlideEffectsMenu, icon::ShapeEffects, S::Medium, T(L"Shape Effects"), L"SE");
    add(CmdObjectOutlineWeight, 0, S::Medium, T(L"Weight"), L"SW").newColumn = true;
    arrange();
  }
  if (kinds.pictures > 0) {
    b.Tab(T(L"Picture Format"), L"JP", true);
    b.Group(T(L"Picture Styles"));
    add(CmdObjectOutline, 0, S::Medium, T(L"Picture Border"), L"SO");
    add(CmdObjectOutlineWeight, 0, S::Medium, T(L"Weight"), L"SW");
    add(CmdSlideEffectsMenu, icon::ShapeEffects, S::Medium, T(L"Picture Effects"), L"SE");
    arrange();
    b.Group(T(L"Size"));
    add(CmdPictureCrop, 0, S::Large, T(L"Crop"), L"V");
  }
  if (kinds.videos > 0) {  // P3: a video (only the media tabs, as PowerPoint)
    b.Tab(T(L"Video Format"), L"JV", true);
    b.Group(T(L"Video Styles"));
    add(CmdObjectOutline, 0, S::Medium, T(L"Video Border"), L"SO");
    add(CmdObjectOutlineWeight, 0, S::Medium, T(L"Weight"), L"SW");
    add(CmdSlideEffectsMenu, icon::ShapeEffects, S::Medium, T(L"Video Effects"), L"SE");
    arrange();
    b.Group(T(L"Size"));
    add(CmdPictureCrop, 0, S::Large, T(L"Crop"), L"V");
  }
  if (kinds.videos + kinds.audios == 1) {  // P3: one video or audio: its playback settings
    const bool video = kinds.videos == 1;
    b.Tab(T(L"Playback"), L"JN", true);
    b.Group(video ? T(L"Video Options") : T(L"Audio Options"));
    RibbonItem& volume = add(CmdMediaVolume, L'\xE767', S::Large, T(L"Volume"), L"V");
    volume.tool.dropdown = true;
    volume.tool.choices = {T(L"Low"), T(L"Medium"), T(L"High"), T(L"Mute")};
    fw::slide::edit::MediaPlayback set;
    if (Objects().SelectedPlayback(&set, nullptr))
      volume.tool.choice = set.mute ? 3 : set.volume < 0.35 ? 0 : set.volume < 0.65 ? 1 : 2;
    volume.tool.onChange = [this](const ToolValue& v) {
      fw::slide::edit::MediaPlayback p;
      if (v.index < 0 || v.index > 3 || !Objects().SelectedPlayback(&p, nullptr)) return;
      static const double kLevels[] = {0.2, 0.5, 0.8};  // Low, Medium, High: PowerPoint's (its UI probe, 24 Nov)
      p.mute = v.index == 3;
      if (v.index < 3) p.volume = kLevels[v.index];
      Objects().SetSelectedPlayback(p);
    };
    RibbonItem& start = add(CmdMediaStart, 0, S::Small, T(L"Start"), L"S");
    start.tool.kind = ToolKind::Dropdown;
    start.tool.width = 140;
    start.tool.choices = {T(L"In Click Sequence"), T(L"Automatically"), T(L"When Clicked On")};
    int startNow = 0;
    if (Objects().SelectedStart(&startNow)) start.tool.choice = startNow;
    start.tool.onChange = [this](const ToolValue& v) {
      if (v.index >= 0) Objects().SetSelectedStart(v.index);
    };
    add(CmdMediaLoop, 0, S::Medium, T(L"Loop until Stopped"), L"L", S::Medium);
    add(CmdMediaHide, 0, S::Medium, video ? T(L"Hide While Not Playing") : T(L"Hide During Show"), L"H", S::Medium);
    if (video) add(CmdMediaFullScreen, 0, S::Medium, T(L"Play Full Screen"), L"F", S::Medium);
    else add(CmdMediaAcrossSlides, 0, S::Medium, T(L"Play Across Slides"), L"A", S::Medium);
  }
  if (kinds.tables == 1) {
    b.Tab(T(L"Table Layout"), L"JL", true);
    b.Group(T(L"Rows & Columns"));
    add(CmdSlideTableDeleteMenu, icon::TableDelete, S::Large, T(L"Delete"), L"D");
    add(CmdTableRowAbove, icon::TableInsertAbove, S::Large, T(L"Insert\nAbove"), L"IA");  // "A" would cut off Arrange's AF, AE, AA, AY
    add(CmdTableRowBelow, icon::TableInsertBelow, S::Medium, T(L"Insert Below"), L"BE");
    add(CmdTableColumnLeft, icon::TableInsertLeft, S::Medium, T(L"Insert Left"), L"L");
    add(CmdTableColumnRight, icon::TableInsertRight, S::Medium, T(L"Insert Right"), L"R");
    b.Group(T(L"Merge"));
    add(CmdTableMergeCells, icon::MergeCells, S::Large, T(L"Merge\nCells"), L"M");
    add(CmdTableSplitCell, icon::SplitCells, S::Large, T(L"Split\nCells"), L"P");
    arrange();
  }
  // File › Print's settings, as PowerPoint's: what Print and Export PDF lay out on each page.
  struct PrintEntry {
    int cmd;
    UiText label;
    bool setting;
  };
  constexpr PrintEntry kPrint[] = {
      {CmdPrintSlides, N_(L"Full Page Slides"), false},    {CmdPrintNotes, N_(L"Notes Pages"), false},
      {CmdPrintHandouts1, N_(L"Handouts: 1 Slide"), false}, {CmdPrintHandouts2, N_(L"Handouts: 2 Slides"), false},
      {CmdPrintHandouts3, N_(L"Handouts: 3 Slides"), false}, {CmdPrintHandouts4, N_(L"Handouts: 4 Slides"), false},
      {CmdPrintHandouts6, N_(L"Handouts: 6 Slides"), false}, {CmdPrintHandouts9, N_(L"Handouts: 9 Slides"), false},
      {CmdPrintFrame, N_(L"Frame Slides"), true},           {CmdPrintHidden, N_(L"Print Hidden Slides"), true},
  };
  for (const PrintEntry& e : kPrint) {
    RibbonFileItem f;
    f.place = RibbonFilePlace::Print;
    f.cmd = e.cmd;
    f.label = e.label;
    f.setting = e.setting;
    f.choice = !e.setting;
    if (e.cmd == CmdPrintSlides) f.description = T(L"Export PDF lays out the pages the same way");
    m.file.push_back(std::move(f));
  }
  // p3-review: File > Info > Protect Presentation.
  {
    RibbonFileItem f;
    f.place = RibbonFilePlace::Info;
    f.cmd = CmdEncryptPassword;
    f.label = T(L"Protect Presentation: Encrypt with Password");
    f.description = file_ && file_->Protected() ? T(L"A password is required to open this presentation.")
                                                : T(L"Require a password to open this presentation.");
    f.glyph = icon::Lock;
    m.file.push_back(std::move(f));
  }
  // File › Export (P3, p3-files): the slides as PNG pictures.
  for (const auto& [cmd, about] : {std::pair{CmdExportSlidePng, T(L"The slide you're on, as a picture")},
                                   {CmdExportSlidesPng, T(L"Every slide as a picture, in a folder")}}) {
    RibbonFileItem f;
    f.place = RibbonFilePlace::Export;
    f.cmd = cmd;
    f.glyph = icon::Picture;
    f.description = about;
    m.file.push_back(std::move(f));
  }
  // A picture, shape or text box just inserted: its Format tab, as PowerPoint switches to it.
  if (const uint32_t id = Objects().TakeInserted()) {
    if (Objects().IsMedia(id)) {
      m.show = kinds.videos > 0 ? T(L"Video Format") : T(L"Playback");  // P3, as PowerPoint
    } else {
      m.show = Objects().IsPicture(id) ? T(L"Picture Format") : T(L"Shape Format");
    }
  }
}

}  // namespace fw::slide::view
