#pragma once

#include <pugixml.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "draw/Shapes.h"

// fwchart (plan D5): charts shown read-only from the values their part caches (c:chartSpace):
// column, bar, line, pie and doughnut, area and scatter, with the title, axes, gridlines, legend and
// data labels. fwdraw reads a chart through the hook Install() sets, so charts show on screen,
// printed and in PDF alike. Other types stay placeholders; a chart Word stores with a picture for
// older versions (mc:AlternateContent) shows that picture. Nothing here writes: the chart part is
// saved as it was read.
namespace fw::chart {

enum class Type : uint8_t { Column, Bar, Line, Pie, Doughnut, Area, Scatter };
enum class Grouping : uint8_t { Clustered, Stacked, Percent };

// A fill and an outline from c:spPr: plain colors, or none.
struct Look {
  std::optional<draw::Color> fill, line;
  bool noFill = false, noLine = false;
  float lineWidth = -1;  // points; negative: the type's default
};

struct Series {
  std::wstring name;
  std::vector<std::wstring> categories;  // c:cat's cached text (numbers formatted)
  std::vector<double> xs;                // scatter: c:xVal (NaN where missing)
  std::vector<double> values;            // c:val or c:yVal (NaN where missing)
  Look look;
  std::vector<std::pair<uint32_t, Look>> points;  // c:dPt: a point's own look
  std::optional<bool> marker;  // line and scatter: c:marker (nullopt: as the group has it)
  bool smooth = false;         // line and scatter: c:smooth (Office smooths when it's missing)
  std::optional<bool> labels, percent, categoryLabels;  // c:dLbls (nullopt: as the group has it)
};

struct Group {
  Type type = Type::Column;
  Grouping grouping = Grouping::Clustered;
  bool varyColors = false;
  double gap = 150, overlap = 0;  // percent of a bar's width
  double hole = 50;               // doughnut: percent of the radius
  double firstAngle = 0;          // pie: degrees clockwise from 12 o'clock
  bool marker = false;            // line: markers unless a series says not
  bool lines = true;              // scatter: lines between points
  bool labels = false, percent = false, categoryLabels = false;
  bool secondary = false;  // on the secondary value axis
  std::vector<Series> series;
};

struct Axis {
  bool shown = true;      // c:delete, c:tickLblPos none
  bool gridlines = false;  // c:majorGridlines
  std::string format;     // c:numFmt formatCode; empty: General
  std::optional<double> min, max;
  bool reversed = false;  // c:scaling/c:orientation maxMin
  bool between = true;    // categories between tick marks (c:crossBetween)
  std::wstring title;
};

enum class LegendPos : uint8_t { None, Right, Left, Top, Bottom };

struct Chart {
  std::wstring title;  // empty: none
  std::vector<Group> groups;
  Axis category, value, secondary;  // scatter: `category` is the x value axis
  LegendPos legend = LegendPos::None;
  Look area, plot;  // the chart's and the plot area's c:spPr
  float textSize = 9, titleSize = 14;  // points
  std::optional<draw::Color> textColor, titleColor;  // nullopt: black
  bool titleBold = true;
};

// The chart a c:chartSpace holds; false when it holds nothing Featherweight draws. Colors are
// resolved with `colors` (the document's theme).
bool Read(pugi::xml_node chartSpace, const draw::ColorContext& colors, Chart& out);
// A number as an Excel format code shows it (the first section: decimals, thousands separators,
// percent, literal text, dates); "General" or empty: up to 10 significant digits.
std::wstring FormatNumber(double v, std::string_view code);
// The decimal and thousands separators FormatNumber uses: '.' and ',' until Install() sets the
// user's (Windows' regional settings), as Office does.
void SetNumberSeparators(wchar_t decimal, wchar_t group);
// The user's short date as Windows writes it ("dd/MM/yyyy"), which Excel's built-in short date
// (m/d/yyyy) shows as; empty: m/d/yyyy as it is. Install() sets it.
void SetShortDate(std::string_view picture);
// The chart laid out as vector art at `size` (points), its text in `font`.
draw::Art Draw(const Chart& chart, draw::Size size, const draw::ColorContext& colors, const std::wstring& font);

// fwdraw reads charts with fwchart from now on (draw::SetChartReader), numbers shown with the
// user's separators.
void Install();

}  // namespace fw::chart
