#pragma once

#include <windows.h>

#include <functional>
#include <string>

#include "Outcome.h"
#include "Settings.h"
#include "ui/Banner.h"
#include "ui/Gfx.h"
#include "ui/Widgets.h"

// Modifier flags, identical to PDFium's FWL_EVENTFLAG values.
constexpr UINT kModShift = 1;
constexpr UINT kModCtrl = 2;
constexpr UINT kModAlt = 4;

class TabContent;
class PagedView;
class PdfView;
struct ToolbarModel;

// What every kind of tab needs from the window around it.
class ViewHost {
 public:
  virtual ~ViewHost() = default;
  virtual void ViewInvalidate() = 0;
  virtual void ViewStateChanged() = 0;  // page, zoom or modified state changed
  virtual void ViewMessage(BannerKind kind, const std::wstring& text) = 0;
  virtual HWND ViewHwnd() = 0;
  virtual void ViewRequestPrint() = 0;
  virtual void ViewFocusTextBox(TextBox* box) = 0;
  virtual bool ViewInverted() = 0;  // night reading
  virtual void ViewPasswordAccepted(TabContent* view) = 0;
};

// The window's own state, for the parts of a tab's toolbar that show it.
struct ToolbarContext {
  bool inverted = false;
  bool searchOpen = false;
  bool fullscreen = false;
};

struct CommandState {
  bool enabled = false;
  bool checked = false;
};

// The file type Save As offers.
struct SaveFormat {
  const wchar_t* name;     // e.g. "PDF document"
  const wchar_t* pattern;  // e.g. "*.pdf"
  const wchar_t* ext;      // e.g. "pdf"
};

enum class TabKind { Pdf };

// One tab: a PDF today, and documents, spreadsheets and presentations later. The window owns the
// chrome (title bar, toolbar, find panel, banners); the tab owns everything inside its viewport.
class TabContent {
 public:
  virtual ~TabContent() = default;
  virtual TabKind Kind() const = 0;

  // ---- Loading and identity ----
  virtual OpenResult Load(const std::wstring& password, std::wstring& error) = 0;
  virtual bool Ready() const = 0;  // loaded, and not waiting for a password
  virtual bool NeedsPassword() const { return false; }
  virtual TextBox* PasswordBox() { return nullptr; }
  virtual const std::wstring& Path() const = 0;
  virtual std::wstring Title() const = 0;
  virtual bool Modified() const = 0;

  // ---- Geometry and painting ----
  virtual void SetViewport(const RECT& rc, float ui) = 0;
  virtual void Draw(gfx::Canvas& c, ID2D1DeviceContext* dc) = 0;
  virtual bool HasIdleWork() const = 0;
  virtual bool IdleWork(ID2D1DeviceContext* dc, double deadlineMs) = 0;
  virtual void ReleaseDeviceResources() = 0;
  // Called when the tab goes to the background: frees memory that is quick to rebuild.
  virtual void OnDeactivated() = 0;
  // App settings that affect how the tab looks have changed.
  virtual void SettingsChanged() {}

  // ---- Input ----
  virtual void OnMouseDown(int x, int y, int button, UINT mods, bool doubleClick) = 0;
  virtual void OnMouseMove(int x, int y, UINT mods) = 0;
  virtual void OnMouseUp(int x, int y, int button, UINT mods) = 0;
  virtual void OnMouseLeave() = 0;
  virtual void OnWheel(int delta, bool horizontal, UINT mods, int x, int y) = 0;
  virtual bool OnKeyDown(UINT vk, UINT mods) = 0;
  virtual bool OnChar(wchar_t ch, UINT mods) = 0;
  virtual HCURSOR CursorAt(int x, int y) = 0;
  virtual std::wstring TooltipAt(int x, int y) = 0;
  virtual bool Dragging() const = 0;
  // Keys go to the tab before the window's shortcuts (a focused form field, text being typed).
  virtual bool HasKeyFocus() const = 0;

  // ---- Commands ----
  // The command a key runs in this kind of tab, or 0. Ctrl+I can mean night reading in a PDF and
  // italic in a document, so each kind has its own keys; the window's own keys come first.
  virtual int ShortcutFor(UINT vk, UINT mods) const = 0;
  virtual CommandState QueryCommand(int cmd) = 0;
  // Runs one of the tab's commands. Returns false for commands it doesn't have.
  virtual bool Execute(int cmd) = 0;
  virtual void BuildToolbar(ToolbarModel& m, const ToolbarContext& ctx) = 0;
  // Fills the menu of one of the tab's toolbar dropdowns (zoom, page layout).
  virtual void FillMenu(int /*which*/, HMENU /*menu*/) {}
  // Fills the right-click menu for a point in the viewport (client coordinates).
  virtual void FillContextMenu(HMENU menu, int x, int y) = 0;

  // ---- Find ----
  virtual void StartSearch(const std::wstring& query, bool matchCase) = 0;
  virtual void ClearSearch() = 0;
  virtual void SearchStep(bool forward) = 0;
  virtual int SearchCount() const = 0;
  virtual int SearchCurrent() const = 0;
  virtual bool SearchDone() const = 0;
  virtual std::wstring SelectedText() = 0;

  // ---- File ----
  virtual bool Save(const std::wstring& path, std::wstring& error) = 0;
  virtual SaveFormat SaveAsFormat() const = 0;
  virtual bool CanPrint() const = 0;
  // Shows the print dialog and prints. `progress(done, total)` lets the window repaint between pages.
  virtual PrintOutcome Print(HWND owner, const std::function<void(int, int)>& progress, std::wstring& message) = 0;
  virtual FileState State() const = 0;
  virtual void ApplyState(const FileState& s) = 0;

  // ---- What kind of content ----
  // Content shown as pages (page box, zoom, page layout, presenting), or null.
  virtual PagedView* Paged() { return nullptr; }
  virtual PdfView* AsPdf() { return nullptr; }
};
