// The document's ribbon (plan §5.6, milestone R1): Word's tabs, groups and buttons, in Word's order
// and with Word's KeyTips (read from Word 365 itself, FeatherweightNotes\r1\r1-word-state.md), each
// running a command the document already has. Word's buttons with no Featherweight command are left
// out. Contextual tabs follow the selection: a table, a picture, a shape, a header or footer.
#include "word/DocView.h"

#include <algorithm>
#include <cstdio>
#include <set>

#include "Commands.h"
#include "ui/Icons.h"
#include "ui/RibbonModel.h"

namespace {

// Segoe Fluent Icons glyphs (each in Segoe MDL2 Assets too) beyond ui/Icons.h.
namespace glyph {
constexpr wchar_t Paste = 0xE77F, Cut = 0xE8C6, GrowFont = 0xE8E8, ShrinkFont = 0xE8E7, ClearFormat = 0xE75C,
                  Strike = 0xEDE0, Replace = 0xE8AB, SelectAll = 0xE8B3, Spelling = 0xF87B, Language = 0xF2B7,
                  Panes = 0xE8F2, Track = 0xE932, Accept = 0xE8FB, Up = 0xE74A, Down = 0xE74B, Refresh = 0xE72C,
                  TextBox = 0xE8AC, Zoom = 0xE71E, FitWidth = 0xE9A6, Portrait = 0xF56E, Reset = 0xE777,
                  CheckBox = 0xE739;
// Word's own pictures with no glyph in the font; the nearest until ui/Icons.h draws them (r1-control).
constexpr wchar_t PageBreak = 0xF56F, Shapes = 0xE8A9, Bookmark = 0xE8A4, Header = 0xE8A1, PageNumber = 0xF146,
                  Watermark = 0xE790, Margins = 0xE9A6, Size = 0xE7C3, Columns = 0xE89A, Breaks = 0xF56F,
                  Hyphen = 0xE8E9, Wrap = 0xE8A1, Toc = 0xE8C4, Footnote = 0xE8E9, WordCount = 0xE8EF,
                  Multilevel = 0xEA37, Spacing = 0xE14C, Shading = 0xE790, Borders = 0xE80A, Insert = 0xE710,
                  Merge = 0xE8A9, Fit = 0xE9A6, List = 0xE8FD;
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
  Builder b(m, [this](int cmd) { return QueryCommand(cmd); });
  const bool editing = Editing();
  auto width = [](ToolItem t, float w) {
    t.width = w;
    return t;
  };

  // ---- Home
  b.Tab(L"Home", L"H");
  b.Group(L"Clipboard", L"ZL");
  b.Split(CmdPaste, glyph::Paste, Size::Large, L"Paste", L"V", {CmdPaste, CmdPasteMerge}).smallest = Size::Large;
  b.Button(CmdCut, glyph::Cut, Size::Medium, L"Cut", L"X");
  b.Button(CmdCopy, icon::Copy, Size::Medium, L"Copy", L"C");
  b.Toggle(CmdFormatPainter, icon::Brush, Size::Medium, L"Format Painter", L"FP");

  b.Group(L"Font", L"ZF", 0, 2);
  b.Add(width(format_ ? format_->FontBox() : Greyed(ToolKind::ComboBox, CmdFontBox), 128), Size::Small, L"Font", L"FF");
  b.Row(b.Add(width(format_ ? format_->SizeBox() : Greyed(ToolKind::ComboBox, CmdSizeBox), 48), Size::Small, L"Font Size", L"FS"));
  b.Row(b.Button(CmdGrowFont, glyph::GrowFont, Size::Small, L"Grow Font", L"FG"));
  b.Row(b.Button(CmdShrinkFont, glyph::ShrinkFont, Size::Small, L"Shrink Font", L"FK"));
  b.Row(b.Menu(CmdCaseBox, icon::FontCase, Size::Small, L"Change Case", L"7",
               {CmdCaseSentence, CmdCaseLower, CmdCaseUpper, CmdCaseCapitalize, CmdCaseToggle}));
  b.Row(b.Button(CmdClearAllFormat, glyph::ClearFormat, Size::Small, L"Clear All Formatting", L"E"));
  b.Toggle(CmdBold, icon::Bold, Size::Small, L"Bold", L"1");
  b.Row(b.Toggle(CmdItalic, icon::Italic, Size::Small, L"Italic", L"2"));
  b.Row(b.Toggle(CmdUnderline, icon::Underline, Size::Small, L"Underline", L"3"));
  b.Row(b.Toggle(CmdStrike, glyph::Strike, Size::Small, L"Strikethrough", L"4"));
  b.Row(b.Toggle(CmdSubscript, 0, Size::Small, L"Subscript", L"5"));
  b.Row(b.Toggle(CmdSuperscript, 0, Size::Small, L"Superscript", L"6"));
  if (format_) {
    b.Row(b.Add(format_->ColorButton(true), Size::Small, L"Text Highlight Color", L"I"));
    b.Row(b.Add(format_->ColorButton(false), Size::Small, L"Font Color", L"FC"));
  }

