#pragma once

#include <windows.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "PagedView.h"
#include "doc/edit/Editor.h"
#include "doc/edit/Pictures.h"
#include "doc/layout/Layout.h"
#include "draw/Drawing.h"
#include "doc/proof/AutoCorrect.h"
#include "doc/proof/Spelling.h"
#include "doc/model/Document.h"
#include "pkg/XmlDom.h"
#include "text/Fonts.h"
#include "word/DocFile.h"
#include "word/DocFormat.h"
#include "word/DocPage.h"
#include "word/DocStyles.h"
#include "word/DocTable.h"

class PicturePanel;

// A Word document in a tab (plan milestones D1 and D2): the model, laid out page by page by
// fw::doc::layout (the first page at once, the rest in idle time), drawn through draw::Painter,
// with selection and copy, find, links, comments and tracked changes shown, and printing. A
// document Featherweight can write back is edited in place (DocEdit.cpp): the caret, typing,
// deleting, undo and redo, cut and paste, through fw::doc::edit; formatting through DocFormat.
// Its file (saving, backups, recovery, changes on disk) is kept by a DocFile.
class DocView : public PagedView {
 public:
  DocView(ViewHost& host, std::wstring path);
  ~DocView() override;
  // A new document from the built-in template: "Document <number>".
  static std::unique_ptr<DocView> NewDocument(ViewHost& host, int number);
  // A document that isn't its file: a recovered one (`unsaved` changes) or a backup. Save goes to
  // Save As, starting at `suggested`.
  static std::unique_ptr<DocView> FromBytes(ViewHost& host, std::vector<uint8_t> bytes, std::wstring title,
                                            std::wstring suggested, bool unsaved);

  // The document viewer's keys.
  static int Shortcut(UINT vk, UINT mods);

  // ---- TabContent ----
  TabKind Kind() const override { return TabKind::Document; }
  OpenResult Load(const std::wstring& password, std::wstring& error) override;
  bool Ready() const override { return pagination_ != nullptr; }
  const std::wstring& Path() const override { return path_; }
  std::wstring Title() const override;
  bool Modified() const override { return file_.Dirty(); }

  // The rulers, when shown, take their bands of the tab's area; the pages get the rest (DocRuler.cpp).
  void SetViewport(const RECT& rc, float ui) override;
  void Draw(gfx::Canvas& c, ID2D1DeviceContext* dc) override;
  bool HasIdleWork() const override;
  bool IdleWork(ID2D1DeviceContext* dc, double deadlineMs) override;
  void ReleaseDeviceResources() override {}
  void OnDeactivated() override;

  void OnMouseDown(int x, int y, int button, UINT mods, bool doubleClick) override;
  void OnMouseMove(int x, int y, UINT mods) override;
  void OnMouseUp(int x, int y, int button, UINT mods) override;
  void OnMouseLeave() override;
  bool Dragging() const override {
    return PagedView::Dragging() || picDrag_.kind != PictureDrag::None || cellDrag_.has_value() ||
           textDrag_ != TextDrag::None;
  }
  bool OnKeyDown(UINT vk, UINT mods) override;
  bool OnChar(wchar_t ch, UINT mods) override;
  HCURSOR CursorAt(int x, int y) override;
  std::wstring TooltipAt(int x, int y) override;
  bool HasKeyFocus() const override { return Editing() && keyFocus_; }
  void SetKeyFocus(bool has) override;
  std::optional<RECT> CaretRect() override;

  int ShortcutFor(UINT vk, UINT mods) const override;
  CommandState QueryCommand(int cmd) override;
  std::vector<int> Commands() override;
  bool Execute(int cmd) override;
  void BuildToolbar(ToolbarModel& m, const ToolbarContext& ctx) override;
  void FillContextMenu(HMENU menu, int x, int y) override;

  bool HasStatusBar() const override { return Ready(); }
  void BuildStatusBar(StatusBarModel& m) override;
  // Spelling or AutoCorrect turned on or off.
  void SettingsChanged() override;

