#include "pdf/sign/Stamp.h"

#include <fpdf_edit.h>
#include <fpdf_transformpage.h>

#include <algorithm>
#include <cmath>
#include <string>

#include "pdf/PdfText.h"

namespace pdfsign {

namespace {
// Ink: a dark blue, as a pen signs.
constexpr unsigned kInkR = 0x1A, kInkG = 0x2F, kInkB = 0x80;
}  // namespace

Ink InkFromPoints(const std::vector<std::vector<std::pair<float, float>>>& points) {
  Ink ink;
  float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
  for (const auto& s : points)
    for (const auto& [x, y] : s) {
      x0 = std::min(x0, x);
      y0 = std::min(y0, y);
      x1 = std::max(x1, x);
      y1 = std::max(y1, y);
    }
  if (x1 < x0) return ink;
  // A little room around the ink, so the pen's width isn't cut off at the edges.
  const float pad = std::max(x1 - x0, y1 - y0) * 0.03f + 1;
  x0 -= pad;
  y0 -= pad;
  x1 += pad;
  y1 += pad;
  const float w = x1 - x0, h = y1 - y0;
  ink.aspect = std::clamp(w / h, 0.5f, 12.0f);
  for (const auto& s : points) {
    std::vector<std::pair<float, float>> out;
    for (const auto& [x, y] : s) out.emplace_back((x - x0) / w, (y - y0) / h);
    if (!out.empty()) ink.strokes.push_back(std::move(out));
  }
  return ink;
}

FS_RECTF StampRect(FPDF_PAGE page, float x, float y, float widthPt, float aspect) {
  FS_RECTF box{};
  if (!FPDFPage_GetCropBox(page, &box.left, &box.bottom, &box.right, &box.top) &&
      !FPDFPage_GetMediaBox(page, &box.left, &box.bottom, &box.right, &box.top)) {
    box = {0, static_cast<float>(FPDF_GetPageHeightF(page)), static_cast<float>(FPDF_GetPageWidthF(page)), 0};
  }
  const float pw = std::fabs(box.right - box.left), ph = std::fabs(box.top - box.bottom);
  float w = std::min(widthPt, pw * 0.8f), h = w / std::max(0.1f, aspect);
  if (h > ph * 0.5f) {
    h = ph * 0.5f;
    w = h * aspect;
  }
  const float left = std::min(box.left, box.right), bottom = std::min(box.top, box.bottom);
  const float l = std::clamp(x - w / 2, left, left + pw - w), b = std::clamp(y - h / 2, bottom, bottom + ph - h);
  return {l, b + h, l + w, b};
}

int AddSignatureStamp(FPDF_DOCUMENT doc, FPDF_PAGE page, const FS_RECTF& rect, const Ink& ink) {
  if (!doc || !page || ink.Empty()) return -1;
  FPDF_ANNOTATION annot = FPDFPage_CreateAnnot(page, FPDF_ANNOT_STAMP);
  if (!annot) return -1;
  const float l = rect.left, t = rect.top, w = rect.right - rect.left, h = rect.top - rect.bottom;
  bool ok = FPDFAnnot_SetRect(annot, &rect) && FPDFAnnot_SetFlags(annot, FPDF_ANNOT_FLAG_PRINT);
  // What other readers show for the stamp (wchar_t is UTF-16 here, as FPDF_WIDESTRING is).
  FPDFAnnot_SetStringValue(annot, "Contents", reinterpret_cast<FPDF_WIDESTRING>(T(L"Signature")));
  if (ok && !ink.bgra.empty()) {
    // A picture: near-white is see-through, so it sits on the page as ink would.
    std::vector<uint8_t> px = ink.bgra;
    for (size_t i = 0; i + 3 < px.size(); i += 4) {
      const int lightest = std::min({px[i], px[i + 1], px[i + 2]});
      if (lightest > 225) px[i + 3] = 0;
    }
    FPDF_BITMAP bmp = FPDFBitmap_CreateEx(ink.width, ink.height, FPDFBitmap_BGRA, px.data(), ink.width * 4);
    FPDF_PAGEOBJECT image = bmp ? FPDFPageObj_NewImageObj(doc) : nullptr;
    ok = image && FPDFImageObj_SetBitmap(nullptr, 0, image, bmp) && FPDFImageObj_SetMatrix(image, w, 0, 0, h, l, t - h) &&
         FPDFAnnot_AppendObject(annot, image);
    if (bmp) FPDFBitmap_Destroy(bmp);
  } else if (ok) {
    const float pen = std::clamp(w / 110.0f, 0.8f, 2.4f);
    for (const auto& stroke : ink.strokes) {
      if (stroke.empty()) continue;
      auto at = [&](const std::pair<float, float>& p) { return std::pair<float, float>{l + p.first * w, t - p.second * h}; };
      const auto first = at(stroke.front());
      FPDF_PAGEOBJECT path = FPDFPageObj_CreateNewPath(first.first, first.second);
      if (!path) continue;
      if (stroke.size() == 1) {
        FPDFPath_LineTo(path, first.first + pen * 0.3f, first.second);  // a dot
      } else {
        // Through the midpoints, with each point as a control point: smooth curves, not corners.
        for (size_t i = 1; i + 1 < stroke.size(); ++i) {
          const auto c = at(stroke[i]), n = at(stroke[i + 1]);
          const float mx = (c.first + n.first) / 2, my = (c.second + n.second) / 2;
          FPDFPath_BezierTo(path, c.first, c.second, c.first, c.second, mx, my);
        }
        const auto last = at(stroke.back());
        FPDFPath_LineTo(path, last.first, last.second);
      }
      FPDFPageObj_SetStrokeColor(path, kInkR, kInkG, kInkB, 255);
      FPDFPageObj_SetStrokeWidth(path, pen);
      FPDFPageObj_SetLineCap(path, FPDF_LINECAP_ROUND);
      FPDFPageObj_SetLineJoin(path, FPDF_LINEJOIN_ROUND);
      FPDFPath_SetDrawMode(path, FPDF_FILLMODE_NONE, 1);
      ok = FPDFAnnot_AppendObject(annot, path) && ok;
    }
  }
  int index = -1;
  if (ok) index = FPDFPage_GetAnnotIndex(page, annot);
  FPDFPage_CloseAnnot(annot);
  if (index < 0) {
    const int n = FPDFPage_GetAnnotCount(page);
    if (n > 0 && !ok) FPDFPage_RemoveAnnot(page, n - 1);
  }
  return index;
}

}  // namespace pdfsign
