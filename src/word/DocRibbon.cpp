// The document's ribbon (plan §5.6, milestone R1): Word's tabs, groups and buttons, in Word's order
// and with Word's KeyTips (read from Word 365 itself, FeatherweightNotes\r1\r1-word-state.md), each
// running a command the document already has. Word's buttons with no Featherweight command are left
// out. Contextual tabs follow the selection: a table, a picture, a shape, a header or footer.
#include "word/DocView.h"

#include <cstdio>

#include "Commands.h"
#include "core/Lang.h"
#include "ui/Icons.h"
#include "ui/RibbonModel.h"

namespace {

// Segoe Fluent Icons glyphs (each in Segoe MDL2 Assets too) beyond ui/Icons.h; Word's own pictures
// with no glyph in the font are r1-control's drawn icons (icon::, kDrawnFirst on).
namespace glyph {
constexpr wchar_t Paste = 0xE77F, Cut = 0xE8C6, GrowFont = 0xE8E8, ShrinkFont = 0xE8E7, SelectAll = 0xE8B3,
                  Language = 0xF2B7, Panes = 0xE8F2, Track = 0xE932, Accept = 0xE8FB, Up = 0xE74A,
                  Down = 0xE74B, Refresh = 0xE72C, Zoom = 0xE71E, FitWidth = 0xE9A6, Reset = 0xE777,
                  CheckBox = 0xE739, Pilcrow = 0xE9AA;
}  // namespace glyph

using Size = RibbonSize;

// Adds tabs, groups and items to a model, with each item's state from the tab's command table.
class Builder {
 public:
  Builder(RibbonModel& m, std::function<CommandState(int)> query) : m_(m), query_(std::move(query)) {}

  void Tab(const wchar_t* name, const wchar_t* keyTip, bool contextual = false) {
    m_.tabs.push_back({name, keyTip, contextual, {}});
  }
  // A group of the last tab; `rows` 2 for Font and Paragraph.
  void Group(const wchar_t* name, const wchar_t* keyTip, int launcher = 0, int rows = 3) {
    RibbonGroup g;
    g.name = name;
    g.keyTip = keyTip;
    g.launcher = launcher;
    g.rows = rows;
    m_.tabs.back().groups.push_back(std::move(g));
  }
  // An item made elsewhere (a box, a color button, the table grid).
  RibbonItem& Add(ToolItem t, Size size, const wchar_t* label, const wchar_t* keyTip) {
    RibbonItem it;
    it.tool = std::move(t);
    it.size = size;
    it.label = label;
    it.keyTip = keyTip;
    auto& items = m_.tabs.back().groups.back().items;
    items.push_back(std::move(it));
    return items.back();
  }
  // A button running `cmd`, enabled as the command is.
  RibbonItem& Button(int cmd, wchar_t glyph, Size size, const wchar_t* label, const wchar_t* keyTip) {
    const CommandState q = query_(cmd);
    ToolItem t;
    t.cmd = cmd;
    t.glyph = glyph;
    t.enabled = q.enabled;
    return Add(std::move(t), size, label, keyTip);
  }
  // A button that stays on while its command is (Bold, Track Changes, a pane shown).
  RibbonItem& Toggle(int cmd, wchar_t glyph, Size size, const wchar_t* label, const wchar_t* keyTip) {
    RibbonItem& it = Button(cmd, glyph, size, label, keyTip);
    it.tool.toggle = true;
    it.tool.checked = query_(cmd).checked;
    return it;
  }
  // A button that opens a menu of commands (0: a separator).
  RibbonItem& Menu(int cmd, wchar_t glyph, Size size, const wchar_t* label, const wchar_t* keyTip, std::vector<int> menu) {
    RibbonItem& it = Button(cmd, glyph, size, label, keyTip);
    it.tool.dropdown = true;
    it.tool.tooltip = label;
    it.menu = std::move(menu);
    return it;
  }
  // A split button: its main part runs `cmd`, its arrow opens `menu`.
  RibbonItem& Split(int cmd, wchar_t glyph, Size size, const wchar_t* label, const wchar_t* keyTip, std::vector<int> menu) {
    RibbonItem& it = Button(cmd, glyph, size, label, keyTip);
    it.tool.kind = ToolKind::SplitButton;
    it.menu = std::move(menu);
    return it;
  }
  // The same in the previous item's row.
  RibbonItem& Row(RibbonItem& it) {
    it.sameRow = true;
    return it;
  }

