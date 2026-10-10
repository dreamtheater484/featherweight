#pragma once

#include <windows.h>

#include <fpdf_doc.h>
#include <fpdf_formfill.h>
#include <fpdf_text.h>
#include <fpdfview.h>

#include <memory>
#include <string>
#include <vector>

#include "core/Layout.h"

// Callbacks from PDFium's form engine and actions, implemented by DocumentView.
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

struct LinkTarget {
  enum class Kind { None, Page, Uri, Blocked };
  Kind kind = Kind::None;
  int page = -1;
  bool hasPos = false;
  float x = 0, y = 0;  // page coordinates (points), when hasPos
  std::wstring uri;
  FS_RECTF rect{};     // link area in page coordinates
};

enum class OpenResult { Ok, NeedPassword, WrongPassword, Failed };

// A highlight or note found on a page.
struct AnnotHit {
  enum class Kind { None, Highlight, Note };
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

  bool Save(const std::wstring& path, std::wstring& error);

  // Highlights and notes, stored as standard PDF annotations so other readers show them too.
  bool AddHighlight(int page, const std::vector<FS_RECTF>& rects);
  // Adds a note whose icon's top-left corner is at (x, y). Returns its index, or -1.
  int AddNote(int page, float x, float y, const std::wstring& text);
  bool SetNoteText(int page, int index, const std::wstring& text);
  std::wstring NoteText(int page, int index);
  AnnotHit AnnotAt(int page, double x, double y);
  bool RemoveAnnot(int page, int index);

  int CurrentPageForForm() const;
  DocumentHost* HostPtr() const { return host_; }

 private:
  friend struct FormEnv;
  Document() = default;
  void ClosePage(int index);
  LinkTarget DestToTarget(FPDF_DEST dest);

  std::wstring path_;
  std::string memory_;  // the whole file, for files that fit comfortably in memory
  HANDLE file_ = INVALID_HANDLE_VALUE;
  FPDF_FILEACCESS access_{};
  FPDF_DOCUMENT doc_ = nullptr;
  FPDF_FORMHANDLE form_ = nullptr;
  std::unique_ptr<FormEnv> env_;
  bool hasForm_ = false;
  bool isXfa_ = false;
  bool modified_ = false;
  std::vector<fw::PageSizePt> sizes_;
  std::vector<FPDF_PAGE> pages_;
  std::vector<FPDF_TEXTPAGE> textPages_;
  std::vector<std::unique_ptr<std::vector<WebLink>>> webLinks_;
  std::vector<uint64_t> lastUse_;
  uint64_t useCounter_ = 0;
  DocumentHost* host_ = nullptr;
};
