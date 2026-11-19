#pragma once

#include <cstdint>
#include <string>

#include "sheet/model/Types.h"

namespace fw::sheet {
class Sheet;
}

// Charts as Excel writes them (S3, s3-charts): a new chart's part (xl/charts/chartN.xml) with its
// series formulas and caches in Excel 2013 and later's default look, and its anchor in the sheet's
// drawing. The edit layer's Editor ops put them into the package.
namespace fw::sheet::edit {

enum class ChartKind : uint8_t { Column, Bar, Line, Pie, Doughnut, Area, Scatter };

namespace charts {

// A new chart of `data` on sheet `s`, its series and categories guessed from the cells as Excel
// guesses them (a header row and column; series in columns when there are at least as many rows).
// Empty when there's nothing to chart.
std::string NewChartXml(const Sheet& s, const Range& data, ChartKind kind, const std::string& title);

// A drawing's anchor for a chart (relationship `rel` from the drawing) over the cells `from` to
// `to` (their top left corners), shape id `id`.
std::string ChartAnchor(CellAddr from, CellAddr to, const std::string& rel, uint32_t id);

}  // namespace charts
}  // namespace fw::sheet::edit
