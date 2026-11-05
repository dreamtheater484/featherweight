#pragma once

#include <windows.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "PagedView.h"
#include "doc/edit/Editor.h"
#include "doc/layout/Layout.h"
#include "draw/Drawing.h"
#include "doc/model/Document.h"
#include "pkg/XmlDom.h"
#include "text/Fonts.h"
#include "word/DocFile.h"

// A Word document in a tab (plan milestones D1, D2): the model, laid out page by page by
// fw::doc::layout (the first page at once, the rest in idle time), drawn through draw::Painter,
// with selection and copy, find, links, comments and tracked changes shown, and printing. Its
// file (saving, backups, recovery, changes on disk) is kept by a DocFile.
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

  void Draw(gfx::Canvas& c, ID2D1DeviceContext* dc) override;
  bool HasIdleWork() const override;
  bool IdleWork(ID2D1DeviceContext* dc, double deadlineMs) override;
  void ReleaseDeviceResources() override {}
  void OnDeactivated() override { drag_ = Drag::None; }

  void OnMouseDown(int x, int y, int button, UINT mods, bool doubleClick) override;
  void OnMouseMove(int x, int y, UINT mods) override;
  void OnMouseUp(int x, int y, int button, UINT mods) override;
  void OnMouseLeave() override;
  bool OnKeyDown(UINT vk, UINT mods) override;
  bool OnChar(wchar_t, UINT) override { return false; }
  HCURSOR CursorAt(int x, int y) override;
  std::wstring TooltipAt(int x, int y) override;
  bool HasKeyFocus() const override { return false; }

  int ShortcutFor(UINT vk, UINT mods) const override { return Shortcut(vk, mods); }
  CommandState QueryCommand(int cmd) override;
  std::vector<int> Commands() override;
  bool Execute(int cmd) override;
  void BuildToolbar(ToolbarModel& m, const ToolbarContext& ctx) override;
  void FillContextMenu(HMENU menu, int x, int y) override;

  bool HasStatusBar() const override { return Ready(); }
  void BuildStatusBar(StatusBarModel& m) override;

  void StartSearch(const std::wstring& query, bool matchCase) override;
  void ClearSearch() override;
  void SearchStep(bool forward) override;
  int SearchCount() const override { return static_cast<int>(matches_.size()); }
  int SearchCurrent() const override { return current_; }
  bool SearchDone() const override { return true; }
  std::wstring SelectedText() override;

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
  // After an edit, its undo or redo: the layout lays out again from there, the page list follows
  // it (it may be shorter), and the file learns whether the document now differs from the saved one.
  void OnEdited(const fw::doc::edit::Change& change);
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
  size_t words_ = 0;
  size_t wordsNext_ = 0;  // the next main-story paragraph to count
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
};
