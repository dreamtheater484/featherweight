#pragma once

#include <pugixml.hpp>

#include <cstdint>
#include <optional>
#include <string_view>
#include <string>
#include <utility>
#include <vector>

#include "draw/Shapes.h"

// fwchart (plan D5): charts shown read-only from the values their part caches (c:chartSpace):
// column, bar, line, pie and doughnut, area, scatter, bubble and radar, with the title, axes, gridlines,
// legend and data labels. fwdraw reads a chart through the hook Install() sets, so charts show on screen,
// printed and in PDF alike. Other types stay placeholders; a chart Word stores with a picture for
// older versions (mc:AlternateContent) shows that picture. Nothing here writes: the chart part is
// saved as it was read.
namespace fw::chart {

enum class Type : uint8_t { Column, Bar, Line, Pie, Doughnut, Area, Scatter, Radar, Bubble };
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
  std::vector<double> xs;                // scatter and bubble: c:xVal (NaN where missing)
  std::vector<double> values;            // c:val or c:yVal (NaN where missing)
  std::vector<double> sizes;             // bubble: c:bubbleSize (NaN where missing)
  Look look;
  std::vector<std::pair<uint32_t, Look>> points;  // c:dPt: a point's own look
  std::optional<bool> marker;  // line and scatter: c:marker (nullopt: as the group has it)
  std::string symbol;          // c:marker/c:symbol ("circle", "diamond"...; empty: Office's choice)
  float markerSize = 7;        // c:marker/c:size, points (unstated: Office 2007's 7, PowerPoint's PDFs)
  bool smooth = false;         // line and scatter: c:smooth (Office smooths when it's missing)
  // c:dLbls: values, percentages, category names, series names, legend keys (nullopt: as the
  // group has it), and the text between them.
  std::optional<bool> labels, percent, categoryLabels, seriesNames, keys;
  std::optional<std::wstring> separator;
  std::string labelFormat;  // c:dLbls/c:numFmt not linked to the source; empty: the group's
  std::string labelPos;     // c:dLbls/c:dLblPos ("outEnd", "ctr"...); empty: the group's
  double explosion = 0;     // pie: c:explosion, percent of the radius
  std::vector<std::pair<uint32_t, double>> explosions;  // c:dPt/c:explosion: a slice's own
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
  bool filled = false;            // radar: filled shapes rather than lines (c:radarStyle filled)
  bool labels = false, percent = false, categoryLabels = false, seriesNames = false, keys = false;
  std::optional<std::wstring> separator;  // nullopt: the user's list separator and a space
  std::string labelFormat;  // c:dLbls/c:numFmt not linked to the source; empty: the value axis's
  std::string labelPos;     // c:dLbls/c:dLblPos; empty: the type's default (pie: best fit)
  double bubbleScale = 100;  // bubble: percent of the default size (the largest a quarter of the plot)
  bool bubbleWidth = false;  // bubble: c:sizeRepresents w (sizes are diameters, not areas)
  bool secondary = false;  // on the secondary value axis
  std::vector<Series> series;
};

struct Axis {
  bool shown = true;      // c:delete, c:tickLblPos none
  bool gridlines = false;  // c:majorGridlines
  Look grid, line;         // the gridlines' and the axis line's c:spPr (unstated: Office 2007's gray)
  enum class Tick : uint8_t { None, Out, In, Cross };
  Tick ticks = Tick::Cross;  // c:majorTickMark (unstated: cross)
  std::string format;     // c:numFmt formatCode; empty: General
  std::optional<double> min, max;
  bool reversed = false;  // c:scaling/c:orientation maxMin
  bool between = true;    // categories between tick marks (c:crossBetween)
  std::wstring title;
  bool titleOverlay = false;  // the title drawn over the plot, which keeps its room
};

enum class LegendPos : uint8_t { None, Right, Left, Top, Bottom };

