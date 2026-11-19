// OpenDocument spreadsheet (.ods) to SpreadsheetML: the parts an .xlsx needs, written from the ODF
// content, styles and settings, zipped. The .xlsx reader then opens it as any workbook, so the
// grid, editing, recalculation and saving are the same code (as D5 does for .odt).
#include "sheet/ods/Ods.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <map>
#include <set>
#include <unordered_map>

#include "pkg/Odf.h"
#include "pkg/Snapshot.h"
#include "pkg/XmlDom.h"
#include "pkg/Zip.h"
#include "sheet/ods/OdsStyles.h"
#include "sheet/xlsx/Internal.h"

namespace fw::sheet::ods {
namespace {

using pugi::xml_node;
using pkg::odf::Escape;

constexpr std::string_view kDecl = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n";
constexpr std::string_view kMain = "http://schemas.openxmlformats.org/spreadsheetml/2006/main";
constexpr std::string_view kRelNs = "http://schemas.openxmlformats.org/officeDocument/2006/relationships";
constexpr std::string_view kRelType = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/";
constexpr int kMaxRow = 1048576, kMaxCol = 16384;
constexpr int kMaxDepth = 32;               // nested spans in a cell, nested row groups

std::string_view Attr(xml_node n, const char* name) { return n.attribute(name).value(); }

int Repeat(xml_node n, const char* name) {
  const char* v = n.attribute(name).value();
  if (!*v) return 1;
  const long long x = std::atoll(v);
  return static_cast<int>(std::clamp<long long>(x, 1, kMaxRow));
}

std::string Col(int col) {
  std::string s;
  for (int c = col + 1; c > 0; c = (c - 1) / 26) s.insert(s.begin(), static_cast<char>('A' + (c - 1) % 26));
  return s;
}
std::string Ref(int row, int col) { return Col(col) + std::to_string(row + 1); }

std::string Num(double v) {
  char buf[40];
  auto r = std::to_chars(buf, buf + sizeof(buf), v);
  return std::string(buf, r.ptr);
}

bool ParseNumber(std::string_view s, double& v) {
  while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
  if (!s.empty() && s.front() == '+') s.remove_prefix(1);
  auto r = std::from_chars(s.data(), s.data() + s.size(), v);
  return r.ec == std::errc() && r.ptr == s.data() + s.size() && std::isfinite(v);
}

// Days from 1970-01-01 (Howard Hinnant's days_from_civil).
int64_t Days(int64_t y, int m, int d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const int64_t yoe = y - era * 400;
  const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

// "2024-03-15" or "2024-03-15T10:30:00.5" as days and the day's fraction.
bool ParseDate(std::string_view s, double& days) {
  int y = 0, m = 0, d = 0;
  bool neg = !s.empty() && s[0] == '-';
  if (neg) s.remove_prefix(1);
  auto num = [&](std::string_view& t, int& out, size_t n) {
    if (t.size() < n) return false;
    auto r = std::from_chars(t.data(), t.data() + n, out);
    if (r.ec != std::errc() || r.ptr != t.data() + n) return false;
    t.remove_prefix(n);
    return true;
  };
  const size_t dash = s.find('-');
  if (dash == std::string_view::npos || dash < 4 || dash > 6 || !num(s, y, dash) || s.empty() || s[0] != '-') return false;
  s.remove_prefix(1);
  if (!num(s, m, 2) || s.empty() || s[0] != '-') return false;
  s.remove_prefix(1);
  if (!num(s, d, 2) || m < 1 || m > 12 || d < 1 || d > 31) return false;
  days = static_cast<double>(Days(neg ? -y : y, m, d));
  if (!s.empty() && s[0] == 'T') {
    s.remove_prefix(1);
    int h = 0, mi = 0;
    double sec = 0;
    if (!num(s, h, 2) || s.empty() || s[0] != ':') return true;
    s.remove_prefix(1);
    if (!num(s, mi, 2)) return true;
    if (!s.empty() && s[0] == ':') {
      s.remove_prefix(1);
      size_t k = 0;
      while (k < s.size() && (std::isdigit(static_cast<unsigned char>(s[k])) || s[k] == '.')) ++k;
      ParseNumber(s.substr(0, k), sec);
    }
    days += (h * 3600 + mi * 60 + sec) / 86400.0;
  }
  return true;
}

// "PT12H30M15S", "-P1DT2H" as days.
bool ParseDuration(std::string_view s, double& days) {
  bool neg = !s.empty() && s[0] == '-';
  if (neg) s.remove_prefix(1);
  if (s.empty() || s[0] != 'P') return false;
  s.remove_prefix(1);
  double total = 0, n = 0;
  bool time = false, any = false;
  while (!s.empty()) {
    if (s[0] == 'T') {
      time = true;
      s.remove_prefix(1);
      continue;
    }
    size_t k = 0;
    while (k < s.size() && (std::isdigit(static_cast<unsigned char>(s[k])) || s[k] == '.')) ++k;
    if (k == 0 || k == s.size() || !ParseNumber(s.substr(0, k), n)) return false;
    const char unit = s[k];
    s.remove_prefix(k + 1);
    any = true;
    if (unit == 'D') total += n * 86400;
    else if (unit == 'H' && time) total += n * 3600;
    else if (unit == 'M' && time) total += n * 60;
    else if (unit == 'S' && time) total += n;
    else if (unit == 'Y') total += n * 365 * 86400;
    else if (unit == 'M') total += n * 30 * 86400;
    else return false;
  }
  days = (neg ? -total : total) / 86400.0;
  return any;
}

bool IsError(std::string_view s) {
  for (std::string_view e : {"#NULL!", "#DIV/0!", "#VALUE!", "#REF!", "#NAME?", "#NUM!", "#N/A", "#GETTING_DATA", "#SPILL!", "#CALC!"})
    if (s == e) return true;
  return false;
}

// A cell's text: its paragraphs on lines of their own, spaces, tabs and line breaks as written.
void Inline(xml_node n, std::string& out, int depth, std::string* link) {
  for (xml_node c : n.children()) {
    if (c.type() == pugi::node_pcdata || c.type() == pugi::node_cdata) {
      out += c.value();
      continue;
    }
    if (c.type() != pugi::node_element) continue;
    const std::string_view name = c.name();
    if (name == "text:s") {
      const int count = std::clamp(std::atoi(std::string(Attr(c, "text:c")).c_str()), 1, 1000);
      out.append(count, ' ');
    } else if (name == "text:tab") {
      out += '\t';
    } else if (name == "text:line-break") {
      out += '\n';
    } else if (name == "office:annotation" || name == "text:note" || name == "draw:frame") {
      continue;
    } else if (depth < kMaxDepth) {
      if (name == "text:a" && link && link->empty()) *link = Attr(c, "xlink:href");
      Inline(c, out, depth + 1, link);
    }
  }
}

std::string CellText(xml_node cell, std::string* link) {
  std::string out;
  bool first = true;
  for (xml_node p : cell.children()) {
    const std::string_view n = p.name();
    if (n != "text:p" && n != "text:h") continue;
    if (!first) out += '\n';
    first = false;
    Inline(p, out, 0, link);
    if (out.size() > 32767) break;
  }
  if (out.size() > 32767) {
    out.resize(32767);
    while (!out.empty() && (static_cast<unsigned char>(out.back()) & 0xC0) == 0x80) out.pop_back();
    if (!out.empty() && (static_cast<unsigned char>(out.back()) & 0x80)) out.pop_back();
  }
  return out;
}

bool ValidName(std::string_view n) {
  if (n.empty() || n.size() > 255) return false;
  if (!(std::isalpha(static_cast<unsigned char>(n[0])) || n[0] == '_' || n[0] == '\\' || (n[0] & 0x80))) return false;
  for (char c : n)
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '\\' || (c & 0x80))) return false;
  // Not a reference: A1, R1C1.
  size_t i = 0;
  while (i < n.size() && std::isalpha(static_cast<unsigned char>(n[i]))) ++i;
  if (i && i <= 3 && i < n.size() && std::all_of(n.begin() + i, n.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); }))
    return false;
  return n != "R" && n != "C" && n != "r" && n != "c";
}

