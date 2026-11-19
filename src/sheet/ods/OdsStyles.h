#pragma once

// The ODS converter's styles: ODF cell styles and data styles as SpreadsheetML's cell formats,
// fonts, fills, borders and number format codes (Ods.cpp writes the cells, OdsStyles.cpp these).
#include <pugixml.hpp>

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fw::sheet::ods::detail {

struct Line {
  std::string style, color;  // Excel's border style ("thin", "double"), "" for none
};

struct Xf {
  std::string font = "Calibri";
  double size = 11;
  bool bold = false, italic = false, strike = false;
  int underline = 0;  // 1 single, 2 double
  int position = 0;   // 1 superscript, 2 subscript
  std::string color, fill;  // "RRGGBB", "" automatic or none
  Line left, right, top, bottom, diagonal;
  bool diagUp = false, diagDown = false;
  std::string horizontal, vertical;  // Excel's values, "" for the default
  bool wrap = false, shrink = false;
  int indent = 0, rotation = 0;
  bool locked = true, hidden = false;
  std::string numFmt;  // Excel's code, "" for General
};

class Styles {
 public:
  // Reads the font faces, cell styles and data styles of styles.xml and content.xml.
  void Read(pugi::xml_node stylesRoot, pugi::xml_node contentRoot);
  // The cell format index (into Xml's cellXfs) of a cell style, "" for the default style.
  int Resolve(std::string_view name);
  // Properties of the other families: table-column, table-row, table.
  pugi::xml_node Find(std::string_view family, std::string_view name) const;
  const Xf& Default() { return xfs_[Resolve("")]; }
  // Excel's width in characters for `points` of column width, with the default font's digit.
  double ColumnChars(double points);
  // styles.xml.
  std::string Xml() const;

 private:
  void Apply(pugi::xml_node style, Xf& xf);
  std::string NumberCode(pugi::xml_node style);
  std::string Section(pugi::xml_node style, bool& text);

  std::unordered_map<std::string, std::string> faces_;  // style:font-face name -> family
  std::unordered_map<std::string, pugi::xml_node> cell_, other_, data_;
  pugi::xml_node defaultCell_;
  std::unordered_map<std::string, int> resolved_;     // style name -> xf
  std::unordered_map<std::string, int> xfIndex_;      // serialized Xf -> xf
  std::vector<Xf> xfs_;
  std::unordered_map<std::string, std::string> codes_;  // data style name -> code
};

}  // namespace fw::sheet::ods::detail