 private:
  RibbonModel& m_;
  std::function<CommandState(int)> query_;
};

// A box shown greyed in a document that can't be edited (no formatting there to fill it from).
ToolItem Greyed(ToolKind kind, int cmd) {
  ToolItem t;
  t.kind = kind;
  t.cmd = cmd;
  t.enabled = false;
  return t;
}

}  // namespace

void DocView::BuildRibbon(RibbonModel& m, const ToolbarContext& ctx) {
  if (!Ready()) return;  // loading, or the password card: the toolbar
  SyncPicturePanel();  // the Picture and Shape panels follow the selection
  SyncShapePanels();
  Builder b(m, [this](int cmd) { return QueryCommand(cmd); });
  const bool editing = Editing();
  auto width = [](ToolItem t, float w) {
    t.width = w;
    return t;
  };

  // ---- Home
  b.Tab(T(L"Home"), L"H");
  b.Group(T(L"Clipboard"), L"ZL");
  b.Split(CmdPaste, glyph::Paste, Size::Large, T(L"Paste"), L"V", {CmdPaste, CmdPasteMerge}).smallest = Size::Large;
  b.Button(CmdCut, glyph::Cut, Size::Medium, T(L"Cut"), L"X");
  b.Button(CmdCopy, icon::Copy, Size::Medium, T(L"Copy"), L"C");
  b.Toggle(CmdFormatPainter, icon::Brush, Size::Medium, T(L"Format Painter"), L"FP");

  b.Group(T(L"Font"), L"ZF", 0, 2);
  b.Add(width(format_ ? format_->FontBox() : Greyed(ToolKind::ComboBox, CmdFontBox), 128), Size::Small, T(L"Font"), L"FF");
  b.Row(b.Add(width(format_ ? format_->SizeBox() : Greyed(ToolKind::ComboBox, CmdSizeBox), 60), Size::Small, T(L"Font Size"), L"FS"));
  b.Row(b.Button(CmdGrowFont, glyph::GrowFont, Size::Small, T(L"Grow Font"), L"FG"));
  b.Row(b.Button(CmdShrinkFont, glyph::ShrinkFont, Size::Small, T(L"Shrink Font"), L"FK"));
  b.Row(b.Menu(CmdCaseBox, icon::FontCase, Size::Small, T(L"Change Case"), L"7",
               {CmdCaseSentence, CmdCaseLower, CmdCaseUpper, CmdCaseCapitalize, CmdCaseToggle}));
  b.Row(b.Button(CmdClearAllFormat, icon::ClearFormatting, Size::Small, T(L"Clear All Formatting"), L"E"));
  b.Toggle(CmdBold, icon::Bold, Size::Small, T(L"Bold"), L"1");
  b.Row(b.Toggle(CmdItalic, icon::Italic, Size::Small, T(L"Italic"), L"2"));
  b.Row(b.Toggle(CmdUnderline, icon::Underline, Size::Small, T(L"Underline"), L"3"));
  b.Row(b.Toggle(CmdStrike, icon::Strikethrough, Size::Small, T(L"Strikethrough"), L"4"));
  b.Row(b.Toggle(CmdSubscript, icon::Subscript, Size::Small, T(L"Subscript"), L"5"));
  b.Row(b.Toggle(CmdSuperscript, icon::Superscript, Size::Small, T(L"Superscript"), L"6"));
  if (format_) {
    b.Row(b.Add(format_->ColorButton(true), Size::Small, T(L"Text Highlight Color"), L"I"));
    b.Row(b.Add(format_->ColorButton(false), Size::Small, T(L"Font Color"), L"FC"));
  }

  b.Group(T(L"Paragraph"), L"ZP", CmdParagraph, 2);
  b.Toggle(CmdBullets, icon::BulletedList, Size::Small, T(L"Bullets"), L"U");
  b.Row(b.Split(CmdNumbering, icon::NumberedList, Size::Small, T(L"Numbering"), L"N",
                {CmdNumbering, CmdRestartNumbering, CmdContinueNumbering}))
      .tool.checked = QueryCommand(CmdNumbering).checked;
  b.Row(b.Button(CmdMultilevel, icon::MultilevelList, Size::Small, T(L"Multilevel List"), L"M"));
  b.Row(b.Button(CmdOutdent, icon::DecreaseIndent, Size::Small, T(L"Decrease Indent"), L"AO"));
  b.Row(b.Button(CmdIndent, icon::IncreaseIndent, Size::Small, T(L"Increase Indent"), L"AI"));
  b.Row(b.Toggle(CmdShowMarks, glyph::Pilcrow, Size::Small, L"Show/Hide ¶", L"8"));
  b.Toggle(CmdAlignLeft, icon::AlignLeft, Size::Small, T(L"Align Left"), L"AL");
  b.Row(b.Toggle(CmdAlignCenter, icon::AlignCenter, Size::Small, T(L"Center"), L"AC"));
  b.Row(b.Toggle(CmdAlignRight, icon::AlignRight, Size::Small, T(L"Align Right"), L"AR"));
  b.Row(b.Toggle(CmdAlignJustify, icon::AlignJustify, Size::Small, T(L"Justify"), L"AJ"));
  // CmdAlignBox: the toolbar's paragraph menu once, Word's Line and Paragraph Spacing menu now.
  b.Row(b.Menu(CmdAlignBox, icon::LineSpacing, Size::Small, T(L"Line and Paragraph Spacing"), L"K",
               {CmdSpacing1, CmdSpacing115, CmdSpacing15, CmdSpacing2, 0, CmdParagraph, CmdSpaceBefore}));
  b.Row(b.Split(CmdShading, icon::FillBucket, Size::Small, T(L"Shading"), L"H", {CmdShading, CmdShadingNone}));
  b.Row(b.Split(CmdBorderBottom, icon::BordersBottom, Size::Small, T(L"Borders"), L"B", {CmdBorderBottom, CmdBorderBox, CmdBorderNone}));

  b.Group(T(L"Styles"), L"ZS");
  {
    RibbonItem& styles = b.Add(format_ ? format_->StyleBox() : Greyed(ToolKind::Dropdown, CmdStyleBox), Size::Large, T(L"Styles"), L"L");
    styles.tool.glyph = icon::FontCase;  // the gallery folded into one button (a narrow window)
    RibbonGallery g;
    g.columns = 7;  // five at its middle width (1366 px), where Word's first row also reaches Title (RF7)
    g.popupColumns = 6;
    g.choiceWidth = 84;
    if (format_)
      g.drawChoice = [this](gfx::Canvas& c, const D2D1_RECT_F& r, int i, float ui) { format_->StylePreview(c, r, i, ui); };
    g.menu = {CmdNewStyle, CmdModifyStyle, CmdClearAllFormat};
    styles.gallery = std::move(g);
  }

  b.Group(T(L"Editing"), L"ZN");
  b.Split(CmdFind, icon::Search, Size::Medium, T(L"Find"), L"FD", {CmdFind, CmdGoToPage}).tool.enabled = true;
  b.Button(CmdReplace, icon::Replace, Size::Medium, T(L"Replace"), L"R").tool.enabled = editing;
  b.Button(CmdSelectAll, glyph::SelectAll, Size::Medium, T(L"Select All"), L"SL");

  // ---- Insert
  b.Tab(T(L"Insert"), L"N");
  b.Group(T(L"Pages"), L"ZP");
  b.Button(CmdPageBreak, icon::PageBreak, Size::Large, T(L"Page\nBreak"), L"B");
  b.Group(T(L"Tables"), L"ZB");
  if (table_) b.Add(table_->GridItem(), Size::Large, T(L"Table"), L"T");
  else b.Button(CmdInsertTable, icon::Table, Size::Large, T(L"Table"), L"T");
  b.Group(T(L"Illustrations"), L"ZI");
  b.Button(CmdInsertPicture, icon::Picture, Size::Large, T(L"Pictures"), L"P");
  const std::vector<int> shapes = {CmdInsertRectangle, CmdInsertRoundedRectangle, CmdInsertOval,
                                   CmdInsertLine,      CmdInsertArrow,            CmdInsertCallout};
  b.Menu(CmdInsertRectangle, icon::Shapes, Size::Large, T(L"Shapes"), L"SH", shapes);
  b.Group(T(L"Links"), L"ZL");
  b.Button(CmdInsertLink, icon::Link, Size::Large, T(L"Link"), L"I");
  b.Button(CmdBookmark, icon::Bookmark, Size::Large, T(L"Bookmark"), L"K");
  b.Group(T(L"Comments"), L"Z1");
  b.Button(CmdNewComment, icon::Comment, Size::Large, T(L"Comment"), L"L");
  b.Group(T(L"Header & Footer"), L"ZH");
  b.Button(CmdEditHeader, icon::Header, Size::Large, T(L"Header"), L"H");
  b.Button(CmdEditFooter, icon::Footer, Size::Large, T(L"Footer"), L"O");
  b.Button(CmdPageNumber, icon::PageNumber, Size::Large, T(L"Page\nNumber"), L"NU");
  b.Group(T(L"Text"), L"ZT");
  b.Button(CmdInsertTextBox, icon::TextBox, Size::Large, T(L"Text\nBox"), L"X");
  if (format_) b.Add(format_->DateBox(), Size::Medium, T(L"Date & Time"), L"D");

  // ---- Design
  b.Tab(T(L"Design"), L"G");
  b.Group(T(L"Page Background"), L"ZB");
  b.Menu(CmdWatermarkPanel, icon::Watermark, Size::Large, T(L"Watermark"), L"PW", {CmdWatermarkPanel, CmdWatermarkRemove});

  // ---- Layout
  const uint32_t d = editing ? SelectedPicture() : fw::doc::kNone;
  const bool object = d != fw::doc::kNone && Movable(d);
  const bool picture = object && PictureInfoOf(d).picture, shape = object && !picture;
  const int objectPanel = shape ? CmdShapePanel : CmdPicturePanel;
  auto arrange = [&] {
    b.Group(T(L"Arrange"), L"ZA");
    b.Menu(CmdPictureWrapBox, icon::WrapSquare, Size::Large, T(L"Wrap\nText"), L"TW",
           {CmdPictureInline, CmdPictureSquare, CmdPictureTight, CmdPictureTopBottom, CmdPictureBehind, CmdPictureInFront});
    b.Menu(CmdPictureRotate, icon::Rotate, Size::Medium, T(L"Rotate"), L"AY", {CmdPictureRotate, 0, objectPanel});
  };
  b.Tab(T(L"Layout"), L"P");
  b.Group(T(L"Page Setup"), L"ZS", CmdPageSetup);
  b.Menu(CmdMarginsNormal, icon::Margins, Size::Large, T(L"Margins"), L"M",
         {CmdMarginsNormal, CmdMarginsNarrow, CmdMarginsModerate, CmdMarginsWide, 0, CmdPageSetup});
  b.Menu(CmdPortrait, icon::Orientation, Size::Large, T(L"Orientation"), L"O", {CmdPortrait, CmdLandscape});
  if (page_) b.Add(page_->SizeMenu(), Size::Large, T(L"Size"), L"SZ").tool.glyph = icon::PaperSize;
  b.Menu(CmdColumnsOne, icon::Columns, Size::Large, T(L"Columns"), L"J", {CmdColumnsOne, CmdColumnsTwo, CmdColumnsThree, 0, CmdPageSetup});
  b.Menu(CmdPageBreak, icon::PageBreak, Size::Medium, T(L"Breaks"), L"B",
         {CmdPageBreak, CmdColumnBreak, 0, CmdSectionNextPage, CmdSectionContinuous, CmdSectionEvenPage, CmdSectionOddPage});
  b.Menu(CmdHyphenation, icon::Hyphenation, Size::Medium, T(L"Hyphenation"), L"H", {CmdHyphenation, 0, CmdOptionalHyphen, CmdNonBreakingHyphen});
  if (page_) {
    b.Group(T(L"Paragraph"), L"ZG", CmdParagraph, 2);
    b.Add(page_->ParaSpin(CmdIndentLeftBox), Size::Medium, T(L"Left"), L"IL");
    b.Add(page_->ParaSpin(CmdIndentRightBox), Size::Medium, T(L"Right"), L"IR");
    b.Add(page_->ParaSpin(CmdSpaceBeforeBox), Size::Medium, T(L"Before"), L"SB").newColumn = true;
    b.Add(page_->ParaSpin(CmdSpaceAfterBox), Size::Medium, T(L"After"), L"SA");
  }
  arrange();

  // ---- References
  b.Tab(T(L"References"), L"S");
  b.Group(T(L"Table of Contents"), L"ZC");
  b.Button(CmdInsertToc, icon::TableOfContents, Size::Large, T(L"Table of\nContents"), L"T");
  b.Split(CmdUpdateToc, glyph::Refresh, Size::Medium, T(L"Update Table"), L"U", {CmdUpdateTocPages, CmdUpdateToc});
  b.Group(T(L"Footnotes"), L"ZF");
  b.Button(CmdInsertFootnote, icon::Footnote, Size::Large, T(L"Insert\nFootnote"), L"F");
  b.Button(CmdInsertEndnote, icon::Endnote, Size::Medium, T(L"Insert Endnote"), L"E");

  // ---- Review
  b.Tab(T(L"Review"), L"R");
  b.Group(T(L"Proofing"), L"ZP");
  b.Button(CmdSpelling, icon::Spelling, Size::Large, T(L"Spelling &\nGrammar"), L"S");
  b.Button(CmdWordCount, icon::WordCount, Size::Large, T(L"Word\nCount"), L"W");
  b.Group(T(L"Language"), L"ZL");
  b.Button(CmdLanguage, glyph::Language, Size::Large, T(L"Language"), L"U");
  b.Group(T(L"Comments"), L"ZC");
  b.Button(CmdNewComment, icon::Comment, Size::Large, T(L"New\nComment"), L"C");
  b.Group(T(L"Markup"), L"ZM", CmdReviewerName);
  {
    ToolItem show;
    show.kind = ToolKind::Dropdown;
    show.cmd = CmdMarkupSimple;
    show.width = 120;
    show.enabled = Ready();
    show.tooltip = T(L"Display for Review");
    for (int cmd = CmdMarkupSimple; cmd <= CmdMarkupOriginal; ++cmd) {
      const CommandState q = QueryCommand(cmd);
      show.choices.push_back(q.label);
      if (q.checked) show.choice = cmd - CmdMarkupSimple;
    }
    show.onChange = [this](const ToolValue& v) {
      if (v.index >= 0 && v.index <= CmdMarkupOriginal - CmdMarkupSimple) Execute(CmdMarkupSimple + v.index);
    };
    b.Add(std::move(show), Size::Medium, T(L"Display for Review"), L"TD");
  }
  b.Toggle(CmdReviewPane, glyph::Panes, Size::Medium, T(L"Reviewing Pane"), L"TP");
  b.Group(T(L"Tracking"), L"ZT");
  b.Toggle(CmdTrackChanges, glyph::Track, Size::Large, T(L"Track\nChanges"), L"G");
  b.Split(CmdAcceptChange, glyph::Accept, Size::Large, T(L"Accept"), L"A2", {CmdAcceptChange, CmdAcceptAll});
  b.Split(CmdRejectChange, icon::Cancel, Size::Large, T(L"Reject"), L"J", {CmdRejectChange, CmdRejectAll});
  b.Button(CmdPrevChange, glyph::Up, Size::Medium, T(L"Previous"), L"F");
  b.Button(CmdNextChange, glyph::Down, Size::Medium, T(L"Next"), L"H1");

  // ---- View
  b.Tab(T(L"View"), L"W");
  b.Group(T(L"Views"), L"ZV");
  b.Button(CmdPresentation, icon::Slideshow, Size::Large, T(L"Present"), L"F").tool.enabled = true;
  b.Group(T(L"Immersive"), L"Z1");
  RibbonItem& full = b.Toggle(CmdFullScreen, icon::FullScreen, Size::Large, T(L"Full\nScreen"), L"O");
  full.tool.enabled = true;
  full.tool.checked = ctx.fullscreen;
  RibbonItem& night = b.Toggle(CmdToggleInvert, icon::QuietHours, Size::Large, T(L"Night\nReading"), L"D");
  night.tool.enabled = true;
  night.tool.checked = ctx.inverted;
  b.Group(T(L"Page Movement"), L"Z2");
  b.Toggle(CmdViewContinuous, icon::ViewContinuous, Size::Large, T(L"Vertical"), L"C");
  b.Toggle(CmdViewSingle, icon::ViewSingle, Size::Large, T(L"Side\nto Side"), L"H");
  b.Group(T(L"Show"), L"ZS");
  RibbonItem& ruler = b.Toggle(CmdRuler, 0, Size::Medium, T(L"Ruler"), L"R");
  ruler.tool.enabled = editing;
  ruler.tool.checked = host_.ViewRuler();
  b.Toggle(CmdNavigationPane, 0, Size::Medium, T(L"Navigation Pane"), L"K");
  b.Group(T(L"Zoom"), L"ZZ");
  b.Menu(CmdZoomMenu, glyph::Zoom, Size::Large, T(L"Zoom"), L"Q", {}).tool.enabled = true;
  b.Button(CmdZoomActual, icon::Zoom100, Size::Large, L"100%", L"J");
  b.Toggle(CmdZoomFitPage, icon::Page, Size::Medium, T(L"One Page"), L"1");
  b.Toggle(CmdViewFacing, icon::ViewFacing, Size::Medium, T(L"Multiple Pages"), L"2");
  b.Toggle(CmdZoomFitWidth, glyph::FitWidth, Size::Medium, T(L"Page Width"), L"I");

  // ---- Developer: content controls, which Word has only here. Hidden until File > Options > Show
  // Developer tab, as in Word; Alt+Q finds them either way.
  if (host_.ViewDeveloperTab() || ribbonAll_) {
    b.Tab(T(L"Developer"), L"L");
    b.Group(T(L"Controls"), L"ZC");
    b.Button(CmdInsertRichText, icon::FontCase, Size::Small, T(L"Rich Text Content Control"), L"R");
    b.Button(CmdInsertPlainText, glyph::GrowFont, Size::Small, T(L"Plain Text Content Control"), L"P");
    b.Button(CmdInsertCheckBox, glyph::CheckBox, Size::Small, T(L"Check Box Content Control"), L"K");
    b.Button(CmdInsertDropDown, icon::DropDownList, Size::Small, T(L"Drop-Down List Content Control"), L"Q");
    b.Button(CmdInsertDatePicker, icon::Calendar, Size::Small, T(L"Date Picker Content Control"), L"D");
  }

  // ---- Contextual tabs
  // ribbon:check builds every contextual tab, to walk their commands too.
  if (table_ && ((editing && table_->InTable()) || ribbonAll_)) {
    b.Tab(T(L"Table Design"), L"JT", true);
    b.Group(T(L"Table Styles"), L"ZS");
    b.Add(table_->ShadingButton(), Size::Large, T(L"Shading"), L"H").tool.glyph = icon::FillBucket;
    b.Group(T(L"Borders"), L"ZB", CmdTableProperties);
    b.Add(table_->BordersButton(), Size::Large, T(L"Borders"), L"B").tool.glyph = icon::BordersAll;

    b.Tab(T(L"Table Layout"), L"JL", true);
    b.Group(T(L"Table"), L"ZT");
    b.Menu(CmdTableSelectCell, glyph::SelectAll, Size::Large, T(L"Select"), L"K",
           {CmdTableSelectCell, CmdTableSelectColumn, CmdTableSelectRow, CmdTableSelectTable});
    b.Button(CmdTableProperties, icon::Settings, Size::Large, T(L"Properties"), L"O");
    b.Group(T(L"Rows & Columns"), L"ZR");
    b.Menu(CmdTableDeleteColumns, icon::TableDelete, Size::Large, T(L"Delete"), L"D",
           {CmdTableDeleteColumns, CmdTableDeleteRows, CmdTableDeleteTable});
    b.Button(CmdTableInsertAbove, icon::TableInsertAbove, Size::Large, T(L"Insert\nAbove"), L"A");
    b.Button(CmdTableInsertBelow, icon::TableInsertBelow, Size::Large, T(L"Insert\nBelow"), L"BE");
    b.Button(CmdTableInsertLeft, icon::TableInsertLeft, Size::Large, T(L"Insert\nLeft"), L"L");
    b.Button(CmdTableInsertRight, icon::TableInsertRight, Size::Large, T(L"Insert\nRight"), L"R");
    b.Group(T(L"Merge"), L"ZM");
    b.Button(CmdTableMerge, icon::MergeCells, Size::Large, T(L"Merge\nCells"), L"M");
    b.Button(CmdTableSplit, icon::SplitCells, Size::Large, T(L"Split\nCells"), L"P");
    b.Group(T(L"Cell Size"), L"ZS", CmdTableProperties, 2);
    b.Menu(CmdTableAutoFitContents, icon::AutoFit, Size::Large, L"AutoFit", L"F",
           {CmdTableAutoFitContents, CmdTableAutoFitWindow, CmdTableFixedWidth});
    b.Add(table_->SizeBox(true), Size::Medium, T(L"Height"), L"H");
    b.Add(table_->SizeBox(false), Size::Medium, T(L"Width"), L"W");
    b.Button(CmdTableDistributeColumns, icon::DistributeColumns, Size::Medium, T(L"Distribute Columns"), L"UC").newColumn = true;
    b.Group(T(L"Data"), L"ZD2");
    b.Toggle(CmdTableHeaderRow, icon::RepeatHeader, Size::Large, T(L"Repeat\nHeader Rows"), L"J");
    b.Button(CmdTableToText, icon::ConvertToText, Size::Large, T(L"Convert\nto Text"), L"V");
  }
  if (picture || ribbonAll_) {
    b.Tab(T(L"Picture Format"), L"JP", true);
    b.Group(T(L"Adjust"), L"ZT");
    b.Button(CmdPictureReset, glyph::Reset, Size::Medium, T(L"Reset Picture"), L"Q");
    b.Group(T(L"Accessibility"), L"Z1");
    b.Toggle(CmdPicturePanel, icon::AltText, Size::Large, T(L"Alt\nText"), L"AT");
    arrange();
    b.Group(T(L"Size"), L"ZZ", CmdPicturePanel, 2);
    b.Toggle(CmdPictureCrop, icon::Crop, Size::Large, T(L"Crop"), L"V");
    b.Add(ObjectSizeBox(true), Size::Medium, T(L"Height"), L"H");
    b.Add(ObjectSizeBox(false), Size::Medium, T(L"Width"), L"W");
  }
  if (shape || ribbonAll_) {
    b.Tab(T(L"Shape Format"), L"JD", true);
    b.Group(T(L"Insert Shapes"), L"ZH");
    b.Menu(CmdInsertRectangle, icon::Shapes, Size::Large, T(L"Shapes"), L"SH", shapes);
    b.Button(CmdInsertTextBox, icon::TextBox, Size::Medium, T(L"Draw Text Box"), L"X");
    b.Group(T(L"Shape Styles"), L"ZY", CmdShapePanel);
    b.Toggle(CmdShapePanel, icon::Settings, Size::Large, T(L"Format\nShape"), L"O");
    arrange();
    b.Group(T(L"Size"), L"ZZ", CmdShapePanel, 2);
    b.Add(ObjectSizeBox(true), Size::Medium, T(L"Height"), L"H");
    b.Add(ObjectSizeBox(false), Size::Medium, T(L"Width"), L"W");
  }
  if ((editing && hf_) || ribbonAll_) {
    b.Tab(T(L"Header & Footer"), L"JH", true);
    b.Group(T(L"Header & Footer"), L"ZH");
    b.Button(CmdPageNumber, icon::PageNumber, Size::Large, T(L"Page\nNumber"), L"NU");
    b.Group(T(L"Insert"), L"ZI");
    if (format_) b.Add(format_->DateBox(), Size::Large, T(L"Date &\nTime"), L"D");
    b.Button(CmdInsertPicture, icon::Picture, Size::Large, T(L"Pictures"), L"P");
    b.Group(T(L"Navigation"), L"ZN");
    b.Toggle(CmdLinkToPrevious, icon::Link, Size::Medium, T(L"Link to Previous"), L"K");
    b.Group(T(L"Options"), L"ZO");
    b.Toggle(CmdDifferentFirstPage, 0, Size::Medium, T(L"Different First Page"), L"A");
    b.Toggle(CmdDifferentOddEven, 0, Size::Medium, T(L"Different Odd & Even Pages"), L"V");
    b.Group(T(L"Close"), L"ZC");
    b.Button(CmdCloseHeaderFooter, icon::Cancel, Size::Large, T(L"Close Header\nand Footer"), L"C");
  }
  // A picture or shape just inserted shows its tab, as in Word; selecting one later doesn't.
  if (!object || d != ribbonInserted_) ribbonInserted_ = fw::doc::kNone;
  else m.show = picture ? T(L"Picture Format") : T(L"Shape Format");

  // ---- The File page: Word's proofing and user name options.
  // (Spelling and AutoCorrect as you type are the File page's own, under Editing.)
  m.file = {{RibbonFilePlace::Options, CmdDeveloperTab, T(L"Show Developer tab"), T(L"Content controls: text, check boxes, lists and dates"), 0, true},
            {RibbonFilePlace::Options, CmdReviewerName, T(L"User name…"), T(L"The name your changes and comments carry"), 0}};
}

