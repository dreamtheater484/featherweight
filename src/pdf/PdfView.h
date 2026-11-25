#pragma once

#include <windows.h>

#include <wincrypt.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "PagedView.h"
#include "PasswordCard.h"
#include "pdf/Document.h"
#include "pdf/RenderCache.h"
#include "pdf/Search.h"
#include "pdf/TextSelect.h"
#include "ui/Gfx.h"
#include "ui/Widgets.h"

class PdfView;

// A note to open or create: index -1 means a new note at (x, y) in page coordinates.
struct NoteTarget {
  int page = -1;
  int index = -1;
  float x = 0, y = 0;
};

// What a PDF tab needs from the window, beyond what every tab needs.
class PdfHost : public ViewHost {
 public:
  virtual bool ViewCoverAlone() = 0;
  virtual bool ViewHighlightFields() = 0;
  virtual void ViewOpenNote(PdfView* view, const NoteTarget& note) = 0;
};

// A PDF in a tab: rendering, forms, links, text selection, search, highlights and notes.
class PdfView : public PagedView, public DocumentHost {
 public:
  PdfView(PdfHost& host, std::wstring path);
  ~PdfView() override;

  // The reader's keys (also used while no tab is open).
  static int Shortcut(UINT vk, UINT mods);

  // ---- TabContent ----
  TabKind Kind() const override { return TabKind::Pdf; }
  void DescribeUia(uia::Tree& t, int parent) override;
  PdfView* AsPdf() override { return this; }
  OpenResult Load(const std::wstring& password, std::wstring& error) override;
  bool Ready() const override { return state_ == LoadState::Ready; }
  bool NeedsPassword() const override { return state_ == LoadState::NeedPassword; }
  TextBox* PasswordBox() override { return NeedsPassword() ? &pw_.Box() : nullptr; }
  const std::wstring& Path() const override;
  std::wstring Title() const override;
  bool Modified() const override { return doc_ && doc_->Modified(); }
  Document* Doc() { return doc_.get(); }

  void Draw(gfx::Canvas& c, ID2D1DeviceContext* dc) override;
  bool HasIdleWork() const override;
  bool IdleWork(ID2D1DeviceContext* dc, double deadlineMs) override;
  void ReleaseDeviceResources() override;
  void OnDeactivated() override;
  void SettingsChanged() override;  // cover page, field highlight

  void OnMouseDown(int x, int y, int button, UINT mods, bool doubleClick) override;
  void OnMouseMove(int x, int y, UINT mods) override;
  void OnMouseUp(int x, int y, int button, UINT mods) override;
  void OnMouseLeave() override;
  bool OnKeyDown(UINT vk, UINT mods) override;
  bool OnChar(wchar_t ch, UINT mods) override;
  HCURSOR CursorAt(int x, int y) override;
  std::wstring TooltipAt(int x, int y) override;
  bool HasKeyFocus() const override;

  int ShortcutFor(UINT vk, UINT mods) const override;
  CommandState QueryCommand(int cmd) override;
  std::vector<int> Commands() override;
  bool Execute(int cmd) override;
  void BuildToolbar(ToolbarModel& m, const ToolbarContext& ctx) override;
  void FillContextMenu(HMENU menu, int x, int y) override;

  void StartSearch(const std::wstring& query, bool matchCase, bool wholeWords) override;
  void ClearSearch() override;
  void SearchStep(bool forward) override;
  int SearchCount() const override;
  int SearchCurrent() const override;
  bool SearchDone() const override;
  std::wstring SelectedText() override;
  bool SearchActive() const { return search_.Active(); }

  bool Save(const std::wstring& path, std::wstring& error) override;
  SaveFormat SaveAsFormat() const override { return {L"PDF document", L"*.pdf", L"pdf"}; }
  bool CanPrint() const override { return doc_ && doc_->CanPrint(); }
  PrintOutcome Print(HWND owner, const std::function<void(int, int)>& progress, std::wstring& message) override;
  // A copy with form fields and annotations flattened into the pages: what printing to a PDF
  // printer gives, without the printer. Only where printing is allowed.
  bool CanExportPdf() const override { return doc_ && doc_->CanPrint() && doc_->CanExportFlattened(); }
  bool ExportPdf(const std::wstring& path, std::wstring& message) override;

  // ---- Text ----
  bool HasSelection() const { return !selection_.Empty(); }
  void Copy();
  void SelectAll();
  void SelectAllOnPage();

  // ---- Highlights and notes ----
  bool HighlightSelection();
  // Annotation under a window point, with its page.
  AnnotHit AnnotAtPoint(int x, int y, int& page);
  void RemoveAnnot(int page, int index);
  // A new note at a window point (false if the point is not on a page).
  bool NewNoteAt(int x, int y, NoteTarget& note);
  std::wstring NoteText(const NoteTarget& note);
  // Saves the note's text (creating it if new). Empty text removes the note.
  void SaveNote(const NoteTarget& note, const std::wstring& text);
  // Window position of a note's icon (or of the spot where a new note goes).
  D2D1_POINT_2F NoteScreenPos(const NoteTarget& note);

