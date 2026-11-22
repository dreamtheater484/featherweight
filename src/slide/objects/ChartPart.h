#pragma once

#include <pugixml.hpp>

#include <cstdint>
#include <string>
#include <vector>

// A chart inserted on a slide (P2, p2-objects; plan §8.2 "insert ... charts"): the chart part,
// drawn by fwchart as every chart is, and the small workbook embedded beside it that holds its
// data, as PowerPoint writes them, so PowerPoint's Edit Data opens it in Excel. The chart's caches
// and the embedded workbook are always written together (plan §4.3 rule 2, derived data).
namespace fw::slide::objects {

enum class ChartType : uint8_t { Column, Bar, Line, Pie };

// The data: one row per category, one column per series (A1 is empty, series names in row 1,
// categories in column A).
struct ChartData {
  std::vector<std::string> categories;  // UTF-8
  std::vector<std::string> series;
  std::vector<std::vector<double>> values;  // [series][category]
};

// PowerPoint's starting data: Category 1-4, Series 1-3.
ChartData DefaultChartData();

// The chart part (c:chartSpace) for `data`, its series' caches filled in and their formulas
// pointing into Sheet1 of the embedded workbook, which its relationship `workbookRelId` names.
std::string ChartXml(ChartType type, const ChartData& data, const std::string& workbookRelId = "rId1");

// The embedded workbook: a minimal .xlsx with Sheet1 holding `data`.
std::vector<uint8_t> ChartWorkbook(const ChartData& data);

// The data a chart part shows (its first plot's series caches), for editing it; false when it
// has no series to read.
bool ReadChartData(pugi::xml_node chartSpace, ChartData& out);

// Writes `data` into the chart's first plot: its series' names, categories and values (caches
// and Sheet1 formulas). Series added copy the last one's formatting; series beyond the data go.
// Everything else in the chart stays. False when the chart has no plot Featherweight writes into.
bool WriteChartData(pugi::xml_node chartSpace, const ChartData& data);

// Package names, types and the part's content type, for the edit layer.
inline constexpr const char* kChartRelType = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/chart";
inline constexpr const char* kPackageRelType = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/package";
inline constexpr const char* kChartContentType = "application/vnd.openxmlformats-officedocument.drawingml.chart+xml";
inline constexpr const char* kXlsxContentType = "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet";
inline constexpr const char* kImageRelType = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/image";

// A column's letters in a cell reference (0 = A).
std::string ColumnName(int column);

}  // namespace fw::slide::objects