  b.Group(L"Paragraph", L"ZP", CmdParagraph, 2);
  b.Toggle(CmdBullets, icon::BulletedList, Size::Small, L"Bullets", L"U");
  b.Row(b.Split(CmdNumbering, icon::NumberedList, Size::Small, L"Numbering", L"N",
                {CmdNumbering, CmdRestartNumbering, CmdContinueNumbering}))
      .tool.checked = QueryCommand(CmdNumbering).checked;
  b.Row(b.Button(CmdMultilevel, glyph::Multilevel, Size::Small, L"Multilevel List", L"M"));
  b.Row(b.Button(CmdOutdent, icon::DecreaseIndent, Size::Small, L"Decrease Indent", L"AO"));
  b.Row(b.Button(CmdIndent, icon::IncreaseIndent, Size::Small, L"Increase Indent", L"AI"));
  b.Row(b.Toggle(CmdShowMarks, 0x00B6, Size::Small, L"Show/Hide ¶", L"8"));
  b.Toggle(CmdAlignLeft, icon::AlignLeft, Size::Small, L"Align Left", L"AL");
  b.Row(b.Toggle(CmdAlignCenter, icon::AlignCenter, Size::Small, L"Center", L"AC"));
  b.Row(b.Toggle(CmdAlignRight, icon::AlignRight, Size::Small, L"Align Right", L"AR"));
  b.Row(b.Toggle(CmdAlignJustify, icon::AlignJustify, Size::Small, L"Justify", L"AJ"));
  // CmdAlignBox: the toolbar's paragraph menu once, Word's Line and Paragraph Spacing menu now.
  b.Row(b.Menu(CmdAlignBox, glyph::Spacing, Size::Small, L"Line and Paragraph Spacing", L"K",
               {CmdSpacing1, CmdSpacing115, CmdSpacing15, CmdSpacing2, 0, CmdParagraph, CmdSpaceBefore}));
  b.Row(b.Split(CmdShading, glyph::Shading, Size::Small, L"Shading", L"H", {CmdShading, CmdShadingNone}));
  b.Row(b.Split(CmdBorderBottom, glyph::Borders, Size::Small, L"Borders", L"B", {CmdBorderBottom, CmdBorderBox, CmdBorderNone}));

  b.Group(L"Styles", L"ZS");
  {
    RibbonItem& styles = b.Add(format_ ? format_->StyleBox() : Greyed(ToolKind::Dropdown, CmdStyleBox), Size::Large, L"Styles", L"L");
    RibbonGallery g;
    g.columns = 6;
    g.choiceWidth = 84;
    if (format_)
      g.drawChoice = [this](gfx::Canvas& c, const D2D1_RECT_F& r, int i, float ui) { format_->StylePreview(c, r, i, ui); };
    g.menu = {CmdNewStyle, CmdModifyStyle, CmdClearAllFormat};
    styles.gallery = std::move(g);
  }

  b.Group(L"Editing", L"ZN");
  b.Split(CmdFind, icon::Search, Size::Medium, L"Find", L"FD", {CmdFind, CmdGoToPage}).tool.enabled = true;
  b.Button(CmdReplace, glyph::Replace, Size::Medium, L"Replace", L"R").tool.enabled = editing;
  b.Button(CmdSelectAll, glyph::SelectAll, Size::Medium, L"Select All", L"SL");