  void StartSearch(const std::wstring& query, bool matchCase, bool wholeWords) override;
  void ClearSearch() override;
  void SearchStep(bool forward) override;
  int SearchCount() const override { return static_cast<int>(matches_.size()); }
  int SearchCurrent() const override { return current_; }
  bool SearchDone() const override { return true; }
  std::wstring SelectedText() override;
  bool CanReplace() const override { return Editing(); }
  bool ReplaceCurrent(const std::wstring& with) override;
  int ReplaceAll(const std::wstring& with) override;

  bool Save(const std::wstring& path, std::wstring& error) override;
  SaveFormat SaveAsFormat() const override;
  std::wstring SuggestedSavePath() const override;
  void Autosave() override;
  FileChange CheckFileOnDisk() override { return Ready() ? static_cast<FileChange>(file_.CheckDisk()) : FileChange::None; }
  bool CanPrint() const override { return Ready(); }
  PrintOutcome Print(HWND owner, const std::function<void(int, int)>& progress, std::wstring& message) override;
  // Every page to Microsoft Print to PDF, into a file, with no dialog (the visual checks' print step).
  PrintOutcome PrintToFile(const std::wstring& path, std::wstring& message);

  // ---- Editing (D2) ----
  // The document's editor: every change goes through it (transactions, undo and redo). Null until
  // loaded. Its changes reach the layout, the page list and the file (modified, autosave, Save).
  fw::doc::edit::Editor* Editor() { return editor_.get(); }
  // Marks a part edited without the editor (tests).
  void NoteEdit(uint32_t part);
  // The UI checks' "edited" step: " (edited)" typed at the end of the first paragraph.
  void EditForTesting();
  DocFile& File() { return file_; }

  // ---- Text ----
  bool HasSelection() const { return anchor_ != focus_; }
  void Copy();
  void SelectAll();
  // The selection on the clipboard, as plain text, RTF and HTML. False when it has no text.
  bool PutOnClipboard();

  // ---- Editing (DocEdit.cpp) ----
  bool Editing() const { return editor_ != nullptr && Ready() && !presentation_; }
  void Cut();
  // Pastes the clipboard's formatted text (Featherweight's own, RTF or HTML), or its plain text.
  // `merge`: the text takes the look of where it goes, keeping its emphasis (Clip.h PasteClip).
  void Paste(bool formatted = true, bool merge = false);
  void Undo();
  void Redo();

  // ---- Pictures (DocPictures.cpp, D3) ----
  // Inserts the picture files at the caret, or where they were dropped (x, y); false when none was.
  bool InsertPictureFiles(const std::vector<std::wstring>& files, int x = -1, int y = -1);
  // Whether these are all pictures InsertPictureFiles takes (by their names).
  static bool PictureFiles(const std::vector<std::wstring>& files);
  bool TakesPictures(const std::vector<std::wstring>& files) const override { return Editing() && PictureFiles(files); }
  void DropPictures(const std::vector<std::wstring>& files, int x, int y) override { InsertPictureFiles(files, x, y); }
  // The selected picture (the selection is its one character), by index into
  // Document::Drawings(); kNone when there's none.
  uint32_t SelectedPicture() const;

 protected:
  // ---- PagedView ----
  const std::vector<fw::PageSizePt>* Pages() const override { return Ready() ? &sizes_ : nullptr; }
  void OnPresentationChanged(bool on) override;

 private:
  using TextPos = fw::doc::layout::TextPos;
  using Rects = std::vector<std::pair<size_t, fw::doc::layout::Rect>>;
  struct Match {
    TextPos from, to;
    bool measured = false;  // rects are known
    Rects rects;
  };
  // What is under a point on a page: a link, a comment.
  struct Hit {
    std::wstring link;     // a URL, or "#bookmark"
    int comment = -1;      // index into comments_
    int change = -1;       // index into changes_
  };

  // Coordinates: page points <-> window pixels, with the view's rotation.
  draw::Matrix PageToScreen(int page) const;
  bool ToPagePoint(int page, int x, int y, float& px, float& py) const;
  D2D1_RECT_F PageRectToScreen(int page, const fw::doc::layout::Rect& r) const;

