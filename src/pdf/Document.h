#pragma once

#include <windows.h>

#include <fpdf_doc.h>
#include <fpdf_formfill.h>
#include <fpdf_text.h>
#include <fpdfview.h>

#include <memory>
#include <string>
#include <vector>

#include "Outcome.h"
#include "core/Layout.h"

// Callbacks from PDFium's form engine and actions, implemented by PdfView.
class DocumentHost {
 public:
  virtual ~DocumentHost() = default;
  virtual void OnFormInvalidate(int page, double left, double top, double right, double bottom) = 0;
  virtual void OnFormCursor(int cursorType) = 0;
  virtual void OnFormChanged() = 0;
  virtual int OnFormCurrentPage() = 0;
  virtual void OnFormGoToPage(int page, bool hasPos, float x, float y) = 0;
  virtual void OnFormOpenUri(const std::wstring& uri) = 0;
  virtual void OnFormNamedAction(const std::string& name) = 0;
  virtual int OnFormAlert(const std::wstring& message, const std::wstring& title, int type, int icon) = 0;
  virtual void OnFormPrint() = 0;
  virtual void OnFormPageCountChanged() = 0;
  virtual void OnFormCaret(int page, bool visible, double left, double top, double right, double bottom) = 0;
  virtual void OnFormFocusChanged(int page, bool hasRect, const FS_RECTF& rect) = 0;
};

// A note on a page, with its position as a fraction of the page (0..1 from the top-left).
struct NoteInfo {
  int page = 0;
  int index = 0;
  float fx = 0, fy = 0;
  FS_RECTF rect{};
};

struct LinkTarget {
  enum class Kind { None, Page, Uri, Blocked };
  Kind kind = Kind::None;
  int page = -1;
  bool hasPos = false;
  float x = 0, y = 0;  // page coordinates (points), when hasPos
  std::wstring uri;
  FS_RECTF rect{};     // link area in page coordinates
};

// A highlight or note found on a page.
struct AnnotHit {
  enum class Kind { None, Highlight, Note, Stamp };  // Stamp: a drawn signature (f1-pdf)
  Kind kind = Kind::None;
  int index = -1;   // annotation index on the page (changes when annotations are removed)
  FS_RECTF rect{};  // page coordinates
};

struct FormEnv;

// One open PDF. All calls must happen on the UI thread.
class Document {
 public:
  ~Document();

  static OpenResult Open(const std::wstring& path, const std::wstring& password, std::unique_ptr<Document>& out,
                         std::wstring& error);
  static void InitLibrary();
  static bool LibraryAvailable();

  // Sets up the form environment; call once the host is ready to receive callbacks.
  void AttachHost(DocumentHost* host, bool highlightFields);
  void SetHighlightFields(bool on);

  const std::wstring& Path() const { return path_; }
  FPDF_DOCUMENT Handle() const { return doc_; }
  FPDF_FORMHANDLE Form() const { return form_; }
  bool HasForm() const { return hasForm_; }
  bool IsXfa() const { return isXfa_; }
  // f1-ship: the PDF has form scripts or XFA, which only the optional script engine runs, and it isn't loaded.
  bool NeedsScriptEngine() const { return scriptsWanted_ && !ScriptEngineLoaded(); }
  static bool ScriptEngineLoaded();  // the pdfium.dll in use is the build with V8 and XFA
  int PageCount() const { return static_cast<int>(sizes_.size()); }
  const std::vector<fw::PageSizePt>& PageSizes() const { return sizes_; }
  void RefreshPageSizes();
  bool CanPrint() const;
  bool Modified() const { return modified_; }
  void SetModified(bool m) { modified_ = m; }

  FPDF_PAGE Page(int index);           // loaded on demand
  FPDF_PAGE PageIfLoaded(int index) const;
  int IndexOfPage(FPDF_PAGE page) const;
  FPDF_TEXTPAGE TextPage(int index);   // loaded on demand
  // Unloads pages outside [keepFirst, keepLast], except `pinned` (e.g. the focused field's page).
  void TrimPages(int keepFirst, int keepLast, int pinned);
  int LoadedPageCount() const;

  LinkTarget LinkAt(int page, double x, double y);
  // Plain-text web addresses found on a page (not real link annotations).
  struct WebLink {
    std::wstring url;
    std::vector<FS_RECTF> rects;
  };
  const std::vector<WebLink>& WebLinks(int page);

