// fwtool torture for workbooks (S2's "never damage a file" gate). Each workbook gets rounds of
// seeded random edits through the edit API, aimed at what breaks files: typing values and formulas
// in both regions (en-NL and en-US), formatting, inserting and deleting rows, columns and cells,
// adding, renaming, moving and deleting sheets, merging, sorting, filtering, pasting, undo and
// redo. After each round the workbook is saved, and the saved file must open, pass fwtool check
// with no errors the original didn't have, raise no new model problems, read back as the edited
// workbook, and keep every part the save didn't write byte for byte. A failure is shrunk to the
// fewest edits that still fail and kept as a reproducer. scripts\torture.ps1 then runs the
// validator and the Excel open check on the saved copies.
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <set>

#include "Common.h"
#include "SheetTorture.h"
#include "core/Strings.h"
#include "pkg/Check.h"
#include "pkg/Package.h"
#include "pkg/Snapshot.h"
#include "pkg/Zip.h"
#include "sheet/model/Sheet.h"
#include "sheet/xlsx/Xlsx.h"

namespace fwtool {
namespace sheettorture {
namespace {

namespace fs = std::filesystem;
namespace sh = fw::sheet;

constexpr struct {
  Op op;
  const char* name;
} kOps[] = {
    {Op::Value, "value"},           {Op::Formula, "formula"},         {Op::Clear, "clear"},
    {Op::Format, "format"},         {Op::InsertRows, "insert-rows"},  {Op::DeleteRows, "delete-rows"},
    {Op::InsertCols, "insert-cols"}, {Op::DeleteCols, "delete-cols"}, {Op::InsertCells, "insert-cells"},
    {Op::DeleteCells, "delete-cells"}, {Op::AddSheet, "add-sheet"},   {Op::RenameSheet, "rename-sheet"},
    {Op::MoveSheet, "move-sheet"},  {Op::DeleteSheet, "delete-sheet"}, {Op::Merge, "merge"},
    {Op::Unmerge, "unmerge"},       {Op::Sort, "sort"},               {Op::Filter, "filter"},
    {Op::Paste, "paste"},           {Op::Undo, "undo"},               {Op::Redo, "redo"},
    {Op::Save, "save"},
};

const char* OpName(Op op) {
  for (const auto& o : kOps)
    if (o.op == op) return o.name;
  return "?";
}

std::string Quote(std::string_view text) {
  std::string out = "\"";
  for (char c : text) {
    if (c == '"' || c == '\\') out += '\\', out += c;
    else if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else out += c;
  }
  return out + "\"";
}

// Reads a quoted string at `s` (which starts at its opening quote) and moves past it.
bool Unquote(std::string_view& s, std::string& out) {
  if (s.empty() || s.front() != '"') return false;
  out.clear();
  for (size_t i = 1; i < s.size(); ++i) {
    char c = s[i];
    if (c == '\\' && i + 1 < s.size()) {
      char n = s[++i];
      out += n == 'n' ? '\n' : n == 'r' ? '\r' : n;
    } else if (c == '"') {
      s.remove_prefix(i + 1);
      return true;
    } else {
      out += c;
    }
  }
  return false;
}

bool RowsOnly(Op op) { return op == Op::InsertRows || op == Op::DeleteRows; }
bool ColsOnly(Op op) { return op == Op::InsertCols || op == Op::DeleteCols; }

std::string RangeText(Op op, const sh::Range& r) {
  if (RowsOnly(op)) return std::to_string(r.firstRow + 1) + ":" + std::to_string(r.lastRow + 1);
  if (ColsOnly(op)) return sh::ColumnName(r.firstCol) + ":" + sh::ColumnName(r.lastCol);
  return sh::RangeName(r);
}

uint32_t ColumnNumber(std::string_view s, bool& ok) {
  uint32_t c = 0;
  ok = !s.empty() && s.size() <= 3;
  for (char ch : s) {
    if (ch < 'A' || ch > 'Z') ok = false;
    c = c * 26 + static_cast<uint32_t>(ch - 'A' + 1);
  }
  return c - 1;
}

bool ParseRangeFor(Op op, std::string_view s, sh::Range& r) {
  size_t colon = s.find(':');
  if (RowsOnly(op) || ColsOnly(op)) {
    if (colon == std::string_view::npos) return false;
    std::string_view a = s.substr(0, colon), b = s.substr(colon + 1);
    if (RowsOnly(op)) {
      r.firstRow = static_cast<uint32_t>(std::strtoul(std::string(a).c_str(), nullptr, 10)) - 1;
      r.lastRow = static_cast<uint32_t>(std::strtoul(std::string(b).c_str(), nullptr, 10)) - 1;
      r.firstCol = 0, r.lastCol = 16383;
      return r.firstRow <= r.lastRow && r.lastRow < 1048576;
    }
    bool ok1 = false, ok2 = false;
    r.firstCol = ColumnNumber(a, ok1), r.lastCol = ColumnNumber(b, ok2);
    r.firstRow = 0, r.lastRow = 1048575;
    return ok1 && ok2 && r.firstCol <= r.lastCol;
  }
  return sh::ParseRange(s, r);
}

// "3!B2:C4": a sheet index and a range.
bool ParseSheetRange(Op op, std::string_view s, uint32_t& sheet, sh::Range& r) {
  size_t bang = s.find('!');
  if (bang == std::string_view::npos) return false;
  sheet = static_cast<uint32_t>(std::strtoul(std::string(s.substr(0, bang)).c_str(), nullptr, 10));
  return ParseRangeFor(op, s.substr(bang + 1), r);
}

std::string_view NextWord(std::string_view& s) {
  while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
  size_t end = s.find(' ');
  std::string_view w = s.substr(0, end);
  s.remove_prefix(end == std::string_view::npos ? s.size() : end);
  while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
  return w;
}

}  // namespace

std::string FormatEdit(const Edit& e) {
  std::string s = OpName(e.op);
  auto at = [&](uint32_t sheet, const sh::Range& r) { return " " + std::to_string(sheet) + "!" + RangeText(e.op, r); };
  switch (e.op) {
    case Op::Value:
    case Op::Formula: s += at(e.sheet, e.range) + " " + Quote(e.text) + " " + e.arg; break;
    case Op::Clear:
    case Op::Merge:
    case Op::Unmerge:
    case Op::InsertRows:
    case Op::DeleteRows:
    case Op::InsertCols:
    case Op::DeleteCols: s += at(e.sheet, e.range); break;
    case Op::Format:
    case Op::InsertCells:
    case Op::DeleteCells: s += at(e.sheet, e.range) + " " + e.arg; break;
    case Op::Sort: s += at(e.sheet, e.range) + " " + Quote(e.arg); break;
    case Op::Filter:
      s += at(e.sheet, e.range);
      if (!e.arg.empty()) s += " " + Quote(e.arg) + " " + Quote(e.text);
      break;
    case Op::Paste: s += at(e.sheet, e.range) + at(e.toSheet, e.to) + " " + e.arg; break;
    case Op::AddSheet:
    case Op::RenameSheet: s += " " + std::to_string(e.sheet) + " " + Quote(e.text); break;
    case Op::MoveSheet: s += " " + std::to_string(e.sheet) + " " + std::to_string(e.toSheet); break;
    case Op::DeleteSheet: s += " " + std::to_string(e.sheet); break;
    case Op::Undo:
    case Op::Redo: s += " " + std::to_string(e.count); break;
    case Op::Save: break;
  }
  if (!e.target.empty()) s += "  # " + e.target;
  return s;
}

bool ParseEdit(std::string_view line, Edit& e) {
  e = Edit{};
  if (size_t hash = line.find("  # "); hash != std::string_view::npos) {
    e.target = std::string(line.substr(hash + 4));
    line = line.substr(0, hash);
  }
  std::string_view s = line;
  std::string_view name = NextWord(s);
  bool known = false;
  for (const auto& o : kOps)
    if (name == o.name) e.op = o.op, known = true;
  if (!known) return false;
  auto sheetRange = [&](uint32_t& sheet, sh::Range& r) { return ParseSheetRange(e.op, NextWord(s), sheet, r); };
  auto number = [&](uint32_t& n) {
    std::string w(NextWord(s));
    if (w.empty()) return false;
    n = static_cast<uint32_t>(std::strtoul(w.c_str(), nullptr, 10));
    return true;
  };
  switch (e.op) {
    case Op::Value:
    case Op::Formula:
      if (!sheetRange(e.sheet, e.range) || !Unquote(s, e.text)) return false;
      e.arg = std::string(NextWord(s));
      return true;
    case Op::Clear:
    case Op::Merge:
    case Op::Unmerge:
    case Op::InsertRows:
    case Op::DeleteRows:
    case Op::InsertCols:
    case Op::DeleteCols: return sheetRange(e.sheet, e.range);
    case Op::Format:
    case Op::InsertCells:
    case Op::DeleteCells:
      if (!sheetRange(e.sheet, e.range)) return false;
      e.arg = std::string(NextWord(s));
      return !e.arg.empty();
    case Op::Sort: return sheetRange(e.sheet, e.range) && Unquote(s, e.arg);
    case Op::Filter:
      if (!sheetRange(e.sheet, e.range)) return false;
      while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
      if (s.empty()) return true;
      if (!Unquote(s, e.arg)) return false;
      while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
      return Unquote(s, e.text);
    case Op::Paste:
      if (!sheetRange(e.sheet, e.range) || !sheetRange(e.toSheet, e.to)) return false;
      e.arg = std::string(NextWord(s));
      return true;
    case Op::AddSheet:
    case Op::RenameSheet:
      if (!number(e.sheet)) return false;
      while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
      return Unquote(s, e.text);
    case Op::MoveSheet: return number(e.sheet) && number(e.toSheet);
    case Op::DeleteSheet: return number(e.sheet);
    case Op::Undo:
    case Op::Redo: return number(e.count);
    case Op::Save: return true;
  }
  return false;
}

namespace {

// ---- The target without the edit API: every edit refused; Save writes the model's parts.

class ModelOnlyTarget : public SheetTarget {
 public:
  explicit ModelOnlyTarget(std::unique_ptr<sh::Workbook> book) : book_(std::move(book)) {}
  const char* Name() const override { return "model only (no edit API in this build)"; }
  const sh::Workbook& Book() const override { return *book_; }
  Applied Apply(const Edit&, std::string& why) override {
    why = "no edit API in this build";
    return Applied::Refused;
  }
  fw::pkg::Status Save(std::vector<uint8_t>& out) override {
    fw::pkg::Package* pkg = book_->GetPackage();
    if (!pkg) return fw::pkg::Status::Fail(fw::pkg::Status::Code::NotFound, "the workbook has no package");
    written = sh::xlsx::ModelParts(*book_);
    for (const std::string& name : written) {
      std::string xml;
      if (fw::pkg::Status st = sh::xlsx::WritePart(*book_, name, xml); !st.Ok()) return st;
      pkg->SetPart(name, std::vector<uint8_t>(xml.begin(), xml.end()));
    }
    fw::pkg::MemorySink sink;
    if (fw::pkg::Status st = pkg->Save(sink); !st.Ok()) return st;
    out = std::move(sink.bytes);
    return {};
  }