  // The text a reader sees in a paragraph (no field codes, hidden text or markers), and for each
  // of its characters the offset in the paragraph's text.
  void VisibleText(uint32_t para, std::wstring& text, std::vector<uint32_t>& map) const;
  // Pages laid out since the last call: their sizes join the view's. True when there were any.
  bool TakeNewPages();
  void ShowNotices();
  void DrawPage(gfx::Canvas& c, ID2D1DeviceContext* dc, int page, bool inverted);
  void DrawRanges(gfx::Canvas& c, int first, int last);
  // Draws a picture, shape or text box frame with fwdraw, made from the model's XML on first use.
  void DrawDrawing(draw::Painter& painter, const fw::doc::layout::DrawingBox& box);
  PrintOutcome PrintPages(const std::wstring& printer, const DEVMODEW* devmode, const std::vector<int>& pages,
                          IStream* output, const std::function<void(int, int)>& progress, std::wstring& message);

  std::optional<TextPos> PosAt(int x, int y, bool nearest) const;
  Hit HitAt(int x, int y);
  void FollowLink(const std::wstring& link);
  void ScrollToPos(TextPos from, TextPos to);
  std::wstring TextOf(TextPos from, TextPos to, size_t limit) const;
  // The words around a position, for double-click.
  void WordAt(TextPos pos, TextPos& from, TextPos& to) const;
  uint32_t StoryOf(TextPos pos) const { return doc_->Para(pos.para).story; }
  // Comments, tracked changes and links, with where they are in the text.
  void CollectAnnotations();

  // ---- Proofing (DocProof.cpp)
  // Spelling and words in idle time until `deadlineMs`; true when the squiggles or counts changed.
  bool ProofIdle(double deadlineMs);
  bool HasProofWork() const;
  // An edit, undo or redo: the paragraphs it replaced are checked and counted again.
  void ProofEdited(const fw::doc::edit::Change& change);
  void DrawSquiggles(gfx::Canvas& c, int first, int last);
  // The spelling error under (x, y): its suggestions, Ignore all and Add to dictionary, first.
  void FillSpellingMenu(HMENU m, int x, int y);
  void FillLanguageMenu(HMENU m);
  bool ExecuteProof(int cmd);
  void NextMisspelling();  // F7
  void ShowWordCount();    // Ctrl+Shift+G
  void ChooseLanguage();   // the language menu at the mouse
  void SetLanguage(const std::wstring& tag);
  // After a character was typed before the caret, or Enter ended `para`: AutoCorrect.
  void AutoCorrectTyped(char16_t typed);
  void AutoCorrectBreak(uint32_t para);
  void ProofStatus(StatusBarModel& m);

  // ---- Editing (DocEdit.cpp)
  // Edits, their undo and redo: the layout lays out again, positions kept here move, and the file
  // learns whether the document now differs from the saved one.
  void OnEdited(const fw::doc::edit::Change& change);
  // The file learns whether the document differs from the saved one. OnEdited does it for each
  // change; a group of changes (a paste, a page number) is only one undo step once it ends, so
  // whatever made one calls this after it.
  void SyncFile();
  // After an edit (or a refusal, whose message shows): the caret goes where it says.
  bool Apply(const fw::doc::edit::Result& result);
  bool EditKey(UINT vk, UINT mods);
  void TypeText(std::u16string_view text);
  void DeleteBy(bool forward, bool word);
  // Moves the caret (extending the selection, or not), breaking the typing undo step.
  void MoveCaret(TextPos to, bool extend);
  // The caret moved or the text changed: its place is measured again, it's scrolled into view
  // (when `show`) and blinks from the start.
  void CaretMoved(bool show);
  const std::optional<fw::doc::layout::Layout::Caret>& CaretBox();
  void DrawCaret(gfx::Canvas& c, int first, int last);
  void StopBlinking();
  // The characters of a paragraph the caret moves over: offsets of those a reader sees (a
  // surrogate pair once, at its first half), in order.
  std::vector<uint32_t> Stops(uint32_t para) const;
  TextPos StepChar(TextPos p, bool forward) const;
  TextPos StepWord(TextPos p, bool forward) const;
  TextPos StoryEdge(TextPos p, bool end) const;
  fw::doc::Pos SelFrom() const { return {std::min(anchor_, focus_).para, std::min(anchor_, focus_).offset}; }
  fw::doc::Pos SelTo() const { return {std::max(anchor_, focus_).para, std::max(anchor_, focus_).offset}; }

