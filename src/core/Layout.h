#pragma once

#include <vector>

namespace fw {

enum class ViewMode { Single = 0, Continuous = 1, Facing = 2 };
enum class ZoomMode { Custom = 0, Automatic = 1, FitWidth = 2, FitPage = 3, ActualSize = 4 };

constexpr double kMinZoom = 0.10;  // 10 %
constexpr double kMaxZoom = 16.0;  // 1600 %
constexpr double kAutoMaxZoom = 1.25;

// Page size in PDF points (1/72 inch) as displayed, i.e. with the page's own /Rotate applied.
struct PageSizePt {
  double w = 0, h = 0;
};

PageSizePt RotatedSize(PageSizePt s, int quarterTurns);

struct PageBox {
  int x = 0, y = 0, w = 0, h = 0;  // device pixels, document space
  int Right() const { return x + w; }
  int Bottom() const { return y + h; }
};

struct LayoutRow {
  int first = 0;  // first page index in the row
  int count = 0;  // 1 or 2
  int y = 0, h = 0;
};

struct LayoutInput {
  std::vector<PageSizePt> pages;
  int rotation = 0;  // view rotation, quarter turns clockwise
  ViewMode mode = ViewMode::Continuous;
  bool coverAlone = true;  // facing mode: first page on its own, on the right
  double scale = 1.0;      // device pixels per point
  int viewportW = 0;       // device pixels
  int margin = 16;         // around the content, device pixels
  int gap = 12;            // between pages and rows, device pixels
};

struct Layout {
  std::vector<PageBox> boxes;    // one per page
  std::vector<LayoutRow> rows;
  std::vector<int> rowOfPage;    // page index -> row index
  int width = 0;                 // document width (>= viewport width)
  int height = 0;                // continuous: all rows; single: tallest row
  ViewMode mode = ViewMode::Continuous;

  // Height of the scrollable document for the given row in single-page mode.
  int RowDocHeight(int row, int margin) const;
  // Rows overlapping [top, bottom) in document space (continuous modes).
  void VisibleRows(int top, int bottom, int& firstRow, int& lastRow) const;
};

Layout ComputeLayout(const LayoutInput& in);

struct FitInput {
  const std::vector<PageSizePt>* pages = nullptr;
  int rotation = 0;
  ViewMode mode = ViewMode::Continuous;
  bool coverAlone = true;
  int refPage = 0;  // the page (row) that should fit
  int viewportW = 0, viewportH = 0;
  int margin = 16, gap = 12;
  double dpi = 96;
};

// Device pixels per point for the given zoom mode. For ZoomMode::Custom returns 0.
double ComputeFitScale(ZoomMode mode, const FitInput& in);

inline double ZoomToScale(double zoom, double dpi) { return zoom * dpi / 72.0; }
inline double ScaleToZoom(double scale, double dpi) { return scale * 72.0 / dpi; }
double ClampZoom(double zoom);

// Preset steps used by zoom in/out and the zoom menu.
const std::vector<double>& ZoomPresets();
double NextZoomStep(double zoom, bool in);

// Pages that belong to the same facing row as `page`.
int FacingRowStart(int page, bool coverAlone);

}  // namespace fw