  // ---- Insert
  b.Tab(L"Insert", L"N");
  b.Group(L"Pages", L"ZP");
  b.Button(CmdPageBreak, glyph::PageBreak, Size::Large, L"Page\nBreak", L"B");
  b.Group(L"Tables", L"ZB");
  if (table_) b.Add(table_->GridItem(), Size::Large, L"Table", L"T");
  else b.Button(CmdInsertTable, icon::Table, Size::Large, L"Table", L"T");
  b.Group(L"Illustrations", L"ZI");
  b.Button(CmdInsertPicture, icon::Picture, Size::Large, L"Pictures", L"P");
  const std::vector<int> shapes = {CmdInsertRectangle, CmdInsertRoundedRectangle, CmdInsertOval,
                                   CmdInsertLine,      CmdInsertArrow,            CmdInsertCallout};
  b.Menu(CmdInsertRectangle, glyph::Shapes, Size::Large, L"Shapes", L"SH", shapes);
  b.Group(L"Links", L"ZL");
  b.Button(CmdInsertLink, icon::Link, Size::Large, L"Link", L"I");
  b.Button(CmdBookmark, glyph::Bookmark, Size::Large, L"Bookmark", L"K");
  b.Group(L"Comments", L"Z1");
  b.Button(CmdNewComment, icon::Comment, Size::Large, L"Comment", L"L");
  b.Group(L"Header & Footer", L"ZH");
  b.Button(CmdEditHeader, glyph::Header, Size::Large, L"Header", L"H");
  b.Button(CmdEditFooter, glyph::Header, Size::Large, L"Footer", L"O");
  b.Button(CmdPageNumber, glyph::PageNumber, Size::Large, L"Page\nNumber", L"NU");
  b.Group(L"Text", L"ZT");
  b.Button(CmdInsertTextBox, glyph::TextBox, Size::Large, L"Text\nBox", L"X");
  if (format_) b.Add(format_->DateBox(), Size::Medium, L"Date & Time", L"D");

  // ---- Design
  b.Tab(L"Design", L"G");
  b.Group(L"Page Background", L"ZB");
  b.Menu(CmdWatermarkPanel, glyph::Watermark, Size::Large, L"Watermark", L"PW", {CmdWatermarkPanel, CmdWatermarkRemove});

  // ---- Layout
  const uint32_t d = editing ? SelectedPicture() : fw::doc::kNone;
  const bool object = d != fw::doc::kNone && Movable(d);
  const bool picture = object && PictureInfoOf(d).picture, shape = object && !picture;
  const int objectPanel = shape ? CmdShapePanel : CmdPicturePanel;
  auto arrange = [&] {
    b.Group(L"Arrange", L"ZA");
    b.Menu(CmdPictureWrapBox, glyph::Wrap, Size::Large, L"Wrap\nText", L"TW",
           {CmdPictureInline, CmdPictureSquare, CmdPictureTight, CmdPictureTopBottom, CmdPictureBehind, CmdPictureInFront});
    b.Menu(CmdPictureRotate, icon::Rotate, Size::Medium, L"Rotate", L"AY", {CmdPictureRotate, 0, objectPanel});
  };
  b.Tab(L"Layout", L"P");
  b.Group(L"Page Setup", L"ZS", CmdPageSetup);
  b.Menu(CmdMarginsNormal, glyph::Margins, Size::Large, L"Margins", L"M",
         {CmdMarginsNormal, CmdMarginsNarrow, CmdMarginsModerate, CmdMarginsWide, 0, CmdPageSetup});
  b.Menu(CmdPortrait, glyph::Portrait, Size::Large, L"Orientation", L"O", {CmdPortrait, CmdLandscape});
  if (page_) b.Add(page_->SizeMenu(), Size::Large, L"Size", L"SZ").tool.glyph = glyph::Size;
  b.Menu(CmdColumnsOne, glyph::Columns, Size::Large, L"Columns", L"J", {CmdColumnsOne, CmdColumnsTwo, CmdColumnsThree, 0, CmdPageSetup});
  b.Menu(CmdPageBreak, glyph::Breaks, Size::Medium, L"Breaks", L"B",
         {CmdPageBreak, CmdColumnBreak, 0, CmdSectionNextPage, CmdSectionContinuous, CmdSectionEvenPage, CmdSectionOddPage});
  b.Menu(CmdHyphenation, glyph::Hyphen, Size::Medium, L"Hyphenation", L"H", {CmdHyphenation, 0, CmdOptionalHyphen, CmdNonBreakingHyphen});
  if (page_) {
    b.Group(L"Paragraph", L"ZG", CmdParagraph, 2);
    b.Add(page_->ParaSpin(CmdIndentLeftBox), Size::Medium, L"Left", L"IL");
    b.Add(page_->ParaSpin(CmdIndentRightBox), Size::Medium, L"Right", L"IR");
    b.Add(page_->ParaSpin(CmdSpaceBeforeBox), Size::Medium, L"Before", L"SB").newColumn = true;
    b.Add(page_->ParaSpin(CmdSpaceAfterBox), Size::Medium, L"After", L"SA");
  }
  arrange();