  // ---- Page tools (F1, f1-pdf: PdfPages.cpp) ----
  // Why the page tools are off for this PDF (an XFA form, protected against changes), or empty.
  std::wstring PageToolsBlocked() const;
  // The pages a tool acts on: those selected in the Pages panel, or the current page.
  std::vector<int> ToolPages() const;
  bool RotatePages(const std::vector<int>& pages, int quarterTurns);
  bool DeletePages(const std::vector<int>& pages);
  // Moves `pages` to the gap before page `gap` (PageCount(): after the last).
  bool MovePages(const std::vector<int>& pages, int gap);
  // Asks for PDF files and inserts their pages before page `at` (PageCount(): at the end).
  bool InsertPagesFromFiles(int at);
  // Asks where, then writes `pages` as a new PDF.
  bool ExtractPages(const std::vector<int>& pages);
  // Asks for the name, then writes each part as a PDF of its own beside it.
  bool SplitPages(const std::vector<std::vector<int>>& parts);
  // The same without their dialogs (also the screen tests' --script pdftool:...).
  bool InsertFiles(const std::vector<std::wstring>& paths, int at);
  bool ExtractTo(const std::vector<int>& sortedPages, const std::wstring& path);
  bool SplitTo(const std::vector<std::vector<int>>& parts, const std::wstring& path);
  bool RunPageScript(const std::wstring& arg);
  bool CanUndoPages() const { return !pageUndo_.empty() && !ocr_; }
  bool CanRedoPages() const { return !pageRedo_.empty() && !ocr_; }
  bool UndoPages();
  bool RedoPages();
  // ---- Recognize text (F1, f1-pdf: PdfOcr.cpp) ----
  // Windows' OCR on the pages that look scanned among `pages` (empty: every page), in the
  // background, one page at a time; Esc stops it. Undo takes it back.
  void StartOcr(std::vector<int> pages);
  void StopOcr();
  bool OcrRunning() const { return ocr_ != nullptr; }
  // The same, finished before it returns (the screen tests' pdftool:ocr). Returns the pages read.
  int OcrNow(const std::vector<int>& pages);

  // ---- Signing (F1, f1-pdf: PdfSign.cpp) ----
  // Checks every signature against the file and says what it found (`always`: also when unsigned).
  void CheckSignatures(bool always);
  // Why this PDF can't be signed (XFA, protected), or empty.
  std::wstring SigningBlocked() const;
  // Picks a certificate from the Windows store and where the signed PDF goes, then signs.
  bool SignWithCertificate();
  // Signs with `cert` and writes the signed PDF to `path`, which the tab then shows.
  bool SignWith(const CERT_CONTEXT* cert, const std::wstring& path);
  // The screen tests: a PFX loaded in memory, never into a certificate store.
  bool SignWithPfxForTesting(const std::wstring& pfx, const std::wstring& password, const std::wstring& out);
  void FillMenu(int which, HMENU menu) override;
  // A drawn signature (the Signature panel): Place, then a click on a page puts it there as a stamp.
  void OpenSignaturePanel();
  bool PlacingSignature() const { return placingSignature_; }
  void StartPlacingSignature();
  void StopPlacingSignature();
  // Puts the panel's signature centred on (x, y) of `page` (page coordinates).
  bool PlaceSignatureAt(int page, float x, float y);
  bool StampForTesting();

  // The thumbnail for the Pages panel, asking for it when it isn't there yet.
  ID2D1Bitmap* Thumbnail(int page, int priority);
  int ViewRotation() const { return rotation_; }

  // ---- DocumentHost (PDFium callbacks) ----
  void OnFormInvalidate(int page, double l, double t, double r, double b) override;
  void OnFormCursor(int type) override;
  void OnFormChanged() override;
  int OnFormCurrentPage() override { return currentPage_; }
  void OnFormGoToPage(int page, bool hasPos, float x, float y) override;
  void OnFormOpenUri(const std::wstring& uri) override;
  void OnFormNamedAction(const std::string& name) override;
  int OnFormAlert(const std::wstring& message, const std::wstring& title, int type, int icon) override;
  void OnFormPrint() override;
  void OnFormPageCountChanged() override;
  void OnFormCaret(int page, bool visible, double l, double t, double r, double b) override;
  void OnFormFocusChanged(int page, bool hasRect, const FS_RECTF& rect) override;

 protected:
  // ---- PagedView ----
  const std::vector<fw::PageSizePt>* Pages() const override;
  bool CoverAlone() const override;
  void OnPresentationChanged(bool on) override;

 private:
  enum class LoadState { Empty, NeedPassword, Ready };

  // Coordinates
  bool ToPagePoint(int page, int x, int y, double& px, double& py);
  D2D1_RECT_F PageRectToScreen(int page, const FS_RECTF& r);
  void ScrollPageRectIntoView(int page, const FS_RECTF& r, bool center);
  void GoToPosition(int page, bool hasPos, float x, float y);