  // ---- Headers and footers, page numbers, links, bookmarks, navigation, marks (D3, DocHeaders.cpp)
  class PushButton;
  class LinkPanel;
  class BookmarkPanel;
  class PageNumberPanel;
  class NavPanel;
  // A header or footer being edited in place (double-click it; Esc or a double-click in the body
  // leaves): its story, the page it's shown on, and which of the section's it is.
  struct HeaderEdit {
    uint32_t story = fw::doc::kNone;
    size_t page = 0;
    uint32_t section = 0;
    bool footer = false;
    fw::doc::HdrFtrType type = fw::doc::HdrFtrType::Default;
  };
  // Which lines a click may land on: the body's, or while one is edited the headers' and footers'.
  fw::doc::layout::HitStories ClickStories() const;
  // The page the caret's header or footer is shown on (SIZE_MAX in the body).
  size_t CaretPage() const;
  // Where a point on a page is: in its header area (-1), its body (0) or its footer area (1).
  int ZoneAt(int page, float py) const;
  // Clicks in and around headers and footers; true when handled.
  bool HeaderFooterClick(int x, int y, bool doubleClick);
  bool EnterHeaderFooter(int page, bool footer, std::optional<std::pair<float, float>> at = std::nullopt);
  void LeaveHeaderFooter();
  // After undo or redo: leaves a header or footer that is no longer the section's.
  void CheckHeaderFooter();
  void DrawHeaderFooterMode(gfx::Canvas& c, int page);
  std::wstring HeaderFooterLabel(int page, bool footer) const;
  // The commands, toolbar items and context menu entries of D3.
  bool ExecuteD3(int cmd);
  std::optional<CommandState> QueryD3(int cmd);
  void AddD3Toolbar(ToolbarModel& m);
  void FillD3Menu(HMENU m, int x, int y);
  // Formatting marks: the layout is made again with them (and hidden text) on or off.
  void ToggleMarks();
  // The layout's options: formatting marks as shown, and DATE and TIME fields at this moment.
  fw::doc::layout::Options LayoutOptions() const;
  // Opens a side panel of this view's, or closes it when it's open.
  void ShowPanel(PanelContent* panel, bool toggle);
  void ClosePanels();
  // The headings (paragraphs with an outline level), for the navigation pane and links.
  struct Heading {
    uint32_t para = 0;
    int level = 0;  // 0 = Heading 1
    std::wstring text;
  };
  std::vector<Heading> Headings() const;
  void GoTo(TextPos at, TextPos to);
  std::optional<HeaderEdit> hf_;
  TextPos bodyCaret_;  // where the caret was in the body before a header was edited
  bool marks_ = false;
  std::unique_ptr<LinkPanel> linkPanel_;
  std::unique_ptr<BookmarkPanel> bookmarkPanel_;
  std::unique_ptr<PageNumberPanel> pageNumberPanel_;
  std::unique_ptr<NavPanel> navPanel_;