 private:
  std::unique_ptr<sh::Workbook> book_;
};

}  // namespace

// s2-model's edit API plugs in here when it lands (src/sheet/edit): an EditApiTarget that applies
// each Op through the same commands the grid runs.
std::unique_ptr<SheetTarget> OpenSheetTarget(std::unique_ptr<sh::Workbook> book, fw::pkg::Status& status) {
  status = {};
  return std::make_unique<ModelOnlyTarget>(std::move(book));
}

namespace {

// ---- Random edits

struct Rng {
  uint64_t s;
  explicit Rng(uint64_t seed) : s(seed ? seed : 0x9E3779B97F4A7C15ull) {}
  uint64_t Next() {  // splitmix64
    uint64_t z = (s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }
  uint32_t Below(uint32_t n) { return n ? static_cast<uint32_t>(Next() % n) : 0; }
  bool Chance(uint32_t percent) { return Below(100) < percent; }
  template <typename T, size_t N>
  const T& Pick(const T (&items)[N]) { return items[Below(N)]; }
};

uint64_t RoundSeed(uint64_t seed, const std::string& name, uint32_t round) {
  uint64_t h = 1469598103934665603ull;
  for (unsigned char c : name) h = (h ^ c) * 1099511628211ull;
  Rng mix(seed ^ h ^ (static_cast<uint64_t>(round) << 32));
  return mix.Next();
}

std::string SheetRef(const std::string& name) {
  bool plain = !name.empty() && !std::isdigit(static_cast<unsigned char>(name.front()));
  for (char c : name)
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') plain = false;
  if (plain) return name;
  std::string q = "'";
  for (char c : name) q += c == '\'' ? std::string("''") : std::string(1, c);
  return q + "'";
}

class Generator {
 public:
  explicit Generator(uint64_t seed) : rng_(seed) {}

