// Fuzz target: fwchart (plan D5): a chart part (c:chartSpace) read into the chart model and laid
// out as art at a few sizes, from roomy to too small, with its text as glyph outlines.
// Seeds: tests/fuzz/seeds/chart (each chart type as Word writes it, from make_chart_tests.py).
//
// Every mark must come out finite and within the art (data far outside the axes is clipped there),
// and the number of marks within fwchart's cap.
#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string_view>

#include "chart/Chart.h"
#include "draw/Theme.h"
#include "pkg/XmlDom.h"

namespace {

void Require(bool ok) {
  if (!ok) std::abort();
}

bool Finite(const draw::Rect& r) {
  return std::isfinite(r.left) && std::isfinite(r.top) && std::isfinite(r.right) && std::isfinite(r.bottom);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  static const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  (void)com;
  static const std::shared_ptr<const draw::Theme> theme = draw::Theme::Default();
  static const draw::ColorMap map;
  draw::ColorContext colors;
  colors.theme = theme.get();
  colors.colorMap = &map;

  fw::pkg::Limits limits;
  limits.maxStringLength = 1u << 20;
  fw::pkg::XmlDom dom;
  if (!dom.Parse(std::string_view(reinterpret_cast<const char*>(data), size), limits).Ok()) return 0;
  fw::chart::Chart chart;
  if (!fw::chart::Read(dom.Root(), colors, chart)) return 0;
  for (const draw::Size s : {draw::Size{432, 252}, draw::Size{60, 40}, draw::Size{1500, 8}}) {
    const draw::Art art = fw::chart::Draw(chart, s, colors, L"Calibri");
    Require(art.marks.size() <= 60001);
    for (const draw::Art::Mark& m : art.marks) {
      const draw::Rect b = m.path.Bounds();
      if (b.right < b.left) continue;  // empty
      Require(Finite(b));
      // Marks stay near the art: a little past it for markers and labels at the edges at most.
      for (const draw::Point corner : {draw::Point{b.left, b.top}, draw::Point{b.right, b.bottom}, draw::Point{b.left, b.bottom},
                                       draw::Point{b.right, b.top}}) {
        const draw::Point p = m.transform.Apply(corner);
        Require(std::isfinite(p.x) && std::isfinite(p.y) && p.x > -s.width - 200 && p.x < 2 * s.width + 200 &&
                p.y > -s.height - 200 && p.y < 2 * s.height + 200);
      }
    }
  }
  return 0;
}
