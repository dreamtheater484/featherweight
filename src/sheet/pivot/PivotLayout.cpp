// Refreshing a pivot cache from its source cells, and laying a pivot table out as Excel does
// (Pivot.h). Excel's rules, measured on Excel 2021 (tests/sheet/excel_pivots.py):
//  - a refreshed cache lists a field's items in the order they first appear in the source, then the
//    items no longer there, kept as unused; a pivot field keeps its order, new items go last (a
//    sorted field is sorted again);
//  - rows: a compact or outline field has a line of its own above its children (with the subtotals
//    when they're at the top), a tabular one puts its label on its first child's line and adds an
//    "<item> Total" line after them; columns are always laid out like tabular ones; no subtotal
//    lines when only the data fields are deeper;
//  - the header rows: a caption row (the data field's name in the corner, "Column Labels" or the
//    column fields' names), one row per column field, the row labels' captions on the last one.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>

#include "sheet/model/Workbook.h"
#include "sheet/numfmt/NumFormat.h"
#include "sheet/pivot/Pivot.h"
#include "sheet/pivot/PivotXml.h"

namespace fw::sheet::pivot {

namespace {

constexpr int32_t kData = -2;  // the data fields' place on an axis

// ---- Reading the source

bool SourceRange(Workbook& book, const Cache& cache, size_t& sheet, Range& r, std::u16string& why) {
  why = u"The reference to the PivotTable's data source isn't valid.";
  if (!cache.name.empty()) {
    if (const sheet::Table* t = book.FindTable(cache.name, &sheet)) {
      r = t->ref;
      if (t->totalsRows && r.lastRow > r.firstRow) r.lastRow -= t->totalsRows;
      return true;
    }
    const DefinedName* n = book.FindName(cache.name);
    if (!n) return false;
    std::string_view f = n->formula;
    size_t bang = f.rfind('!');
    if (bang == std::string_view::npos) return false;
    std::string name(f.substr(0, bang));
    if (name.size() >= 2 && name.front() == '\'' && name.back() == '\'') {
      name = name.substr(1, name.size() - 2);
      for (size_t at = 0; (at = name.find("''", at)) != std::string::npos; ++at) name.erase(at, 1);
    }
    int s = book.FindSheet(name);
    if (s < 0 || !ParseRange(f.substr(bang + 1), r)) return false;
    sheet = static_cast<size_t>(s);
    return true;
  }
  int s = book.FindSheet(cache.sheet);
  if (s < 0 || !ParseRange(cache.ref, r)) return false;
  sheet = static_cast<size_t>(s);
  return true;
}

bool IsDateFormat(const Styles& styles, uint32_t style) {
  if (style >= styles.CellXfs().size()) return false;
  uint32_t id = styles.CellXfs()[style].numFmt;
  if ((id >= 14 && id <= 22) || (id >= 45 && id <= 47)) return true;
  if (id < 164) return false;
  std::string_view code = styles.FormatCode(id);
  std::wstring w(code.begin(), code.end());
  return NumberFormat(w).IsDateTime();
}

std::string ShortNumber(double v) {
  char buf[40];
  for (int p = 15; p <= 17; ++p) {
    std::snprintf(buf, sizeof buf, "%.*g", p, v);
    if (std::strtod(buf, nullptr) == v) break;
  }
  return buf;
}

// A header cell's text, as Excel names the field.
std::string HeaderText(const Value& v) {
  switch (v.type) {
    case ValueType::String: return std::string(v.text);
    case ValueType::Number: return ShortNumber(v.number);
    case ValueType::Bool: return v.boolean ? "TRUE" : "FALSE";
    case ValueType::Error: return std::string(ErrorText(v.error));
    default: return {};
  }
}

// Excel's order for a sorted field: numbers and dates, text without case, FALSE before TRUE,
// errors, and blanks last.
int Rank(const Item& a) {
  switch (a.kind) {
    case Item::Kind::Number:
    case Item::Kind::Date: return 0;
    case Item::Kind::String: return 1;
    case Item::Kind::Bool: return 2;
    case Item::Kind::Error: return 3;
    default: return 4;
  }
}

int CompareText(std::string_view a, std::string_view b) {
  size_t n = std::min(a.size(), b.size());
  for (size_t i = 0; i < n; ++i) {
    unsigned char x = static_cast<unsigned char>(a[i]), y = static_cast<unsigned char>(b[i]);
    if (x < 128) x = static_cast<unsigned char>(std::tolower(x));
    if (y < 128) y = static_cast<unsigned char>(std::tolower(y));
    if (x != y) return x < y ? -1 : 1;
  }
  return a.size() == b.size() ? 0 : a.size() < b.size() ? -1 : 1;
}

bool ItemLess(const Item& a, const Item& b) {
  int ra = Rank(a), rb = Rank(b);
  if (ra != rb) return ra < rb;
  if (ra == 1 || ra == 3) return CompareText(a.text, b.text) < 0;
  return a.number < b.number;
}

}  // namespace

bool RefreshCache(Workbook& book, Cache& cache, const std::vector<bool>& axisFields,
                  std::vector<std::vector<uint32_t>>& remap, std::u16string& why) {
  if (!cache.why.empty()) {
    why = u"This PivotTable uses features Featherweight keeps but doesn't refresh.";
    return false;
  }
  size_t sheet = 0;
  Range r;
  if (!SourceRange(book, cache, sheet, r, why)) return false;
  if (!book.LoadSheet(sheet).Ok()) return false;
  const Sheet& s = book.GetSheet(sheet);
  if (r.lastRow > r.firstRow + kMaxRows || r.lastCol - r.firstCol >= 16384) return false;
  const size_t n = r.lastCol - r.firstCol + 1;
  // The fields by their header text: the same names the cache has, in any order.
  std::vector<size_t> column(cache.fields.size(), SIZE_MAX);
  for (size_t c = 0; c < n; ++c) {
    std::string name = HeaderText(s.GetValue(r.firstRow, r.firstCol + static_cast<uint32_t>(c)));
    if (name.empty()) {
      why = u"The PivotTable field name is not valid. To create a PivotTable, you must use data that is organized as a "
            u"list with labeled columns.";
      return false;
    }
    for (size_t f = 0; f < cache.fields.size(); ++f)
      if (column[f] == SIZE_MAX && detail::TextEqual(cache.fields[f].name, name)) {
        column[f] = c;
        break;
      }
  }
  for (size_t f = 0; f < cache.fields.size(); ++f)
    if (column[f] == SIZE_MAX || cache.fields[f].kept) {
      why = u"The PivotTable's source no longer has the column “" + std::u16string(cache.fields[f].name.begin(), cache.fields[f].name.end()) +
            u"”. Featherweight refreshes PivotTables whose source keeps its columns.";
      if (cache.fields[f].kept) why = u"This PivotTable uses features Featherweight keeps but doesn't refresh.";
      return false;
    }
  const Styles& styles = book.GetStyles();
  const size_t records = r.lastRow - r.firstRow;
  const size_t nf = cache.fields.size();
  std::vector<uint32_t> out(records * nf);
  remap.assign(nf, {});
  std::vector<std::map<double, uint32_t>> numbers(nf);  // a field's number items, for speed
  for (size_t f = 0; f < nf; ++f) {
    CacheField& cf = cache.fields[f];
    const bool items = cf.hasItems || (f < axisFields.size() && axisFields[f]);
    std::vector<Item> old = std::move(cf.items);
    std::vector<Item> fresh;
    std::vector<uint32_t> idx(records);
    const uint32_t col = r.firstCol + static_cast<uint32_t>(column[f]);
    for (size_t i = 0; i < records; ++i) {
      CellInfo info;
      Item it;
      if (s.GetCell(r.firstRow + 1 + static_cast<uint32_t>(i), col, info)) {
        const Value& v = info.value;
        switch (v.type) {
          case ValueType::Number:
            it.kind = IsDateFormat(styles, info.style) ? Item::Kind::Date : Item::Kind::Number;
            it.number = v.number;
            break;
          case ValueType::String:
            it.kind = Item::Kind::String;
            it.text = std::string(v.text);
            if (it.text.empty()) it.kind = Item::Kind::Missing;
            break;
          case ValueType::Bool: it.kind = Item::Kind::Bool, it.number = v.boolean ? 1 : 0; break;
          case ValueType::Error: it.kind = Item::Kind::Error, it.text = std::string(ErrorText(v.error)); break;
          default: break;
        }
      }
      uint32_t k = 0;
      if (it.kind == Item::Kind::Number || it.kind == Item::Kind::Date) {
        auto found = numbers[f].find(it.number);
        if (found != numbers[f].end() && fresh[found->second].kind == it.kind) {
          k = found->second;
        } else {
          k = static_cast<uint32_t>(fresh.size());
          numbers[f].emplace(it.number, k);
          fresh.push_back(std::move(it));
        }
      } else {
        k = 0;
        while (k < fresh.size() && !(fresh[k] == it)) ++k;
        if (k == fresh.size()) fresh.push_back(std::move(it));
      }
      idx[i] = k;
    }
    // Old items: where they are now; those gone are kept, unused, after the others (shared items only).
    std::vector<uint32_t>& map = remap[f];
    map.assign(old.size(), kNone);
    if (items && cf.hasItems)
      for (size_t o = 0; o < old.size(); ++o) {
        size_t k = 0;
        if (old[o].kind == Item::Kind::Number || old[o].kind == Item::Kind::Date) {
          auto found = numbers[f].find(old[o].number);
          k = found != numbers[f].end() ? found->second : fresh.size();
        } else {
          while (k < fresh.size() && !(fresh[k] == old[o])) ++k;
        }
        if (k == fresh.size()) {
          Item gone = old[o];
          gone.unused = true;
          fresh.push_back(std::move(gone));
        }
        map[o] = static_cast<uint32_t>(k);
      }
    for (size_t i = 0; i < records; ++i) out[i * nf + f] = idx[i];
    cf.items = std::move(fresh);
    cf.hasItems = items;
  }
  cache.records = std::move(out);
  return true;
}

void RemapItems(Table& table, const Cache& cache, const std::vector<std::vector<uint32_t>>& remap) {
  for (size_t f = 0; f < table.fields.size() && f < cache.fields.size(); ++f) {
    Field& fd = table.fields[f];
    const CacheField& cf = cache.fields[f];
    const bool onAxis = fd.axis == Axis::Row || fd.axis == Axis::Col || fd.axis == Axis::Page;
    if (!cf.hasItems || (!fd.hasItems && !onAxis)) continue;
    // The page field's chosen item, by its shared item.
    int64_t pageX = -1;
    PageField* page = nullptr;
    for (PageField& p : table.pageFields)
      if (p.field == f) page = &p;
    if (page && page->item >= 0 && static_cast<size_t>(page->item) < fd.items.size()) pageX = fd.items[page->item].x;
    std::vector<FieldItem> data, totals;
    std::vector<bool> listed(cf.items.size(), false);
    for (FieldItem& it : fd.items) {
      if (it.type != FieldItem::Type::Data) {
        totals.push_back(std::move(it));
        continue;
      }
      if (f < remap.size() && it.x < remap[f].size() && remap[f][it.x] != kNone) it.x = remap[f][it.x];
      if (it.x >= cf.items.size() || listed[it.x]) continue;
      listed[it.x] = true;
      it.missing = cf.items[it.x].unused;
      data.push_back(std::move(it));
    }
    if (page && pageX >= 0 && f < remap.size() && static_cast<size_t>(pageX) < remap[f].size()) pageX = remap[f][pageX];
    const bool fresh = !fd.hasItems;
    for (uint32_t x = 0; x < cf.items.size(); ++x)
      if (!listed[x] && !cf.items[x].unused) {
        FieldItem it;
        it.x = x;
        data.push_back(std::move(it));
      }
    if (fd.sort != Field::Sort::Manual || fresh) {
      const bool down = fd.sort == Field::Sort::Descending;
      std::stable_sort(data.begin(), data.end(), [&](const FieldItem& a, const FieldItem& b) {
        const Item &x = cf.items[a.x], &y = cf.items[b.x];
        if (Rank(x) == 4 || Rank(y) == 4) return Rank(x) < Rank(y);
        return down ? ItemLess(y, x) : ItemLess(x, y);
      });
    }
    if (fresh && fd.defaultSubtotal) {
      FieldItem t;
      t.type = FieldItem::Type::Default;
      totals.push_back(t);
    }
    fd.items = std::move(data);
    fd.items.insert(fd.items.end(), std::make_move_iterator(totals.begin()), std::make_move_iterator(totals.end()));
    fd.hasItems = true;
    if (page) {
      page->item = -1;
      if (pageX >= 0)
        for (size_t i = 0; i < fd.items.size(); ++i)
          if (fd.items[i].type == FieldItem::Type::Data && fd.items[i].x == pageX) page->item = static_cast<int64_t>(i);
    }
  }
}

namespace {

// ---- Laying out

struct AxisLine {
  Line line;
  std::vector<uint32_t> path;     // per level: the items index (a data level: the data field)
  uint32_t depth = 0;             // levels the line names
  std::vector<uint32_t> records;  // the records it totals
  bool values = true;             // shows values (a compact header line under which data fields are)
};

struct Acc {
  double sum = 0, sumSq = 0, product = 1, min = 0, max = 0;
  uint32_t count = 0, nums = 0;
  bool any = false;
  std::string error;
};

struct Builder {
  const Cache& cache;
  Table& t;
  std::vector<std::vector<bool>> visible;  // per field, per shared item

