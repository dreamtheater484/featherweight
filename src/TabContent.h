#pragma once

#include <windows.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

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
class PanelContent;
struct StatusBarModel;
struct ToolbarModel;
namespace uia {
class Tree;
}

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
  // Word documents: spelling checked as you type, and AutoCorrect (Settings, [General]).
  virtual bool ViewSpelling() { return true; }
  virtual bool ViewAutoCorrect() { return true; }
  virtual void ViewPasswordAccepted(TabContent* view) = 0;
  // Opens a side panel with the tab's content in place of whatever was open (null closes it).
  virtual void ViewShowPanel(PanelContent* content) = 0;
  // The content of the open side panel, or null.
  virtual PanelContent* ViewShownPanel() = 0;
};

// The window's own state, for the parts of a tab's toolbar that show it.
struct ToolbarContext {
  bool inverted = false;
  bool searchOpen = false;
  bool fullscreen = false;
};

// What a command looks like now: the toolbar, menus and command search show it this way.
struct CommandState {
  bool enabled = false;
  bool checked = false;
  bool visible = true;    // offered in this kind of tab at all
  std::wstring label;     // "Rotate clockwise"; empty for commands that have no name of their own
  std::wstring shortcut;  // "Ctrl+Shift+=", as the tab's keys have it

  // "Rotate clockwise (Ctrl+Shift+=)", as tooltips show a command.
  std::wstring Tooltip() const { return shortcut.empty() ? label : label + L" (" + shortcut + L")"; }
};

// The file type Save As offers.
struct SaveFormat {
  const wchar_t* name;     // e.g. "PDF document"
  const wchar_t* pattern;  // e.g. "*.pdf"
  const wchar_t* ext;      // e.g. "pdf"
};

enum class TabKind { Pdf, Document, Gallery };

// What happened to a tab's file on disk since it was opened or saved.
enum class FileChange { None, Changed, Missing };

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
  // The window took the keyboard for its own boxes (find, a toolbar control), or gave it back.
  // A tab with focusable parts hides or shows its focus.
  virtual void SetKeyFocus(bool /*has*/) {}
  // The text caret, in client pixels, while the tab has one with key focus: where input methods,
  // dictation and the emoji panel open.
  virtual std::optional<RECT> CaretRect() { return std::nullopt; }

  // ---- Commands ----
  // The command a key runs in this kind of tab, or 0. Ctrl+I can mean night reading in a PDF and
  // italic in a document, so each kind has its own keys; the window's own keys come first.
  virtual int ShortcutFor(UINT vk, UINT mods) const = 0;
  virtual CommandState QueryCommand(int cmd) = 0;
  // The commands command search offers in this kind of tab, besides the window's own. Each has a
  // label from QueryCommand.
  virtual std::vector<int> Commands() { return {}; }
  // Runs one of the tab's commands. Returns false for commands it doesn't have.
  virtual bool Execute(int cmd) = 0;
  virtual void BuildToolbar(ToolbarModel& m, const ToolbarContext& ctx) = 0;
  // Fills the menu of one of the tab's toolbar dropdowns (zoom, page layout).
  virtual void FillMenu(int /*which*/, HMENU /*menu*/) {}
  // Fills the right-click menu for a point in the viewport (client coordinates).
  virtual void FillContextMenu(HMENU menu, int x, int y) = 0;

  // ---- Replace (tabs that edit) ----
  virtual bool CanReplace() const { return false; }
  // Replaces the current match with `with` and goes on to the next. False when it couldn't (the
  // tab says why).
  virtual bool ReplaceCurrent(const std::wstring& /*with*/) { return false; }
  // Replaces every match, as one undo step; how many it replaced.
  virtual int ReplaceAll(const std::wstring& /*with*/) { return 0; }

  // ---- Status bar (editors; PDFs have none) ----
  virtual bool HasStatusBar() const { return false; }
  // What the status bar shows: page and word counts, the language, the zoom.
  virtual void BuildStatusBar(StatusBarModel& /*m*/) {}

  // ---- Find ----
  virtual void StartSearch(const std::wstring& query, bool matchCase) = 0;
  virtual void ClearSearch() = 0;
  virtual void SearchStep(bool forward) = 0;
  virtual int SearchCount() const = 0;
  virtual int SearchCurrent() const = 0;
  virtual bool SearchDone() const = 0;
  virtual std::wstring SelectedText() = 0;

  // ---- File ----
  // Saves to `path`: the file itself, or another (Save As), which the tab then is.
  virtual bool Save(const std::wstring& path, std::wstring& error) = 0;
  virtual SaveFormat SaveAsFormat() const = 0;
  // Where Save As starts: the file, or for a tab without one (a new document) a suggested name.
  virtual std::wstring SuggestedSavePath() const { return Path(); }
  // Editors: unsaved changes go to the recovery folder now and then (the window's timer).
  virtual void Autosave() {}
  // Editors: whether the file changed on disk, when the window or the tab is activated. Each
  // change is reported once.
  virtual FileChange CheckFileOnDisk() { return FileChange::None; }
  virtual bool CanPrint() const = 0;
  // Shows the print dialog and prints. `progress(done, total)` lets the window repaint between pages.
  virtual PrintOutcome Print(HWND owner, const std::function<void(int, int)>& progress, std::wstring& message) = 0;
  virtual FileState State() const = 0;
  virtual void ApplyState(const FileState& s) = 0;

  // ---- Accessibility ----
  // Adds the tab's own UI Automation elements under `parent`, the pane that holds the tab.
  virtual void DescribeUia(uia::Tree& /*t*/, int /*parent*/) {}

  // ---- Pictures dragged over the window
  // Whether the tab adds these files where they're dropped (a Word document being edited takes
  // pictures), rather than the window opening them.
  virtual bool TakesPictures(const std::vector<std::wstring>& /*files*/) const { return false; }
  // Adds them at (x, y), client pixels.
  virtual void DropPictures(const std::vector<std::wstring>& /*files*/, int /*x*/, int /*y*/) {}

  // ---- What kind of content ----
  // Content shown as pages (page box, zoom, page layout, presenting), or null.
  virtual PagedView* Paged() { return nullptr; }
  virtual PdfView* AsPdf() { return nullptr; }
};
