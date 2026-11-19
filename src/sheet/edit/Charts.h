#pragma once

#include <cstdint>
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

}  // namespace charts
}  // namespace fw::sheet::edit