  // ---- Review (D4, DocReview.cpp): comments and tracked changes. The 💬 button opens the Review
  // pane; Ctrl+Shift+E turns Track Changes on or off; the ⋯ menu has the rest.
  class ReviewPanel;
  enum class Markup : uint8_t { Simple, All, None, Original };  // Word's four views of changes
  bool ExecuteD4(int cmd);
  std::optional<CommandState> QueryD4(int cmd);
  void AddD4Toolbar(ToolbarModel& m);
  void FillD4Menu(HMENU m);
  // The tracked change after (or before) the caret, selected: in its story, then the next story.
  bool GoToChange(bool back);
  // Accepts or rejects the changes the selection touches (the one at the caret), then goes to the next.
  void ReviewChange(bool accept);
  // The name changes and comments are made with: the one set in ⋯, else Windows'.
  std::string ReviewerName() const;
  // Whether the layout can show deleted text and hide inserted text (All markup and Original).
  static bool MarkupLaidOut();
  // Whether a run in this change is shown in the markup chosen (deleted text in All markup and
  // Original, inserted text not in Original): the caret stops in it, copying takes it.
  bool RevisionShown(fw::doc::Node revision) const;
  Markup markup_ = Markup::Simple;
  std::unique_ptr<ReviewPanel> reviewPanel_;
  size_t allChanges_ = 0;  // tracked changes of every kind, formatting too (CollectAnnotations)
  uint32_t editStamp_ = 0;  // counts edits, undos and redos: the Review pane reads them again
  // ---- Rulers (DocRuler.cpp), on demand (the ⋯ menu), as Word's: along the top the caret's page
  // with its margins and columns, and the paragraph's indents and tab stops; down the left its top
  // and bottom margins. Dragging one changes it, one undo step each; a click on the ruler sets a tab
  // stop of the kind the corner box shows (a click there picks the next), and a stop dragged off goes.
  struct RulerInfo;  // what the rulers show now
  enum class RulerPart : uint8_t {
    None, Corner, Strip, MarginLeft, MarginRight, MarginTop, MarginBottom,
    FirstLine, Hanging, LeftIndent, RightIndent, Tab, ColumnStart, ColumnEnd, CellEdge
  };
  struct RulerHit {
    RulerPart part = RulerPart::None;
    int index = -1;  // a tab stop (RulerInfo::tabs), a column, a cell edge (RulerInfo::cells)
    bool operator==(const RulerHit&) const = default;
  };
  bool RulerOn() const;
  RECT RulerInner(const RECT& rc, float ui) const;  // what the rulers leave of the tab's area
  bool OnRuler(int x, int y) const;  // in the rulers' bands
  RulerInfo MeasureRuler();
  void MeasureCells(RulerInfo& r, const fw::doc::layout::Page& page, uint32_t para);  // the caret's table row
  RulerHit RulerAt(const RulerInfo& r, int x, int y) const;
  void DrawRulers(gfx::Canvas& c);
  void RulerMouseDown(int x, int y);
  void RulerMouseMove(int x, int y, UINT mods);
  void RulerMouseUp();
  void RulerHover(int x, int y);
  std::wstring RulerTooltip(int x, int y);
  HCURSOR RulerCursor(int x, int y);
  // A right-click on the ruler: a tab stop's kind and leader, clearing, the Tabs panel (DocStyles).
  void FillRulerMenu(HMENU m, int x, int y);
  friend class DocStyles;  // its panels' buttons

  // ---- Formatting (DocFormat)
  // What formatting applies to: the selection, or the caret and the word it's in.
  DocFormat::Target FormatTarget() const;
  // After a formatting command: the caret where it says, or why nothing happened.
  void AfterFormat(const DocFormat::Done& done);
  // Tab and Shift+Tab at the start of list items change their level.
  bool ListTab(bool back);

  // ---- Pictures (DocPictures.cpp)
  struct PicBox {
    int page = 0;
    fw::doc::layout::Rect rect;  // page points
  };
  // Where a drawing is shown; nothing when it isn't.
  std::optional<PicBox> BoxOf(uint32_t drawing) const;
  // The drawing under (x, y): the top one; one behind the text only where there's no text.
  uint32_t PictureAt(int x, int y) const;
  const fw::doc::edit::PictureInfo& PictureInfoOf(uint32_t drawing) const;
  // The selected picture's handle under (x, y): 0 top left, clockwise to 7; 8 turns it; -1 none.
  int HandleAt(int x, int y) const;
  void SelectDrawing(uint32_t drawing);
  // Apply, keeping the picture selected.
  bool ApplyPicture(const fw::doc::edit::Result& r);
  void DrawPictureFrame(gfx::Canvas& c, int first, int last);
  // The pointer and keys over pictures: true when they were the picture's.
  bool PictureMouseDown(int x, int y, int button, UINT mods, bool doubleClick);
  bool PictureMouseMove(int x, int y, UINT mods);
  bool PictureMouseUp(int x, int y);
  HCURSOR PictureCursor(int x, int y) const;
  bool PictureKey(UINT vk, UINT mods);
  // The text column's width at `pos`: a picture inserted there is made to fit it.
  fw::Emu ColumnWidthAt(TextPos pos);
  // Inserts a picture over the selection (or at `at`), as one undo step.
  bool InsertPictureBytes(const std::vector<uint8_t>& bytes, std::u16string_view name, std::optional<TextPos> at);
  void InsertPictureDialog();
  // The clipboard's picture, when it has no text; true when it had one.
  bool PastePicture();
  bool HasPictureOnClipboard() const;
  // The selected picture on the clipboard (PNG or JPEG as stored, and a bitmap).
  bool CopyPicture();
  std::optional<CommandState> QueryPicture(int cmd);
  bool ExecutePicture(int cmd);
  void SetPictureWrap(fw::doc::edit::Wrap wrap);
  void AddPictureToolbar(ToolbarModel& m);
  void FillPictureMenu(HMENU m);
  // The Picture panel shows the selected picture.
  void SyncPicturePanel();