  Edit Next(const sh::Workbook& book) {
    std::vector<uint32_t> sheets;
    for (uint32_t i = 0; i < book.SheetCount(); ++i)
      if (book.GetSheet(i).Kind() == sh::SheetKind::Worksheet && book.GetSheet(i).Loaded()) sheets.push_back(i);
    Edit e;
    uint32_t roll = rng_.Below(100);
    if (sheets.empty()) roll = 85 + rng_.Below(15);  // only sheet edits, undo, redo and save
    e.sheet = sheets.empty() ? 0 : sheets[rng_.Below(static_cast<uint32_t>(sheets.size()))];
    const sh::Sheet* sheet = sheets.empty() ? nullptr : &book.GetSheet(e.sheet);
    sh::Range area{0, 0, 9, 4};
    if (sheet)
      if (auto used = sheet->UsedRange()) area = *used;
    // A little past the used range too: edits at its edges and just outside it.
    area.lastRow = std::min<uint32_t>(area.lastRow + 3, 1048575), area.lastCol = std::min<uint32_t>(area.lastCol + 2, 16383);
    const bool nl = rng_.Chance(50);
    e.arg = nl ? "nl" : "us";
    if (roll < 30) {
      e.op = Op::Value;
      e.range = Cell(area);
      e.text = TypedValue(nl);
    } else if (roll < 44) {
      e.op = Op::Formula;
      e.range = Cell(area);
      e.text = TypedFormula(book, nl, e.range);
    } else if (roll < 48) {
      e.op = Op::Clear;
      e.range = Block(area, 4, 3);
      e.arg.clear();
    } else if (roll < 58) {
      e.op = Op::Format;
      e.range = Block(area, 6, 4);
      static const char* kFormats[] = {"bold", "italic", "underline", "numfmt=0.00", "numfmt=dd/mm/yyyy", "numfmt=#,##0",
                                       "numfmt=0%", "numfmt=@", "fill=FFFF00", "color=C00000", "border=thin", "border=none",
                                       "align=center", "align=right", "wrap", "font=Arial", "size=14"};
      e.arg = rng_.Pick(kFormats);
    } else if (roll < 64) {
      e.op = rng_.Chance(50) ? Op::InsertRows : Op::DeleteRows;
      uint32_t first = area.firstRow + rng_.Below(area.lastRow - area.firstRow + 1);
      e.range = {first, 0, first + rng_.Below(3), 16383};
      e.arg.clear();
    } else if (roll < 68) {
      e.op = rng_.Chance(50) ? Op::InsertCols : Op::DeleteCols;
      uint32_t first = area.firstCol + rng_.Below(area.lastCol - area.firstCol + 1);
      e.range = {0, first, 1048575, first + rng_.Below(2)};
      e.arg.clear();
    } else if (roll < 71) {
      bool insert = rng_.Chance(50);
      e.op = insert ? Op::InsertCells : Op::DeleteCells;
      e.range = Block(area, 3, 3);
      e.arg = insert ? (rng_.Chance(50) ? "down" : "right") : (rng_.Chance(50) ? "up" : "left");
    } else if (roll < 74) {
      e.op = Op::Merge;
      e.range = Block(area, 2, 3);
      if (e.range.firstRow == e.range.lastRow && e.range.firstCol == e.range.lastCol) e.range.lastCol++;
      e.arg.clear();
    } else if (roll < 75) {
      e.op = Op::Unmerge;
      e.range = sheet && !sheet->Merges().empty()
                    ? sheet->Merges()[rng_.Below(static_cast<uint32_t>(sheet->Merges().size()))]
                    : Block(area, 2, 2);
      e.arg.clear();
    } else if (roll < 78) {
      e.op = Op::Sort;
      e.range = Block(area, 20, 4);
      if (e.range.lastRow == e.range.firstRow) e.range.lastRow++;
      e.arg = sh::ColumnName(e.range.firstCol + rng_.Below(e.range.lastCol - e.range.firstCol + 1)) +
              (rng_.Chance(50) ? " asc" : " desc");
    } else if (roll < 80) {
      e.op = Op::Filter;
      e.range = Block(area, 20, 4);
      e.arg.clear();
      if (rng_.Chance(50)) {
        uint32_t col = e.range.firstCol + rng_.Below(e.range.lastCol - e.range.firstCol + 1);
        e.arg = sh::ColumnName(col);
        sh::CellInfo info;
        if (sheet && sheet->GetCell(e.range.firstRow + 1, col, info) && info.value.type == sh::ValueType::String)
          e.text = std::string(info.value.text);
        else e.text = "x";
      }
    } else if (roll < 85) {
      e.op = Op::Paste;
      e.range = Block(area, 3, 3);
      e.toSheet = sheets.empty() ? 0 : sheets[rng_.Below(static_cast<uint32_t>(sheets.size()))];
      e.to = Cell(area);
      static const char* kModes[] = {"all", "all", "values", "formats"};
      e.arg = rng_.Pick(kModes);
    } else if (roll < 89) {
      SheetEdit(book, e);
    } else if (roll < 95) {
      e.op = Op::Undo;
      e.count = 1 + rng_.Below(3);
      e.arg.clear();
    } else if (roll < 98) {
      e.op = Op::Redo;
      e.count = 1 + rng_.Below(2);
      e.arg.clear();
    } else {
      e.op = Op::Save;
      e.arg.clear();
    }
    return e;
  }