  bool Outline(int32_t f) const {
    if (f == kData) return t.outlineData || t.compactData;
    const Field& fd = t.fields[f];
    return fd.outline;
  }
  bool CompactCol(int32_t f) const {
    if (f == kData) return t.compactData && t.outlineData;
    const Field& fd = t.fields[f];
    return fd.compact && fd.outline;
  }

  void Walk(const std::vector<int32_t>& fields, bool rows, size_t level, const std::vector<uint32_t>& recs,
            std::vector<uint32_t>& path, std::vector<AxisLine>& out) {
    const size_t n = fields.size();
    auto emit = [&](FieldItem::Type type, uint32_t depth, uint32_t dataIndex, const std::vector<uint32_t>& rs, bool values) {
      AxisLine l;
      l.line.type = type;
      l.line.dataIndex = dataIndex;
      l.path.assign(path.begin(), path.begin() + depth);
      l.depth = depth;
      l.records = rs;
      l.values = values;
      out.push_back(std::move(l));
    };
    const int32_t f = fields[level];
    bool deeperField = false, deeperData = false;
    for (size_t k = level + 1; k < n; ++k) (fields[k] == kData ? deeperData : deeperField) = true;
    const bool last = level + 1 == n;
    const uint32_t dataIndexAbove = [&] {
      for (size_t k = 0; k < level; ++k)
        if (fields[k] == kData) return path[k];
      return 0u;
    }();
    if (f == kData) {
      for (uint32_t d = 0; d < t.dataFields.size(); ++d) {
        path[level] = d;
        if (last) {
          emit(FieldItem::Type::Data, static_cast<uint32_t>(level + 1), d, recs, true);
          continue;
        }
        if (rows && Outline(f)) emit(FieldItem::Type::Data, static_cast<uint32_t>(level + 1), d, recs, false);
        Walk(fields, rows, level + 1, recs, path, out);
      }
      return;
    }
    const Field& fd = t.fields[f];
    const size_t nf = cache.fields.size();
    for (uint32_t i = 0; i < fd.items.size(); ++i) {
      const FieldItem& it = fd.items[i];
      if (it.type != FieldItem::Type::Data || it.hidden || it.missing) continue;
      std::vector<uint32_t> sub;
      for (uint32_t rec : recs)
        if (cache.records[static_cast<size_t>(rec) * nf + f] == it.x) sub.push_back(rec);
      if (sub.empty() && !fd.showAll) continue;
      path[level] = i;
      if (last) {
        emit(FieldItem::Type::Data, static_cast<uint32_t>(level + 1), dataIndexAbove, sub, true);
        continue;
      }
      const bool header = rows && Outline(f);
      const bool subtotals = fd.defaultSubtotal && deeperField;
      if (header) emit(FieldItem::Type::Data, static_cast<uint32_t>(level + 1), dataIndexAbove, sub, subtotals && fd.subtotalTop && !deeperData);
      Walk(fields, rows, level + 1, sub, path, out);
      if (subtotals && (!header || !fd.subtotalTop || deeperData)) {
        if (deeperData)
          for (uint32_t d = 0; d < t.dataFields.size(); ++d) emit(FieldItem::Type::Default, static_cast<uint32_t>(level + 1), d, sub, true);
        else
          emit(FieldItem::Type::Default, static_cast<uint32_t>(level + 1), dataIndexAbove, sub, true);
      }
    }
  }

