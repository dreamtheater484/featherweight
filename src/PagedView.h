#pragma once

#include <windows.h>

#include <climits>
#include <vector>

#include "TabContent.h"
#include "core/Layout.h"
#include "ui/Anim.h"

// A tab that shows its content as pages: page layout, zoom, smooth scrolling, overlay scrollbars,
// page navigation and presenting. PDFs use it, and documents will too. Subclasses provide the page
// sizes, draw the pages and handle whatever is on them.
class PagedView : public TabContent {
 public:
  explicit PagedView(ViewHost& host) : host_(host) {}

  PagedView* Paged() override { return this; }

  // ---- Geometry ----
  void SetViewport(const RECT& rc, float ui) override;
  const RECT& Viewport() const { return vp_; }
  bool Animating() const;

  // ---- Navigation and view ----
  int PageCount() const;
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
  void SetPresentation(bool on);
  bool Presentation() const { return presentation_; }

  // ---- TabContent ----
  void OnWheel(int delta, bool horizontal, UINT mods, int x, int y) override;
  bool Dragging() const override { return drag_ != Drag::None; }
  CommandState QueryCommand(int cmd) override;
  bool Execute(int cmd) override;
  void FillMenu(int which, HMENU menu) override;
  FileState State() const override;
  void ApplyState(const FileState& s) override;

 protected:
  // What the mouse is doing. Subclasses use the kinds after ThumbH for what is on their pages.
  enum class Drag { None, Pan, ThumbV, ThumbH, Select, Form, Link, Note, Ruler };
  struct Anchor {
    int page = 0;
    double fx = 0, fy = 0;  // position inside the page box, as a fraction
    double vx = 0, vy = 0;  // where that point sits in the viewport
  };

  // The size of every page, in points, or null while nothing is loaded.
  virtual const std::vector<fw::PageSizePt>* Pages() const = 0;
  // Two-page view: show the first page on its own, like a book cover.
  virtual bool CoverAlone() const { return false; }
  // Presenting starts or stops, before the layout changes.
  virtual void OnPresentationChanged(bool /*on*/) {}

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
  void UpdateCurrentPage();
  void ScrollToDocY(double y, bool smooth);
  // Scrolls so that a rectangle on screen is in view, with some room around it.
  void ScrollScreenRectIntoView(const D2D1_RECT_F& r, bool center);
  void StepScroll();

  // Scrollbars
  bool ScrollbarRects(D2D1_RECT_F& vTrack, D2D1_RECT_F& vThumb, D2D1_RECT_F& hTrack, D2D1_RECT_F& hThumb) const;
  void DrawScrollbars(gfx::Canvas& c);
  bool OverScrollbar(int x, int y) const;

  // Input helpers for subclasses
  // Remembers where a press started and stops any smooth scrolling right there.
  void BeginPress(int x, int y);
  // A press on a scrollbar track or thumb. Returns true if it was one.
  bool ScrollbarMouseDown(int x, int y, int button);
  // Panning or dragging a scrollbar thumb. Returns true if one of those is going on.
  bool ScrollDragMove(int x, int y);
  void UpdateScrollbarHover(int x, int y);
  void ScrollbarMouseLeave();
  // Arrow keys, Page Up/Down, Space, Home/End, and Backspace while presenting.
  bool NavigationKey(UINT vk, UINT mods);

  ViewHost& host_;
  float ui_ = 1.0f;
  double dpi_ = 96;

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
  double thumbGrab_ = 0;
  Anim vScrollHover_, hScrollHover_;
};
