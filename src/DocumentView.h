#pragma once

#include <windows.h>

#include <climits>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "Document.h"
#include "Gfx.h"
#include "RenderCache.h"
#include "Search.h"
#include "Settings.h"
#include "TextSelect.h"
#include "Widgets.h"
#include "core/Layout.h"

enum class BannerKind { Info, Success, Warning, Error };

// Modifier flags, identical to PDFium's FWL_EVENTFLAG values.
constexpr UINT kModShift = 1;
constexpr UINT kModCtrl = 2;
constexpr UINT kModAlt = 4;

class DocumentView;

class ViewHost {
 public:
  virtual ~ViewHost() = default;
  virtual void ViewInvalidate() = 0;
  virtual void ViewStateChanged() = 0;  // page, zoom or modified state changed
  virtual void ViewMessage(BannerKind kind, const std::wstring& text) = 0;
  virtual HWND ViewHwnd() = 0;
  virtual void ViewRequestPrint() = 0;
  virtual void ViewFocusTextBox(TextBox* box) = 0;
  virtual bool ViewInverted() = 0;
  virtual bool ViewCoverAlone() = 0;
  virtual bool ViewHighlightFields() = 0;
  virtual void ViewPasswordAccepted(DocumentView* view) = 0;
};

class DocumentView : public DocumentHost {
 public:
  DocumentView(ViewHost& host, std::wstring path);
  ~DocumentView() override;

  // ---- Loading ----
  OpenResult Load(const std::wstring& password, std::wstring& error);
  bool Ready() const { return state_ == LoadState::Ready; }
  bool NeedsPassword() const { return state_ == LoadState::NeedPassword; }
  TextBox* PasswordBox() { return NeedsPassword() ? &pwBox_ : nullptr; }
  const std::wstring& Path() const;
  std::wstring Title() const;
  Document* Doc() { return doc_.get(); }

  // ---- Geometry and painting ----
  void SetViewport(const RECT& rc, float ui);
  const RECT& Viewport() const { return vp_; }
  void Draw(gfx::Canvas& c, ID2D1DeviceContext* dc);
  bool HasIdleWork() const;
  bool IdleWork(ID2D1DeviceContext* dc, double deadlineMs);
  bool Animating() const;
  void ReleaseDeviceResources();
  // Called when the tab goes to the background: frees memory that is quick to rebuild.
  void OnDeactivated();

  // ---- Input ----
  void OnMouseDown(int x, int y, int button, UINT mods, bool doubleClick);
  void OnMouseMove(int x, int y, UINT mods);
  void OnMouseUp(int x, int y, int button, UINT mods);
  void OnMouseLeave();
  void OnWheel(int delta, bool horizontal, UINT mods, int x, int y);
  bool OnKeyDown(UINT vk, UINT mods);
  bool OnChar(wchar_t ch, UINT mods);
  HCURSOR CursorAt(int x, int y);
  std::wstring TooltipAt(int x, int y);
  bool HasFieldFocus() const;
  bool Dragging() const { return drag_ != Drag::None; }

  // ---- Navigation and view ----
  int PageCount() const { return doc_ ? doc_->PageCount() : 0; }
  int CurrentPage() const { return currentPage_; }
  void GoToPage(int page);
  void NextPage();
  void PrevPage();
  void ScrollBy(double dx, double dy, bool smooth);
  fw::ZoomMode ZoomMode() const { return zoomMode_; }
  double Zoom() const;
  void SetZoomMode(fw::ZoomMode mode);
  void SetZoom(double zoom, int anchorX = INT_MIN, int anchorY = INT_MIN);
  void ZoomStep(bool in, int anchorX = INT_MIN, int anchorY = INT_MIN);
  fw::ViewMode ViewMode() const { return viewMode_; }
  void SetViewMode(fw::ViewMode mode);
  void Rotate(bool clockwise);
  int Rotation() const { return rotation_; }
  void SettingsChanged();  // cover page, field highlight
  void SetPresentation(bool on);
  bool Presentation() const { return presentation_; }

  // ---- Text ----
  bool HasSelection() const { return !selection_.Empty(); }
  std::wstring SelectedText();
  void Copy();
  void SelectAll();
  void SelectAllOnPage();

  // ---- Search ----
  void StartSearch(const std::wstring& query, bool matchCase);
  void ClearSearch();
  void SearchStep(bool forward);
  int SearchCount() const;
  int SearchCurrent() const;
  bool SearchDone() const;
  bool SearchActive() const { return search_.Active(); }