  std::vector<AxisLine> Axis(const std::vector<int32_t>& fields, bool rows, const std::vector<uint32_t>& recs, bool grand) {
    std::vector<AxisLine> out;
    bool realField = false, data = false;
    for (int32_t f : fields) (f == kData ? data : realField) = true;
    if (!fields.empty()) {
      std::vector<uint32_t> path(fields.size(), 0);
      Walk(fields, rows, 0, recs, path, out);
    }
    if (realField && grand) {
      for (uint32_t d = 0; d < (data ? t.dataFields.size() : 1); ++d) {
        AxisLine l;
        l.line.type = FieldItem::Type::Grand;
        l.line.dataIndex = d;
        l.records = recs;
        out.push_back(std::move(l));
      }
    }
    if (out.empty()) {  // no fields: one line for every record
      AxisLine l;
      l.records = recs;
      out.push_back(std::move(l));
    }
    // r and x as Excel writes them.
    const std::vector<uint32_t>* prev = nullptr;
    for (AxisLine& l : out) {
      Line& line = l.line;
      line.members.clear();
      if (line.type == FieldItem::Type::Grand) {
        line.repeat = 0;
        line.members.push_back(0);
      } else if (line.type == FieldItem::Type::Default) {
        line.repeat = l.depth - 1;
        line.members.push_back(l.path[l.depth - 1]);
      } else {
        uint32_t r = 0;
        if (prev)
          while (r + 1 < l.depth && r < prev->size() && (*prev)[r] == l.path[r]) ++r;
        line.repeat = r;
        line.members.assign(l.path.begin() + r, l.path.begin() + l.depth);
      }
      prev = &l.path;
    }
    return out;
  }
};

void Add(Acc& a, const Item& v) {
  switch (v.kind) {
    case Item::Kind::Missing: return;
    case Item::Kind::Number:
    case Item::Kind::Date:
      if (a.nums == 0) a.min = a.max = v.number;
      a.sum += v.number;
      a.sumSq += v.number * v.number;
      a.product *= v.number;
      a.min = std::min(a.min, v.number);
      a.max = std::max(a.max, v.number);
      ++a.nums;
      break;
    case Item::Kind::Error:
      if (a.error.empty()) a.error = v.text;
      break;
    default: break;
  }
  ++a.count;
  a.any = true;
}

Item Result(const Acc& a, Func f) {
  Item out;
  if (!a.any) return out;  // no records: empty
  auto number = [&](double v) {
    out.kind = Item::Kind::Number;
    out.number = v;
    return out;
  };
  if (f == Func::Count) return number(a.count);
  if (!a.error.empty()) {
    out.kind = Item::Kind::Error;
    out.text = a.error;
    return out;
  }
  auto div0 = [&] {
    out.kind = Item::Kind::Error;
    out.text = "#DIV/0!";
    return out;
  };
  const double n = a.nums;
  switch (f) {
    case Func::Sum: return number(a.sum);
    case Func::CountNums: return number(n);
    case Func::Average: return n ? number(a.sum / n) : div0();
    case Func::Max: return number(n ? a.max : 0);
    case Func::Min: return number(n ? a.min : 0);
    case Func::Product: return number(n ? a.product : 0);
    case Func::Var:
    case Func::StdDev: {
      if (n < 2) return div0();
      double v = std::max(0.0, (a.sumSq - a.sum * a.sum / n) / (n - 1));
      return number(f == Func::Var ? v : std::sqrt(v));
    }
    case Func::VarP:
    case Func::StdDevP: {
      if (n < 1) return div0();
      double v = std::max(0.0, (a.sumSq - a.sum * a.sum / n) / n);
      return number(f == Func::VarP ? v : std::sqrt(v));
    }
    default: return number(a.sum);
  }
}

Item TextItem(std::string s) {
  Item i;
  i.kind = Item::Kind::String;
  i.text = std::move(s);
  return i;
}

// An item's text, as Excel puts it into captions ("2024 Total").
std::string ItemText(const Item& v) {
  switch (v.kind) {
    case Item::Kind::String:
    case Item::Kind::Error: return v.text;
    case Item::Kind::Number: return ShortNumber(v.number);
    case Item::Kind::Bool: return v.number != 0 ? "TRUE" : "FALSE";
    case Item::Kind::Date: return ShortNumber(v.number);
    default: return "(blank)";
  }
}

}  // namespace

bool Lay(const Cache& cache, Table& t, Layout& out, std::u16string& why) {
  why = u"This PivotTable uses features Featherweight keeps but doesn't refresh.";
  if (!t.why.empty() || !cache.why.empty() || t.fields.size() != cache.fields.size()) return false;
  const size_t nf = cache.fields.size();
  for (const std::vector<int32_t>* list : {&t.rowFields, &t.colFields})
    for (int32_t f : *list) {
      if (f == kData) {
        if (t.dataFields.size() < 2) return false;
        continue;
      }
      if (f < 0 || static_cast<size_t>(f) >= nf || !t.fields[f].hasItems || !cache.fields[f].hasItems) return false;
    }
  for (const PageField& p : t.pageFields)
    if (p.field >= nf || !t.fields[p.field].hasItems || !cache.fields[p.field].hasItems) return false;
  for (const DataField& d : t.dataFields)
    if (d.field >= nf) return false;
  for (size_t f = 0; f < nf; ++f)
    for (const FieldItem& it : t.fields[f].items)
      if (it.type == FieldItem::Type::Data && it.x >= cache.fields[f].items.size()) return false;
  const size_t records = cache.RecordCount();
  if (cache.records.size() != records * nf) return false;

  // The records that pass every filter: hidden items of the fields in the layout, the page fields' choices.
  std::vector<std::vector<bool>> pass(nf);
  auto filters = [&](size_t f) {
    const Field& fd = t.fields[f];
    pass[f].assign(cache.fields[f].items.size(), true);
    for (const FieldItem& it : fd.items)
      if (it.type == FieldItem::Type::Data && it.hidden) pass[f][it.x] = false;
  };
  for (int32_t f : t.rowFields)
    if (f != kData) filters(f);
  for (int32_t f : t.colFields)
    if (f != kData) filters(f);
  for (const PageField& p : t.pageFields) {
    filters(p.field);
    if (p.item >= 0 && static_cast<size_t>(p.item) < t.fields[p.field].items.size()) {
      const uint32_t x = t.fields[p.field].items[p.item].x;
      for (size_t k = 0; k < pass[p.field].size(); ++k) pass[p.field][k] = k == x;
    }
  }
  std::vector<uint32_t> all;
  for (uint32_t rec = 0; rec < records; ++rec) {
    bool ok = true;
    for (size_t f = 0; f < nf && ok; ++f)
      if (!pass[f].empty()) ok = pass[f][cache.records[static_cast<size_t>(rec) * nf + f]];
    if (ok) all.push_back(rec);
  }

  Builder b{cache, t, {}};
  std::vector<AxisLine> rows = b.Axis(t.rowFields, true, all, t.rowGrandTotals);
  std::vector<AxisLine> cols = b.Axis(t.colFields, false, all, t.colGrandTotals);
  const bool dataOnRows = std::find(t.rowFields.begin(), t.rowFields.end(), kData) != t.rowFields.end();
  const bool dataOnCols = std::find(t.colFields.begin(), t.colFields.end(), kData) != t.colFields.end();

  // Geometry.
  std::vector<uint32_t> colOfLevel(t.rowFields.size(), 0);
  for (size_t k = 1; k < t.rowFields.size(); ++k)
    colOfLevel[k] = colOfLevel[k - 1] + (b.CompactCol(t.rowFields[k - 1]) ? 0 : 1);
  const uint32_t fdc = t.rowFields.empty() ? 0 : colOfLevel.back() + 1;
  const bool valuesOnly = t.colFields.size() == 1 && t.colFields[0] == kData && t.hideValuesRow;
  const uint32_t fhr = t.colFields.empty() ? 1 : (valuesOnly ? 0 : 1);
  const uint32_t fdr = t.colFields.empty() ? 1 : fhr + static_cast<uint32_t>(t.colFields.size());
  const uint32_t top = t.location.firstRow, left = t.location.firstCol;
  const uint64_t lastRow = uint64_t{top} + fdr + rows.size() - 1, lastCol = uint64_t{left} + fdc + cols.size() - 1;
  const uint32_t pages = static_cast<uint32_t>(t.pageFields.size());
  if (lastRow >= kMaxRows || lastCol >= kMaxCols || (pages && top < pages + 1)) {
    why = u"The PivotTable doesn't fit on the sheet.";
    return false;
  }
  t.firstHeaderRow = fhr;
  t.firstDataRow = fdr;
  t.firstDataCol = fdc;
  t.rowPageCount = pages;
  t.colPageCount = pages ? 1 : 0;
  t.location = {top, left, static_cast<uint32_t>(lastRow), static_cast<uint32_t>(lastCol)};
  t.rowItems.clear();
  t.colItems.clear();
  for (const AxisLine& l : rows) t.rowItems.push_back(l.line);
  for (const AxisLine& l : cols) t.colItems.push_back(l.line);

  out = Layout{};
  out.location = t.location;
  out.area = t.location;
  if (pages) out.area.firstRow = top - pages - 1;
  auto put = [&](uint32_t row, uint32_t col, Item v, uint32_t fmt = kNone, uint8_t indent = 0) {
    OutCell c;
    c.at = {row, col};
    c.value = std::move(v);
    c.numFmtId = fmt;
    c.indent = indent;
    out.cells.push_back(std::move(c));
  };
  auto fieldName = [&](int32_t f) -> std::string {
    if (f == kData) return t.dataCaption;
    return !t.fields[f].caption.empty() ? t.fields[f].caption : cache.fields[f].name;
  };
  auto labelFormat = [&](uint32_t f, const Item& v) {
    if (v.kind != Item::Kind::Number && v.kind != Item::Kind::Date) return kNone;
    uint32_t id = t.fields[f].numFmtId != kNone ? t.fields[f].numFmtId : cache.fields[f].numFmtId;
    return id == 0 ? kNone : id;
  };
  // A member's label: the item (or its caption), a data field's name.
  auto member = [&](int32_t f, uint32_t index, uint32_t& fmt) -> Item {
    fmt = kNone;
    if (f == kData) return TextItem(index < t.dataFields.size() ? t.dataFields[index].name : std::string());
    const FieldItem& it = t.fields[f].items[index];
    if (!it.caption.empty()) return TextItem(it.caption);
    const Item& v = cache.fields[f].items[it.x];
    if (v.kind == Item::Kind::Missing) return TextItem("(blank)");
    fmt = labelFormat(static_cast<uint32_t>(f), v);
    Item copy = v;
    copy.unused = false;
    return copy;
  };
  auto memberText = [&](int32_t f, uint32_t index) {
    if (f == kData) return index < t.dataFields.size() ? t.dataFields[index].name : std::string();
    const FieldItem& it = t.fields[f].items[index];
    return !it.caption.empty() ? it.caption : ItemText(cache.fields[f].items[it.x]);
  };
  const std::string grand = t.grandTotalCaption.empty() ? "Grand Total" : t.grandTotalCaption;
  auto totalLabel = [&](const std::vector<int32_t>& fields, const AxisLine& l, bool dataHere) {
    if (l.line.type == FieldItem::Type::Grand)
      return dataHere ? "Total " + t.dataFields[l.line.dataIndex].name : grand;
    std::string s = memberText(fields[l.depth - 1], l.path[l.depth - 1]);
    bool dataDeeper = false;
    for (size_t k = l.depth; k < fields.size(); ++k) dataDeeper |= fields[k] == kData;
    return s + (dataDeeper ? " " + t.dataFields[l.line.dataIndex].name : std::string(" Total"));
  };

  // Page fields, above the table with a blank row between.
  for (uint32_t i = 0; i < pages; ++i) {
    const PageField& p = t.pageFields[i];
    const uint32_t row = top - pages - 1 + i;
    put(row, left, TextItem(fieldName(static_cast<int32_t>(p.field))));
    const Field& fd = t.fields[p.field];
    bool hidden = false;
    for (const FieldItem& it : fd.items) hidden |= it.type == FieldItem::Type::Data && it.hidden;
    if (p.item >= 0 && static_cast<size_t>(p.item) < fd.items.size()) {
      uint32_t fmt = kNone;
      Item v = member(static_cast<int32_t>(p.field), static_cast<uint32_t>(p.item), fmt);
      put(row, left + 1, v, fmt);
    } else {
      put(row, left + 1, TextItem(hidden ? "(Multiple Items)" : "(All)"));
    }
  }
  // Captions.
  const std::string rowCaption = t.rowHeaderCaption.empty() ? "Row Labels" : t.rowHeaderCaption;
  const std::string colCaption = t.colHeaderCaption.empty() ? "Column Labels" : t.colHeaderCaption;
  if (!t.colFields.empty() && fhr == 1) {
    if (t.dataFields.size() == 1) put(top, left, TextItem(t.dataFields[0].name));
    if (t.compact) {
      put(top, left + fdc, TextItem(t.colFields.size() == 1 && t.colFields[0] == kData ? t.dataCaption : colCaption));
    } else {
      for (size_t k = 0; k < t.colFields.size(); ++k)
        put(top, left + fdc + static_cast<uint32_t>(k), TextItem(fieldName(t.colFields[k])));
    }
  }
  if (t.colFields.empty() && t.dataFields.size() == 1 && !dataOnRows) put(top, left + fdc, TextItem(t.dataFields[0].name));
  if (t.colFields.empty() && dataOnRows && t.gridDropZones) put(top, left + fdc, TextItem("Total"));  // the classic layout
  for (size_t k = 0; k < t.rowFields.size(); ++k)
    if (k == 0 || colOfLevel[k] != colOfLevel[k - 1]) {
      const int32_t f = t.rowFields[k];
      const bool compact = f == kData ? t.compactData : t.fields[f].compact;
      put(top + fdr - 1, left + colOfLevel[k], TextItem(compact ? rowCaption : fieldName(f)));
    }
  // Column labels.
  for (size_t j = 0; j < cols.size(); ++j) {
    const AxisLine& l = cols[j];
    const uint32_t col = left + fdc + static_cast<uint32_t>(j);
    if (l.line.type == FieldItem::Type::Grand || l.line.type == FieldItem::Type::Default) {
      put(top + fhr + (l.line.type == FieldItem::Type::Grand ? 0 : l.depth - 1), col, TextItem(totalLabel(t.colFields, l, dataOnCols)));
      continue;
    }
    for (uint32_t level = l.line.repeat; level < l.depth; ++level) {
      uint32_t fmt = kNone;
      Item v = member(t.colFields[level], l.path[level], fmt);
      put(top + fhr + level, col, v, fmt);
    }
  }
  // Row labels.
  for (size_t i = 0; i < rows.size(); ++i) {
    const AxisLine& l = rows[i];
    const uint32_t row = top + fdr + static_cast<uint32_t>(i);
    if (l.line.type == FieldItem::Type::Grand) {
      put(row, left, TextItem(totalLabel(t.rowFields, l, dataOnRows)));
      continue;
    }
    if (l.line.type == FieldItem::Type::Default) {
      const uint32_t level = l.depth - 1;
      put(row, left + colOfLevel[level], TextItem(totalLabel(t.rowFields, l, dataOnRows)), kNone,
          static_cast<uint8_t>(std::min<uint32_t>(15, level - [&] {
            uint32_t first = level;
            while (first > 0 && colOfLevel[first - 1] == colOfLevel[level]) --first;
            return first;
          }())));
      continue;
    }
    for (uint32_t level = l.line.repeat; level < l.depth; ++level) {
      uint32_t first = level;
      while (first > 0 && colOfLevel[first - 1] == colOfLevel[level]) --first;
      uint32_t fmt = kNone;
      Item v = member(t.rowFields[level], l.path[level], fmt);
      put(row, left + colOfLevel[level], v, fmt, static_cast<uint8_t>(std::min<uint32_t>(15, level - first)));
    }
  }
  // Values: each record into the column lines that total it, then row by row.
  if (!t.dataFields.empty()) {
    std::vector<std::vector<uint32_t>> colsOf(records);
    for (uint32_t j = 0; j < cols.size(); ++j)
      for (uint32_t rec : cols[j].records) colsOf[rec].push_back(j);
    std::vector<Acc> acc(cols.size());
    for (size_t i = 0; i < rows.size(); ++i) {
      const AxisLine& rl = rows[i];
      if (!rl.values) continue;
      std::fill(acc.begin(), acc.end(), Acc{});
      const uint32_t rowData = dataOnRows ? rl.line.dataIndex : 0;
      // Each column may total a different data field (the data fields on columns).
      for (uint32_t rec : rl.records)
        for (uint32_t j : colsOf[rec]) {
          const uint32_t d = dataOnCols ? cols[j].line.dataIndex : rowData;
          if (d >= t.dataFields.size()) continue;
          const DataField& df = t.dataFields[d];
          Add(acc[j], cache.fields[df.field].items[cache.records[static_cast<size_t>(rec) * nf + df.field]]);
        }
      for (uint32_t j = 0; j < cols.size(); ++j) {
        const uint32_t d = dataOnCols ? cols[j].line.dataIndex : rowData;
        if (d >= t.dataFields.size() || !acc[j].any) continue;
        const DataField& df = t.dataFields[d];
        put(top + fdr + static_cast<uint32_t>(i), left + fdc + j, Result(acc[j], df.func), df.numFmtId == 0 ? kNone : df.numFmtId);
      }
    }
  }
  return true;
}

}  // namespace fw::sheet::pivot
