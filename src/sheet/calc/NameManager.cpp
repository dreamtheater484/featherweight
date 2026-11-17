#include "sheet/calc/NameManager.h"

#include <algorithm>

#include "pkg/Formula.h"
#include "sheet/calc/Bind.h"
#include "sheet/calc/Engine.h"
#include "sheet/calc/Eval.h"
#include "sheet/calc/Fn.h"
#include "sheet/model/Workbook.h"

namespace fw::sheet::calc {

namespace {

bool AsciiLetter(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
bool Digit(char c) { return c >= '0' && c <= '9'; }

// "A1", "xfd1048576", "$B$2": a cell reference in A1 style (inside the sheet).
bool LooksLikeA1(std::string_view s) {
  size_t i = 0, letters = 0;
  while (i < s.size() && AsciiLetter(s[i])) ++i, ++letters;
  if (letters == 0 || letters > 3 || i == s.size()) return false;
  int64_t col = 0;
  for (size_t k = 0; k < letters; ++k) col = col * 26 + ((s[k] | 32) - 'a' + 1);
  int64_t row = 0;
  for (; i < s.size(); ++i) {
    if (!Digit(s[i])) return false;
    row = row * 10 + (s[i] - '0');
    if (row > 1048576) return false;
  }
  return col <= 16384 && row >= 1;
}

// "R", "C", "R1C1", "RC", "R12", "C3": R1C1-style references.
bool LooksLikeR1C1(std::string_view s) {
  size_t i = 0;
  auto digits = [&]() {
    while (i < s.size() && Digit(s[i])) ++i;
  };
  bool r = i < s.size() && (s[i] | 32) == 'r';
  if (r) ++i, digits();
  bool c = i < s.size() && (s[i] | 32) == 'c';
  if (c) ++i, digits();
  return (r || c) && i == s.size();
}

std::string Shown(const Value& v, const FormulaLocale& region) {
  if (v.IsText()) return "\"" + std::string(v.text()) + "\"";
  if (v.IsError()) return std::string(ErrText(v.error()));
  if (v.IsEmpty()) return "";
  return ValueText(v, region.decimal);
}

}  // namespace

std::vector<NameRow> ListNames(const Workbook& book, WorkbookCalc& calc, const FormulaLocale& region) {
  std::vector<NameRow> rows;
  Engine& engine = calc.engine();
  for (const DefinedName& d : book.Names()) {
    NameRow row;
    row.builtIn = d.name.rfind("_xlnm.", 0) == 0;
    row.name = row.builtIn ? d.name.substr(6) : d.name;
    row.scope = d.sheet;
    row.hidden = d.hidden;
    CompileOptions at;
    at.sheet = d.sheet >= 0 ? d.sheet : 0;
    at.array = true;  // a range's values, not one of them
    auto f = Compile(d.formula, at, engine);
    if (!f) {
      row.refersTo = "=" + ToLocal(d.formula, region);
      row.value = "#NAME?";
      rows.push_back(std::move(row));
      continue;
    }
    row.refersTo = ShowFormula(*f, at, engine, region);
    Evaluator ev(calc, &engine);
    Value v;
    Cell cell{at.sheet, 0, 0};
    if (ev.Evaluate(*f, cell, v) != EvalResult::kDone) {
      row.value = "";
    } else if (v.IsRef() && v.type() == Type::Ref) {
      const Area& a = v.area();
      if (a.r1 == a.r2 && a.c1 == a.c2) {
        row.value = Shown(FromView(calc.Cell(a.s1, a.r1, a.c1)), region);
      } else {
        std::string s = "{";
        int shown = 0;
        for (int32_t r = a.r1; r <= a.r2 && shown < 64; ++r)
          for (int32_t c = a.c1; c <= a.c2 && shown < 64; ++c, ++shown) {
            if (r > a.r1 || c > a.c1) s += c > a.c1 ? std::string(1, region.arrayCol) : std::string(1, region.arrayRow);
            s += Shown(FromView(calc.Cell(a.s1, r, c)), region);
          }
        row.value = s + (shown >= 64 ? "...}" : "}");
      }
    } else if (v.IsRef()) {
      row.value = "{...}";
    } else if (v.IsArray()) {
      const Array& a = v.array();
      std::string s = "{";
      for (int32_t r = 0; r < a.rows(); ++r)
        for (int32_t c = 0; c < a.cols(); ++c) {
          if (r || c) s += c ? std::string(1, region.arrayCol) : std::string(1, region.arrayRow);
          s += Shown(a.at(r, c), region);
        }
      row.value = s + "}";
    } else {
      row.value = Shown(v, region);
    }
    rows.push_back(std::move(row));
  }
  std::stable_sort(rows.begin(), rows.end(), [](const NameRow& a, const NameRow& b) {
    std::string x = a.name, y = b.name;
    for (char& ch : x) ch = static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch + 32 : ch);
    for (char& ch : y) ch = static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch + 32 : ch);
    return x != y ? x < y : a.scope < b.scope;
  });
  return rows;
}

