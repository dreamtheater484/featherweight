// A PivotTable's style in the grid (Pivot.h: Look, LookFormat): Excel's built-in PivotTable
// styles (read from Excel 2021 by scripts/excel-pivot-styles.py) over the regions of the table as
// its part lays them out: the header rows, the row labels, subheadings, subtotals, grand totals
// and the page fields. Elements apply in the order Excel's Modify PivotTable Style lists them, the
// later winning; a cell's border on a side is the last element's that sets it there (its area's
// edge, else its inside line).
#include <algorithm>
#include <memory>
#include <mutex>

#include "pkg/Package.h"
#include "sheet/model/Workbook.h"
#include "sheet/pivot/Pivot.h"

namespace fw::sheet::pivot {

namespace {

struct BuiltIn {
  uint8_t style, element, part, value;
  float tint;
};

constexpr BuiltIn kBuiltIn[] = {
#include "sheet/pivot/PivotStylesData.inc"
};

// Excel's XlTableStyleElementType for PivotTables.
enum El : uint8_t {
  kWhole = 0, kHeaderRow = 1, kGrandRow = 2, kFirstColumn = 3, kGrandColumn = 4, kRowStripe1 = 5, kRowStripe2 = 6,
  kColStripe1 = 7, kColStripe2 = 8, kFirstHeaderCell = 9, kSubtotalCol1 = 13, kSubtotalRow1 = 16, kBlankRow = 19,
  kColSubheading1 = 20, kRowSubheading1 = 23, kPageLabels = 26, kPageValues = 27, kElements = 28
};

// Light1-28, Medium1-28, Dark1-28 -> 0-83; -1 for another name.
int StyleIndex(std::string_view name) {
  constexpr std::string_view kPrefix = "PivotStyle";
  if (!name.starts_with(kPrefix)) return -1;
  name.remove_prefix(kPrefix.size());
  static constexpr std::pair<std::string_view, int> kFamilies[] = {{"Light", 0}, {"Medium", 28}, {"Dark", 56}};
  for (const auto& [family, base] : kFamilies) {
    if (!name.starts_with(family)) continue;
    std::string_view n = name.substr(family.size());
    if (n.empty() || n.size() > 2 || n[0] == '0') return -1;
    int v = 0;
    for (char c : n) {
      if (c < '0' || c > '9') return -1;
      v = v * 10 + (c - '0');
    }
    return v >= 1 && v <= 28 ? base + v - 1 : -1;
  }
  return -1;
}

using Elements = std::array<TableStyleElement, kElements>;

const Elements* BuiltInStyle(std::string_view name) {
  const int index = StyleIndex(name);
  if (index < 0) return nullptr;
  static std::mutex mutex;
  static std::unique_ptr<Elements> made[84];
  std::lock_guard<std::mutex> lock(mutex);
  if (!made[index]) {
    made[index] = std::make_unique<Elements>();
    for (const BuiltIn& b : kBuiltIn) {
      if (b.style != index || b.element >= kElements) continue;
      TableStyleElement& e = (*made[index])[b.element];
      e.set = true;
      const Color c{Color::Kind::Theme, b.value, b.tint};
      const int part = b.part & 15;
      if (part == 0) e.fill.pattern = PatternType::Solid, e.fill.fg = c;
      else if (part == 1) e.fontColor = c;
      else if (part == 2) e.bold = true;
      else if (part == 3) e.italic = true;
      else if (part == 4) e.stripeSize = b.value;
      else if (part >= 8 && part < 14) {
        BorderSide* sides[] = {&e.left, &e.top, &e.bottom, &e.right, &e.vertical, &e.horizontal};
        *sides[part - 8] = {static_cast<BorderStyle>(b.part >> 4), c};
      }
    }
  }
  return made[index].get();
}

// Excel's order (Modify PivotTable Style), the later winning.
constexpr uint8_t kOrder[] = {kWhole, kPageLabels, kPageValues, kColStripe1, kColStripe2, kRowStripe1, kRowStripe2,
                              kFirstColumn, kHeaderRow, kFirstHeaderCell, kSubtotalCol1, kSubtotalCol1 + 1,
                              kSubtotalCol1 + 2, kSubtotalRow1, kSubtotalRow1 + 1, kSubtotalRow1 + 2, kBlankRow,
                              kColSubheading1, kColSubheading1 + 1, kColSubheading1 + 2, kRowSubheading1,
                              kRowSubheading1 + 1, kRowSubheading1 + 2, kGrandColumn, kGrandRow};

}  // namespace

bool ReadLook(std::string_view xml, Look& out) {
  Table t;
  if (!ReadTable(xml, t)) return false;
  Look l;
  l.location = t.location;
  l.pages = static_cast<uint32_t>(t.pageFields.size());
  l.area = AreaOf(t.location, l.pages);
  l.firstHeaderRow = t.firstHeaderRow;
  l.firstDataRow = t.firstDataRow;
  l.firstDataCol = t.firstDataCol;
  l.style = t.style;
  l.rowHeaders = t.showRowHeaders, l.colHeaders = t.showColHeaders;
  l.rowStripes = t.showRowStripes, l.colStripes = t.showColStripes;
  auto kinds = [&](const std::vector<Line>& lines, const std::vector<int32_t>& fields, bool rows, std::vector<uint8_t>& out) {
    const size_t n = fields.size();
    for (const Line& line : lines) {
      const uint32_t depth = line.repeat + static_cast<uint32_t>(line.members.size());
      uint8_t k = Look::kData;
      if (line.type == FieldItem::Type::Grand) {
        k = Look::kGrand;
      } else if (line.type != FieldItem::Type::Data) {
        k = static_cast<uint8_t>(Look::kSubtotal | std::min<uint32_t>(2, depth ? depth - 1 : 0));
      } else if (rows && depth < n && depth > 0) {
        // A compact or outline line of its own: a subheading (its subtotals at the top, if any,
        // don't make it a subtotal row: Excel styles it as the subheading).
        k = static_cast<uint8_t>(Look::kHeading | std::min<uint32_t>(2, depth - 1));
      }
      out.push_back(k);
    }
  };
  kinds(t.rowItems, t.rowFields, true, l.rowKind);
  kinds(t.colItems, t.colFields, false, l.colKind);
  l.colLevels = static_cast<uint32_t>(t.colFields.size());
  for (size_t k = 0; k < t.rowFields.size(); ++k) {
    uint32_t c = 0;
    if (k > 0) {
      const int32_t f = t.rowFields[k - 1];
      const bool compact = f == -2 ? t.compactData && t.outlineData
                                   : f >= 0 && static_cast<size_t>(f) < t.fields.size() && t.fields[f].compact && t.fields[f].outline;
      c = l.levelCol.back() + (compact ? 0 : 1);
    }
    l.levelCol.push_back(c);
    const int32_t f = t.rowFields[k];
    l.levelTabular.push_back(f >= 0 && static_cast<size_t>(f) < t.fields.size() && !t.fields[f].outline);
  }
  out = std::move(l);
  return true;
}

std::vector<Look> SheetLooks(const Workbook& book, size_t sheet) {
  std::vector<Look> out;
  const pkg::Package* package = book.GetPackage();
  if (!package) return out;
  for (const Place& p : List(book)) {
    if (p.sheet != sheet) continue;
    std::vector<uint8_t> bytes;
    Look l;
    if (package->ReadPart(p.table, bytes).Ok() &&
        ReadLook(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), l))
      out.push_back(std::move(l));
  }
  return out;
}

