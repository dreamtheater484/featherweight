#pragma once

#include <windows.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "PagedView.h"
#include "doc/layout/Layout.h"
#include "draw/Drawing.h"
#include "doc/edit/Editor.h"
#include "doc/proof/AutoCorrect.h"
#include "doc/proof/Spelling.h"
#include "doc/model/Document.h"
#include "pkg/XmlDom.h"
#include "text/Fonts.h"

// A Word document in a tab (plan milestones D1 and D2): the model, laid out page by page by
// fw::doc::layout (the first page at once, the rest in idle time), drawn through draw::Painter,
// with selection and copy, find, links, comments and tracked changes shown, and printing. A
// document Featherweight can write back is edited in place (DocEdit.cpp): the caret, typing,
// deleting, undo and redo, cut and paste, through fw::doc::edit.
class DocView : public PagedView {
 public:
  DocView(ViewHost& host, std::wstring path);
  ~DocView() override;

  // The document viewer's keys.
  static int Shortcut(UINT vk, UINT mods);

  // ---- TabContent ----
  TabKind Kind() const override { return TabKind::Document; }
  OpenResult Load(const std::wstring& password, std::wstring& error) override;
  bool Ready() const override { return pagination_ != nullptr; }
  const std::wstring& Path() const override { return path_; }
  std::wstring Title() const override;
  bool Modified() const override { return editor_ && editor_->Modified(); }

  void Draw(gfx::Canvas& c, ID2D1DeviceContext* dc) override;
  bool HasIdleWork() const override;
  bool IdleWork(ID2D1DeviceContext* dc, double deadlineMs) override;
  void ReleaseDeviceResources() override {}
  void OnDeactivated() override;

  void OnMouseDown(int x, int y, int button, UINT mods, bool doubleClick) override;
  void OnMouseMove(int x, int y, UINT mods) override;
  void OnMouseUp(int x, int y, int button, UINT mods) override;
  void OnMouseLeave() override;
  bool OnKeyDown(UINT vk, UINT mods) override;
  bool OnChar(wchar_t ch, UINT mods) override;
  HCURSOR CursorAt(int x, int y) override;
  std::wstring TooltipAt(int x, int y) override;
  bool HasKeyFocus() const override { return Editing() && keyFocus_; }
  void SetKeyFocus(bool has) override;

  int ShortcutFor(UINT vk, UINT mods) const override { return Shortcut(vk, mods); }
  CommandState QueryCommand(int cmd) override;
  std::vector<int> Commands() override;
  bool Execute(int cmd) override;
  void BuildToolbar(ToolbarModel& m, const ToolbarContext& ctx) override;
  void FillContextMenu(HMENU menu, int x, int y) override;

  bool HasStatusBar() const override { return Ready(); }
  void BuildStatusBar(StatusBarModel& m) override;
  // Spelling or AutoCorrect turned on or off.
  void SettingsChanged() override;

  void StartSearch(const std::wstring& query, bool matchCase) override;
  void ClearSearch() override;
  void SearchStep(bool forward) override;
  int SearchCount() const override { return static_cast<int>(matches_.size()); }
  int SearchCurrent() const override { return current_; }
  bool SearchDone() const override { return true; }
  std::wstring SelectedText() override;

  // The file with the parts edits changed written anew, the rest as it was; read back before it
  // replaces anything. Unedited, a copy of the file as it was opened.
  bool Save(const std::wstring& path, std::wstring& error) override;
  SaveFormat SaveAsFormat() const override;
  bool CanPrint() const override { return Ready(); }
  PrintOutcome Print(HWND owner, const std::function<void(int, int)>& progress, std::wstring& message) override;
  // Every page to Microsoft Print to PDF, into a file, with no dialog (the visual checks' print step).
  PrintOutcome PrintToFile(const std::wstring& path, std::wstring& message);

  // ---- Text ----
  bool HasSelection() const { return anchor_ != focus_; }
  void Copy();
  void SelectAll();

  // ---- Editing (DocEdit.cpp) ----
  bool Editing() const { return editor_ != nullptr && Ready() && !presentation_; }
  void Cut();
  void Paste();
  void Undo();
  void Redo();

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
  // Edits, their undo and redo: the layout lays out again, positions kept here move.
  void OnEdited(const fw::doc::edit::Change& change);
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

  std::wstring path_;
  std::unique_ptr<fw::doc::Document> doc_;
  std::unique_ptr<fw::text::Fonts> fonts_;
  std::unique_ptr<fw::doc::layout::Layout> pagination_;
  std::vector<fw::PageSizePt> sizes_;
  bool placeholder_ = false;  // nothing is laid out: one empty page

  // Drawings, by index into Document::Drawings(), made when first shown. The pictures are shared
  // and read from the package, which the document keeps.
  struct DrawingSlot {
    bool tried = false;
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
    size_t author = 0;  // its color
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

  // Find
  std::vector<Match> matches_;
  int current_ = -1;

  // Editing: null for a document shown but not edited (one the model can't write back).
  std::unique_ptr<fw::doc::edit::Editor> editor_;
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

  // Proofing (DocProof.cpp): spelling checked in idle time with Windows' spell checker, the
  // pages on screen first; nothing runs once everything is checked.
  std::unique_ptr<fw::doc::proof::Spelling> spelling_;
  bool spellingWanted_ = true;  // made on the first idle tick, when the setting is on
  int preferredPage_ = -1;      // the page whose paragraphs were checked first
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
};