std::u16string CheckName(const Workbook& book, std::string_view name, int scope, std::string_view oldName) {
  const std::u16string invalid = u"The name that you entered is not valid.";
  if (name.empty() || name.size() > 255) return invalid;
  char first = name[0];
  if (!(AsciiLetter(first) || first == '_' || first == '\\' || static_cast<unsigned char>(first) >= 0x80)) return invalid;
  for (char ch : name)
    if (!(AsciiLetter(ch) || Digit(ch) || ch == '_' || ch == '.' || ch == '\\' || ch == '?' ||
          static_cast<unsigned char>(ch) >= 0x80))
      return invalid;
  if (LooksLikeA1(name) || LooksLikeR1C1(name)) return invalid;
  if (!oldName.empty() && NameEqualsNoCase(name, oldName)) return {};
  for (const DefinedName& d : book.Names())
    if (d.sheet == scope && NameEqualsNoCase(d.name, name)) return u"The name entered already exists. Enter a unique name.";
  return {};
}

bool ReadRefersTo(std::string_view typed, const FormulaLocale& region, const CompileOptions& at, Names& names,
                  std::string& formula, std::string& message) {
  std::string_view t = typed;
  while (!t.empty() && t.front() == ' ') t.remove_prefix(1);
  if (t.empty() || t.front() != '=') {  // a constant, as Excel stores it: a number, else text
    bool number = !t.empty();
    std::string n;
    for (size_t i = 0; i < t.size(); ++i) {
      char ch = t[i];
      if (Digit(ch)) n += ch;
      else if (ch == region.decimal && n.find('.') == std::string::npos) n += '.';
      else if (ch == '-' && i == 0) n += ch;
      else number = false;
    }
    if (number && !n.empty() && n != "-" && n != ".") {
      formula = n;
      return true;
    }
    formula = "\"";
    for (char ch : t) formula += ch == '"' ? std::string("\"\"") : std::string(1, ch);
    formula += "\"";
    return true;
  }
  std::string text(t);
  EnteredFormula e = EnterFormula(text, region, at, names);
  if (!e.formula) {
    message = e.message;
    return false;
  }
  formula = e.text.substr(1);
  return true;
}

std::string RangeFormula(const Workbook& book, int sheet, const Range& cells) {
  auto cell = [](uint32_t r, uint32_t c) { return "$" + ColumnName(static_cast<int32_t>(c)) + "$" + std::to_string(r + 1); };
  std::string s = pkg::QuoteSheetName(book.GetSheet(static_cast<size_t>(sheet)).Name()) + "!" + cell(cells.firstRow, cells.firstCol);
  if (cells.lastRow != cells.firstRow || cells.lastCol != cells.firstCol) s += ":" + cell(cells.lastRow, cells.lastCol);
  return s;
}

