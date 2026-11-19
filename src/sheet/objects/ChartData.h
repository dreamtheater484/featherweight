#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "chart/Chart.h"
#include "sheet/numfmt/NumFormat.h"

namespace fw::sheet {
class Workbook;
}

namespace fw::sheet::objects {

// A chart's data from the workbook's cells (fwchart's DataSource), as they show: references like
// 'Sheet 1'!$B$2:$B$7, several areas in parentheses, or a defined name. A reference without a
// sheet is on the chart's own sheet. Other workbooks' cells aren't read (the cache is used).
class WorkbookData final : public chart::DataSource {
 public:
  WorkbookData(Workbook& book, int sheet) : book_(book), sheet_(sheet) {}
  bool Cells(std::string_view ref, std::vector<std::string>& texts, std::vector<double>& numbers,
             std::string* format) override;
  bool Numbers(std::string_view ref, std::vector<double>& numbers) override;

 private:
  bool Collect(std::string_view ref, int depth, std::vector<std::string>& texts, std::vector<double>& numbers,
               std::string* format);
  bool Area(std::string_view area, int depth, std::vector<std::string>& texts, std::vector<double>& numbers,
            std::string* format);
  const NumberFormat& FormatOf(uint32_t numFmt);

  Workbook& book_;
  int sheet_;
  bool numbersOnly_ = false;  // Numbers: the cells' texts left empty, not formatted
  std::unordered_map<uint32_t, NumberFormat> formats_;
};

}  // namespace fw::sheet::objects