std::string Absolute(std::string_view ref) {  // "A1:D20" as "$A$1:$D$20"
  std::string out;
  bool start = true;
  for (char c : ref) {
    if (start && std::isalpha(static_cast<unsigned char>(c))) out += '$';
    if (std::isdigit(static_cast<unsigned char>(c)) && !out.empty() && std::isalpha(static_cast<unsigned char>(out.back())))
      out += '$';
    out += c;
    start = c == ':';
  }
  return out;
}

struct Name {
  std::string name, value;
  int sheet = -1;  // localSheetId
  bool hidden = false;
};

struct Sheet {
  std::string odfName, name;
  xml_node table;
  bool hidden = false;
  std::string tabColor;
  std::string autoFilter;
};

class Converter {
 public:
  Converter(const pkg::ZipReader& zip, const pkg::Limits& limits, const DigitWidth& digit) : zip_(zip), limits_(limits) {
    st_.digit = digit;
  }
  pkg::Status Run(std::vector<uint8_t>& xlsx);

 private:
  struct Row {
    int index;
    std::string xml;
  };
  struct ColRun {
    int first, last, xf;
    double width;
    bool hidden;
    int level;
  };
  struct View {
    int xSplit = 0, ySplit = 0, left = 0, top = 0, cursorX = -1, cursorY = -1, zoom = 0;
    bool grid = true;
  };

  std::string WriteSheet(size_t index);
  void Columns(xml_node parent, std::vector<ColRun>& runs, int& col, int level, int depth, int& headerFirst, int& headerLast,
               bool header);
  void Rows(xml_node parent, size_t sheet, std::vector<Row>& rows, int& row, int level, int depth, bool header);
  void WriteRow(xml_node row, int index, int repeat, int level, size_t sheet, std::vector<Row>& rows);
  int Shared(const std::string& s);
  void ReadSettings();
  void ReadNames(xml_node parent, int sheet);
  std::string SheetName(std::string_view odf) const;

  const pkg::ZipReader& zip_;
  const pkg::Limits& limits_;
  pkg::XmlDom content_, styles_, settings_;
  detail::Styles st_;
  std::vector<Sheet> sheets_;
  std::unordered_map<std::string, std::string> sheetNames_;  // ODF name -> workbook name
  SheetNames mapNames_;
  std::vector<std::string> strings_;
  std::unordered_map<std::string, int> stringIndex_;
  std::vector<Name> names_;
  std::map<std::string, View> views_;
  std::string activeSheet_;
  bool date1904_ = false;
  double nullDate_ = 0;  // days from 1970 of the date 0
  // Repeats multiply a small file: cells written and cells looked at stop at these (from the limits).
  uint64_t cells_ = 0, maxCells_ = 0, work_ = 0, maxWork_ = 0;

  // The sheet being written.
  std::vector<int> colXf_;
  struct Merge {
    int r1, c1, r2, c2;
  };
  std::vector<Merge> merges_;  // in the order the file has them
  std::vector<std::pair<std::string, std::string>> links_;  // cell, target
  double defaultHeight_ = 15;
  int headerRowFirst_ = -1, headerRowLast_ = -1;
  int minRow_ = kMaxRow, maxRow_ = -1, minCol_ = kMaxCol, maxCol_ = -1;
  int hiddenFrom_ = -1;  // rows from here to the end are hidden (zeroHeight)
};

int Converter::Shared(const std::string& s) {
  auto [it, added] = stringIndex_.emplace(s, static_cast<int>(strings_.size()));
  if (added) strings_.push_back(s);
  return it->second;
}

std::string Converter::SheetName(std::string_view odf) const {
  auto it = sheetNames_.find(std::string(odf));
  return it == sheetNames_.end() ? std::string() : it->second;  // unknown: #REF!
}