  // ---- File ----
  bool Modified() const { return doc_ && doc_->Modified(); }
  bool CanPrint() const { return doc_ && doc_->CanPrint(); }
  bool Save(const std::wstring& path, std::wstring& error);
  FileState State() const;
  void ApplyState(const FileState& s);

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

 private:
  enum class LoadState { Empty, NeedPassword, Ready };
  enum class Drag { None, Select, Pan, Form, Link, ThumbV, ThumbH };
  struct Anchor {
    int page = 0;
    double fx = 0, fy = 0;  // position inside the page box, as a fraction
    double vx = 0, vy = 0;  // where that point sits in the viewport
  };

  // Layout and coordinates
  void Relayout(const Anchor* anchor);
  void RecomputeScale();
  Anchor CaptureAnchor(double vx, double vy) const;
  void RestoreAnchor(const Anchor& a);
  int DocHeight() const;
  int CenterOffsetY() const;
  int VpW() const { return vp_.right - vp_.left; }
  int VpH() const { return vp_.bottom - vp_.top; }
  void ClampScroll();
  void VisiblePages(int& first, int& last) const;
  RECT PageRect(int page) const;
  int PageAtPoint(int x, int y) const;
  int NearestPage(int x, int y) const;
  bool ToPagePoint(int page, int x, int y, double& px, double& py);
  D2D1_RECT_F PageRectToScreen(int page, const FS_RECTF& r);
  void UpdateCurrentPage();
  void ScrollToDocY(double y, bool smooth);
  void ScrollPageRectIntoView(int page, const FS_RECTF& r, bool center);
  void GoToPosition(int page, bool hasPos, float x, float y);
  void StepScroll();

  // Painting
  void DrawPage(gfx::Canvas& c, ID2D1DeviceContext* dc, int page, bool inverted);
  void DrawBitmap(ID2D1DeviceContext* dc, ID2D1Bitmap* bmp, const D2D1_RECT_F& dest, const D2D1_RECT_F& src,
                  bool inverted, bool exact);
  void DrawOverlays(gfx::Canvas& c, int first, int last);
  void DrawScrollbars(gfx::Canvas& c);
  void DrawPasswordCard(gfx::Canvas& c);
  bool ScrollbarRects(D2D1_RECT_F& vTrack, D2D1_RECT_F& vThumb, D2D1_RECT_F& hTrack, D2D1_RECT_F& hThumb) const;

  // Interaction helpers
  void AfterFormEvent();
  void FollowLink(const LinkTarget& link);
  bool FieldKey(UINT vk, UINT mods);
  bool FocusFieldOnNextPage(int fromPage, bool backwards);
  void ScrollToMatch(int index);
  void SubmitPassword();
  float ui_ = 1.0f;
  double dpi_ = 96;

  ViewHost& host_;
  std::wstring path_;
  std::unique_ptr<Document> doc_;
  std::unique_ptr<RenderCache> cache_;
  LoadState state_ = LoadState::Empty;

  // Password prompt
  TextBox pwBox_;
  Button pwOpen_;
  ButtonSet pwButtons_;
  std::wstring pwError_;

  // View state
  RECT vp_{};
  fw::Layout layout_;
  double scale_ = 1.0;  // device pixels per point
  fw::ZoomMode zoomMode_ = fw::ZoomMode::Automatic;
  fw::ViewMode viewMode_ = fw::ViewMode::Continuous;
  int rotation_ = 0;
  int singleRow_ = 0;
  int margin_ = 16, gap_ = 12;
  double sx_ = 0, sy_ = 0;    // scroll position (document pixels)
  double tsy_ = 0, tsx_ = 0;  // smooth-scroll target
  double lastStep_ = 0;
  int currentPage_ = 0;
  bool presentation_ = false;
  struct Saved {
    fw::ViewMode viewMode;
    fw::ZoomMode zoomMode;
    double zoom;
  } beforePresentation_{};

  // Interaction
  Drag drag_ = Drag::None;
  int downX_ = 0, downY_ = 0;
  double downSx_ = 0, downSy_ = 0;
  int formPage_ = -1;
  int formCursor_ = 0;
  LinkTarget pendingLink_;
  TextSelection selection_;
  std::map<int, std::vector<FS_RECTF>> selRects_;
  int selRectsVersion_ = -1;
  double thumbGrab_ = 0;
  Anim vScrollHover_, hScrollHover_;
  int mouseX_ = -1, mouseY_ = -1;
  bool lastModified_ = false;

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
};
