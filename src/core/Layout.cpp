#include "core/Layout.h"

#include <algorithm>
#include <cmath>

namespace fw {

PageSizePt RotatedSize(PageSizePt s, int quarterTurns) {
  if (quarterTurns & 1) return {s.h, s.w};
  return s;
}

int FacingRowStart(int page, bool coverAlone) {
  if (coverAlone) {
    if (page == 0) return 0;
    return page - ((page - 1) % 2);
  }
  return page - (page % 2);
}

namespace {

struct RowSpec {
  int first, count;
  bool rightOnly;  // a single page shown in the right column (the cover)
};

std::vector<RowSpec> BuildRows(int pageCount, ViewMode mode, bool coverAlone) {
  std::vector<RowSpec> rows;
  if (mode != ViewMode::Facing) {
    rows.reserve(pageCount);
    for (int i = 0; i < pageCount; ++i) rows.push_back({i, 1, false});
    return rows;
  }
  int i = 0;
  if (coverAlone && pageCount > 0) {
    rows.push_back({0, 1, pageCount > 1});
    i = 1;
  }
  for (; i < pageCount; i += 2) rows.push_back({i, std::min(2, pageCount - i), false});
  return rows;
}

int Px(double v) { return static_cast<int>(std::lround(v)); }

}  // namespace

Layout ComputeLayout(const LayoutInput& in) {
  Layout L;
  L.mode = in.mode;
  const int n = static_cast<int>(in.pages.size());
  L.boxes.resize(n);
  L.rowOfPage.resize(n);
  if (n == 0) {
    L.width = in.viewportW;
    return L;
  }

  std::vector<int> pw(n), ph(n);
  for (int i = 0; i < n; ++i) {
    PageSizePt s = RotatedSize(in.pages[i], in.rotation);
    pw[i] = std::max(1, Px(s.w * in.scale));
    ph[i] = std::max(1, Px(s.h * in.scale));
  }

  auto specs = BuildRows(n, in.mode, in.coverAlone);
  const bool facing = in.mode == ViewMode::Facing;

  // Content width: facing pages sit on either side of a center line.
  int contentW = 0;
  if (facing) {
    int half = 0;
    for (const auto& r : specs) {
      for (int k = 0; k < r.count; ++k) half = std::max(half, pw[r.first + k]);
    }
    contentW = 2 * half + in.gap;
  } else {
    for (int i = 0; i < n; ++i) contentW = std::max(contentW, pw[i]);
  }
  L.width = std::max(in.viewportW, contentW + 2 * in.margin);
  const int center = L.width / 2;

  int y = in.margin;
  int tallest = 0;
  L.rows.reserve(specs.size());
  for (size_t ri = 0; ri < specs.size(); ++ri) {
    const auto& r = specs[ri];
    int rowH = 0;
    for (int k = 0; k < r.count; ++k) rowH = std::max(rowH, ph[r.first + k]);
    tallest = std::max(tallest, rowH);
    const int rowY = in.mode == ViewMode::Single ? in.margin : y;

    for (int k = 0; k < r.count; ++k) {
      int p = r.first + k;
      PageBox& b = L.boxes[p];
      b.w = pw[p];
      b.h = ph[p];
      b.y = rowY + (rowH - b.h) / 2;
      if (!facing) {
        b.x = center - b.w / 2;
      } else if (r.count == 2) {
        b.x = k == 0 ? center - in.gap / 2 - b.w : center + (in.gap - in.gap / 2);
      } else if (r.rightOnly) {
        b.x = center + (in.gap - in.gap / 2);
      } else {
        b.x = center - in.gap / 2 - b.w;  // trailing odd page on the left
      }
      L.rowOfPage[p] = static_cast<int>(ri);
    }
    L.rows.push_back({r.first, r.count, rowY, rowH});
    y += rowH + in.gap;
  }

  if (in.mode == ViewMode::Single) {
    L.height = tallest + 2 * in.margin;
  } else {
    L.height = y - in.gap + in.margin;
  }
  return L;
}

int Layout::RowDocHeight(int row, int margin) const {
  if (row < 0 || row >= static_cast<int>(rows.size())) return height;
  return rows[row].h + 2 * margin;
}

void Layout::VisibleRows(int top, int bottom, int& firstRow, int& lastRow) const {
  firstRow = 0;
  lastRow = -1;
  if (rows.empty()) return;
  // First row whose bottom is below `top`.
  auto it = std::lower_bound(rows.begin(), rows.end(), top,
                             [](const LayoutRow& r, int value) { return r.y + r.h <= value; });
  firstRow = static_cast<int>(it - rows.begin());
  lastRow = firstRow - 1;
  for (int i = firstRow; i < static_cast<int>(rows.size()) && rows[i].y < bottom; ++i) lastRow = i;
}

double ComputeFitScale(ZoomMode mode, const FitInput& in) {
  if (mode == ZoomMode::Custom || !in.pages || in.pages->empty()) return 0;
  if (mode == ZoomMode::ActualSize) return ZoomToScale(1.0, in.dpi);

  const auto& pages = *in.pages;
  const int n = static_cast<int>(pages.size());
  const int ref = std::clamp(in.refPage, 0, n - 1);

  // Size (in points) of the row containing the reference page.
  double rowW = 0, rowH = 0;
  int rowPages = 1;
  if (in.mode == ViewMode::Facing) {
    int start = FacingRowStart(ref, in.coverAlone);
    int count = (in.coverAlone && start == 0) ? 1 : std::min(2, n - start);
    double half = 0;
    for (int k = 0; k < count; ++k) {
      PageSizePt s = RotatedSize(pages[start + k], in.rotation);
      half = std::max(half, s.w);
      rowH = std::max(rowH, s.h);
    }
    rowW = 2 * half;
    rowPages = 2;
  } else {
    PageSizePt s = RotatedSize(pages[ref], in.rotation);
    rowW = s.w;
    rowH = s.h;
  }
  if (rowW <= 0 || rowH <= 0) return ZoomToScale(1.0, in.dpi);

  const double gapW = rowPages > 1 ? in.gap : 0;
  const double availW = std::max(1.0, static_cast<double>(in.viewportW - 2 * in.margin) - gapW);
  const double availH = std::max(1.0, static_cast<double>(in.viewportH - 2 * in.margin));
  const double widthScale = availW / rowW;
  const double heightScale = availH / rowH;
  const double minScale = ZoomToScale(kMinZoom, in.dpi);
  const double maxScale = ZoomToScale(kMaxZoom, in.dpi);

  double scale = 0;
  switch (mode) {
    case ZoomMode::FitWidth: scale = widthScale; break;
    case ZoomMode::FitPage: scale = std::min(widthScale, heightScale); break;
    case ZoomMode::Automatic: {
      PageSizePt s = RotatedSize(pages[ref], in.rotation);
      double horizontal = s.h >= s.w ? widthScale : std::min(widthScale, heightScale);
      scale = std::min(horizontal, ZoomToScale(kAutoMaxZoom, in.dpi));
      break;
    }
    default: scale = ZoomToScale(1.0, in.dpi); break;
  }
  return std::clamp(scale, minScale, maxScale);
}

double ClampZoom(double zoom) { return std::clamp(zoom, kMinZoom, kMaxZoom); }

const std::vector<double>& ZoomPresets() {
  static const std::vector<double> presets = {0.10, 0.25, 0.50, 0.75, 1.00, 1.25, 1.50,
                                              2.00, 3.00, 4.00, 6.00, 8.00, 12.0, 16.0};
  return presets;
}

double NextZoomStep(double zoom, bool in) {
  const auto& p = ZoomPresets();
  if (in) {
    for (double v : p)
      if (v > zoom * 1.001) return v;
    return p.back();
  }
  for (auto it = p.rbegin(); it != p.rend(); ++it)
    if (*it < zoom * 0.999) return *it;
  return p.front();
}

}  // namespace fw