struct Chart {
  std::wstring title;  // empty: none
  std::vector<Group> groups;
  Axis category, value, secondary;  // scatter: `category` is the x value axis
  LegendPos legend = LegendPos::None;
  // c:overlay: the title or legend drawn over the plot area, which keeps the room it would take.
  bool titleOverlay = false, legendOverlay = false;
  Look area, plot;  // the chart's and the plot area's c:spPr
  float textSize = 10, titleSize = 14;  // points (unstated: Office 2007's 10)
  std::optional<draw::Color> textColor, titleColor;  // nullopt: black
  bool titleBold = true;
  std::wstring titleFont;  // empty: the chart's (the theme's minor font)
  bool roundedCorners = false;  // the chart area's (c:roundedCorners; Excel rounds them when it's missing)
  // Office 2007's look, which Excel keeps for a chart without c14:style (Office 2010 and later
  // write it): gridlines and axis lines it doesn't color black rather than gray, and a title the
  // chart's text size. Word draws such charts in Office 2010's look.
  bool legacy = false;
  bool workbook = false;  // in a workbook (Read had a DataSource): Excel's denser value axis labels
  // In a presentation: Excel's denser value axis labels too, and no chart area border unless the
  // chart states one (PowerPoint's PDFs).
  bool presentation = false;
  bool styled = false;  // the chart states c14:style (Office 2010 and later wrote it)
};

// Where a chart in a workbook reads its data: the cells its references (c:f) name, as they show,
// rather than the values cached in the chart part (missing in files other programs write, stale
// in some).
class DataSource {
 public:
  virtual ~DataSource() = default;
  // The cells of a reference ('Sheet 1'!$B$2:$B$7, or several areas in parentheses), in order:
  // each one's text as shown (UTF-8) and its number (NaN when it isn't one); `format`, when not
  // null, the first cell's number format code. False when it can't be resolved: the cache is used.
  virtual bool Cells(std::string_view ref, std::vector<std::string>& texts, std::vector<double>& numbers,
                     std::string* format) = 0;
  // Only the numbers (a series' values), which needn't be formatted as text: Cells' by default.
  virtual bool Numbers(std::string_view ref, std::vector<double>& numbers) {
    std::vector<std::string> texts;
    return Cells(ref, texts, numbers, nullptr);
  }
};

// The chart a c:chartSpace holds; false when it holds nothing Featherweight draws. Colors are
// resolved with `colors` (the document's theme); `data`, when given, supplies the series' values,
// and says the chart is in a workbook, which Excel draws as it does (Chart::legacy, corners).
// `presentation`: in a PowerPoint deck, which keeps Office 2007's look as Excel does.
bool Read(pugi::xml_node chartSpace, const draw::ColorContext& colors, Chart& out, DataSource* data = nullptr,
          bool presentation = false);
// A number as an Excel format code shows it (the first section: decimals, thousands separators,
// percent, literal text, dates); "General" or empty: up to 10 significant digits.
std::wstring FormatNumber(double v, std::string_view code);
// The decimal and thousands separators FormatNumber uses: '.' and ',' until Install() sets the
// user's (Windows' regional settings), as Office does.
void SetNumberSeparators(wchar_t decimal, wchar_t group);
// What goes between the parts of a data label (series, category, value, percentage) that don't
// state it: the list separator and a space, ", " until Install() sets the user's (Office's choice).
void SetListSeparator(wchar_t separator);
// The user's short date as Windows writes it ("dd/MM/yyyy"), which Excel's built-in short date
// (m/d/yyyy) shows as; empty: m/d/yyyy as it is. Install() sets it.
void SetShortDate(std::string_view picture);
// The chart laid out as vector art at `size` (points), its text in `font`.
draw::Art Draw(const Chart& chart, draw::Size size, const draw::ColorContext& colors, const std::wstring& font);

// fwdraw reads charts with fwchart from now on (draw::SetChartReader), numbers shown with the
// user's separators.
void Install();

}  // namespace fw::chart