void Converter::ReadSettings() {
  // ooo:view-settings: Views / the first view / ActiveTable, ShowGrid and the Tables map.
  xml_node root = settings_.Root();
  if (!root) return;
  auto items = [](xml_node set, auto&& fn) {
    for (xml_node i : set.children("config:config-item")) fn(Attr(i, "config:name"), std::string_view(i.text().get()));
  };
  xml_node views;
  for (xml_node set : root.child("office:settings").children("config:config-item-set"))
    if (Attr(set, "config:name") == "ooo:view-settings")
      for (xml_node m : set.children("config:config-item-map-indexed"))
        if (Attr(m, "config:name") == "Views") views = m.child("config:config-item-map-entry");
  if (!views) return;
  View global;
  items(views, [&](std::string_view k, std::string_view v) {
    if (k == "ActiveTable") activeSheet_ = v;
    if (k == "ShowGrid") global.grid = v != "false";
  });
  for (xml_node m : views.children("config:config-item-map-named")) {
    if (Attr(m, "config:name") != "Tables") continue;
    for (xml_node e : m.children("config:config-item-map-entry")) {
      View v = global;
      int hMode = 0, vMode = 0, hPos = 0, vPos = 0;
      items(e, [&](std::string_view k, std::string_view val) {
        const int n = std::atoi(std::string(val).c_str());
        if (k == "HorizontalSplitMode") hMode = n;
        else if (k == "VerticalSplitMode") vMode = n;
        else if (k == "HorizontalSplitPosition") hPos = n;
        else if (k == "VerticalSplitPosition") vPos = n;
        else if (k == "PositionRight") v.left = n;
        else if (k == "PositionBottom") v.top = n;
        else if (k == "CursorPositionX") v.cursorX = n;
        else if (k == "CursorPositionY") v.cursorY = n;
        else if (k == "ZoomValue") v.zoom = n;
        else if (k == "ShowGrid") v.grid = val != "false";
      });
      if (hMode == 2) v.xSplit = std::clamp(hPos, 0, kMaxCol - 1);
      if (vMode == 2) v.ySplit = std::clamp(vPos, 0, kMaxRow - 1);
      views_[std::string(Attr(e, "config:name"))] = v;
    }
  }
}

void Converter::ReadNames(xml_node parent, int sheet) {
  for (xml_node n : parent.child("table:named-expressions").children()) {
    const std::string_view kind = n.name();
    std::string name(Attr(n, "table:name")), value;
    bool ok = false;
    if (kind == "table:named-range") ok = TranslateAddress(Attr(n, "table:cell-range-address"), value, &mapNames_);
    else if (kind == "table:named-expression") ok = TranslateFormula(Attr(n, "table:expression"), value, 0, 0, &mapNames_);
    if (!ok) continue;
    int scope = sheet;
    std::string lower = name;
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lower == "print_area" || lower == "print_titles") {
      name = lower == "print_area" ? "_xlnm.Print_Area" : "_xlnm.Print_Titles";
      if (scope < 0) {  // Excel's are per sheet: the sheet the range is on
        const size_t bang = value.find('!');
        std::string on = bang == std::string::npos ? std::string() : value.substr(0, bang);
        if (on.size() > 1 && on.front() == '\'') on = on.substr(1, on.size() - 2);
        for (size_t i = 0; i < sheets_.size(); ++i)
          if (sheets_[i].name == on) scope = static_cast<int>(i);
        if (scope < 0) continue;
      }
    } else if (!ValidName(name)) {
      continue;
    }
    bool dup = false;
    for (const Name& e : names_) dup |= e.sheet == scope && e.name == name;
    if (!dup) names_.push_back({name, value, scope, false});
  }
}

void Converter::Columns(xml_node parent, std::vector<ColRun>& runs, int& col, int level, int depth, int& headerFirst,
                        int& headerLast, bool header) {
  for (xml_node c : parent.children()) {
    const std::string_view n = c.name();
    if (n == "table:table-column") {
      const int count = std::min(Repeat(c, "table:number-columns-repeated"), kMaxCol - col);
      if (count <= 0) return;
      ColRun r{col, col + count - 1, st_.Resolve(Attr(c, "table:default-cell-style-name")), 0, false, level};
      if (xml_node s = st_.Find("table-column", Attr(c, "table:style-name")))
        if (auto w = pkg::odf::Points(Attr(s.child("style:table-column-properties"), "style:column-width")); w && *w >= 0)
          r.width = st_.ColumnChars(*w);
      r.hidden = Attr(c, "table:visibility") == "collapse" || Attr(c, "table:visibility") == "filter";
      runs.push_back(r);
      for (int i = col; i < col + count; ++i) colXf_[i] = r.xf;
      if (header) {
        if (headerFirst < 0) headerFirst = col;
        headerLast = col + count - 1;
      }
      col += count;
    } else if (depth < 8 && (n == "table:table-column-group" || n == "table:table-header-columns" || n == "table:table-columns")) {
      Columns(c, runs, col, n == "table:table-column-group" ? level + 1 : level, depth + 1, headerFirst, headerLast,
              header || n == "table:table-header-columns");
    }
  }
}

void Converter::Rows(xml_node parent, size_t sheet, std::vector<Row>& rows, int& row, int level, int depth, bool header) {
  for (xml_node r : parent.children()) {
    if (row >= kMaxRow) return;
    const std::string_view n = r.name();
    if (n == "table:table-row") {
      const int count = std::min(Repeat(r, "table:number-rows-repeated"), kMaxRow - row);
      if (header) {
        if (headerRowFirst_ < 0) headerRowFirst_ = row;
        headerRowLast_ = row + count - 1;
      }
      WriteRow(r, row, count, level, sheet, rows);
      row += count;
    } else if (depth < kMaxDepth && (n == "table:table-row-group" || n == "table:table-header-rows" || n == "table:table-rows")) {
      Rows(r, sheet, rows, row, n == "table:table-row-group" ? level + 1 : level, depth + 1,
           header || n == "table:table-header-rows");
    }
  }
}