TableCellFormat LookFormat(const Look& l, uint32_t row, uint32_t col) {
  TableCellFormat f;
  if (!l.area.Contains(row, col)) return f;
  const Elements* style = BuiltInStyle(l.style);
  if (!style) return f;
  struct Hit {
    uint8_t element;
    Range area;
  };
  Hit hits[kElements];
  int count = 0;
  auto add = [&](uint8_t e, Range area) {
    if ((*style)[e].set && area.Contains(row, col)) hits[count++] = {e, area};
  };
  const Range& loc = l.location;
  const uint32_t top = loc.firstRow, left = loc.firstCol;
  const uint32_t dataTop = top + l.firstDataRow, dataLeft = left + l.firstDataCol;
  // Page fields: their labels and values, above the table.
  if (row < top) {
    if (row >= l.area.firstRow && row + 1 < top) {
      if (col == left) add(kPageLabels, Range::Cell(row, col));
      if (col == left + 1) add(kPageValues, Range::Cell(row, col));
    }
  }
  const int line = row >= dataTop ? static_cast<int>(row - dataTop) : -1;
  const int column = col >= dataLeft ? static_cast<int>(col - dataLeft) : -1;
  const uint8_t rowKind = line >= 0 && static_cast<size_t>(line) < l.rowKind.size() ? l.rowKind[line] : Look::kData;
  const uint8_t colKind = column >= 0 && static_cast<size_t>(column) < l.colKind.size() ? l.colKind[column] : Look::kData;
  if (loc.Contains(row, col)) {
    const Range rowBand{row, loc.firstCol, row, loc.lastCol}, colBand{loc.firstRow, col, loc.lastRow, col};
    for (uint8_t e : kOrder) {
      switch (e) {
        case kWhole: add(e, loc); break;
        case kRowStripe1:
        case kRowStripe2:
          if (l.rowStripes && line >= 0 && (line % 2 == 0) == (e == kRowStripe1)) add(e, rowBand);
          break;
        case kColStripe1:
        case kColStripe2:
          if (l.colStripes && column >= 0 && (column % 2 == 0) == (e == kColStripe1)) add(e, colBand);
          break;
        case kFirstColumn:  // the row labels' columns, the header row's cells too (Light9's line)
          if (l.rowHeaders && l.firstDataCol) add(e, {top, left, loc.lastRow, dataLeft - 1});
          break;
        case kHeaderRow:
          if (l.colHeaders && l.firstDataRow) add(e, {top, left, dataTop - 1, loc.lastCol});
          break;
        case kFirstHeaderCell:
          if (l.firstDataRow && l.firstDataCol) add(e, {top, left, top, dataLeft - 1});
          break;
        case kGrandColumn:
          if (colKind == Look::kGrand) add(e, colBand);
          break;
        case kGrandRow:
          if (rowKind == Look::kGrand) add(e, rowBand);
          break;
        default:
          if (e >= kSubtotalCol1 && e < kSubtotalCol1 + 3) {
            if ((colKind & Look::kSubtotal) && colKind != Look::kGrand && (colKind & 3) == e - kSubtotalCol1) add(e, colBand);
          } else if (e >= kSubtotalRow1 && e < kSubtotalRow1 + 3) {
            if ((rowKind & Look::kSubtotal) && rowKind != Look::kGrand && (rowKind & 3) == e - kSubtotalRow1) add(e, rowBand);
          } else if (e >= kRowSubheading1 && e < kRowSubheading1 + 3) {
            const uint32_t level = e - kRowSubheading1;
            if ((rowKind & Look::kHeading) && static_cast<uint32_t>(rowKind & 3) == level) add(e, rowBand);
            // A tabular or outline outer field's column, down its items' lines (not their totals).
            const size_t n = l.levelCol.size();
            if (line >= 0 && !(rowKind & (Look::kSubtotal | Look::kGrand)) && n > 1)
              for (size_t k = level; k + 1 < n && k <= level; ++k)
                if (l.levelTabular[k] && l.levelCol[k] != l.levelCol[n - 1] && col == left + l.levelCol[k] && (k < 2 || level == 2))
                  add(e, Range::Cell(row, col));
          } else if (e >= kColSubheading1 && e < kColSubheading1 + 3) {
            // A column field's labels, but the innermost's.
            const uint32_t level = e - kColSubheading1;
            if (l.colLevels > level + 1 && row == top + l.firstHeaderRow + level && col >= dataLeft) add(e, Range::Cell(row, col));
          }
          break;
      }
    }
  }
  bool fill = false, font = false, bold = false, italic = false;
  const BorderSide* sides[4] = {};
  for (int i = count - 1; i >= 0; --i) {
    const TableStyleElement& e = (*style)[hits[i].element];
    const Range& a = hits[i].area;
    // The whole table's lines don't show under another element's fill (Medium15's header and
    // grand total rows, as Excel draws them).
    const bool wholeHidden = hits[i].element == kWhole && fill;
    if (!fill &&(e.fill.pattern != PatternType::None || e.fill.gradient)) f.fill = e.fill, fill = true;
    if (!font && e.fontColor.IsSet()) f.fontColor = e.fontColor, font = true;
    if (!bold && e.bold) f.bold = *e.bold, bold = true;
    if (!italic && e.italic) f.italic = *e.italic, italic = true;
    const BorderSide* mine[4] = {a.firstCol == col ? &e.left : &e.vertical, a.firstRow == row ? &e.top : &e.horizontal,
                                 a.lastRow == row ? &e.bottom : &e.horizontal, a.lastCol == col ? &e.right : &e.vertical};
    for (int s = 0; s < 4 && !wholeHidden; ++s)
      if (!sides[s] && mine[s]->style != BorderStyle::None) sides[s] = mine[s];
  }
  if (sides[0]) f.left = *sides[0];
  if (sides[1]) f.top = *sides[1];
  if (sides[2]) f.bottom = *sides[2];
  if (sides[3]) f.right = *sides[3];
  return f;
}

TableCellFormat LookFormatAt(const std::vector<Look>& looks, uint32_t row, uint32_t col) {
  for (const Look& l : looks)
    if (l.area.Contains(row, col)) return LookFormat(l, row, col);
  return {};
}

}  // namespace fw::sheet::pivot