  std::wstring path_;            // the file; empty for a new or recovered document
  DocFile file_;
  std::vector<uint8_t> bytes_;   // NewDocument, FromBytes: what Load reads instead of a file
  std::unique_ptr<fw::doc::Document> doc_;
  std::unique_ptr<fw::doc::edit::Editor> editor_;  // after doc_: goes first
  std::unique_ptr<fw::text::Fonts> fonts_;
  std::unique_ptr<fw::doc::layout::Layout> pagination_;
  std::vector<fw::PageSizePt> sizes_;
  bool placeholder_ = false;  // nothing is laid out: one empty page

  // Drawings, by index into Document::Drawings(), made when first shown. The pictures are shared
  // and read from the package, which the document keeps.
  struct DrawingSlot {
    bool tried = false;
    uint32_t version = 0;  // the drawing's (Drawing::version): made again when it changes
    std::unique_ptr<fw::pkg::XmlDom> xml;
    std::unique_ptr<draw::PackageResources> resources;
    std::unique_ptr<draw::Drawing> drawing;
  };
  std::vector<DrawingSlot> drawings_;
  std::shared_ptr<const draw::Theme> theme_;
  draw::ColorMap colorMap_;
  std::shared_ptr<draw::ImageCache> images_;
  uint32_t revision_ = 0;
  bool noticesShown_ = false;
  // Words in the main story, for the status bar: counted in idle time by paragraph, and for the
  // paragraphs an edit replaces as it's made (DocProof.cpp).
  size_t words_ = 0;
  uint32_t wordsNext_ = 0;           // the next paragraph to count (Document::Paragraphs() order)
  std::vector<uint32_t> paraWords_;  // the counted paragraphs' words (0 outside the main story)
  bool wordsCounted_ = false;

  // Comments, with their ranges and text for the tooltip.
  struct Comment {
    TextPos from, to;
    std::wstring text;  // "Author: what they wrote"
    bool measured = false;
    Rects rects;
  };
  std::vector<Comment> comments_;
  // Tracked changes, as Word's Simple Markup shows them: deleted text isn't laid out, so a
  // deletion is a mark where it was; inserted text is underlined; both get a bar in the margin.
  struct Change {
    TextPos from, to;
    bool deletion = false;
    bool barOnly = false;  // formatting or a paragraph mark: only the bar, beside its lines
    size_t author = 0;     // its color
    std::wstring text;  // "Inserted by Ann", "Deleted by Ann: “old words”"
    bool measured = false;
    Rects rects;                                       // insertions
    std::optional<fw::doc::layout::Layout::Caret> at;  // deletions
  };
  std::vector<Change> changes_;
  // HYPERLINK fields: their results, and where they go.
  struct Link {
    TextPos from, to;
    std::wstring target;
  };
  std::vector<Link> links_;

  // Selection: from anchor_ to focus_ in one story (empty when equal).
  TextPos anchor_, focus_;
  TextPos selFrom_, selTo_;  // what selRects_ covers
  uint32_t selRevision_ = 0;
  Rects selRects_;
  std::wstring pendingLink_;
  int mouseX_ = -1, mouseY_ = -1;

  // Rulers
  RECT full_{};  // the tab's area: the rulers' bands, then vp_
  fw::doc::TabAlign rulerTab_ = fw::doc::TabAlign::Start;  // what a click on the ruler sets
  struct RulerDrag {
    RulerHit hit;
    float start = 0, at = 0;  // where the mark was and is now, page points along its ruler
    float grab = 0;           // where the mouse took it
    bool off = false;         // a tab stop dragged off the ruler: it goes
  } rulerDrag_;
  RulerHit rulerHover_;