void Converter::WriteRow(xml_node row, int index, int repeat, int level, size_t, std::vector<Row>& rows) {
  double height = defaultHeight_;
  bool custom = false;
  if (xml_node s = st_.Find("table-row", Attr(row, "table:style-name"))) {
    xml_node p = s.child("style:table-row-properties");
    if (auto h = pkg::odf::Points(Attr(p, "style:row-height")); h && *h >= 0) height = std::min(409.5, std::round(*h * 4) / 4);
    custom = Attr(p, "style:use-optimal-row-height") == "false";
  }
  const bool hidden = Attr(row, "table:visibility") == "collapse" || Attr(row, "table:visibility") == "filter";
  const std::string_view rowStyle = Attr(row, "table:default-cell-style-name");
  const int rowXf = rowStyle.empty() ? -1 : st_.Resolve(rowStyle);

  // The cells: where each starts, how often it repeats, its format, and whether it has content.
  struct Item {
    xml_node node;
    int col, count, xf;
    bool content;
    bool formatted = false;  // empty, but some of its cells are formatted unlike their column
  };
  std::vector<Item> items;
  int col = 0;
  bool content = false;
  int rowFormat = -1;  // a format filling the rest of the row
  for (xml_node c : row.children()) {
    const std::string_view n = c.name();
    if (n != "table:table-cell" && n != "table:covered-table-cell") continue;
    if (col >= kMaxCol) break;
    const int count = std::min(Repeat(c, "table:number-columns-repeated"), kMaxCol - col);
    const std::string_view style = Attr(c, "table:style-name");
    const int xf = !style.empty() ? st_.Resolve(style) : rowXf;  // -1: the column's
    const bool has = !c.attribute("office:value-type").empty() || !c.attribute("table:formula").empty() ||
                     c.child("text:p") || c.child("text:h");
    if (!has && xf >= 0 && col + count >= kMaxCol && count > 256) rowFormat = xf;
    else items.push_back({c, col, count, xf, has, false});
    content |= has;
    col += count;
  }
  const bool special = hidden || level > 0 || custom || std::abs(height - defaultHeight_) > 0.01 || rowFormat >= 0;
  // Empty cells are written only where formatted unlike their column.
  bool formatted = false;
  for (Item& it : items) {
    if (it.content || it.xf < 0) continue;
    for (int k = 0; k < it.count && !it.formatted && work_ < maxWork_; ++k, ++work_) it.formatted = it.xf != colXf_[it.col + k];
    formatted |= it.formatted;
  }
  if (!content && !special && !formatted) return;
  if (!content && repeat > 1000) {
    // A hidden block to (nearly) the end: zeroHeight. Other big empty blocks are left as they are.
    if (hidden && index + repeat >= kMaxRow - 1024 && repeat > 10000) hiddenFrom_ = index;
    if (!hidden || hiddenFrom_ == index) return;
  }
  for (int k = 0; k < repeat && cells_ < maxCells_ && work_ < maxWork_; ++k) {
    ++work_;
    const int r = index + k;
    std::string xml = "<row r=\"" + std::to_string(r + 1) + "\"";
    if (rowFormat >= 0) xml += " s=\"" + std::to_string(rowFormat) + "\" customFormat=\"1\"";
    if (std::abs(height - defaultHeight_) > 0.01 || custom) xml += " ht=\"" + Num(height) + "\"";
    if (custom) xml += " customHeight=\"1\"";
    if (hidden) xml += " hidden=\"1\"";
    if (level > 0) xml += " outlineLevel=\"" + std::to_string(std::min(level, 7)) + "\"";
    std::string cellsXml;
    for (const Item& it : items) {
      if (!it.content && !it.formatted && rowFormat < 0) continue;
      for (int j = 0; j < it.count && cells_ < maxCells_ && work_ < maxWork_; ++j) {
        ++work_;
        const int c = it.col + j;
        const int xf = it.xf >= 0 ? it.xf : colXf_[c];
        if (!it.content) {
          if (xf == colXf_[c] && rowFormat < 0) continue;
          cellsXml += "<c r=\"" + Ref(r, c) + "\" s=\"" + std::to_string(xf) + "\"/>";
          ++cells_;
          continue;
        }
        xml_node cell = it.node;
        const std::string_view type = Attr(cell, "office:value-type");
        std::string link;
        const std::string text = CellText(cell, &link);
        std::string attrs = " r=\"" + Ref(r, c) + "\"";
        if (xf) attrs += " s=\"" + std::to_string(xf) + "\"";
        std::string formula, v, t;
        if (auto f = Attr(cell, "table:formula"); !f.empty()) {
          if (TranslateFormula(f, formula, k, j, &mapNames_)) {
            const int mc = std::min(Repeat(cell, "table:number-matrix-columns-spanned"), kMaxCol - c);
            const int mr = std::min(Repeat(cell, "table:number-matrix-rows-spanned"), kMaxRow - r);
            const bool matrix = !cell.attribute("table:number-matrix-columns-spanned").empty();
            formula = matrix ? "<f t=\"array\" ref=\"" + Ref(r, c) + (mc > 1 || mr > 1 ? ":" + Ref(r + mr - 1, c + mc - 1) : "") +
                                   "\">" + Escape(xlsx::EncodeXstring(formula)) + "</f>"
                             : "<f>" + Escape(xlsx::EncodeXstring(formula)) + "</f>";
          } else {
            formula.clear();
          }
        }
        double d = 0;
        if (!formula.empty() && IsError(text) && type != "string") {
          t = "e", v = text;
        } else if (type == "float" || type == "percentage" || type == "currency") {
          if (ParseNumber(Attr(cell, "office:value"), d)) v = Num(d);
        } else if (type == "date") {
          if (ParseDate(Attr(cell, "office:date-value"), d)) {
            double serial = d - nullDate_;
            if (!date1904_ && nullDate_ == static_cast<double>(Days(1899, 12, 30)) && serial < 61 && serial >= 1) serial -= 1;
            v = Num(serial);
          }
        } else if (type == "time") {
          if (ParseDuration(Attr(cell, "office:time-value"), d)) v = Num(d);
        } else if (type == "boolean") {
          const std::string_view b = Attr(cell, "office:boolean-value");
          t = "b", v = b == "true" || b == "1" ? "1" : "0";
        } else if (formula.empty()) {
          if (!text.empty() || type == "string") t = "s", v = std::to_string(Shared(text));
        } else {
          std::string s = cell.attribute("office:string-value").empty() ? text : std::string(Attr(cell, "office:string-value"));
          if (IsError(s)) t = "e", v = s;
          else t = "str", v = s;
        }
        if (formula.empty() && v.empty()) {
          if (xf != colXf_[c]) cellsXml += "<c" + attrs + "/>";
          continue;
        }
        if (!t.empty()) attrs += " t=\"" + t + "\"";
        cellsXml += "<c" + attrs + ">" + formula;
        if (!v.empty() || t == "str") cellsXml += "<v>" + Escape(xlsx::EncodeXstring(v)) + "</v>";
        cellsXml += "</c>";
        ++cells_;
        minRow_ = std::min(minRow_, r), maxRow_ = std::max(maxRow_, r);
        minCol_ = std::min(minCol_, c), maxCol_ = std::max(maxCol_, c);
        if (j == 0) {  // each repeated row has its own merged area (one spanning rows: the first only)
          const int sc = std::min(Repeat(cell, "table:number-columns-spanned"), kMaxCol - c);
          const int sr = std::min(Repeat(cell, "table:number-rows-spanned"), kMaxRow - r);
          if ((sc > 1 || sr > 1) && (k == 0 || sr == 1)) merges_.push_back({r, c, r + sr - 1, c + sc - 1});
        }
        if (!link.empty() && links_.size() < 65530) links_.emplace_back(Ref(r, c), link);
      }
      if (!it.content) continue;
    }
    // Merged areas start on empty cells too.
    for (const Item& it : items) {
      if (it.content || merges_.size() > 1000000) continue;
      const int sc = std::min(Repeat(it.node, "table:number-columns-spanned"), kMaxCol - it.col);
      const int sr = std::min(Repeat(it.node, "table:number-rows-spanned"), kMaxRow - r);
      if ((sc > 1 || sr > 1) && (k == 0 || sr == 1) && it.node.name() == std::string_view("table:table-cell"))
        merges_.push_back({r, it.col, r + sr - 1, it.col + sc - 1});
    }
    if (cellsXml.empty() && !special) continue;
    rows.push_back({r, cellsXml.empty() ? xml + "/>" : xml + ">" + cellsXml + "</row>"});
  }
}