 private:
  sh::Range Cell(const sh::Range& a) {
    uint32_t r = a.firstRow + rng_.Below(a.lastRow - a.firstRow + 1), c = a.firstCol + rng_.Below(a.lastCol - a.firstCol + 1);
    return {r, c, r, c};
  }
  sh::Range Block(const sh::Range& a, uint32_t rows, uint32_t cols) {
    sh::Range r = Cell(a);
    r.lastRow = std::min(a.lastRow, r.firstRow + rng_.Below(rows));
    r.lastCol = std::min(a.lastCol, r.firstCol + rng_.Below(cols));
    return r;
  }

  // What someone types into a cell, in the region's form: Excel turns it into a number, a date, a
  // percentage, currency or text as it does when typing.
  std::string TypedValue(bool nl) {
    switch (rng_.Below(14)) {
      case 0: return std::to_string(rng_.Below(100000));
      case 1: return nl ? "12,5" : "12.5";
      case 2: return nl ? "-1.234,56" : "-1,234.56";
      case 3: return nl ? "17/11/2026" : "11/17/2026";
      case 4: return nl ? "17-11-2026 14:30" : "11/17/2026 2:30 PM";
      case 5: return "15%";
      case 6: return nl ? "\xE2\x82\xAC 12,50" : "$12.50";  // € 12,50
      case 7: return "'0012";
      case 8: return "TRUE";
      case 9: return "Total";
      case 10: return "D\xC3\xA9penses \xE6\x97\xA5\xE6\x9C\xAC \xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D";  // Dépenses 日本 שלום
      case 11: return "line one\nline two";  // Alt+Enter
      case 12: return "1/2";
      default: return std::string(300, 'x');
    }
  }

  std::string TypedFormula(const sh::Workbook& book, bool nl, const sh::Range& at) {
    const char* sep = nl ? ";" : ",";
    const char* dec = nl ? "," : ".";
    auto near = [&](int dr, int dc) {
      int64_t r = static_cast<int64_t>(at.firstRow) + dr, c = static_cast<int64_t>(at.firstCol) + dc;
      return sh::CellName(static_cast<uint32_t>(std::max<int64_t>(0, r)), static_cast<uint32_t>(std::max<int64_t>(0, c)));
    };
    switch (rng_.Below(11)) {
      case 0: return "=SUM(" + near(-5, 0) + ":" + near(-1, 0) + ")";
      case 1: return "=ROUND(" + near(0, -1) + sep + "2)";
      case 2: return "=IF(" + near(0, -1) + ">0" + sep + "\"yes\"" + sep + "\"no\")";
      case 3: return "=" + near(0, -1) + "*1" + dec + "5";
      case 4: return "=AVERAGE(" + near(-3, -2) + ":" + near(-1, -1) + sep + "10)";
      case 5: {
        const sh::Sheet& other = book.GetSheet(rng_.Below(static_cast<uint32_t>(book.SheetCount())));
        return "=" + SheetRef(other.Name()) + "!A1+1";
      }
      case 6: return "=VLOOKUP(" + near(0, -1) + sep + "A1:B10" + sep + "2" + sep + "FALSE)";
      case 7: return "=SUM(A:A)";
      case 8: return "=1/0";
      case 9: return "=CONCATENATE(\"a\"" + std::string(sep) + near(0, -1) + ")";
      default: return "=COUNTIF(" + near(-10, 0) + ":" + near(-1, 0) + sep + "\">1" + dec + "5\")";
    }
  }

  void SheetEdit(const sh::Workbook& book, Edit& e) {
    uint32_t count = static_cast<uint32_t>(book.SheetCount());
    e.arg.clear();
    switch (rng_.Below(4)) {
      case 0: {
        e.op = Op::AddSheet;
        e.sheet = rng_.Below(count + 1);
        static const char* kNames[] = {"Sheet9", "Budget 2027", "Donn\xC3\xA9" "es", "a'b", "Sheet1"};
        e.text = rng_.Pick(kNames);
        break;
      }
      case 1: {
        e.op = Op::RenameSheet;
        e.sheet = rng_.Below(count);
        // Valid names, and ones Excel refuses (a slash, 32 characters, a name in use).
        static const char* kNames[] = {"Renamed", "Q1 & Q2", "\xE6\x97\xA5\xE6\x9C\xAC", "a/b",
                                       "abcdefghijklmnopqrstuvwxyz012345", "History"};
        e.text = rng_.Pick(kNames);
        if (rng_.Chance(20) && count > 1) e.text = book.GetSheet((e.sheet + 1) % count).Name();
        break;
      }
      case 2:
        e.op = Op::MoveSheet;
        e.sheet = rng_.Below(count);
        e.toSheet = rng_.Below(count);
        break;
      default:
        e.op = Op::DeleteSheet;
        e.sheet = rng_.Below(count);
        break;
    }
  }