  // ---- References
  b.Tab(L"References", L"S");
  b.Group(L"Table of Contents", L"ZC");
  b.Button(CmdInsertToc, glyph::Toc, Size::Large, L"Table of\nContents", L"T");
  b.Split(CmdUpdateToc, glyph::Refresh, Size::Medium, L"Update Table", L"U", {CmdUpdateTocPages, CmdUpdateToc});
  b.Group(L"Footnotes", L"ZF");
  b.Button(CmdInsertFootnote, glyph::Footnote, Size::Large, L"Insert\nFootnote", L"F");
  b.Button(CmdInsertEndnote, glyph::Footnote, Size::Medium, L"Insert Endnote", L"E");

  // ---- Review
  b.Tab(L"Review", L"R");
  b.Group(L"Proofing", L"ZP");
  b.Button(CmdSpelling, glyph::Spelling, Size::Large, L"Spelling &\nGrammar", L"S");
  b.Button(CmdWordCount, glyph::WordCount, Size::Large, L"Word\nCount", L"W");
  b.Group(L"Language", L"ZL");
  b.Button(CmdLanguage, glyph::Language, Size::Large, L"Language", L"U");
  b.Group(L"Comments", L"ZC");
  b.Button(CmdNewComment, icon::Comment, Size::Large, L"New\nComment", L"C");
  b.Group(L"Markup", L"ZM", CmdReviewerName);
  {
    ToolItem show;
    show.kind = ToolKind::Dropdown;
    show.cmd = CmdMarkupSimple;
    show.width = 120;
    show.enabled = Ready();
    show.tooltip = L"Display for Review";
    for (int cmd = CmdMarkupSimple; cmd <= CmdMarkupOriginal; ++cmd) {
      const CommandState q = QueryCommand(cmd);
      show.choices.push_back(q.label);
      if (q.checked) show.choice = cmd - CmdMarkupSimple;
    }
    show.onChange = [this](const ToolValue& v) {
      if (v.index >= 0 && v.index <= CmdMarkupOriginal - CmdMarkupSimple) Execute(CmdMarkupSimple + v.index);
    };
    b.Add(std::move(show), Size::Medium, L"Display for Review", L"TD");
  }
  b.Toggle(CmdReviewPane, glyph::Panes, Size::Medium, L"Reviewing Pane", L"TP");
  b.Group(L"Tracking", L"ZT");
  b.Toggle(CmdTrackChanges, glyph::Track, Size::Large, L"Track\nChanges", L"G");
  b.Split(CmdAcceptChange, glyph::Accept, Size::Large, L"Accept", L"A2", {CmdAcceptChange, CmdAcceptAll});
  b.Split(CmdRejectChange, icon::Cancel, Size::Large, L"Reject", L"J", {CmdRejectChange, CmdRejectAll});
  b.Button(CmdPrevChange, glyph::Up, Size::Medium, L"Previous", L"F");
  b.Button(CmdNextChange, glyph::Down, Size::Medium, L"Next", L"H1");