std::string Converter::WriteSheet(size_t index) {
  Sheet& sheet = sheets_[index];
  xml_node table = sheet.table;
  colXf_.assign(kMaxCol, 0);
  merges_.clear();
  links_.clear();
  headerRowFirst_ = headerRowLast_ = -1;
  minRow_ = kMaxRow, maxRow_ = -1, minCol_ = kMaxCol, maxCol_ = -1;
  hiddenFrom_ = -1;

  std::vector<ColRun> cols;
  int col = 0, headerFirst = -1, headerLast = -1;
  Columns(table, cols, col, 0, 0, headerFirst, headerLast, false);

  // The default row height: the height most rows have.
  std::map<double, int64_t> heights;
  std::vector<xml_node> stack = {table};
  for (size_t i = 0; i < stack.size() && i < 100000; ++i)
    for (xml_node r : stack[i].children()) {
      const std::string_view n = r.name();
      if (n == "table:table-row") {
        double h = 15;
        if (xml_node s = st_.Find("table-row", Attr(r, "table:style-name")))
          if (auto p = pkg::odf::Points(Attr(s.child("style:table-row-properties"), "style:row-height"))) h = std::round(*p * 4) / 4;
        heights[h] += Repeat(r, "table:number-rows-repeated");
      } else if (n == "table:table-row-group" || n == "table:table-header-rows" || n == "table:table-rows") {
        stack.push_back(r);
      }
    }
  defaultHeight_ = 15;
  int64_t most = -1;
  for (const auto& [h, count] : heights)
    if (count > most) most = count, defaultHeight_ = h;

  std::vector<Row> rows;
  int row = 0;
  Rows(table, index, rows, row, 0, 0, false);

  std::string out = std::string(kDecl) + "<worksheet xmlns=\"" + std::string(kMain) + "\" xmlns:r=\"" + std::string(kRelNs) + "\">";
  if (!sheet.tabColor.empty()) out += "<sheetPr><tabColor rgb=\"FF" + sheet.tabColor + "\"/></sheetPr>";
  out += "<dimension ref=\"" + (maxRow_ < 0 ? std::string("A1") : Ref(minRow_, minCol_) + ":" + Ref(maxRow_, maxCol_)) + "\"/>";
  // The view: frozen panes, gridlines, zoom, the active cell.
  View view;
  if (auto it = views_.find(sheet.odfName); it != views_.end()) view = it->second;
  out += "<sheetViews><sheetView";
  if (!view.grid) out += " showGridLines=\"0\"";
  if (view.zoom >= 10 && view.zoom <= 400 && view.zoom != 100) out += " zoomScale=\"" + std::to_string(view.zoom) + "\"";
  if ((activeSheet_.empty() ? index == 0 : sheet.odfName == activeSheet_)) out += " tabSelected=\"1\"";
  out += " workbookViewId=\"0\">";
  if (view.xSplit > 0 || view.ySplit > 0) {
    const int left = std::max(view.left, view.xSplit), top = std::max(view.top, view.ySplit);
    const char* pane = view.xSplit && view.ySplit ? "bottomRight" : view.xSplit ? "topRight" : "bottomLeft";
    out += "<pane";
    if (view.xSplit) out += " xSplit=\"" + std::to_string(view.xSplit) + "\"";
    if (view.ySplit) out += " ySplit=\"" + std::to_string(view.ySplit) + "\"";
    out += " topLeftCell=\"" + Ref(std::min(top, kMaxRow - 1), std::min(left, kMaxCol - 1)) + "\" activePane=\"" + pane +
           "\" state=\"frozen\"/>";
  }
  if (view.cursorX >= 0 && view.cursorY >= 0 && view.cursorX < kMaxCol && view.cursorY < kMaxRow) {
    const std::string a = Ref(view.cursorY, view.cursorX);
    out += "<selection activeCell=\"" + a + "\" sqref=\"" + a + "\"/>";
  }
  out += "</sheetView></sheetViews>";
  out += "<sheetFormatPr defaultRowHeight=\"" + Num(defaultHeight_) + "\"";
  if (std::abs(defaultHeight_ - 15) > 0.01) out += " customHeight=\"1\"";
  if (hiddenFrom_ >= 0) out += " zeroHeight=\"1\"";
  out += "/>";
  if (!cols.empty()) {
    out += "<cols>";
    for (const ColRun& c : cols) {
      out += "<col min=\"" + std::to_string(c.first + 1) + "\" max=\"" + std::to_string(c.last + 1) + "\"";
      out += " width=\"" + Num(c.width > 0 ? c.width : 9.140625) + "\"";
      if (c.xf) out += " style=\"" + std::to_string(c.xf) + "\"";
      if (c.hidden) out += " hidden=\"1\"";
      if (c.width > 0) out += " customWidth=\"1\"";
      if (c.level > 0) out += " outlineLevel=\"" + std::to_string(std::min(c.level, 7)) + "\"";
      out += "/>";
    }
    out += "</cols>";
  }
  out += "<sheetData>";
  // With rows hidden to the end (zeroHeight), every row above them is written, shown.
  int next = 0;
  for (const Row& r : rows) {
    if (hiddenFrom_ >= 0)
      for (; next < r.index && next < hiddenFrom_; ++next)
        out += "<row r=\"" + std::to_string(next + 1) + "\" ht=\"" + Num(defaultHeight_) + "\" customHeight=\"1\"/>";
    out += r.xml;
    next = r.index + 1;
  }
  if (hiddenFrom_ >= 0)
    for (; next < hiddenFrom_; ++next)
      out += "<row r=\"" + std::to_string(next + 1) + "\" ht=\"" + Num(defaultHeight_) + "\" customHeight=\"1\"/>";
  out += "</sheetData>";
  if (Attr(table, "table:protected") == "true") out += "<sheetProtection sheet=\"1\" objects=\"1\" scenarios=\"1\"/>";
  if (!sheet.autoFilter.empty()) out += "<autoFilter ref=\"" + sheet.autoFilter + "\"/>";
  if (!merges_.empty()) {
    // Excel repairs a file whose merged areas overlap: the first one the file has is kept.
    std::unordered_map<int, std::vector<std::pair<int, int>>> taken;  // row -> merged columns
    std::vector<std::string> kept;
    for (const Merge& m : merges_) {
      const int last = std::min(m.r2, m.r1 + 1000);  // tall areas: their first 1,000 rows are checked
      bool overlaps = false;
      for (int r = m.r1; r <= last && !overlaps; ++r)
        if (auto it = taken.find(r); it != taken.end())
          for (const auto& [c1, c2] : it->second) overlaps |= m.c1 <= c2 && c1 <= m.c2;
      if (overlaps) continue;
      for (int r = m.r1; r <= last; ++r) taken[r].emplace_back(m.c1, m.c2);
      kept.push_back(Ref(m.r1, m.c1) + ":" + Ref(m.r2, m.c2));
    }
    out += "<mergeCells count=\"" + std::to_string(kept.size()) + "\">";
    for (const std::string& m : kept) out += "<mergeCell ref=\"" + m + "\"/>";
    out += "</mergeCells>";
  }
  if (!links_.empty()) {
    out += "<hyperlinks>";
    int id = 0;
    for (const auto& [ref, target] : links_) {
      if (target.starts_with("#")) {
        std::string location;
        if (!TranslateAddress(std::string_view(target).substr(1), location, &mapNames_)) location = target.substr(1);
        out += "<hyperlink ref=\"" + ref + "\" location=\"" + Escape(location) + "\"/>";
      } else {
        out += "<hyperlink ref=\"" + ref + "\" r:id=\"rId" + std::to_string(++id) + "\"/>";
      }
    }
    out += "</hyperlinks>";
  }
  out += "<pageMargins left=\"0.7\" right=\"0.7\" top=\"0.75\" bottom=\"0.75\" header=\"0.3\" footer=\"0.3\"/></worksheet>";
  // Print titles from the header rows and columns.
  std::string titles;
  const std::string prefix = [&] {
    std::string q = "'";
    for (char ch : sheet.name) q += ch == '\'' ? std::string("''") : std::string(1, ch);
    return q + "'!";
  }();
  if (headerFirst >= 0) titles = prefix + "$" + Col(headerFirst) + ":$" + Col(headerLast);
  if (headerRowFirst_ >= 0)
    titles += (titles.empty() ? "" : ",") + prefix + "$" + std::to_string(headerRowFirst_ + 1) + ":$" + std::to_string(headerRowLast_ + 1);
  if (!titles.empty()) {
    bool dup = false;
    for (const Name& n : names_) dup |= n.sheet == static_cast<int>(index) && n.name == "_xlnm.Print_Titles";
    if (!dup) names_.push_back({"_xlnm.Print_Titles", titles, static_cast<int>(index), false});
  }
  return out;
}

