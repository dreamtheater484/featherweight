#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "sheet/model/Types.h"

namespace fw::sheet {
class Sheet;
class Workbook;
}

// Charts as Excel writes them (S3, s3-charts): a new chart's part (xl/charts/chartN.xml) with its
// series formulas and caches in Excel 2013 and later's default look, and its anchor in the sheet's
// drawing. The edit layer's Editor ops put them into the package.
namespace fw::sheet::edit {

enum class ChartKind : uint8_t { Column, Bar, Line, Pie, Doughnut, Area, Scatter };

// A chart's series as Select Data lists them: formulas as the file writes them ("Sheet1!$B$1",
// "'My sheet'!$B$2:$B$5"; empty: none).
struct ChartSeries {
  std::string name, values, categories;
};
// A title: shown or not, its text (empty: Excel's own, "Chart Title", the series' name, "Axis Title").
struct ChartText {
  bool shown = true;
  std::string text;
};
// A chart's own settings, as its panel shows them. `kind` is empty for kinds and combinations it
// doesn't make.
struct ChartInfo {
  std::optional<ChartKind> kind;
  ChartText title, xTitle, yTitle;  // the category (X) and value (Y) axes' titles
  char legend = 0;                  // 'b', 'r', 't', 'l' or 0: none
  bool labels = false;              // data labels showing values
  std::vector<ChartSeries> series;
  std::vector<uint32_t> points;     // each series' number of values (as cached)
  std::vector<std::string> names;   // each series' name as it shows (its cache or its text)
};
// A series' color (or one data point's, a pie's slice), 0xRRGGBB.
struct ChartColor {
  uint32_t series = 0;
  int32_t point = -1;
  uint32_t rgb = 0;
};
// What an edit changes (unset: left as it is).
struct ChartEdit {
  std::optional<ChartKind> kind;
  std::optional<ChartText> title, xTitle, yTitle;
  std::optional<char> legend;
  std::optional<bool> labels;
  std::vector<ChartColor> colors;
  std::optional<std::vector<ChartSeries>> series;  // Select Data: the whole list, in order
};

namespace refs {
struct CachePoint;
}

namespace charts {

// A new chart of `data` on sheet `s`, its series and categories guessed from the cells as Excel
// guesses them (a header row and column; series in columns when there are at least as many rows).
// Empty when there's nothing to chart.
std::string NewChartXml(const Sheet& s, const Range& data, ChartKind kind, const std::string& title);

// A drawing's anchor for a chart (relationship `rel` from the drawing) over the cells `from` to
// `to` (their top left corners), shape id `id`.
std::string ChartAnchor(CellAddr from, CellAddr to, const std::string& rel, uint32_t id);

// A chart part's caches read again from the cells (on a save's worker thread: the workbook is
// read-only then). False when none differs, so the part is saved as it is. Sheets not loaded haven't
// changed since the file was read and are left alone.
bool RefreshCaches(const Workbook& book, std::string& xml);
// The cells a series formula ("Sheet1!$B$2:$B$5", 'My sheet'!...) names, as a cache holds them:
// numbers as the cache writes them, text, TRUE/FALSE, errors as #N/A; hidden rows and columns left
// out when `visibleOnly`. False when it isn't one loaded sheet's row or column.
bool CachePoints(const Workbook& book, std::string_view formula, bool visibleOnly, std::vector<refs::CachePoint>& out);

// The series Excel guesses for a range of sheet `s` (as for a new chart), as Select Data lists them.
std::vector<ChartSeries> SeriesFor(const Sheet& s, const Range& data);
// A chart part's settings.
ChartInfo Info(std::string_view xml);
// The chart part with the edit made: what the edit doesn't change stays as it was (a new type or
// series list writes the series anew in that type's look, each keeping its color). Empty when it
// can't be made (a part it can't read; a type or series change on a chart whose prefixes aren't
// Excel's c: and a:).
std::string Edited(const Workbook& book, std::string_view xml, const ChartEdit& e);

}  // namespace charts
}  // namespace fw::sheet::edit