  Rng rng_;
};

// ---- What a workbook holds, line by line: what the saved copy must read back as.

std::string Num(double v) {
  char buf[40];
  std::snprintf(buf, sizeof buf, "%.17g", v);
  return buf;
}

std::string ValueText(const sh::Value& v) {
  switch (v.type) {
    case sh::ValueType::Empty: return "empty";
    case sh::ValueType::Number: return "n " + Num(v.number);
    case sh::ValueType::String: return "s " + Quote(v.text);
    case sh::ValueType::Bool: return v.boolean ? "TRUE" : "FALSE";
    case sh::ValueType::Error: return "e " + std::string(sh::ErrorText(v.error));
  }
  return "?";
}

std::vector<std::string> Digest(const sh::Workbook& book) {
  std::vector<std::string> out;
  out.push_back("sheets " + std::to_string(book.SheetCount()) + ", 1904 " + (book.Date1904() ? "yes" : "no"));
  for (const sh::DefinedName& n : book.Names())
    out.push_back("name " + n.name + " sheet " + std::to_string(n.sheet) + " = " + n.formula + (n.hidden ? " hidden" : ""));
  for (size_t i = 0; i < book.SheetCount(); ++i) {
    const sh::Sheet& s = book.GetSheet(i);
    out.push_back("sheet " + std::to_string(i) + " " + Quote(s.Name()) + " kind " + std::to_string(static_cast<int>(s.Kind())) +
                  " state " + std::to_string(static_cast<int>(s.State())));
    if (s.Kind() != sh::SheetKind::Worksheet || !s.Loaded()) continue;
    const std::string at = "  " + std::to_string(i) + "!";
    for (const sh::Range& m : s.Merges()) out.push_back(at + "merge " + sh::RangeName(m));
    if (s.Filter()) out.push_back(at + "filter");
    for (const sh::ColumnInfo& c : s.Columns())
      out.push_back(at + "cols " + sh::ColumnName(c.first) + ":" + sh::ColumnName(c.last) + " width " + Num(c.width) +
                    " style " + std::to_string(c.style) + (c.hidden ? " hidden" : ""));
    if (auto used = s.UsedRange())
      s.ForEachRow(0, used->lastRow, [&](uint32_t row, const sh::RowInfo& r) {
        if (r.customHeight || r.hidden || r.customFormat || r.outlineLevel)
          out.push_back(at + "row " + std::to_string(row + 1) + " height " + Num(r.height) + (r.hidden ? " hidden" : "") +
                        (r.customFormat ? " style " + std::to_string(r.style) : "") + " level " +
                        std::to_string(r.outlineLevel));
        return true;
      });
    // The style is compared by index: the S1 writer keeps the styles part as read. When S2's writer
    // renumbers styles, compare what the index resolves to instead.
    s.ForEachCell({0, 0, 1048575, 16383}, [&](const sh::CellInfo& c) {
      std::string line = at + sh::CellName(c.row, c.col) + " " + ValueText(c.value) + " style " + std::to_string(c.style);
      if (c.hasFormula) {
        sh::FormulaInfo f;
        if (s.GetFormula(c.row, c.col, f))
          line += " formula " + std::to_string(static_cast<int>(f.kind)) + " " + Quote(f.text) + " for " +
                  sh::CellName(f.origin.row, f.origin.col) + (f.dynamic ? " dynamic" : "");
      }
      out.push_back(std::move(line));
      return true;
    });
  }
  return out;
}

std::string FirstDifference(const std::vector<std::string>& a, const std::vector<std::string>& b) {
  size_t n = std::min(a.size(), b.size());
  for (size_t i = 0; i < n; ++i)
    if (a[i] != b[i]) return "edited: " + a[i] + " | saved: " + b[i];
  if (a.size() != b.size())
    return a.size() > b.size() ? "edited has more: " + a[n] : "saved has more: " + b[n];
  return {};
}

std::string Joined(const std::vector<std::string>& lines) {
  std::string s;
  for (const std::string& l : lines) s += l + "\n";
  return s;
}

std::string Hex(const fw::pkg::Sha256& h) {
  static const char* digits = "0123456789abcdef";
  std::string s;
  for (uint8_t b : h) s += digits[b >> 4], s += digits[b & 15];
  return s;
}

std::vector<uint8_t> ReadBytes(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
}

void WriteBytes(const fs::path& path, const std::vector<uint8_t>& bytes) {
  std::ofstream(path, std::ios::binary | std::ios::trunc)
      .write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

std::string SafeName(const fs::path& file) {
  std::string name = Display(fs::path(file).replace_extension());
  for (char& c : name)
    if (c == '/' || c == '\\' || c == ':' || c == ' ') c = '_';
  while (!name.empty() && (name.front() == '.' || name.front() == '_')) name.erase(name.begin());
  return name;
}

std::string JsonText(std::string_view s) {
  std::string out = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') out += '\\', out += c;
    else if (static_cast<unsigned char>(c) < 0x20) {
      char buf[8];
      std::snprintf(buf, sizeof buf, "\\u%04x", c);
      out += buf;
    } else out += c;
  }
  return out + "\"";
}

// ---- One round: open, edit, save, check.

struct Failure {
  std::string category;  // open, edit, save, reopen, check, model, readback, untouched
  std::string message;
};

struct Round {
  std::optional<Failure> failure;
  uint32_t done = 0, refused = 0;
  std::vector<Edit> applied;
  std::vector<uint8_t> saved;
  std::vector<std::string> written;
  std::string edited, readBack;  // the digests, when they differ
};

// What the original holds that a saved copy is measured against.
struct Original {
  std::unique_ptr<fw::pkg::Package> pkg;
  std::vector<std::string> damaged, checkErrors, modelProblems;
};

std::optional<Failure> CheckSave(const Original& orig, SheetTarget& target, Round& r) {
  std::vector<uint8_t> bytes;
  if (fw::pkg::Status st = target.Save(bytes); !st.Ok()) return Failure{"save", st.message};
  r.written = target.written;
  r.saved = bytes;
  fw::pkg::Status st;
  auto again = fw::pkg::Package::Open(fw::pkg::Snapshot::FromBytes(std::move(bytes)), orig.pkg->GetLimits(), st);
  std::vector<std::string> damaged;
  if (again) st = again->Verify(&damaged);
  if (!again || !st.Ok()) return Failure{"reopen", "the saved copy doesn't open: " + st.message};
  if (damaged != orig.damaged) return Failure{"reopen", "the saved copy has other damaged parts than the original"};
  for (const std::string& e : fw::pkg::CheckPackage(*again).errors)
    if (std::find(orig.checkErrors.begin(), orig.checkErrors.end(), e) == orig.checkErrors.end())
      return Failure{"check", e};
  // Every part the save didn't write is the original's, byte for byte.
  std::set<std::string> written(r.written.begin(), r.written.end());
  std::vector<uint8_t> a, b;
  for (const auto& part : orig.pkg->Parts()) {
    if (written.contains(part.name)) continue;
    if (!again->ReadPart(part.name, b).Ok()) return Failure{"untouched", part.name + " is missing from the saved copy"};
    if (!orig.pkg->ReadPart(part.name, a).Ok() || a != b) return Failure{"untouched", part.name + " changed"};
  }
  for (const auto& part : again->Parts())
    if (!written.contains(part.name) && !orig.pkg->Parts().empty()) {
      bool known = std::any_of(orig.pkg->Parts().begin(), orig.pkg->Parts().end(),
                               [&](const auto& p) { return p.name == part.name; });
      if (!known) return Failure{"untouched", part.name + " is new, but the save didn't write it"};
    }
  auto book = sh::xlsx::Read(std::move(again), st, {.loadAll = true});
  if (!book) return Failure{"reopen", "the model doesn't read the saved copy: " + st.message};
  for (const std::string& p : CheckWorkbookModel(*book))
    if (std::find(orig.modelProblems.begin(), orig.modelProblems.end(), p) == orig.modelProblems.end())
      return Failure{"model", p};
  std::vector<std::string> edited = Digest(target.Book()), readBack = Digest(*book);
  if (std::string diff = FirstDifference(edited, readBack); !diff.empty()) {
    r.edited = Joined(edited), r.readBack = Joined(readBack);
    return Failure{"readback", diff};
  }
  return std::nullopt;
}

// `replay`: apply these edits instead of generating `edits` of them. `current`: the edits so far,
// rewritten before each one, so that a crash leaves them behind.
Round RunRound(const fs::path& file, uint64_t seed, uint32_t edits, const std::vector<Edit>* replay, const fs::path& current) {
  Round r;
  Original orig;
  fw::pkg::Status st;
  orig.pkg = OpenPackage(file, st);
  if (orig.pkg) st = orig.pkg->Verify(&orig.damaged);
  if (!orig.pkg || !st.Ok()) {
    r.failure = Failure{"open", st.message};
    return r;
  }
  orig.checkErrors = fw::pkg::CheckPackage(*orig.pkg).errors;
  auto pkg = OpenPackage(file, st);
  auto book = pkg ? sh::xlsx::Read(std::move(pkg), st, {.loadAll = true}) : nullptr;
  if (!book) {
    r.failure = Failure{"open", st.message};
    return r;
  }
  orig.modelProblems = CheckWorkbookModel(*book);
  auto target = OpenSheetTarget(std::move(book), st);
  if (!target) {
    r.failure = Failure{"open", st.message};
    return r;
  }
  Generator gen(seed);
  const size_t count = replay ? replay->size() : edits;
  for (size_t i = 0; i < count; ++i) {
    Edit e = replay ? (*replay)[i] : gen.Next(target->Book());
    r.applied.push_back(e);
    if (!current.empty()) {
      std::ofstream cur(current, std::ios::trunc);
      cur << "file: " << fw::WideToUtf8(fs::absolute(file).generic_wstring()) << "\nseed: " << seed << "\n";
      for (const Edit& a : r.applied) cur << FormatEdit(a) << "\n";
    }
    if (e.op == Op::Save) {
      if ((r.failure = CheckSave(orig, *target, r))) return r;
      ++r.done;
      continue;
    }
    std::string why;
    try {
      if (target->Apply(e, why) == Applied::Done) ++r.done;
      else ++r.refused;
    } catch (const std::exception& ex) {
      r.failure = Failure{"edit", FormatEdit(e) + ": " + ex.what()};
      return r;
    }
  }
  r.failure = CheckSave(orig, *target, r);
  return r;
}

// Shrinks the edits to the fewest that still fail the same way (delta debugging, bounded).
std::vector<Edit> Minimize(const fs::path& file, uint64_t seed, std::vector<Edit> edits, const std::string& category) {
  auto fails = [&](const std::vector<Edit>& try_) {
    Round r = RunRound(file, seed, 0, &try_, {});
    return r.failure && r.failure->category == category;
  };
  size_t chunks = 2;
  for (int tries = 0; edits.size() > 1 && tries < 200;) {
    size_t size = (edits.size() + chunks - 1) / chunks;
    bool removed = false;
    for (size_t start = 0; start < edits.size() && tries < 200; start += size, ++tries) {
      std::vector<Edit> rest(edits.begin(), edits.begin() + start);
      rest.insert(rest.end(), edits.begin() + std::min(edits.size(), start + size), edits.end());
      if (fails(rest)) {
        edits = std::move(rest);
        chunks = std::max<size_t>(chunks - 1, 2);
        removed = true;
        break;
      }
    }
    if (!removed) {
      if (size == 1) break;
      chunks = std::min(chunks * 2, edits.size());
    }
  }
  return edits;
}

void WriteEdits(const fs::path& path, const fs::path& file, uint64_t seed, uint32_t round, const std::string& failure,
                const std::vector<Edit>& edits) {
  std::string sha = "unknown";
  if (auto bytes = ReadBytes(file); !bytes.empty()) sha = Hex(fw::pkg::HashBytes(bytes));
  std::ofstream txt(path, std::ios::trunc);
  txt << "# fwtool torture reproducer (workbook): fwtool torture --replay " << fw::WideToUtf8(path.generic_wstring()) << "\n";
  txt << "file: " << fw::WideToUtf8(fs::absolute(file).generic_wstring()) << "\n";
  txt << "sha256: " << sha << "\nseed: " << seed << "\nround: " << round << "\nfailure: " << failure << "\n";
  txt << "edits: " << edits.size() << "\n";
  for (const Edit& e : edits) txt << FormatEdit(e) << "\n";
}

// A reproducer names its file by the path it had where it was made; elsewhere, by the longest
// tail of that path that exists from the current folder.
fs::path FindReproFile(const fs::path& recorded) {
  std::error_code ec;
  if (fs::exists(recorded, ec)) return recorded;
  std::vector<fs::path> parts(recorded.relative_path().begin(), recorded.relative_path().end());
  for (size_t first = 1; first < parts.size(); ++first) {
    fs::path tail;
    for (size_t i = first; i < parts.size(); ++i) tail /= parts[i];
    if (fs::exists(tail, ec)) return tail;
  }
  return recorded;
}

bool ReadReproducer(const fs::path& repro, fs::path& file, uint64_t& seed, std::string& sha, std::vector<Edit>* edits,
                    std::string& error) {
  std::ifstream in(repro);
  if (!in) {
    error = "can't read " + Display(repro);
    return false;
  }
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    if (line.starts_with("file: ")) file = fs::path(fw::Utf8ToWide(line.substr(6)));
    else if (line.starts_with("seed: ")) seed = std::strtoull(line.c_str() + 6, nullptr, 10);
    else if (line.starts_with("sha256: ")) sha = line.substr(8);
    else if (line.starts_with("round: ") || line.starts_with("failure: ") || line.starts_with("edits: ")) continue;
    else if (edits) {
      Edit e;
      if (!ParseEdit(line, e)) {
        error = "can't read the edit \"" + line + "\"";
        return false;
      }
      edits->push_back(std::move(e));
    }
  }
  return true;
}

}  // namespace
}  // namespace sheettorture

using namespace sheettorture;

bool IsSheetReproducer(const std::filesystem::path& repro) {
  fs::path file;
  uint64_t seed = 0;
  std::string sha, error;
  return ReadReproducer(repro, file, seed, sha, nullptr, error) && IsWorkbookFile(file);
}

int ReplaySheet(const std::filesystem::path& repro, const std::filesystem::path& outDir) {
  fs::path file;
  uint64_t seed = 0;
  std::string sha, error;
  std::vector<Edit> edits;
  if (!ReadReproducer(repro, file, seed, sha, &edits, error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 2;
  }
  file = FindReproFile(file);
  if (auto bytes = ReadBytes(file); !bytes.empty() && !sha.empty() && sha != "unknown" && Hex(fw::pkg::HashBytes(bytes)) != sha)
    std::fprintf(stderr, "Note: %s isn't the file this reproducer was made from (its SHA-256 differs).\n", Display(file).c_str());
  Round r = RunRound(file, seed, 0, &edits, {});
  if (!r.saved.empty()) {
    fs::path made = (outDir.empty() ? repro.parent_path() : outDir) / repro.stem();
    std::error_code ec;
    if (!outDir.empty()) fs::create_directories(outDir, ec);
    fs::path saved = made;
    saved += "-replay" + file.extension().string();
    WriteBytes(saved, r.saved);
    if (!r.edited.empty()) {
      fs::path a = made, b = made;
      a += "-replay-edited.txt", b += "-replay-readback.txt";
      WriteBytes(a, std::vector<uint8_t>(r.edited.begin(), r.edited.end()));
      WriteBytes(b, std::vector<uint8_t>(r.readBack.begin(), r.readBack.end()));
    }
  }
  std::string written;
  for (const std::string& part : r.written) written += (written.empty() ? "" : ", ") + part;
  if (r.failure)
    std::printf("[FAIL] %s: %zu edits (%u done, %u refused): %s: %s\n", Display(file).c_str(), edits.size(), r.done,
                r.refused, r.failure->category.c_str(), r.failure->message.c_str());
  else
    std::printf("[ ok ] %s: %zu edits (%u done, %u refused)\n", Display(file).c_str(), edits.size(), r.done, r.refused);
  std::printf("       parts written: %s\n", written.empty() ? "none" : written.c_str());
  return r.failure ? 1 : 0;
}

int RunSheetTorture(const Options& options, const std::vector<std::filesystem::path>& files, bool appendPairs) {
  const uint64_t baseSeed = options.seed ? options.seed : 1;
  const uint32_t edits = static_cast<uint32_t>(options.edits), rounds = options.rounds;
  const fs::path out = options.outDir.empty() ? fs::path("build") / "torture" : options.outDir;
  std::error_code ec;
  fs::create_directories(out / "saved", ec);
  fs::create_directories(out / "repro", ec);
  // original, saved copy: for the validator and the Excel open check (scripts\torture.ps1)
  std::ofstream pairs(out / "pairs.tsv", appendPairs ? std::ios::app : std::ios::trunc);
  std::string json = "{\n  \"kind\": \"workbooks\", \"seed\": " + std::to_string(baseSeed) + ", \"edits\": " +
                     std::to_string(edits) + ", \"rounds\": " + std::to_string(rounds) + ",\n  \"files\": [";
  int failedFiles = 0, unreadFiles = 0;
  uint64_t totalDone = 0, totalRefused = 0;
  std::string targetName;
  for (size_t fi = 0; fi < files.size(); ++fi) {
    const fs::path& file = files[fi];
    const std::string name = Display(file);
    std::string fileJson = "\n    {\"file\": " + JsonText(fw::WideToUtf8(fs::absolute(file).generic_wstring())) + ", \"failures\": [";
    uint32_t done = 0, refused = 0, failures = 0;
    bool unread = false;
    std::string note, savedPath;
    for (uint32_t round = 0; round < rounds; ++round) {
      const uint64_t seed = RoundSeed(baseSeed, fw::WideToUtf8(file.filename().wstring()), round);
      Round r = RunRound(file, seed, edits, nullptr, out / "current-edits.txt");
      done += r.done, refused += r.refused;
      if (r.failure && r.failure->category == "open") {
        unread = true;  // the model doesn't read the original: S1's reader, not an editing failure
        note = r.failure->message;
        break;
      }
      if (targetName.empty()) {
        fw::pkg::Status st;
        if (auto pkg = OpenPackage(file, st))
          if (auto book = sh::xlsx::Read(std::move(pkg), st)) targetName = OpenSheetTarget(std::move(book), st)->Name();
      }
      if (!r.saved.empty() && savedPath.empty()) {
        const fs::path saved = out / "saved" / (SafeName(file) + file.extension().string());
        WriteBytes(saved, r.saved);
        // Its edits, so that what the validator or Excel finds in it can be replayed.
        WriteEdits(out / "saved" / (SafeName(file) + ".edits.txt"), file, seed, round,
                   r.failure ? r.failure->category + ": " + r.failure->message : "none", r.applied);
        savedPath = fw::WideToUtf8(fs::absolute(saved).generic_wstring());
        pairs << fw::WideToUtf8(fs::absolute(file).generic_wstring()) << "\t" << savedPath << "\n";
      }
      if (!r.failure) continue;
      std::vector<Edit> kept = Minimize(file, seed, r.applied, r.failure->category);
      const fs::path repro = out / "repro" / (SafeName(file) + "-round" + std::to_string(round) + ".txt");
      WriteEdits(repro, file, seed, round, r.failure->category + ": " + r.failure->message, kept);
      std::printf("[FAIL] %s: round %u: %s: %s\n       reproducer (%zu of %zu edits): %s\n", name.c_str(), round,
                  r.failure->category.c_str(), r.failure->message.c_str(), kept.size(), r.applied.size(),
                  Display(repro).c_str());
      fileJson += std::string(failures ? "," : "") + "\n      {\"round\": " + std::to_string(round) + ", \"seed\": " +
                  std::to_string(seed) + ", \"category\": " + JsonText(r.failure->category) + ", \"message\": " +
                  JsonText(r.failure->message) + ", \"edits\": " + std::to_string(kept.size()) + ", \"reproducer\": " +
                  JsonText(fw::WideToUtf8(fs::absolute(repro).generic_wstring())) + "}";
      ++failures;
    }
    totalDone += done, totalRefused += refused;
    if (unread) {
      ++unreadFiles;
      std::printf("[skip] %s: %s\n", name.c_str(), note.c_str());
    } else if (!failures) {
      std::printf("[ ok ] %s: %u rounds, %u edits done, %u refused\n", name.c_str(), rounds, done, refused);
    } else {
      ++failedFiles;
    }
    fileJson += std::string(failures ? "\n    " : "") + "], \"done\": " + std::to_string(done) + ", \"refused\": " +
                std::to_string(refused) + ", \"skipped\": " + (unread ? JsonText(note) : "null") +
                ", \"saved\": " + (savedPath.empty() ? "null" : JsonText(savedPath)) + "}";
    json += fileJson + (fi + 1 < files.size() ? "," : "");
  }
  json += "\n  ]\n}\n";
  fs::path jsonPath = options.jsonOut;
  if (!jsonPath.empty() && appendPairs) jsonPath.replace_filename(jsonPath.stem().wstring() + L"-workbooks.json");
  if (!jsonPath.empty()) std::ofstream(jsonPath, std::ios::trunc) << json;
  fs::remove(out / "current-edits.txt", ec);
  std::printf("\n%zu workbooks, %d failed, %d not read by the model; %llu edits done, %llu refused (edits through: %s)\n",
              files.size(), failedFiles, unreadFiles, static_cast<unsigned long long>(totalDone),
              static_cast<unsigned long long>(totalRefused), targetName.empty() ? "?" : targetName.c_str());
  return failedFiles == 0 ? 0 : 1;
}

}  // namespace fwtool
