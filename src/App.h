#pragma once

#include <windows.h>

#include <memory>
#include <string>
#include <vector>

#include "DropTarget.h"
#include "PagedView.h"
#include "Settings.h"
#include "TabContent.h"
#include "Updater.h"
#include "pkg/Recovery.h"
#include "pdf/NoteEditor.h"
#include "pdf/PdfView.h"
#include "ui/Banner.h"
#include "ui/CommandSearch.h"
#include "ui/EmptyState.h"
#include "ui/FilePage.h"
#include "ui/Gfx.h"
#include "ui/Ribbon.h"
#include "ui/SearchPanel.h"
#include "ui/SidePanel.h"
#include "ui/StatusBar.h"
#include "ui/SwapChain.h"
#include "ui/TitleBar.h"
#include "ui/Toolbar.h"
#include "ui/Tooltip.h"
#include "ui/Uia.h"

struct AppOptions {
  std::vector<std::wstring> files;
  int showCommand = SW_SHOWDEFAULT;
  // Snapshot mode (used for automated visual checks): render off-screen to a PNG and exit.
  std::wstring snapshotPath;
  int snapshotW = 1280, snapshotH = 800;
  float snapshotScale = 1.0f;
  std::wstring snapshotTheme;  // "light" or "dark"
  bool snapshotInvert = false;
  // How a snapshot is drawn: Direct2D's software renderer (the default, the same on every PC),
  // the shared Direct3D device off-screen, or the real window and swap chain captured from the screen.
  bool snapshotGpu = false;
  bool snapshotWindow = false;
  std::wstring script;         // steps separated by ';' (see App::RunScript)
  bool forceWarp = false;      // draw with WARP even when a graphics adapter works
  bool widgetGallery = false;  // opens the widget gallery (settings are neither read nor saved)
  // For measurements (fwtool bench-app): settings and recent files are neither read nor saved,
  // there's no update check, notice or registration, and files are never handed to or taken
  // from another running Featherweight.
  bool sandbox = false;
  // --show <deck> [--show-bench <report.json>]: the deck straight into a slide show, without the
  // app's window (trying the show, and the frame-rate bench).
  std::wstring showPath, showBench, showCapture;
  bool showWindowed = false, showPresenter = false;  // --show-windowed, --show-presenter (P3)
};

class App : public PdfHost, public DropHost {
 public:
  static constexpr wchar_t kWindowClass[] = L"FeatherweightWindow";
  // A sandboxed window's class, so a Featherweight that hands files on never finds it.
  static constexpr wchar_t kSandboxWindowClass[] = L"FeatherweightSandboxWindow";
  static constexpr ULONG_PTR kCopyDataOpen = 0x46570001;

  App();
  ~App() override;
  int Run(const AppOptions& opts);

  // ViewHost and PdfHost
  void ViewInvalidate() override;
  void ViewStateChanged() override;
  void ViewMessage(BannerKind kind, const std::wstring& text) override;
  void ViewSaved(TabContent* view, bool ok, const std::wstring& error) override;
  bool ViewConfirm(const std::wstring& text, const std::wstring& ok) override;
  HWND ViewHwnd() override { return hwnd_; }
  void ViewRequestPrint() override;
  void ViewFocusTextBox(TextBox* box) override;
  bool ViewInverted() override { return settings_.invert; }
  bool ViewCoverAlone() override { return settings_.coverAlone; }
  bool ViewHighlightFields() override { return settings_.highlightFields; }
  bool ViewSpelling() override { return settings_.spelling; }
  bool ViewAutoCorrect() override { return settings_.autoCorrect; }
  bool ViewRuler() override { return settings_.ruler; }
  bool ViewDeveloperTab() override { return settings_.developerTab; }
  std::wstring ViewReviewer() override { return settings_.reviewer; }
  void ViewSetReviewer(const std::wstring& name) override {
    settings_.reviewer = name;
    settings_.Save();
  }
  void ViewPasswordAccepted(TabContent* view) override;
  void ViewShowPanel(PanelContent* content) override;
  PanelContent* ViewShownPanel() override { return panel_.Content(); }
  void ViewFocusPage() override;
  void ViewOpenNote(PdfView* view, const NoteTarget& note) override;

  // DropHost
  void OnDragEnter(bool acceptable) override;
  void OnDragLeave() override;
  void OnDropFiles(const std::vector<std::wstring>& files) override;
  bool TakesPictures(const std::vector<std::wstring>& files) override;
  void OnDropPictures(const std::vector<std::wstring>& files, POINT pt) override;

