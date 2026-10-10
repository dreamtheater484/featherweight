#pragma once

#include <windows.h>

#include <memory>
#include <string>
#include <vector>

#include "Banner.h"
#include "DocumentView.h"
#include "DropTarget.h"
#include "EmptyState.h"
#include "NoteEditor.h"
#include "Gfx.h"
#include "SearchPanel.h"
#include "Settings.h"
#include "TitleBar.h"
#include "Toolbar.h"
#include "Tooltip.h"
#include "Updater.h"

struct AppOptions {
  std::vector<std::wstring> files;
  int showCommand = SW_SHOWDEFAULT;
  // Snapshot mode (used for automated visual checks): render off-screen to a PNG and exit.
  std::wstring snapshotPath;
  int snapshotW = 1280, snapshotH = 800;
  float snapshotScale = 1.0f;
  std::wstring snapshotTheme;  // "light" or "dark"
  bool snapshotInvert = false;
  std::wstring script;         // steps separated by ';' (see App::RunScript)
};

class App : public ViewHost, public DropHost {
 public:
  static constexpr wchar_t kWindowClass[] = L"FeatherweightWindow";
  static constexpr ULONG_PTR kCopyDataOpen = 0x46570001;

  App();
  ~App() override;
  int Run(const AppOptions& opts);

  // ViewHost
  void ViewInvalidate() override;
  void ViewStateChanged() override;
  void ViewMessage(BannerKind kind, const std::wstring& text) override;
  HWND ViewHwnd() override { return hwnd_; }
  void ViewRequestPrint() override;
  void ViewFocusTextBox(TextBox* box) override;
  bool ViewInverted() override { return settings_.invert; }
  bool ViewCoverAlone() override { return settings_.coverAlone; }
  bool ViewHighlightFields() override { return settings_.highlightFields; }
  void ViewPasswordAccepted(DocumentView* view) override;
  void ViewOpenNote(DocumentView* view, const NoteTarget& note) override;

  // DropHost
  void OnDragEnter(bool acceptable) override;
  void OnDragLeave() override;
  void OnDropFiles(const std::vector<std::wstring>& files) override;

 private:
  enum class Region { None, Title, Toolbar, Search, Banner, Note, Doc };

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
  ToolbarState CurrentToolbarState();
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
  void Execute(int cmd);
  void ShowMenu(int cmd);
  void ShowContextMenu(int screenX, int screenY);
  int TrackMenu(HMENU menu, const D2D1_RECT_F& anchor, bool alignRight);

  // Documents and tabs
  DocumentView* Active() const;
  void OpenFiles(const std::vector<std::wstring>& files);
  void OpenDialog();
  std::wstring SaveDialog(const std::wstring& suggested);
  void ActivateTab(int index);
  bool CloseTab(int index);
  bool ConfirmClose(DocumentView* view);
  bool SaveActive(bool saveAs);
  void PrintActive();
  void RememberState(DocumentView* view);
  void FocusBox(TextBox* box);
  void OpenSearch();
  // Saves (or cancels) the note being edited and closes the editor.
  void FinishNote(NoteEditor::Action action);
  void CloseSearch();

  // Window modes
  void ToggleFullScreen();
  void SetPresentation(bool on);
  void SetTheme(ThemeSetting t);

  // Startup notices and updates
  void StartupNotices();
  void MaybeCheckForUpdates(bool manual);
  void OnUpdaterDone();
  void ShowAbout();

  // Snapshot / automation
  int RunSnapshot(const AppOptions& opts);
  void RunScript(const std::wstring& script);
  void PumpUntilIdle(double maxMs);

  HWND hwnd_ = nullptr;
  float ui_ = 1.0f;
  bool windowActive_ = true;
  bool snapshot_ = false;
  int simulatedMods_ = -1;

  ComPtr<ID2D1HwndRenderTarget> target_;
  ComPtr<ID2D1RenderTarget> wicTarget_;
  ComPtr<IWICBitmap> wicBitmap_;
  ComPtr<ID2D1DeviceContext> dc_;
  gfx::Canvas canvas_;
  double lastFrame_ = 0;

  TitleBar title_;
  Toolbar toolbar_;
  SearchPanel search_;
  Banner banner_;
  Tooltip tooltip_;
  EmptyState empty_;
  NoteEditor notes_;
  POINT contextPt_{};
  int contextPage_ = -1;
  AnnotHit contextAnnot_;
  std::vector<std::unique_ptr<DocumentView>> tabs_;
  int active_ = -1;

  Settings settings_;
  Updater updater_;
  bool manualUpdateCheck_ = false;
  std::wstring bannerUpdateUrl_;
  std::wstring bannerUpdateVersion_;
  enum BannerActions { kActUpdate = 1, kActDownload, kActSkip, kActMakeDefault, kActNotNow, kActSaveAs };

  // Input state
  Region capture_ = Region::None;
  Region hover_ = Region::None;
  bool mouseTracked_ = false;
  bool ncMouseTracked_ = false;
  TextBox* focusBox_ = nullptr;
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