ToolItem DocView::ObjectSizeBox(bool height) {
  wchar_t measure[4] = L"";
  const bool metric = !(GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_IMEASURE, measure, 4) > 0 && measure[0] == L'1');
  const double per = metric ? 360000.0 : 914400.0;  // EMUs in a centimeter or an inch
  ToolItem t;
  t.kind = ToolKind::SpinBox;
  t.cmd = height ? CmdObjectHeightBox : CmdObjectWidthBox;
  t.width = 100;  // "15,24 cm" beside the spin arrows
  t.unit = metric ? L" cm" : L"\"";
  t.minValue = 0.01;
  t.maxValue = metric ? 55.87 : 22;  // Word's largest
  t.step = metric ? 0.1 : 0.05;
  t.decimals = 2;
  const uint32_t d = SelectedPicture();
  t.enabled = Editing() && d != fw::doc::kNone && Movable(d);
  if (!t.enabled) return t;
  const bool picture = PictureInfoOf(d).picture;
  t.value = static_cast<double>(height ? (picture ? PictureInfoOf(d).cy : ShapeInfoOf(d).cy).Value()
                                       : (picture ? PictureInfoOf(d).cx : ShapeInfoOf(d).cx).Value()) / per;
  t.onChange = [this, height, per](const ToolValue& v) {
    const uint32_t p = SelectedPicture();
    if (p == fw::doc::kNone || v.number <= 0) return;
    const bool pic = PictureInfoOf(p).picture;
    double w = static_cast<double>((pic ? PictureInfoOf(p).cx : ShapeInfoOf(p).cx).Value());
    double h = static_cast<double>((pic ? PictureInfoOf(p).cy : ShapeInfoOf(p).cy).Value());
    const double now = v.number * per;
    if (pic && w > 0 && h > 0) {  // as Word's locked aspect ratio, a picture's default
      (height ? w : h) *= now / (height ? h : w);
    }
    (height ? h : w) = now;
    const fw::Emu cx(std::llround(w)), cy(std::llround(h));
    ApplyPicture(pic ? pictures_->Resize(p, cx, cy) : shapes_->Resize(p, cx, cy));
  };
  return t;
}