  // Focused form field, if any.
  bool FocusedField(int& page) const;

  // Saves through FileSession's safe save: the original goes to the backups the first time the
  // file opened is replaced, and the file is written to a temporary file first. Unedited, it's the
  // file as it is, byte for byte.
  bool Save(const std::wstring& path, std::wstring& error);
  // ---- The whole document (f1-pdf: page tools, undo, signing) ----
  // The document as a save writes it now: the file as opened while nothing changed, otherwise
  // written whole, or incrementally when it's signed (pdftools::SaveDocument). False when it
  // couldn't be written.
  bool CurrentBytes(std::string& out);
  // The document as edited, written and opened again, so it reads only from its own bytes: pages
  // inserted from another PDF go on reading from that file's until then.
  bool ReloadAsEdited();
  // Opens `bytes` in place of the document: the same object, so the view's references stay. Every
  // page and the form start again, without running the document's open scripts. The view drops
  // what it holds of the pages first (RenderCache::Clear).
  bool Reload(std::string bytes, bool modified);
  // Writes `bytes` as the document's file at `path` (safely, keeping the original in the backups
  // as Save does) and opens them in its place: after signing, later saves add to the signed file.
  bool WriteAndReload(const std::wstring& path, std::string bytes, std::wstring& error);
  // Before pages are added, removed, moved or rotated: every page handle closed.
  void CloseAllPages();
  // After: the page sizes read again, and the document marked changed.
  void PagesChanged();
  bool InMemory() const { return file_ == INVALID_HANDLE_VALUE; }
  bool Encrypted() const;
  int SignatureCount() const;
  // Export PDF: a copy at `path` with filled-in form fields, highlights and notes made part of
  // the pages, as printing them would; the document itself stays as it is. Not for protected or
  // XFA documents.
  bool CanExportFlattened() const;
  bool ExportFlattened(const std::wstring& path, std::wstring& error);

  // Highlights and notes, stored as standard PDF annotations so other readers show them too.
  bool AddHighlight(int page, const std::vector<FS_RECTF>& rects);
  // Adds a note whose icon's top-left corner is at (x, y). Returns its index, or -1.
  int AddNote(int page, float x, float y, const std::wstring& text);
  bool SetNoteText(int page, int index, const std::wstring& text);
  std::wstring NoteText(int page, int index);
  AnnotHit AnnotAt(int page, double x, double y);
  // All notes on a page. Doesn't trigger any page scripts, so it is safe to call for every page.
  std::vector<NoteInfo> NotesOnPage(int page);
  bool RemoveAnnot(int page, int index);

  int CurrentPageForForm() const;
  DocumentHost* HostPtr() const { return host_; }

 private:
  friend struct FormEnv;
  Document() = default;
  void ClosePage(int index);
  LinkTarget DestToTarget(FPDF_DEST dest);

  void InitForm(bool openActions);
  bool WriteFile(const std::wstring& path, const std::string& bytes, std::wstring& error);

  std::wstring path_;
  std::string memory_;  // the whole file, for files that fit comfortably in memory
  std::string password_;  // as PDFium took it, for Reload
  bool backedUp_ = false;  // the original is in the backups (or there was none to keep)
  bool savedEdits_ = false;  // saved since memory_ was read: the document is memory_ and its changes
  bool highlight_ = false;
  HANDLE file_ = INVALID_HANDLE_VALUE;
  FPDF_FILEACCESS access_{};
  FPDF_DOCUMENT doc_ = nullptr;
  FPDF_FORMHANDLE form_ = nullptr;
  std::unique_ptr<FormEnv> env_;
  bool hasForm_ = false;
  bool isXfa_ = false;
  bool scriptsWanted_ = false;  // f1-ship
  bool modified_ = false;
  std::vector<fw::PageSizePt> sizes_;
  std::vector<FPDF_PAGE> pages_;
  std::vector<FPDF_TEXTPAGE> textPages_;
  std::vector<std::unique_ptr<std::vector<WebLink>>> webLinks_;
  std::vector<uint64_t> lastUse_;
  uint64_t useCounter_ = 0;
  DocumentHost* host_ = nullptr;
};