  // Painting
  void DrawPage(gfx::Canvas& c, ID2D1DeviceContext* dc, int page, bool inverted);
  void DrawBitmap(ID2D1DeviceContext* dc, ID2D1Bitmap* bmp, const D2D1_RECT_F& dest, const D2D1_RECT_F& src,
                  bool inverted, bool exact);
  void DrawOverlays(gfx::Canvas& c, int first, int last);

  // Interaction helpers
  void AfterFormEvent();
  void FollowLink(const LinkTarget& link);
  bool FieldKey(UINT vk, UINT mods);
  bool FocusFieldOnNextPage(int fromPage, bool backwards);
  void ScrollToMatch(int index);
  void SubmitPassword(const std::wstring& password);
  void AnnotationsChanged(int page);
  // Notes: hover preview, markers along the right edge.
  void UpdateHoverNote(int x, int y);
  void DrawNoteHover(gfx::Canvas& c);
  void DrawNoteMarkers(gfx::Canvas& c);
  void DrawNoteIconsUninverted(gfx::Canvas& c, int first, int last);
  bool ScanNotes(double deadlineMs);
  void RescanNotes(int page);
  D2D1_RECT_F MarkerRect(const NoteInfo& n) const;
  int MarkerAt(int x, int y) const;
  void GoToNote(const NoteInfo& n);
  void DrawSelectionBar(gfx::Canvas& c);
  bool SelectionBarRect(D2D1_RECT_F& rect);

  PdfHost& pdfHost_;
  std::wstring path_;
  std::unique_ptr<Document> doc_;
  std::unique_ptr<RenderCache> cache_;
  LoadState state_ = LoadState::Empty;

  PasswordCard pw_;

  // Interaction
  int formPage_ = -1;
  int formCursor_ = 0;
  LinkTarget pendingLink_;
  NoteTarget pendingNote_;
  struct HoverNote {
    int page = -1, index = -1;
    FS_RECTF rect{};
    std::wstring text;
    double since = 0;
  } hoverNote_;
  std::vector<NoteInfo> notes_;  // every note in the document, for the markers
  std::vector<bool> notesScanned_;
  int noteScanNext_ = 0;
  int formHoverPage_ = -1;
  int markerHover_ = -1;
  // Small "Highlight / Copy" bar shown next to selected text.
  bool selBar_ = false;
  bool barPressed_ = false;
  Button selHighlight_, selCopy_;
  ButtonSet selButtons_;
  TextSelection selection_;
  std::map<int, std::vector<FS_RECTF>> selRects_;
  int selRectsVersion_ = -1;
  int mouseX_ = -1, mouseY_ = -1;
  bool lastModified_ = false;
  // Where the right-click menu was opened, and the highlight or note under it.
  POINT contextPt_{};
  int contextPage_ = -1;
  AnnotHit contextAnnot_;

  // Search
  Search search_;
  int lastSearchCurrent_ = -1;

  // XFA caret
  struct {
    int page = -1;
    bool visible = false;
    double l = 0, t = 0, r = 0, b = 0;
  } caret_;

  ComPtr<ID2D1Effect> invertFx_;
  ID2D1DeviceContext* fxDc_ = nullptr;

  // ---- Page tools (PdfPages.cpp) ----
  class PagesPanel;
  friend class PagesPanel;
  // Before pages change: asks about breaking a signature, keeps the document for Undo and lets go
  // of every page. False when the change shouldn't go ahead.
  bool BeginPageChange();
  // After: the pages, sizes, notes and the panel again; a failed change goes back to how it was.
  void EndPageChange(bool ok, int focusPage);
  // The document was replaced or its pages changed: everything the view holds of them goes.
  void ForgetPages();
  bool ChoosePdfToSave(const std::wstring& suggested, const std::wstring& title, std::wstring& path,
                       bool allowThisFile = false);
  void TogglePagesPanel();
  struct PageUndo {
    std::string bytes;  // the document before (or, for Redo, after) the change
    bool modified = false;
  };
  std::unique_ptr<PagesPanel> pagesPanel_;
  std::vector<PageUndo> pageUndo_, pageRedo_;
  std::string pageBefore_;  // between BeginPageChange and EndPageChange
  bool pageBeforeModified_ = false;
  bool signatureWarned_ = false;  // asked once whether to change a signed PDF

  // ---- Drawn signatures (SignaturePanel.cpp, PdfSign.cpp) ----
  class SignaturePanel;
  friend class SignaturePanel;
  std::unique_ptr<SignaturePanel> signPanel_;
  bool placingSignature_ = false;

  // ---- Recognize text (PdfOcr.cpp) ----
  struct OcrJob;
  // Shared with the thread that runs Windows' OCR, which may outlive the tab by a page.
  std::shared_ptr<OcrJob> ocr_;
  bool OcrHasWork() const;  // the next page to start, or the last one's words to write
  bool OcrStep();  // on the UI thread, from IdleWork
  void FinishOcr();
};