 private:
  // Toolbar is the editors' ribbon too (R1); FilePage covers everything under the title bar.
  enum class Region { None, Title, Toolbar, Search, Banner, Note, Doc, Panel, Status, Commands, FilePage };
  // Where the keyboard is, for F6 and for going back after the side panel or command search.
  enum class KeyArea { Doc, Panel, Status };

  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
  LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);
  bool CreateMainWindow(const AppOptions& opts);
  void MessageLoop();
  bool Idle();

  // Rendering
  bool EnsureTarget();
  void DiscardDeviceResources();
  void Paint();
  void RenderFrame(ID2D1DeviceContext* dc);
  void Layout();
  RECT DocRect() const;
  int TitleHeight() const;
  int ToolbarHeight() const;
  // The active tab's toolbar, with the window's own state.
  ToolbarModel CurrentToolbar();
  std::wstring SearchStatus();

  // Input
  Region RegionAt(int x, int y) const;
  void OnMouseDown(int x, int y, int button, bool dbl);
  void OnMouseMove(int x, int y);
  void OnMouseUp(int x, int y, int button);
  void OnMouseLeave();
  void OnKeyDown(UINT vk);
  void OnChar(wchar_t ch);
  bool OnSetCursor();
  UINT Mods() const;
  void UpdateTooltip(int x, int y);
  void HideTooltip();
  LRESULT HitTestNC(int screenX, int screenY);

  // Commands
  // Keys that work the same in every kind of tab.
  static int AppShortcut(UINT vk, UINT mods);
  void Execute(int cmd);
  void ShowMenu(int cmd);
  // Fills the menu ShowMenu shows for `cmd` (the ⋯ menu, New, zoom); true when it opens right-aligned.
  bool FillWindowMenu(int cmd, HMENU menu);
  void ShowContextMenu(int screenX, int screenY);
  int TrackMenu(HMENU menu, const D2D1_RECT_F& anchor, bool alignRight);

  // Documents and tabs
  TabContent* Active() const;
  PagedView* ActivePaged() const;  // the active tab if it shows pages
  PdfView* ActivePdf() const;
  // A tab for a file. Every file is a PDF for now; documents, spreadsheets and presentations come later.
  std::unique_ptr<TabContent> CreateTab(const std::wstring& path);
  void OpenFiles(const std::vector<std::wstring>& files);
  void OpenDialog(bool templates = false);  // templates: in the personal templates folder (S4)
  // A click, key or UI Automation action on the start screen.
  void OnStartScreen(EmptyState::Result res);
  // `copies`: more types to offer after `format`; `chosen` gets the type picked (0 for `format`,
  // 1 for the first copy type).
  std::wstring SaveDialog(const std::wstring& suggested, const SaveFormat& format,
                          const std::vector<SaveFormat>& copies = {}, size_t* chosen = nullptr);
  bool SaveCopy(TabContent* view, const std::wstring& path, size_t format);
  void ActivateTab(int index);
  bool CloseTab(int index);
  bool ConfirmClose(TabContent* view);
  // `wait`: done when this returns (closing the tab next); otherwise a workbook is written in the
  // background and ViewSaved follows.
  bool SaveActive(bool saveAs, bool wait = false);
  // Saves the active tab to `path`, after the banner when its format loses something (CSV).
  bool SaveActiveTo(std::wstring path, bool saveAs, bool wait = false);
  // After a save: the recent files, "Saved", the title.
  void Saved(TabContent* view);
  void PrintActive();
  // Export PDF: asks where, then writes the tab's pages there.
  void ExportPdfActive();
  bool ExportPdfTo(const std::wstring& path);
  void RememberState(TabContent* view);
  void FocusBox(TextBox* box);
  void OpenSearch();
  // Saves (or cancels) the note being edited and closes the editor.
  void FinishNote(NoteEditor::Action action);
  void CloseSearch();

  // Window modes
  void ToggleFullScreen();
  void SetPresentation(bool on);
  void SetTheme(ThemeSetting t);

  // Files kept safe (AppFiles.cpp): new documents, crash recovery, changes on disk, backups
  // Where backups and recovery autosaves go, and who saves; `userFolders` false keeps them off.
  void InitFiles(bool userFolders);
  void NewDocument(int kind = 0);  // fw::doc::docx::Template
  void NewWorkbook();               // "Book1"
  void NewPresentation();           // "Presentation1" (P2)
  // At startup: documents a crash left unsaved, offered in a banner.
  void OfferRecovery();
  void RecoverDocuments(bool open);  // Recover, or Discard
  // The window or a tab was activated: did the file change on disk?
  void CheckActiveFile();
  void ReloadActive();
  std::wstring LatestBackup() const;  // of the active document, or empty
  void RestoreOriginal();
  void DeleteBackups();
  // The autosave timer runs only while a tab has unsaved changes (no idle wakeups otherwise).
  void UpdateAutosaveTimer();
  void AutosaveAll();
  static constexpr UINT_PTR kTimerAutosave = 5;

  // Startup notices and updates
  void StartupNotices();
  // Registers Featherweight and opens Default apps, where the user picks it for this kind of file.
  void MakeDefaultApp(fw::FileKind kind);
  void MaybeCheckForUpdates(bool manual);
  void OnUpdaterDone();
  void ShowAbout();

  // Keyboard focus and UI Automation
  // The keyboard moved between the window's own boxes and the tab: the tab shows or hides its focus.
  void KeyFocusMoved();
  // The system caret (never shown: Featherweight draws its own) and the input method's windows
  // follow the text caret, so IMEs, dictation and the emoji panel, through TSF's support for
  // IMM32 windows, open beside it and type into it as WM_CHAR. After painting, and when focus
  // or a composition starts.
  void SyncInputCaret(bool force = false);
  // What UI Automation sees (AppUia.cpp).
  void DescribeUia(uia::Tree& t);
  void OpenGallery();

  // The side panel, the status bar and command search (AppShell.cpp)
  void InitShell();
  // A command's state and name: the active tab's, with the window's own commands added.
  CommandState QueryCommand(int cmd);
  // Toolbar items without a tooltip take their command's name and keys; hidden commands go.
  void NameToolbarItems(ToolbarModel& m);
  std::vector<CommandEntry> SearchableCommands();
  void OpenCommandSearch();
  // Closes it and gives the keyboard back to where it was.
  void CloseCommandSearch();
  void ClosePanel();
  bool StatusBarShown() const;
  int StatusBarHeight() const;
  StatusBarModel CurrentStatusBar();
  void LayoutShell();
  void DrawShell();
  KeyArea CurrentKeyArea() const;
  // Moves the keyboard to an area (F6, Tab into the panel, Esc back). `resume`: to the control
  // that had it last. False when it can't go there.
  bool MoveKeyTo(KeyArea area, bool forward, bool resume);
  // Keys for the shell: F6, and keys while the panel or the status bar has the keyboard.
  bool ShellKeyDown(UINT vk, UINT mods);
  bool ShellChar(wchar_t ch);
  // Tab that the tab didn't use moves into the side panel.
  bool TabIntoPanel(UINT vk, UINT mods);
  Region ShellRegionAt(int x, int y) const;
  void ShellMouseDown(Region r, int x, int y, int button, bool dbl, bool shift);
  bool ShellMouseMove(Region r, int x, int y);
  void ShellMouseUp(Region r, int x, int y, int button);
  void ShellMouseLeave(Region r);
  bool ShellWheel(int delta, int x, int y);
  HCURSOR ShellCursor(Region r, int x, int y) const;
  std::wstring ShellTooltip(Region r, int x, int y) const;
  void DescribeShell(uia::Tree& t);

  // The ribbon (AppRibbon.cpp, R1): the editors' ribbon in place of their toolbar, the File page,
  // quick access, KeyTips and Ctrl+F1, and the checks the ribbon's tests run.
  void InitRibbon();
  // The active editor's ribbon, without the commands this kind of tab doesn't offer; empty for
  // the PDF reader, the start screen and an editor whose ribbon isn't done (its toolbar shows).
  RibbonModel CurrentRibbon();
  std::vector<TitleBar::QuickButton> QuickAccess(const RibbonModel& m);
  // A quick access button's list (Undo's actions), from the tab's RibbonModel::quick.
  void ShowQuickList(int index);
  void ShowRibbonMenu(const RibbonItem& item, const D2D1_RECT_F& anchor);
  void ToggleRibbon();
  void OpenFilePage();
  void CloseFilePage();
  ui::FilePage::Model CurrentFilePage();
  // Alt+letter: a tab's KeyTip (Alt+H) or the File page (Alt+F). True when used.
  bool RibbonAltKey(UINT vk);
  // Alt let go alone: KeyTips on or off.
  void RibbonAltReleased();
  // Every command the ribbon, the File page, the right-click menu and command search reach now.
  std::vector<int> ReachableCommands(std::wstring* report = nullptr);
  // What the toolbar and the ⋯ menu reached before R1 (the coverage tests' baseline).
  std::vector<int> ToolbarCommands();
  bool RibbonScriptOp(const std::wstring& op, const std::wstring& arg);

  // Snapshot / automation
  int RunSnapshot(const AppOptions& opts);
  void RunScript(const std::wstring& script);
  void PumpUntilIdle(double maxMs);
  void DrawSnapshotFrame();
  // The finished snapshot as an image, read back from wherever it was drawn.
  ComPtr<IWICBitmapSource> SnapshotImage();
  // Snapshot mode: native child controls paint outside Direct2D, so copy them in.
  void DrawChildControlsForSnapshot(ID2D1DeviceContext* dc);

  HWND hwnd_ = nullptr;
  float ui_ = 1.0f;
  bool windowActive_ = true;
  bool snapshot_ = false;
  bool scriptFailed_ = false;  // a script step failed: the snapshot run exits with 6
  bool snapshotWindow_ = false;
  bool sandbox_ = false;
  int simulatedMods_ = -1;

  SwapChain swap_;
  ComPtr<ID2D1RenderTarget> wicTarget_;
  ComPtr<IWICBitmap> wicBitmap_;
  ComPtr<ID2D1Bitmap1> gpuTarget_;
  ComPtr<ID2D1DeviceContext> dc_;
  gfx::Canvas canvas_;
  double lastFrame_ = 0;

  uia::Root uia_;
  int openMenu_ = 0;  // the toolbar menu being shown
  TitleBar title_;
  Toolbar toolbar_;
  ui::Ribbon ribbon_;
  ui::FilePage filePage_;
  bool ribbonShown_ = false;  // the active tab's ribbon shows (its BuildRibbon filled one)
  bool altAlone_ = false;     // Alt is down with no other key since: letting go shows KeyTips
  bool keyTipAte_ = false;    // a KeyTip used the key: its WM_CHAR goes nowhere
  bool fileBackup_ = false;   // the File page's Restore original has a backup to open
  SearchPanel search_;
  Banner banner_;
  Tooltip tooltip_;
  EmptyState empty_;
  NoteEditor notes_;
  std::vector<std::unique_ptr<TabContent>> tabs_;
  int active_ = -1;
  SidePanel panel_;  // shows content that belongs to the active tab
  StatusBar status_;
  CommandSearch commands_;
  KeyArea panelReturn_ = KeyArea::Doc;     // where Esc in the side panel sends the keyboard
  KeyArea commandsReturn_ = KeyArea::Doc;  // the same for command search

  Settings settings_;
  Updater updater_;
  bool manualUpdateCheck_ = false;
  std::wstring bannerUpdateUrl_;
  std::wstring bannerUpdateVersion_;
  enum BannerActions {
    kActUpdate = 1, kActDownload, kActSkip, kActNotNow = 5, kActSaveAs, kActRecover, kActDiscard, kActReload,
    kActKeepFormat, kActMakeDefault = 20
  };
  std::wstring keepFormatPath_;  // the save the "Keep CSV" banner waits on
  // (kActMakeDefault + fw::FileKind: the default-app offer for that kind of file)

  // Files
  std::vector<fw::pkg::recovery::Entry> orphans_;  // offered for recovery
  int newDocuments_ = 0;
  int newPresentations_ = 0;  // Presentation1, Presentation2 (P2)
  int newWorkbooks_ = 0;  // Book1, Book2, and workbooks from templates (Budget1)
  static constexpr int kUserTemplateCount = 16;
  std::vector<std::wstring> userTemplates_;  // as the last menu listed them (CmdUserTemplate)
  bool autosaveTimer_ = false;

  // Input state
  Region capture_ = Region::None;
  Region hover_ = Region::None;
  bool mouseTracked_ = false;
  bool ncMouseTracked_ = false;
  TextBox* focusBox_ = nullptr;
  bool inputCaret_ = false;  // the system caret exists
  RECT inputCaretAt_{};
  double caretStart_ = 0;
  std::wstring tipText_;
  POINT lastMouse_{-1, -1};
  double lastMouseMove_ = 0;

  // Drag and drop
  DropTarget* dropTarget_ = nullptr;
  bool dragOver_ = false;
  Anim dropAnim_;

  // Window modes
  bool fullscreen_ = false;
  bool presentation_ = false;
  bool presentFromFullscreen_ = false;
  WINDOWPLACEMENT savedPlacement_{sizeof(WINDOWPLACEMENT)};
  LONG_PTR savedStyle_ = 0;
  Anim toolbarReveal_;
  bool inSizeMove_ = false;
};