  // Find
  std::vector<Match> matches_;
  int current_ = -1;
  std::wstring query_;  // what was searched for, to find again after a replace
  bool matchCase_ = false, wholeWords_ = false;

  // Editing: editor_ (declared after doc_) is null for a document shown but not edited (one the
  // model can't write back).
  std::unique_ptr<DocFormat> format_;  // after the editor: it goes first
  std::unique_ptr<DocTable> table_;    // tables (D3), likewise
  std::unique_ptr<DocPage> page_;      // page layout (D3), likewise
  std::unique_ptr<DocStyles> styles_;  // tab stops and styles (D3), likewise
  // After a change of the page: the first pages laid out now, the rest in idle time, and the
  // caret where it was or, with `caret`, where the result says (drawn once its page is laid out
  // again). False, with the refusal shown.
  bool ApplyPageChange(const fw::doc::edit::Result& r, bool caret);
  // The cells a selection across cells covers, as drawn (DrawRanges).
  Rects cellRects_;
  TextPos cellFrom_, cellTo_;
  uint32_t cellRevision_ = UINT32_MAX;
  std::unique_ptr<fw::doc::edit::Pictures> pictures_;  // with the editor, after it
  // Rows chosen in the margin beside a table, columns just above one, or lines in the margin beside
  // text (Word's selection bar): the drag goes on by whole rows, columns or lines from here.
  enum class LineDrag : uint8_t { None, Rows, Columns, Lines };
  LineDrag lineDrag_ = LineDrag::None;
  TextPos lineAnchor_, lineAnchorEnd_;  // for lines, the first line's start and end
  std::optional<TextPos> RowMarginAt(int x, int y) const;
  // A line of text, from its start to the start of the next (its paragraph's end, for the last).
  std::optional<std::pair<TextPos, TextPos>> LineRange(TextPos pos) const;
  // In the left margin beside a line of text (not a table's): that line.
  std::optional<std::pair<TextPos, TextPos>> LineMarginAt(int x, int y) const;
  // Dragging the selected text (DocEdit.cpp): pressed on the selection, moved, let go elsewhere in
  // its story, it moves there (with Ctrl, a copy goes), one undo step; a click without moving puts
  // the caret there. The dropped text stays selected.
  enum class TextDrag : uint8_t { None, Pending, Moving };
  TextDrag textDrag_ = TextDrag::None;
  TextPos dragDown_;               // where it was pressed
  std::optional<TextPos> dropAt_;  // where it would go now
  bool OverSelection(int x, int y) const;
  bool TextDragDown(int x, int y, UINT mods, std::optional<TextPos> pos);
  bool TextDragMove(int x, int y);
  bool TextDragUp(UINT mods);
  void DrawTextDrop(gfx::Canvas& c, int first, int last);
  bool SelectLines(TextPos from, TextPos to);
  // ---- Table cells on the page (DocCells.cpp), from the layout's cell boxes (Layout::Cells): a
  // column border dragged moves its grid line, a row's bottom border sets its height (at least),
  // one undo step each; a click just above a column selects it. Selected cells are filled whole.
  struct CellEdge {
    int page = -1;
    fw::doc::ElemId tbl = fw::doc::kNoElem;
    TextPos at;              // a cell of the table (of the row, for a row's border)
    bool column = true;      // a column border, else a row's bottom
    int32_t line = 0;        // the column border's grid line
    float pos = 0, lo = 0, hi = 0;  // where it is, and how far it can go (page points)
    float top = 0;                  // a row's top
    float from = 0, to = 0;         // the guide's extent, across the table
    float now = 0;                  // while dragging: where it is now
  };
  std::optional<CellEdge> CellEdgeAt(int x, int y);
  // The cell just below (x, y) when (x, y) is just above a table's column; with `tbl`, the cell of
  // that table under x, at any height (a drag along the tops).
  std::optional<TextPos> ColumnTopAt(int x, int y, fw::doc::ElemId tbl = fw::doc::kNoElem);
  // A cell's first paragraph, when it's in the story being edited.
  std::optional<TextPos> CellStart(fw::doc::ElemId tc) const;
  bool CellMouseDown(int x, int y, UINT mods);
  bool CellMouseMove(int x, int y);
  bool CellMouseUp();
  HCURSOR CellCursor(int x, int y);
  void DrawCellGuide(gfx::Canvas& c, int first, int last);
  void MeasureSelectedCells(int first, int last);  // cellRects_
  std::optional<CellEdge> cellDrag_;
  fw::doc::ElemId lineTbl_ = fw::doc::kNoElem;  // the table a column drag is in
  int cellFirst_ = -1, cellLast_ = -1;          // the pages cellRects_ covers
  uint64_t clipSource_ = 0;  // names this document in what it copies, to paste lists back into it
  bool keyFocus_ = true;
  bool annotationsStale_ = false;  // an edit moved comments, changes or links: find them again
  double caretStart_ = 0;          // the caret blinks from here (the last key or click)
  bool blinking_ = false;          // the blink timer runs
  float goalX_ = -1;               // Up and Down keep to this x (page points); < 0: the caret's
  wchar_t highSurrogate_ = 0;      // the first half of a character typed in two
  TextPos caretFor_;               // what caretBox_ measured
  uint32_t caretRevision_ = UINT32_MAX;
  std::optional<fw::doc::layout::Layout::Caret> caretBox_;
  double lastDoubleClick_ = -1e9;  // a click right after a double-click selects the paragraph
  int lastDoubleX_ = 0, lastDoubleY_ = 0;