// ---------------------------------------------------------------------------
// Script steps (checks and snapshots): test builds only (FW_TEST_SCREENS), as the ribbon's own.

bool DocView::ScriptOp(const std::wstring& op, const std::wstring& arg) {
#if !FW_TEST_SCREENS
  (void)op;
  (void)arg;
  return false;
#else
  if (op != L"docribbon") return false;
  if (arg == L"all") {
    // docribbon:all: every contextual tab from now on, whatever is selected (for ribboncheck and
    // the contextual tabs' snapshots).
    ribbonAll_ = true;
    host_.ViewStateChanged();
    return true;
  }
  if (arg.rfind(L"print=", 0) == 0) {
    // docribbon:print=<command>: the item's value now, as "docribbon: <command> = <value>".
    const int cmd = _wtoi(arg.c_str() + 6);
    RibbonModel m;
    BuildRibbon(m, ToolbarContext{});
    for (const RibbonTab& t : m.tabs)
      for (const RibbonGroup& g : t.groups)
        for (const RibbonItem& it : g.items)
          if (it.tool.cmd == cmd) {
            fwprintf(stderr, L"docribbon: %d = %s\n", cmd,
                     it.tool.kind == ToolKind::SpinBox ? std::to_wstring(it.tool.value).c_str() : it.tool.text.c_str());
            return true;
          }
    return true;
  }
  if (arg.rfind(L"set=", 0) == 0) {
    // docribbon:set=<command>,<value>: as if the item's box were set to the number, or its list's
    // choice picked.
    const size_t comma = arg.find(L',');
    const int cmd = _wtoi(arg.c_str() + 4);
    const std::wstring value = comma == std::wstring::npos ? std::wstring() : arg.substr(comma + 1);
    RibbonModel m;
    BuildRibbon(m, ToolbarContext{});
    for (const RibbonTab& t : m.tabs)
      for (const RibbonGroup& g : t.groups)
        for (const RibbonItem& it : g.items)
          if (it.tool.cmd == cmd && it.tool.onChange) {
            ToolValue v;
            v.number = _wtof(value.c_str());
            v.index = _wtoi(value.c_str());
            v.text = value;
            it.tool.onChange(v);
            return true;
          }
    fwprintf(stderr, L"docribbon: no box %d\n", cmd);
    return true;
  }
  return false;
#endif
}
