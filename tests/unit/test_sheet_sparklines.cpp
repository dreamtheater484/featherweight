// Sparklines (S4): the x14 groups of the Excel-made fixture (tests/office/sparklines, made by
// tests/office/make_sparkline_tests.py) read, and drawn as Excel draws them (shapes counted here;
// the pixels are compared with Excel's by tests/office/compare_sparklines.py).
#include "TestHarness.h"

#include <cmath>

#include "SheetTestUtil.h"
#include "sheet/view/CellPaint.h"
#include "sheet/view/Sparklines.h"

using namespace fw;
using namespace fw::sheet;
using view::SparklineGroup;

namespace {

// Counts what a sparkline draws: filled shapes (bars, markers, the axis) and stroked paths (lines).
struct Counter : draw::Painter {
  int fills = 0, strokes = 0, clips = 0;
  float lastWidth = 0;
  void Save() override {}
  void Restore() override {}
  void Transform(const draw::Matrix&) override {}
  draw::Matrix CurrentTransform() const override { return {}; }
  void ClipRect(const draw::Rect&) override { ++clips; }
  void ClipPath(const draw::Path&) override {}
  void BeginGroup(float) override {}
  void FillPath(const draw::Path&, const draw::Brush&) override { ++fills; }
  void StrokePath(const draw::Path&, const draw::Pen& pen) override {
    ++strokes;
    lastWidth = pen.width;
  }
  void DrawGlyphs(draw::Point, const draw::GlyphRun&, const draw::Brush&) override {}
  void DrawBitmap(const draw::Bitmap&, const draw::Rect&, const draw::Rect*, float) override {}
};

struct Paper : view::Geometry {
  Paper() { centered = true, px = 0.75f, rowPx = 0.72f, line = 0.75f; }
  float Left(uint32_t c) const override { return static_cast<float>(c) * 200; }
  float Right(uint32_t c) const override { return static_cast<float>(c + 1) * 200; }
  float Top(uint32_t r) const override { return static_cast<float>(r) * 36; }
  float Bottom(uint32_t r) const override { return static_cast<float>(r + 1) * 36; }
};

Counter DrawCase(const view::Sparklines& s, uint32_t row) {
  Counter c;
  Paper g;
  s.Draw(c, g, row, 1, {g.Left(1), g.Top(row), g.Right(1), g.Bottom(row)});
  return c;
}

std::unique_ptr<Workbook> Fixture() { return sheet_test::OpenZip(sheet_test::TestFile("office/sparklines/sparklines.xlsx")); }

}  // namespace

TEST(sheet_sparklines_read) {
  auto book = Fixture();
  if (!book) return;
  view::Sparklines s(*book, 0);
  CHECK(!s.Empty());
  CHECK_EQ(s.Groups().size(), 17u);
  int lines = 0, columns = 0, winLoss = 0;
  for (const SparklineGroup& g : s.Groups()) {
    CHECK_EQ(g.lines.size(), 1u);
    CHECK_EQ(g.lines[0].col, 1u);       // column B
    CHECK_EQ(g.lines[0].sheet, 1);      // their data on "Data"
    CHECK_EQ(g.lines[0].data.lastCol - g.lines[0].data.firstCol, 11u);
    lines += g.type == SparklineGroup::Type::Line;
    columns += g.type == SparklineGroup::Type::Column;
    winLoss += g.type == SparklineGroup::Type::WinLoss;
  }
  CHECK_EQ(lines, 11);
  CHECK_EQ(columns, 4);
  CHECK_EQ(winLoss, 2);
  // The weighted red line (row 5), the connected gaps (row 8), the custom scale (row 15).
  for (const SparklineGroup& g : s.Groups()) {
    const uint32_t row = g.lines[0].row + 1;
    if (row == 5) {
      CHECK_NEAR(g.lineWeight, 2.25f, 1e-6);
      CHECK_EQ(g.series, 0xFFC00000u);
    }
    if (row == 8) CHECK(g.empty == SparklineGroup::Empty::Span);
    if (row == 6) CHECK(g.empty == SparklineGroup::Empty::Gap);
    if (row == 15) {
      CHECK(g.minScale == SparklineGroup::Scale::Custom && g.maxScale == SparklineGroup::Scale::Custom);
      CHECK_NEAR(g.manualMin, -10, 1e-9);
      CHECK_NEAR(g.manualMax, 15, 1e-9);
    }
    if (row == 16) CHECK(g.rightToLeft && g.markers);
  }
}

TEST(sheet_sparklines_draw) {
  auto book = Fixture();
  if (!book) return;
  view::Sparklines s(*book, 0);
  // A line: one stroke, no markers; with markers, one per point.
  Counter line = DrawCase(s, 0);
  CHECK_EQ(line.strokes, 1);
  CHECK_EQ(line.fills, 0);
  CHECK_EQ(DrawCase(s, 1).fills, 12);
  // High, low, first and last on a line without markers: those points only (3, -2 ... -5: the
  // first, the last, which is also the low, and the high).
  CHECK_EQ(DrawCase(s, 2).fills, 3);
  CHECK_NEAR(DrawCase(s, 4).lastWidth, 2.25f, 1e-6);
  // Gaps: still one path (with breaks); as zero, the same.
  CHECK_EQ(DrawCase(s, 5).strokes, 1);
  // The axis on a line that crosses zero: one fill.
  CHECK_EQ(DrawCase(s, 8).fills, 1);
  // Columns: a bar each; with the axis, one more.
  CHECK_EQ(DrawCase(s, 9).fills, 12);
  CHECK_EQ(DrawCase(s, 10).fills, 13);
  // Win/loss: no bar for the two zeros; with the axis, one more.
  CHECK_EQ(DrawCase(s, 12).fills, 10);
  CHECK_EQ(DrawCase(s, 13).fills, 11);
  // No sparkline in A1.
  Counter none;
  Paper g;
  s.Draw(none, g, 0, 0, {0, 0, 200, 36});
  CHECK_EQ(none.fills + none.strokes + none.clips, 0);
}