  // Proofing (DocProof.cpp): the pages on screen checked in idle time with Windows' spell
  // checker; nothing runs once they are.
  std::unique_ptr<fw::doc::proof::Spelling> spelling_;
  bool spellingWanted_ = true;  // made on the first idle tick, when the setting is on
  bool typed_ = false;          // the last thing done was typing, which left the caret at typedAt_:
  TextPos typedAt_;             // the word there is still being typed, and isn't marked yet
  uint32_t edits_ = 0;          // edits so far, for what is measured from the text
  fw::doc::proof::AutoCorrect autoCorrect_;
  // Squiggles under the errors, measured per page when shown: [x1, x2) at y, page points.
  struct Squiggle {
    uint32_t para, from, to;
    float x1, x2, y;
  };
  struct PageSquiggles {
    uint32_t layout = UINT32_MAX, spelling = 0, edits = 0;
    std::vector<Squiggle> lines;
  };
  std::vector<PageSquiggles> squiggles_;
  // The error the context menu was opened on, and what it offered.
  struct MenuError {
    uint32_t para = 0;
    fw::doc::proof::Misspelling error;
    std::wstring word;
    std::vector<std::wstring> suggestions;
  };
  std::optional<MenuError> menuError_;
  // The selection's words for the status bar, counted when it changes.
  TextPos selWordsFrom_, selWordsTo_;
  uint32_t selWordsEdits_ = UINT32_MAX;
  size_t selWords_ = 0;

  // Pictures (DocPictures.cpp): the selected one's frame and handles; dragging moves, resizes,
  // turns or crops it, shown as an outline until the button is let go.
  std::unique_ptr<PicturePanel> picturePanel_;
  uint64_t panelShown_ = UINT64_MAX;  // what the panel shows (drawing and edits), to show it again
  bool cropMode_ = false;
  mutable uint64_t infoKey_ = UINT64_MAX;  // PictureInfoOf's last answer
  mutable uint32_t infoEdits_ = UINT32_MAX;
  mutable fw::doc::edit::PictureInfo info_;
  struct PictureDrag {
    enum Kind { None, Pending, Move, Resize, Rotate, Crop } kind = None;
    uint32_t drawing = 0;
    int handle = -1;
    int page = 0, dropPage = 0;
    fw::doc::layout::Rect start, now;  // page points
    fw::doc::layout::Rect full;        // crop: the whole picture
    float grabX = 0, grabY = 0;        // where it was taken, page points
    double angle = 0;
    fw::doc::edit::Crop crop, startCrop;
    bool shift = false;
  } picDrag_;
};