pkg::Status Converter::Run(std::vector<uint8_t>& xlsx) {
  if (const std::string t = pkg::odf::MimeType(zip_);
      !t.empty() && !t.starts_with("application/vnd.oasis.opendocument.spreadsheet"))
    return pkg::Status::Fail(pkg::Status::Code::Unsupported, "This OpenDocument file isn't a spreadsheet.");
  constexpr std::string_view kNot = "This isn't an OpenDocument spreadsheet.";
  if (pkg::Status s = pkg::odf::LoadPart(zip_, "content.xml", content_, limits_, true, kNot); !s.Ok()) return s;
  if (pkg::Status s = pkg::odf::LoadPart(zip_, "styles.xml", styles_, limits_, false, kNot); !s.Ok()) return s;
  if (!pkg::odf::LoadPart(zip_, "settings.xml", settings_, limits_, false, kNot).Ok()) settings_ = pkg::XmlDom();
  xml_node book = content_.Root().child("office:body").child("office:spreadsheet");
  if (!book) return pkg::Status::Fail(pkg::Status::Code::Unsupported, "This OpenDocument file isn't a spreadsheet.");
  st_.Read(styles_.Root(), content_.Root());
  maxCells_ = std::clamp<uint64_t>(limits_.maxTotalSize / 32, 100000, 8000000);
  maxWork_ = maxCells_ * 16;
  st_.Resolve("");  // the default format is cell format 0
  ReadSettings();

  // The null date: 1899-12-30 (Excel's 1900 system), or 1904-01-01.
  nullDate_ = static_cast<double>(Days(1899, 12, 30));
  if (auto nd = Attr(book.child("table:calculation-settings").child("table:null-date"), "table:date-value"); !nd.empty()) {
    double d = 0;
    if (ParseDate(nd, d)) nullDate_ = std::floor(d);
    date1904_ = nullDate_ == static_cast<double>(Days(1904, 1, 1));
  }

  // The sheets, with names Excel accepts: at most 31 characters, none of []:*?/\, unique.
  std::set<std::string> taken;
  for (xml_node t : book.children("table:table")) {
    if (sheets_.size() >= 4000) break;
    Sheet s;
    s.table = t;
    s.odfName = Attr(t, "table:name");
    std::string name;
    for (char c : s.odfName)
      if (!std::strchr("[]:*?/\\", c)) name += c;
    while (!name.empty() && name.front() == '\'') name.erase(0, 1);
    if (name.size() > 31) {
      name.resize(31);
      while (!name.empty() && (static_cast<unsigned char>(name.back()) & 0xC0) == 0x80) name.pop_back();
      if (!name.empty() && (static_cast<unsigned char>(name.back()) & 0x80)) name.pop_back();
    }
    while (!name.empty() && name.back() == '\'') name.pop_back();
    if (name.empty()) name = "Sheet" + std::to_string(sheets_.size() + 1);
    auto lower = [](std::string v) {
      for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      return v;
    };
    for (int n = 2; taken.count(lower(name)); ++n) {
      const std::string suffix = " (" + std::to_string(n) + ")";
      name = name.substr(0, std::min<size_t>(name.size(), 31 - suffix.size())) + suffix;
    }
    taken.insert(lower(name));
    s.name = name;
    if (xml_node ts = st_.Find("table", Attr(t, "table:style-name"))) {
      xml_node p = ts.child("style:table-properties");
      s.hidden = Attr(p, "table:display") == "false";
      s.tabColor = pkg::odf::Color(Attr(p, "table:tab-color"));
      if (s.tabColor.empty()) s.tabColor = pkg::odf::Color(Attr(p, "tableooo:tab-color"));
    }
    sheetNames_[s.odfName] = s.name;
    sheets_.push_back(std::move(s));
  }
  if (sheets_.empty()) return pkg::Status::Fail(pkg::Status::Code::Damaged, "This spreadsheet has no sheets.");
  mapNames_ = [this](std::string_view n) { return SheetName(n); };
  // At least one sheet shows.
  if (std::all_of(sheets_.begin(), sheets_.end(), [](const Sheet& s) { return s.hidden; })) sheets_[0].hidden = false;

  // Names: the workbook's, then each sheet's; print ranges; filters.
  ReadNames(book, -1);
  for (size_t i = 0; i < sheets_.size(); ++i) {
    ReadNames(sheets_[i].table, static_cast<int>(i));
    if (auto pr = Attr(sheets_[i].table, "table:print-ranges"); !pr.empty()) {
      std::string all;
      size_t at = 0;
      while (at < pr.size()) {
        size_t end = pr.find(' ', at);
        if (end == std::string_view::npos) end = pr.size();
        std::string one;
        if (end > at && TranslateAddress(pr.substr(at, end - at), one, &mapNames_)) all += (all.empty() ? "" : ",") + one;
        at = end + 1;
      }
      bool dup = false;
      for (const Name& n : names_) dup |= n.sheet == static_cast<int>(i) && n.name == "_xlnm.Print_Area";
      if (!all.empty() && !dup) names_.push_back({"_xlnm.Print_Area", all, static_cast<int>(i), false});
    }
  }
  for (xml_node r : book.child("table:database-ranges").children("table:database-range")) {
    if (Attr(r, "table:display-filter-buttons") != "true") continue;
    std::string ref;
    if (!TranslateAddress(Attr(r, "table:target-range-address"), ref, &mapNames_)) continue;
    const size_t bang = ref.rfind('!');
    if (bang == std::string::npos) continue;
    std::string on = ref.substr(0, bang);
    if (on.size() > 1 && on.front() == '\'') on = on.substr(1, on.size() - 2);
    for (size_t i = 0; i < sheets_.size(); ++i) {
      if (sheets_[i].name != on || !sheets_[i].autoFilter.empty()) continue;
      sheets_[i].autoFilter = ref.substr(bang + 1);
      names_.push_back({"_xlnm._FilterDatabase", ref.substr(0, bang + 1) + Absolute(ref.substr(bang + 1)), static_cast<int>(i), true});
    }
  }

  // The package.
  pkg::MemorySink sink;
  pkg::ZipWriter zip(sink);
  auto put = [&](std::string_view name, std::string_view text) {
    return zip.Add(name, std::span(reinterpret_cast<const uint8_t*>(text.data()), text.size()));
  };
  const std::string relStart = std::string(kDecl) + "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">";
  std::string types = std::string(kDecl) +
                      "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
                      "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
                      "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
                      "<Override PartName=\"/xl/workbook.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml\"/>"
                      "<Override PartName=\"/xl/styles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml\"/>"
                      "<Override PartName=\"/xl/sharedStrings.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sharedStrings+xml\"/>";
  std::string bookRels = relStart, sheetsXml;
  int active = 0;
  for (size_t i = 0; i < sheets_.size(); ++i) {
    const std::string n = std::to_string(i + 1);
    const std::string xml = WriteSheet(i);
    if (pkg::Status s = put("xl/worksheets/sheet" + n + ".xml", xml); !s.Ok()) return s;
    std::string rels;
    int id = 0;
    for (const auto& [ref, target] : links_)
      if (!target.starts_with("#"))
        rels += "<Relationship Id=\"rId" + std::to_string(++id) + "\" Type=\"" + std::string(kRelType) +
                "hyperlink\" Target=\"" + Escape(target) + "\" TargetMode=\"External\"/>";
    if (!rels.empty()) put("xl/worksheets/_rels/sheet" + n + ".xml.rels", relStart + rels + "</Relationships>");
    types += "<Override PartName=\"/xl/worksheets/sheet" + n + ".xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>";
    bookRels += "<Relationship Id=\"rId" + n + "\" Type=\"" + std::string(kRelType) + "worksheet\" Target=\"worksheets/sheet" + n + ".xml\"/>";
    sheetsXml += "<sheet name=\"" + Escape(sheets_[i].name) + "\" sheetId=\"" + n + "\"" +
                 (sheets_[i].hidden ? " state=\"hidden\"" : "") + " r:id=\"rId" + n + "\"/>";
    if (!activeSheet_.empty() && sheets_[i].odfName == activeSheet_ && !sheets_[i].hidden) active = static_cast<int>(i);
  }
  if (sheets_[active].hidden)
    for (size_t i = 0; i < sheets_.size(); ++i)
      if (!sheets_[i].hidden) {
        active = static_cast<int>(i);
        break;
      }
  const size_t count = sheets_.size();
  bookRels += "<Relationship Id=\"rId" + std::to_string(count + 1) + "\" Type=\"" + std::string(kRelType) + "styles\" Target=\"styles.xml\"/>"
              "<Relationship Id=\"rId" + std::to_string(count + 2) + "\" Type=\"" + std::string(kRelType) +
              "sharedStrings\" Target=\"sharedStrings.xml\"/></Relationships>";
  std::string workbook = std::string(kDecl) + "<workbook xmlns=\"" + std::string(kMain) + "\" xmlns:r=\"" + std::string(kRelNs) + "\">";
  if (date1904_) workbook += "<workbookPr date1904=\"1\"/>";
  workbook += "<bookViews><workbookView" + (active ? " activeTab=\"" + std::to_string(active) + "\"" : std::string()) +
              "/></bookViews><sheets>" + sheetsXml + "</sheets>";
  if (!names_.empty()) {
    // Excel wants them sorted by name; one per name and scope.
    std::stable_sort(names_.begin(), names_.end(), [](const Name& a, const Name& b) { return a.name < b.name; });
    workbook += "<definedNames>";
    for (const Name& n : names_) {
      workbook += "<definedName name=\"" + Escape(n.name) + "\"";
      if (n.sheet >= 0) workbook += " localSheetId=\"" + std::to_string(n.sheet) + "\"";
      if (n.hidden) workbook += " hidden=\"1\"";
      workbook += ">" + Escape(xlsx::EncodeXstring(n.value)) + "</definedName>";
    }
    workbook += "</definedNames>";
  }
  workbook += "<calcPr calcId=\"191029\"/></workbook>";
  std::string sst = std::string(kDecl) + "<sst xmlns=\"" + std::string(kMain) + "\" count=\"" + std::to_string(strings_.size()) +
                    "\" uniqueCount=\"" + std::to_string(strings_.size()) + "\">";
  for (const std::string& s : strings_) {
    const bool keep = !s.empty() && (s.front() == ' ' || s.back() == ' ' || s.find('\n') != std::string::npos || s.find('\t') != std::string::npos);
    sst += keep ? "<si><t xml:space=\"preserve\">" : "<si><t>";
    sst += Escape(xlsx::EncodeXstring(s)) + "</t></si>";  // ST_Xstring: the text _x0041_ as _x005F_x0041_
  }
  sst += "</sst>";
  types += "</Types>";
  put("[Content_Types].xml", types);
  put("_rels/.rels", relStart + "<Relationship Id=\"rId1\" Type=\"" + std::string(kRelType) +
                         "officeDocument\" Target=\"xl/workbook.xml\"/></Relationships>");
  put("xl/workbook.xml", workbook);
  put("xl/_rels/workbook.xml.rels", bookRels);
  put("xl/styles.xml", st_.Xml());
  if (pkg::Status s = put("xl/sharedStrings.xml", sst); !s.Ok()) return s;
  if (pkg::Status s = zip.Finish(); !s.Ok()) return s;
  xlsx = std::move(sink.bytes);
  return {};
}

}  // namespace

pkg::Status OdsToXlsx(std::span<const uint8_t> ods, std::vector<uint8_t>& xlsx, const pkg::Limits& limits,
                      const DigitWidth& digit) {
  auto snapshot = pkg::Snapshot::FromBytes(std::vector<uint8_t>(ods.begin(), ods.end()));
  pkg::ZipReader zip;
  if (pkg::Status st = zip.Open(*snapshot, limits); !st.Ok()) return st;
  Converter c(zip, limits, digit);
  return c.Run(xlsx);
}

}  // namespace fw::sheet::ods