  // ---- View
  b.Tab(L"View", L"W");
  b.Group(L"Views", L"ZV");
  b.Button(CmdPresentation, icon::Slideshow, Size::Large, L"Present", L"F").tool.enabled = true;
  b.Group(L"Immersive", L"Z1");
  RibbonItem& full = b.Toggle(CmdFullScreen, icon::FullScreen, Size::Large, L"Full\nScreen", L"O");
  full.tool.enabled = true;
  full.tool.checked = ctx.fullscreen;
  RibbonItem& night = b.Toggle(CmdToggleInvert, icon::QuietHours, Size::Large, L"Night\nReading", L"D");
  night.tool.enabled = true;
  night.tool.checked = ctx.inverted;
  b.Group(L"Page Movement", L"Z2");
  b.Toggle(CmdViewContinuous, icon::ViewContinuous, Size::Large, L"Vertical", L"C");
  b.Toggle(CmdViewSingle, icon::ViewSingle, Size::Large, L"Side\nto Side", L"H");
  b.Group(L"Show", L"ZS");
  RibbonItem& ruler = b.Toggle(CmdRuler, 0, Size::Medium, L"Ruler", L"R");
  ruler.tool.enabled = editing;
  ruler.tool.checked = host_.ViewRuler();
  b.Toggle(CmdNavigationPane, 0, Size::Medium, L"Navigation Pane", L"K");
  b.Group(L"Zoom", L"ZZ");
  b.Menu(CmdZoomMenu, glyph::Zoom, Size::Large, L"Zoom", L"Q", {}).tool.enabled = true;
  b.Button(CmdZoomActual, icon::ZoomIn, Size::Large, L"100%", L"J");
  b.Toggle(CmdZoomFitPage, icon::Page, Size::Medium, L"One Page", L"1");
  b.Toggle(CmdViewFacing, icon::ViewFacing, Size::Medium, L"Multiple Pages", L"2");
  b.Toggle(CmdZoomFitWidth, glyph::FitWidth, Size::Medium, L"Page Width", L"I");

  // ---- Developer: content controls, which Word has only here.
  b.Tab(L"Developer", L"L");
  b.Group(L"Controls", L"ZC");
  b.Button(CmdInsertRichText, icon::FontCase, Size::Small, L"Rich Text Content Control", L"R");
  b.Button(CmdInsertPlainText, glyph::GrowFont, Size::Small, L"Plain Text Content Control", L"P");
  b.Button(CmdInsertCheckBox, glyph::CheckBox, Size::Small, L"Check Box Content Control", L"K");
  b.Button(CmdInsertDropDown, glyph::List, Size::Small, L"Drop-Down List Content Control", L"Q");
  b.Button(CmdInsertDatePicker, icon::Calendar, Size::Small, L"Date Picker Content Control", L"D");