std::string NameFromLabel(std::string_view label) {
  auto ok = [](char ch) {
    return AsciiLetter(ch) || Digit(ch) || ch == '_' || ch == '.' || ch == '\\' || ch == '?' ||
           static_cast<unsigned char>(ch) >= 0x80;
  };
  size_t a = 0, b = label.size();
  while (a < b && !ok(label[a])) ++a;
  while (b > a && !ok(label[b - 1])) --b;
  std::string s;
  for (size_t i = a; i < b; ++i) s += ok(label[i]) ? label[i] : '_';
  if (s.empty()) return s;
  if (Digit(s[0]) || s[0] == '.' || s[0] == '?') s.insert(s.begin(), '_');
  if (s.size() > 255) s.resize(255);
  if (LooksLikeA1(s)) return s + "_";
  if (LooksLikeR1C1(s)) return s.find_first_of("0123456789") == std::string::npos ? s + "_" : "_" + s;
  return s;
}

namespace {

std::string LabelAt(const Sheet& s, uint32_t row, uint32_t col) {
  CellInfo c;
  if (!s.GetCell(row, col, c) || c.value.type != ValueType::String) return {};
  return std::string(c.value.text);
}

}  // namespace

std::vector<LabelName> NamesFromLabels(const Workbook& book, int sheet, const Range& cells, bool top, bool left,
                                       bool bottom, bool right) {
  std::vector<LabelName> out;
  if (sheet < 0 || static_cast<size_t>(sheet) >= book.SheetCount()) return out;
  const Sheet& s = book.GetSheet(static_cast<size_t>(sheet));
  // The cells the labels name: the range without its label lines.
  Range body = cells;
  if (top) ++body.firstRow;
  if (bottom && body.lastRow > 0) --body.lastRow;
  if (left) ++body.firstCol;
  if (right && body.lastCol > 0) --body.lastCol;
  if (body.firstRow > body.lastRow || body.firstCol > body.lastCol || body.lastRow < cells.firstRow ||
      body.lastCol < cells.firstCol)
    return out;
  auto add = [&](const std::string& label, const Range& r) {
    std::string name = NameFromLabel(label);
    if (name.empty()) return;
    for (auto it = out.begin(); it != out.end(); ++it)
      if (NameEqualsNoCase(it->name, name)) {
        out.erase(it);
        break;
      }
    out.push_back({name, RangeFormula(book, sheet, r)});
  };
  // Where two label lines meet: the corner names the body.
  if ((top || bottom) && (left || right)) {
    const uint32_t r = top ? cells.firstRow : cells.lastRow, c = left ? cells.firstCol : cells.lastCol;
    add(LabelAt(s, r, c), body);
  }
  for (bool first : {true, false}) {
    if (first ? top : bottom)
      for (uint32_t c = body.firstCol; c <= body.lastCol; ++c)
        add(LabelAt(s, first ? cells.firstRow : cells.lastRow, c), {body.firstRow, c, body.lastRow, c});
  }
  for (bool first : {true, false}) {
    if (first ? left : right)
      for (uint32_t r = body.firstRow; r <= body.lastRow; ++r)
        add(LabelAt(s, r, first ? cells.firstCol : cells.lastCol), {r, body.firstCol, r, body.lastCol});
  }
  return out;
}

void GuessLabels(const Workbook& book, int sheet, const Range& cells, bool& top, bool& left) {
  top = left = false;
  if (sheet < 0 || static_cast<size_t>(sheet) >= book.SheetCount()) return;
  const Sheet& s = book.GetSheet(static_cast<size_t>(sheet));
  if (cells.lastRow > cells.firstRow)
    for (uint32_t c = cells.firstCol + (cells.lastCol > cells.firstCol ? 1 : 0); c <= cells.lastCol && !top; ++c)
      top = !LabelAt(s, cells.firstRow, c).empty();
  if (cells.lastCol > cells.firstCol)
    for (uint32_t r = cells.firstRow + (cells.lastRow > cells.firstRow ? 1 : 0); r <= cells.lastRow && !left; ++r)
      left = !LabelAt(s, r, cells.firstCol).empty();
}

}  // namespace fw::sheet::calc
