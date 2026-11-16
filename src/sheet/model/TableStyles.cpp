#include "sheet/model/TableStyles.h"

#include <algorithm>
#include <memory>
#include <mutex>

#include "sheet/model/Sheet.h"

namespace fw::sheet {

namespace {

struct BuiltIn {
  uint8_t style, element, part, value;
  float tint;
};

constexpr BuiltIn kBuiltIn[] = {
#include "sheet/model/TableStylesData.inc"
};

constexpr int kBuiltInCount = 60;
constexpr int kElements = static_cast<int>(TableElement::Count);

// TableStyleLight1-21, Medium1-28, Dark1-11 -> 0-59; -1 for any other name.
int BuiltInIndex(std::string_view name) {
  constexpr std::string_view kPrefix = "TableStyle";
  if (!name.starts_with(kPrefix)) return -1;
  name.remove_prefix(kPrefix.size());
  struct Family {
    std::string_view name;
    int base, count;
  };
  static constexpr Family kFamilies[] = {{"Light", 0, 21}, {"Medium", 21, 28}, {"Dark", 49, 11}};
  for (const Family& f : kFamilies) {
    if (!name.starts_with(f.name)) continue;
    std::string_view n = name.substr(f.name.size());
    if (n.empty() || n.size() > 2 || n[0] == '0') return -1;
    int v = 0;
    for (char c : n) {
      if (c < '0' || c > '9') return -1;
      v = v * 10 + (c - '0');
    }
    return v <= f.count ? f.base + v - 1 : -1;
  }
  return -1;
}

std::unique_ptr<TableStyle> MakeBuiltIn(int index, std::string_view name) {
  auto s = std::make_unique<TableStyle>();
  s->name = name;
  for (const BuiltIn& b : kBuiltIn) {
    if (b.style != index) continue;
    TableStyleElement& e = s->elements[b.element];
    e.set = true;
    const Color c{Color::Kind::Theme, b.value, b.tint};
    const int part = b.part & 15;
    if (part == 0) {
      e.fill.pattern = PatternType::Solid;
      e.fill.fg = c;
    } else if (part == 1) {
      e.fontColor = c;
    } else if (part == 2) {
      e.bold = true;
    } else if (part == 3) {
      e.italic = true;
    } else if (part == 4) {
      e.stripeSize = b.value;
    } else if (part >= 8 && part < 14) {
      BorderSide* sides[] = {&e.left, &e.top, &e.bottom, &e.right, &e.vertical, &e.horizontal};
      *sides[part - 8] = {static_cast<BorderStyle>(b.part >> 4), c};
    }
  }
  return s;
}

// Border lines. Excel decides the line between two cells by the most important of these groups
// that sets it (whole table < column stripes < row stripes < last column < first column < header
// row < total row; the corner cells go with their row). Within a group, the lower or right cell's
// element wins; within one cell, its later element. (Measured in Excel 2021 with custom styles
// that set every border of every element.)
constexpr uint8_t kGroup[kElements] = {0, 1, 1, 2, 2, 3, 4, 5, 6, 5, 5, 6, 6};
constexpr int kGroups = 7;

struct Hit {
  TableElement element;
  Range area;  // the element's area the cell is in: one band for a stripe
};

// The elements the style sets that cover a cell, in the order Excel applies them.
struct Cover {
  Hit hits[kElements];
  int count = 0;
};

uint32_t StripeSize(const TableStyle& s, TableElement e) { return std::clamp(s[e].stripeSize, 1u, kMaxRows); }

void Covering(const TableStyle& s, const Table& t, uint32_t r, uint32_t c, Cover& out) {
  out.count = 0;
  const Range& ref = t.ref;
  if (!ref.Contains(r, c)) return;
  auto add = [&](TableElement e, Range area) {
    if (s[e].set) out.hits[out.count++] = {e, area};
  };
  add(TableElement::WholeTable, ref);
  // Stripes band the data rows only, counted from the first data row and the first column.
  const uint32_t dataFirst = ref.firstRow + t.headerRows;
  const int64_t dataLast = static_cast<int64_t>(ref.lastRow) - t.totalsRows;
  if (r >= dataFirst && static_cast<int64_t>(r) <= dataLast) {
    const uint32_t last = static_cast<uint32_t>(dataLast);
    if (t.style.columnStripes) {
      const uint32_t a = StripeSize(s, TableElement::FirstColumnStripe), b = StripeSize(s, TableElement::SecondColumnStripe);
      const uint64_t k = (c - ref.firstCol) % (uint64_t{a} + b);
      const bool first = k < a;
      const uint32_t start = c - static_cast<uint32_t>(first ? k : k - a);
      const uint32_t end = static_cast<uint32_t>(std::min<uint64_t>(uint64_t{start} + (first ? a : b) - 1, ref.lastCol));
      add(first ? TableElement::FirstColumnStripe : TableElement::SecondColumnStripe, {dataFirst, start, last, end});
    }
    if (t.style.rowStripes) {
      const uint32_t a = StripeSize(s, TableElement::FirstRowStripe), b = StripeSize(s, TableElement::SecondRowStripe);
      const uint64_t k = (r - dataFirst) % (uint64_t{a} + b);
      const bool first = k < a;
      const uint32_t start = r - static_cast<uint32_t>(first ? k : k - a);
      const uint32_t end = static_cast<uint32_t>(std::min<uint64_t>(uint64_t{start} + (first ? a : b) - 1, last));
      add(first ? TableElement::FirstRowStripe : TableElement::SecondRowStripe, {start, ref.firstCol, end, ref.lastCol});
    }
  }
  if (t.style.lastColumn && c == ref.lastCol) add(TableElement::LastColumn, {ref.firstRow, c, ref.lastRow, c});
  if (t.style.firstColumn && c == ref.firstCol) add(TableElement::FirstColumn, {ref.firstRow, c, ref.lastRow, c});
  const bool header = t.headerRows > 0 && r == ref.firstRow, total = t.totalsRows > 0 && r == ref.lastRow;
  if (header) add(TableElement::HeaderRow, {r, ref.firstCol, r, ref.lastCol});
  if (total) add(TableElement::TotalRow, {r, ref.firstCol, r, ref.lastCol});
  // The corner cells need their column shown too.
  if (header && t.style.firstColumn && c == ref.firstCol) add(TableElement::FirstHeaderCell, Range::Cell(r, c));
  if (header && t.style.lastColumn && c == ref.lastCol) add(TableElement::LastHeaderCell, Range::Cell(r, c));
  if (total && t.style.firstColumn && c == ref.firstCol) add(TableElement::FirstTotalCell, Range::Cell(r, c));
  if (total && t.style.lastColumn && c == ref.lastCol) add(TableElement::LastTotalCell, Range::Cell(r, c));
}

enum Side { kLeft, kTop, kBottom, kRight };

// The border a cell's elements of one group give one of its sides: an element's edge where the
// side is on its area's edge, else its inside line.
const BorderSide* SideOf(const TableStyle& s, const Cover& cover, int group, Side side, uint32_t r, uint32_t c) {
  for (int i = cover.count - 1; i >= 0; --i) {
    const Hit& h = cover.hits[i];
    if (kGroup[static_cast<int>(h.element)] != group) continue;
    const TableStyleElement& e = s[h.element];
    const BorderSide* b = nullptr;
    switch (side) {
      case kLeft: b = h.area.firstCol == c ? &e.left : &e.vertical; break;
      case kTop: b = h.area.firstRow == r ? &e.top : &e.horizontal; break;
      case kBottom: b = h.area.lastRow == r ? &e.bottom : &e.horizontal; break;
      case kRight: b = h.area.lastCol == c ? &e.right : &e.vertical; break;
    }
    if (b->style != BorderStyle::None) return b;
  }
  return nullptr;
}

// The line between an earlier cell (above or to the left) and a later one; a cell outside the
// table has an empty cover.
BorderSide Line(const TableStyle& s, const Cover& earlier, Side earlierSide, uint32_t er, uint32_t ec,
                const Cover& later, Side laterSide, uint32_t lr, uint32_t lc) {
  for (int g = kGroups - 1; g >= 0; --g) {
    if (const BorderSide* b = SideOf(s, later, g, laterSide, lr, lc)) return *b;
    if (const BorderSide* b = SideOf(s, earlier, g, earlierSide, er, ec)) return *b;
  }
  return {};
}

}  // namespace

bool TableCellFormat::Empty() const {
  return fill.pattern == PatternType::None && !fill.gradient && !fontColor.IsSet() && !bold && !italic &&
         left.style == BorderStyle::None && top.style == BorderStyle::None && bottom.style == BorderStyle::None &&
         right.style == BorderStyle::None;
}

const TableStyle* BuiltInTableStyle(std::string_view name) {
  const int i = BuiltInIndex(name);
  if (i < 0) return nullptr;
  static std::mutex mutex;
  static std::unique_ptr<TableStyle> made[kBuiltInCount];
  std::lock_guard<std::mutex> lock(mutex);
  if (!made[i]) made[i] = MakeBuiltIn(i, name);
  return made[i].get();
}

const TableStyle* FindTableStyle(const Styles& styles, std::string_view name) {
  if (name.empty()) return nullptr;
  for (const TableStyle& s : styles.TableStyles())
    if (s.name == name) return &s;
  return BuiltInTableStyle(name);
}

TableCellFormat TableFormat(const TableStyle& s, const Table& t, uint32_t row, uint32_t col) {
  TableCellFormat f;
  if (!t.ref.Contains(row, col)) return f;
  Cover cell, other;
  Covering(s, t, row, col, cell);  // may be empty: a neighbor's elements can still draw its lines
  bool fill = false, font = false, bold = false, italic = false;
  for (int i = cell.count - 1; i >= 0; --i) {
    const TableStyleElement& e = s[cell.hits[i].element];
    if (!fill && (e.fill.pattern != PatternType::None || e.fill.gradient)) f.fill = e.fill, fill = true;
    if (!font && e.fontColor.IsSet()) f.fontColor = e.fontColor, font = true;
    if (!bold && e.bold) f.bold = *e.bold, bold = true;
    if (!italic && e.italic) f.italic = *e.italic, italic = true;
  }
  auto across = [&](int64_t r, int64_t c) -> const Cover& {
    other.count = 0;
    if (r >= 0 && c >= 0) Covering(s, t, static_cast<uint32_t>(r), static_cast<uint32_t>(c), other);
    return other;
  };
  f.top = Line(s, across(int64_t{row} - 1, col), kBottom, row - 1, col, cell, kTop, row, col);
  f.bottom = Line(s, cell, kBottom, row, col, across(int64_t{row} + 1, col), kTop, row + 1, col);
  f.left = Line(s, across(row, int64_t{col} - 1), kRight, row, col - 1, cell, kLeft, row, col);
  f.right = Line(s, cell, kRight, row, col, across(row, int64_t{col} + 1), kLeft, row, col + 1);
  return f;
}

TableCellFormat TableFormat(const Styles& styles, const Table& table, uint32_t row, uint32_t col) {
  const TableStyle* s = FindTableStyle(styles, table.style.name);
  return s ? TableFormat(*s, table, row, col) : TableCellFormat{};
}

TableCellFormat TableFormatAt(const Styles& styles, const Sheet& sheet, uint32_t row, uint32_t col) {
  const Table* t = sheet.TableAt(row, col);
  return t ? TableFormat(styles, *t, row, col) : TableCellFormat{};
}

TableStyleElement TableElementFromDxf(const Dxf& d, uint32_t stripeSize) {
  TableStyleElement e;
  e.set = true;
  e.stripeSize = std::max(stripeSize, 1u);
  if (d.font) {
    e.fontColor = d.font->color;
    if (d.fontBold) e.bold = d.font->bold;
    if (d.fontItalic) e.italic = d.font->italic;
  }
  if (d.fill) {
    e.fill = *d.fill;
    // A differential solid fill is in its background color; Fill's is fg.
    if (e.fill.pattern == PatternType::Solid && e.fill.bg.IsSet()) e.fill.fg = e.fill.bg;
  }
  if (d.border) {
    e.left = d.border->left, e.top = d.border->top, e.bottom = d.border->bottom, e.right = d.border->right;
    e.vertical = d.border->vertical, e.horizontal = d.border->horizontal;
  }
  return e;
}

}  // namespace fw::sheet