  // ---- Contextual tabs
  // ribbon:check builds every contextual tab, to walk their commands too.
  if (table_ && ((editing && table_->InTable()) || ribbonAll_)) {
    b.Tab(L"Table Design", L"JT", true);
    b.Group(L"Table Styles", L"ZS");
    b.Add(table_->ShadingButton(), Size::Large, L"Shading", L"H").tool.glyph = glyph::Shading;
    b.Group(L"Borders", L"ZB", CmdTableProperties);
    b.Add(table_->BordersButton(), Size::Large, L"Borders", L"B").tool.glyph = glyph::Borders;

    b.Tab(L"Table Layout", L"JL", true);
    b.Group(L"Table", L"ZT");
    b.Menu(CmdTableSelectCell, glyph::SelectAll, Size::Large, L"Select", L"K",
           {CmdTableSelectCell, CmdTableSelectColumn, CmdTableSelectRow, CmdTableSelectTable});
    b.Button(CmdTableProperties, icon::Settings, Size::Large, L"Properties", L"O");
    b.Group(L"Rows & Columns", L"ZR");
    b.Menu(CmdTableDeleteColumns, icon::Delete, Size::Large, L"Delete", L"D",
           {CmdTableDeleteColumns, CmdTableDeleteRows, CmdTableDeleteTable});
    b.Button(CmdTableInsertAbove, glyph::Insert, Size::Large, L"Insert\nAbove", L"A");
    b.Button(CmdTableInsertBelow, glyph::Insert, Size::Large, L"Insert\nBelow", L"BE");
    b.Button(CmdTableInsertLeft, glyph::Insert, Size::Large, L"Insert\nLeft", L"L");
    b.Button(CmdTableInsertRight, glyph::Insert, Size::Large, L"Insert\nRight", L"R");
    b.Group(L"Merge", L"ZM");
    b.Button(CmdTableMerge, glyph::Merge, Size::Large, L"Merge\nCells", L"M");
    b.Button(CmdTableSplit, glyph::Merge, Size::Large, L"Split\nCells", L"P");
    b.Group(L"Cell Size", L"ZS", CmdTableProperties);
    b.Menu(CmdTableAutoFitContents, glyph::Fit, Size::Large, L"AutoFit", L"F",
           {CmdTableAutoFitContents, CmdTableAutoFitWindow, CmdTableFixedWidth});
    b.Button(CmdTableDistributeColumns, glyph::Columns, Size::Medium, L"Distribute Columns", L"UC");
    b.Group(L"Data", L"ZD2");
    b.Toggle(CmdTableHeaderRow, glyph::Header, Size::Large, L"Repeat\nHeader Rows", L"J");
    b.Button(CmdTableToText, glyph::TextBox, Size::Large, L"Convert\nto Text", L"V");
  }
  if (picture || ribbonAll_) {
    b.Tab(L"Picture Format", L"JP", true);
    b.Group(L"Adjust", L"ZT");
    b.Button(CmdPictureReset, glyph::Reset, Size::Medium, L"Reset Picture", L"Q");
    b.Group(L"Accessibility", L"Z1");
    b.Toggle(CmdPicturePanel, icon::Picture, Size::Large, L"Alt\nText", L"AT");
    arrange();
    b.Group(L"Size", L"ZZ", CmdPicturePanel);
    b.Toggle(CmdPictureCrop, icon::Crop, Size::Large, L"Crop", L"V");
  }
  if (shape || ribbonAll_) {
    b.Tab(L"Shape Format", L"JD", true);
    b.Group(L"Insert Shapes", L"ZH");
    b.Menu(CmdInsertRectangle, glyph::Shapes, Size::Large, L"Shapes", L"SH", shapes);
    b.Button(CmdInsertTextBox, glyph::TextBox, Size::Medium, L"Draw Text Box", L"X");
    b.Group(L"Shape Styles", L"ZY", CmdShapePanel);
    b.Toggle(CmdShapePanel, icon::Settings, Size::Large, L"Format\nShape", L"O");
    arrange();
  }
  if ((editing && hf_) || ribbonAll_) {
    b.Tab(L"Header & Footer", L"JH", true);
    b.Group(L"Header & Footer", L"ZH");
    b.Button(CmdPageNumber, glyph::PageNumber, Size::Large, L"Page\nNumber", L"NU");
    b.Group(L"Insert", L"ZI");
    if (format_) b.Add(format_->DateBox(), Size::Large, L"Date &\nTime", L"D");
    b.Button(CmdInsertPicture, icon::Picture, Size::Large, L"Pictures", L"P");
    b.Group(L"Navigation", L"ZN");
    b.Toggle(CmdLinkToPrevious, icon::Link, Size::Medium, L"Link to Previous", L"K");
    b.Group(L"Options", L"ZO");
    b.Toggle(CmdDifferentFirstPage, 0, Size::Medium, L"Different First Page", L"A");
    b.Toggle(CmdDifferentOddEven, 0, Size::Medium, L"Different Odd & Even Pages", L"V");
    b.Group(L"Close", L"ZC");
    b.Button(CmdCloseHeaderFooter, icon::Cancel, Size::Large, L"Close Header\nand Footer", L"C");
  }
  // A picture or shape just inserted shows its tab, as in Word; selecting one later doesn't.
  if (!object || d != ribbonInserted_) ribbonInserted_ = fw::doc::kNone;
  else m.show = picture ? L"Picture Format" : L"Shape Format";

  // ---- The File page: Word's proofing and user name options.
  m.file = {{RibbonFilePlace::Options, CmdToggleSpelling, L"Check spelling as you type", L"", 0},
            {RibbonFilePlace::Options, CmdToggleAutoCorrect, L"AutoCorrect as you type", L"", 0},
            {RibbonFilePlace::Options, CmdReviewerName, L"User name", L"The name your changes and comments carry", 0}};
}

// ---------------------------------------------------------------------------
// Checks

namespace {

// What the document's toolbar and ⋯ menu reached before the ribbon, besides the window's own (New,
// Open, Save, Save As, Print, Export PDF, Restore original, the theme, updates, About: the File page
// and quick access, r1-control's).
constexpr int kOldCommands[] = {
    // The toolbar.
    CmdStyleBox, CmdModifyStyle, CmdNewStyle, CmdFontBox, CmdSizeBox, CmdBold, CmdItalic, CmdUnderline, CmdFontColor,
    CmdHighlightColor, CmdFormatPainter, CmdCaseSentence, CmdCaseLower, CmdCaseUpper, CmdCaseCapitalize, CmdCaseToggle,
    CmdAlignLeft, CmdAlignCenter, CmdAlignRight, CmdAlignJustify, CmdSpacing1, CmdSpacing115, CmdSpacing15, CmdSpacing2,
    CmdBorderBox, CmdBorderBottom, CmdBorderNone, CmdShading, CmdShadingNone, CmdBullets, CmdNumbering, CmdMultilevel,
    CmdRestartNumbering, CmdContinueNumbering, CmdOutdent, CmdIndent, CmdNewComment, CmdInsertLink, CmdBookmark,
    CmdEditHeader, CmdEditFooter, CmdPageNumber, CmdInsertPicture, CmdInsertFootnote, CmdInsertEndnote, CmdInsertToc,
    CmdInsertTextBox, CmdWatermarkPanel, CmdPageBreak, CmdColumnBreak, CmdSectionNextPage, CmdSectionContinuous,
    CmdSectionEvenPage, CmdSectionOddPage, CmdInsertRectangle, CmdInsertRoundedRectangle, CmdInsertOval, CmdInsertLine,
    CmdInsertArrow, CmdInsertCallout, CmdInsertRichText, CmdInsertPlainText, CmdInsertCheckBox, CmdInsertDropDown,
    CmdInsertDatePicker, CmdDifferentFirstPage, CmdDifferentOddEven, CmdLinkToPrevious, CmdCloseHeaderFooter,
    CmdShowMarks, CmdNavigationPane, CmdReviewPane, CmdTableGrid, CmdTableInsertAbove, CmdTableInsertBelow,
    CmdTableInsertLeft, CmdTableInsertRight, CmdTableDeleteRows, CmdTableDeleteColumns, CmdTableDeleteTable,
    CmdTableMerge, CmdTableSplit, CmdTableSelectRow, CmdTableSelectColumn, CmdTableSelectTable, CmdTableAutoFitContents,
    CmdTableAutoFitWindow, CmdTableFixedWidth, CmdTableDistributeColumns, CmdTableHeaderRow, CmdTableToText,
    CmdTableProperties, CmdPortrait, CmdLandscape, CmdMarginsNormal, CmdMarginsNarrow, CmdMarginsModerate,
    CmdMarginsWide, CmdColumnsOne, CmdColumnsTwo, CmdColumnsThree, CmdHyphenation, CmdPageSetup, CmdPictureInline,
    CmdPictureSquare, CmdPictureTight, CmdPictureTopBottom, CmdPictureBehind, CmdPictureInFront, CmdPictureRotate,
    CmdPictureCrop, CmdPicturePanel, CmdShapePanel, CmdViewSingle, CmdViewContinuous, CmdViewFacing, CmdFind,
    CmdToggleInvert, CmdFullScreen, CmdZoomMenu,
    // The ⋯ menu's document entries.
    CmdPresentation, CmdToggleSpelling, CmdToggleAutoCorrect, CmdParagraph, CmdRuler, CmdTrackChanges, CmdAcceptChange,
    CmdRejectChange, CmdAcceptAll, CmdRejectAll, CmdNextChange, CmdPrevChange, CmdMarkupSimple, CmdMarkupAll,
    CmdMarkupNone, CmdMarkupOriginal, CmdReviewerName};

// The window's commands the ribbon runs, which the document's own command table doesn't name.
constexpr int kWindowCommands[] = {CmdFind,  CmdReplace,  CmdFullScreen, CmdToggleInvert, CmdRuler, CmdZoomMenu, CmdPresentation,
                                   CmdToggleSpelling, CmdToggleAutoCorrect};

void MenuIds(HMENU menu, std::set<int>& ids) {
  for (int i = 0; i < GetMenuItemCount(menu); ++i) {
    if (HMENU sub = GetSubMenu(menu, i)) MenuIds(sub, ids);
    else if (const UINT id = GetMenuItemID(menu, i); id != 0 && id != static_cast<UINT>(-1)) ids.insert(static_cast<int>(id));
  }
}

}  // namespace

bool DocView::ScriptOp(const std::wstring& op, const std::wstring& arg) {
  if (op != L"ribbon" || (arg != L"dump" && arg != L"check")) return false;
  const bool check = arg == L"check";
  RibbonModel m;
  ribbonAll_ = check;
  BuildRibbon(m, ToolbarContext{});
  ribbonAll_ = false;
  std::set<int> reach;  // the ribbon's and the File page's
  int items = 0, unknown = 0;
  auto known = [&](int cmd) {
    return !QueryCommand(cmd).label.empty() || std::find(std::begin(kWindowCommands), std::end(kWindowCommands), cmd) != std::end(kWindowCommands);
  };
  auto note = [&](int cmd, const std::wstring& where) {
    if (cmd == 0) return;
    reach.insert(cmd);
    if (check && !known(cmd)) {
      ++unknown;
      fwprintf(stderr, L"ribbon-check: unknown command %d (%s)\n", cmd, where.c_str());
    }
  };
  for (const RibbonTab& t : m.tabs) {
    if (!check) fwprintf(stderr, L"ribbon: tab %s [%s]%s\n", t.name.c_str(), t.keyTip.c_str(), t.contextual ? L" contextual" : L"");
    for (const RibbonGroup& g : t.groups) {
      const std::wstring where = t.name + L" › " + g.name;
      if (!check) fwprintf(stderr, L"ribbon:  group %s [%s] launcher %d\n", g.name.c_str(), g.keyTip.c_str(), g.launcher);
      note(g.launcher, where + L" ↘");
      for (const RibbonItem& it : g.items) {
        ++items;
        if (!check)
          fwprintf(stderr, L"ribbon:   item %d %s [%s] size %d%s%s%s\n", it.tool.cmd, it.label.c_str(), it.keyTip.c_str(),
                   static_cast<int>(it.size), it.tool.enabled ? L"" : L" (off)", it.tool.checked ? L" (on)" : L"",
                   it.tool.text.empty() ? L"" : (L" \"" + it.tool.text + L"\"").c_str());
        note(it.tool.cmd, where);
        for (int cmd : it.menu) note(cmd, where + L" › " + it.label);
        if (it.gallery)
          for (int cmd : it.gallery->menu) note(cmd, where + L" › " + it.label);
        if (it.tool.dropdown && it.menu.empty() && it.tool.choices.empty()) {  // the tab's FillMenu
          HMENU menu = CreatePopupMenu();
          FillMenu(it.tool.cmd, menu);
          std::set<int> ids;
          MenuIds(menu, ids);
          DestroyMenu(menu);
          for (int cmd : ids) reach.insert(cmd);
        }
      }
    }
  }
  if (!check) {
    if (!m.show.empty()) fwprintf(stderr, L"ribbon: show %s\n", m.show.c_str());
    return true;
  }
  for (const RibbonFileItem& f : m.file) note(f.cmd, L"File");
  // Right-click at the caret, and command search (Alt+Q).
  std::set<int> elsewhere;
  HMENU menu = CreatePopupMenu();
  const std::optional<RECT> caret = CaretRect();
  FillContextMenu(menu, caret ? (caret->left + caret->right) / 2 : 0, caret ? (caret->top + caret->bottom) / 2 : 0);
  MenuIds(menu, elsewhere);
  DestroyMenu(menu);
  for (int cmd : Commands()) elsewhere.insert(cmd);
  int unreachable = 0;
  for (int cmd : kOldCommands)
    if (!reach.count(cmd) && !elsewhere.count(cmd)) {
      ++unreachable;
      fwprintf(stderr, L"ribbon-check: unreachable %d %s\n", cmd, QueryCommand(cmd).label.c_str());
    }
  fwprintf(stderr, L"ribbon-check: %s: %d tabs, %d items, %d commands on the ribbon, %d old commands, %d unknown, %d unreachable\n",
           unknown || unreachable ? L"FAILED" : L"ok", static_cast<int>(m.tabs.size()), items, static_cast<int>(reach.size()),
           static_cast<int>(std::size(kOldCommands)), unknown, unreachable);
  return true;
}
