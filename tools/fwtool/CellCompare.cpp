// fwtool compare --cells: S1's displayed cell text against Excel's (docs/s1-plan.md, "Corpus and
// measuring"). scripts/reference-renders.ps1 writes a cells file beside each workbook's PDF: per
// sheet the hidden rows and columns and every non-empty cell's text as Excel shows it
// (Range.Text). Here each workbook is read into the model and each of those cells formatted with
// the number-format engine, in the locale Excel recorded, then compared.
//
// The 99 % target counts the visible cells. Two kinds of difference depend on the column width,
// which the grid measures (s1-grid): "####" and General shortened to fit a narrow column. Until a
// width measure is plugged in (FitsCell below), they're counted on their own. A `*` fill matches
// when Excel's text is ours with the fill character repeated at that place. A formula without a
// cached value needs the engine to show anything, so it's counted apart too.
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include "Common.h"
#include "core/Json.h"
#include "core/Strings.h"
#include "sheet/model/Sheet.h"
#include "sheet/model/Workbook.h"
#include "sheet/numfmt/NumFormat.h"
#include "sheet/numfmt/NumLocale.h"
#include "sheet/xlsx/Xlsx.h"

namespace fwtool {

std::unique_ptr<fw::sheet::Workbook> ReadWorkbook(const std::filesystem::path& file, fw::pkg::Status& status, bool loadAll);

namespace {

using fw::JsonValue;
namespace sh = fw::sheet;

struct Counts {
  size_t cells = 0;          // visible cells compared (the target's denominator)
  size_t same = 0;           // ... showing Excel's text
  size_t fillSame = 0;       // ... of which matched with a `*` fill
  size_t widthOnly = 0;      // different only in what the column width decides ("####", General shortened)
  size_t hidden = 0;         // in hidden rows or columns: not compared
  size_t uncalculated = 0;   // formulas without a cached value: not compared
  size_t missing = 0;        // in Excel's cells file but not in the model
  size_t recalculated = 0;   // formulas Excel recalculated on opening (volatile, stale): not compared
  void Add(const Counts& o) {
    cells += o.cells; same += o.same; fillSame += o.fillSame; widthOnly += o.widthOnly; hidden += o.hidden;
    uncalculated += o.uncalculated; missing += o.missing; recalculated += o.recalculated;
  }
};

struct Difference {
  std::string sheet, ref, excel, ours, format;
};

bool ReadText(const std::filesystem::path& file, std::string& out) {
  std::ifstream in(file, std::ios::binary);
  if (!in) return false;
  std::ostringstream s;
  s << in.rdbuf();
  out = s.str();
  return true;
}

std::wstring W(std::string_view s) { return fw::Utf8ToWide(s); }

// The locale Excel formatted with, from the cells file: Windows' format of that name, with the
// separators, patterns and currency Excel reported.
sh::NumLocale LocaleOf(const JsonValue* loc) {
  std::string name = loc ? loc->StringOr("windowsLocaleName", "en-US") : "en-US";
  sh::WinLocaleInfo info = name == "en-NL" ? sh::EnNlInfo() : name == "en-US" ? sh::EnUsInfo() : sh::ReadWinLocale(W(name));
  if (loc) {
    auto set = [&](std::wstring& field, const char* key) {
      if (const JsonValue* v = loc->Find(key); v && v->type == JsonValue::Type::String && !v->string.empty()) field = W(v->string);
    };
    set(info.sDecimal, "decimalSeparator");
    set(info.sThousand, "thousandsSeparator");
    set(info.sList, "listSeparator");
    set(info.sDate, "dateSeparator");
    set(info.sTime, "timeSeparator");
    set(info.sShortDate, "windowssShortDate");
    set(info.sLongDate, "windowssLongDate");
    set(info.sTimeFormat, "windowssTimeFormat");
    set(info.sShortTime, "windowssShortTime");
    set(info.s1159, "windowss1159");
    set(info.s2359, "windowss2359");
    set(info.sCurrency, "currencySymbol");
    const JsonValue* before = loc->Find("currencyBefore");
    const JsonValue* space = loc->Find("currencySpace");
    if (before && space) info.iCurrency = before->boolean ? (space->boolean ? 2 : 0) : (space->boolean ? 3 : 1);
  }
  return sh::MakeLocale(info);
}

// "3", "5:7" (rows) or "C", "E:F" (columns) into a set of 0-based indexes.
std::set<uint32_t> Runs(const JsonValue* list, bool columns) {
  std::set<uint32_t> out;
  if (!list) return out;
  auto one = [&](std::string_view s) -> uint32_t {
    if (!columns) return static_cast<uint32_t>(std::strtoul(std::string(s).c_str(), nullptr, 10)) - 1;
    uint32_t c = 0;
    for (char ch : s) c = c * 26 + static_cast<uint32_t>(ch - 'A' + 1);
    return c - 1;
  };
  for (const JsonValue& v : list->array) {
    std::string_view s = v.string;
    size_t colon = s.find(':');
    uint32_t a = one(s.substr(0, colon)), b = colon == std::string_view::npos ? a : one(s.substr(colon + 1));
    for (uint32_t i = a; i <= b && i - a < 2000000; ++i) out.insert(i);
  }
  return out;
}

bool SameJson(const JsonValue& a, const JsonValue& b) {
  if (a.type != b.type) return false;
  switch (a.type) {
    case JsonValue::Type::Number: return a.number == b.number;
    case JsonValue::Type::String: return a.string == b.string;
    case JsonValue::Type::Bool: return a.boolean == b.boolean;
    case JsonValue::Type::Object: return a.StringOr("e") == b.StringOr("e");
    default: return true;
  }
}

bool AllHashes(std::string_view s) { return !s.empty() && s.find_first_not_of('#') == std::string_view::npos; }

// What the cell shows, as Range.Text gives it, without a column width; `fill` is set to the `*`
// fill's place and character when there is one.
std::string Show(const sh::CellInfo& cell, const sh::Workbook& book, const sh::FormatContext& ctx, const sh::NumLocale& locale,
                 std::map<uint32_t, sh::NumberFormat>& formats, int& fillAt, wchar_t& fillChar, std::string& code) {
  const sh::Styles& styles = book.GetStyles();
  uint32_t id = styles.CellXf(cell.style).numFmt;
  auto it = formats.find(id);
  if (it == formats.end()) {
    std::string_view custom = styles.FormatCode(id);
    it = formats.emplace(id, custom.empty() ? sh::NumberFormat::Builtin(static_cast<int>(id), locale) : sh::NumberFormat(W(custom))).first;
  }
  std::string_view custom = styles.FormatCode(id);
  code = custom.empty() ? "#" + std::to_string(id) : std::string(custom);
  sh::Formatted f;
  switch (cell.value.type) {
    case sh::ValueType::Number: it->second.Format(cell.value.number, ctx, f); break;
    case sh::ValueType::String: it->second.Format(W(cell.value.text), ctx, f); break;
    case sh::ValueType::Bool: return cell.value.boolean ? "TRUE" : "FALSE";
    case sh::ValueType::Error: return std::string(sh::ErrorText(cell.value.error));
    default: return {};
  }
  fillChar = f.fillChar;
  if (f.fill >= 0 && static_cast<size_t>(f.fill) <= f.text.size()) {
    // The fill's place as a UTF-8 offset (Formatted counts UTF-16 units).
    fillAt = static_cast<int>(fw::WideToUtf8(f.text.substr(0, static_cast<size_t>(f.fill))).size());
  } else {
    fillAt = -1;
  }
  return fw::WideToUtf8(f.text);
}

// Excel's text is ours with the fill character repeated (0 or more times) at the fill's place.
bool SameWithFill(std::string_view excel, std::string_view ours, int fillAt, wchar_t fillChar) {
  std::string before = std::string(ours.substr(0, static_cast<size_t>(fillAt))), after = std::string(ours.substr(static_cast<size_t>(fillAt)));
  std::string fill = fw::WideToUtf8(std::wstring(1, fillChar));
  if (excel.size() < before.size() + after.size() || excel.substr(0, before.size()) != before ||
      excel.substr(excel.size() - after.size()) != after)
    return false;
  std::string_view middle = excel.substr(before.size(), excel.size() - before.size() - after.size());
  for (size_t i = 0; i < middle.size(); i += fill.size())
    if (middle.substr(i, fill.size()) != fill) return false;
  return middle.size() % std::max<size_t>(1, fill.size()) == 0;
}

// A number shown as General (General, or a text format, which shows numbers as General) that
// Excel shortened to fit a narrow column: a shorter number, or scientific.
bool GeneralShortened(const sh::CellInfo& cell, const std::string& code, std::string_view excel, std::string_view ours) {
  return cell.value.type == sh::ValueType::Number && (code == "#0" || code == "General" || code == "#49" || code == "@") &&
         excel.size() < ours.size();
}

struct WorkbookResult {
  std::string source, error;
  Counts counts;
  std::vector<Difference> differences;
};

WorkbookResult CompareWorkbook(const std::filesystem::path& file, const JsonValue& cells, std::string source) {
  WorkbookResult result;
  result.source = std::move(source);
  fw::pkg::Status st;
  auto book = ReadWorkbook(file, st, true);
  if (!book) {
    result.error = "can't read it: " + st.message;
    return result;
  }
  sh::NumLocale locale = LocaleOf(cells.Find("locale"));
  if (!locale.language) locale.language = sh::WindowsLanguage;  // [$-407] and the like, as the app has it
  sh::FormatContext ctx;
  ctx.locale = &locale;
  ctx.date1904 = book->Date1904();
  std::map<uint32_t, sh::NumberFormat> formats;
  const JsonValue* sheets = cells.Find("sheets");
  if (!sheets) return result;
  for (const JsonValue& s : sheets->array) {
    if (s.StringOr("kind") != "worksheet") continue;
    std::string name = s.StringOr("name");
    int index = book->FindSheet(name);
    const JsonValue* list = s.Find("cells");
    if (!list) continue;
    if (index < 0) {
      result.counts.missing += list->array.size();
      continue;
    }
    const sh::Sheet& sheet = book->GetSheet(static_cast<size_t>(index));
    std::set<uint32_t> hiddenRows = Runs(s.Find("hiddenRows"), false), hiddenCols = Runs(s.Find("hiddenColumns"), true);
    // Formulas whose value Excel changed on opening (volatile ones, stale caches): what it shows
    // isn't the cached value, so the text can't be compared before the engine runs.
    std::set<std::string> changedOnOpen;
    if (const JsonValue* fl = s.Find("formulas"))
      for (const JsonValue& f : fl->array)
        if (f.array.size() >= 4 && f.array[2].type != JsonValue::Type::String && !SameJson(f.array[2], f.array[3]))
          changedOnOpen.insert(f.array[0].string);
    for (const JsonValue& c : list->array) {
      if (c.array.size() < 2) continue;
      const std::string& ref = c.array[0].string;
      const std::string& excel = c.array[1].string;
      sh::Range r;
      if (!sh::ParseRange(ref, r)) continue;
      if (hiddenRows.count(r.firstRow) || hiddenCols.count(r.firstCol)) {
        ++result.counts.hidden;
        continue;
      }
      sh::CellInfo info;
      if (!sheet.GetCell(r.firstRow, r.firstCol, info)) {
        ++result.counts.missing;
        continue;
      }
      if (info.hasFormula && changedOnOpen.count(ref)) {
        ++result.counts.recalculated;
        continue;
      }
      if (info.hasFormula && info.value.type == sh::ValueType::Empty) {
        ++result.counts.uncalculated;
        continue;
      }
      ++result.counts.cells;
      int fillAt = -1;
      wchar_t fillChar = 0;
      std::string code;
      std::string ours = Show(info, *book, ctx, locale, formats, fillAt, fillChar, code);
      if (ours == excel) {
        ++result.counts.same;
      } else if (fillAt >= 0 && fillChar != L'#' && SameWithFill(excel, ours, fillAt, fillChar)) {
        ++result.counts.same;
        ++result.counts.fillSame;
      } else if (AllHashes(excel) || GeneralShortened(info, code, excel, ours) || (fillAt >= 0 && fillChar == L'#')) {
        ++result.counts.widthOnly;
      } else if (result.differences.size() < 200) {
        result.differences.push_back({name, ref, excel, ours, code});
      }
    }
  }
  return result;
}

}  // namespace

struct CellText::Impl {
  const sh::Workbook& book;
  const sh::NumLocale& locale;
  sh::FormatContext ctx;
  std::map<uint32_t, sh::NumberFormat> formats;
  Impl(const sh::Workbook& b, const sh::NumLocale& l) : book(b), locale(l) {
    ctx.locale = &locale;
    ctx.date1904 = book.Date1904();
  }
};

CellText::CellText(const sh::Workbook& book, const sh::NumLocale& locale) : impl_(std::make_unique<Impl>(book, locale)) {}
CellText::~CellText() = default;

std::string CellText::operator()(const sh::CellInfo& cell) {
  int fillAt = -1;
  wchar_t fillChar = 0;
  std::string code;
  return Show(cell, impl_->book, impl_->ctx, impl_->locale, impl_->formats, fillAt, fillChar, code);
}

namespace {

double Percent(size_t a, size_t b) { return b ? 100.0 * static_cast<double>(a) / static_cast<double>(b) : 100.0; }

std::string JsonString(std::string_view s) {
  std::string out = "\"";
  for (char ch : s) {
    if (ch == '"' || ch == '\\') { out += '\\'; out += ch; }
    else if (static_cast<unsigned char>(ch) < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", ch); out += b; }
    else out += ch;
  }
  return out + "\"";
}

}  // namespace

int RunCellCompare(const Options& options) {
  if (options.inputs.empty()) {
    std::fprintf(stderr, "fwtool compare --cells <reference folder, its manifest.json, or the corpus folder>\n");
    return 2;
  }
  std::filesystem::path folder = options.inputs[0];
  std::error_code ec;
  if (folder.filename() == L"manifest.json") folder = folder.parent_path();
  if (!std::filesystem::exists(folder / L"manifest.json", ec) && std::filesystem::exists(folder / L"reference" / L"manifest.json", ec))
    folder /= L"reference";
  std::string text;
  if (!ReadText(folder / L"manifest.json", text)) {
    std::fprintf(stderr, "no manifest.json in %s\n", Display(folder).c_str());
    return 2;
  }
  auto manifest = fw::ParseJson(text);
  const JsonValue* files = manifest ? manifest->Find("files") : nullptr;
  if (!files) {
    std::fprintf(stderr, "%s isn't a reference manifest\n", Display(folder / L"manifest.json").c_str());
    return 2;
  }
  std::filesystem::path corpus = fw::Utf8ToWide(manifest->StringOr("corpus"));
  if (corpus.empty() || !std::filesystem::is_directory(corpus, ec)) corpus = folder.parent_path();

  Counts total;
  std::vector<WorkbookResult> results;
  for (const JsonValue& e : files->array) {
    std::string cellsRel = e.StringOr("cells");
    if (cellsRel.empty()) continue;
    std::string source = e.StringOr("source");
    std::string cellsText;
    std::optional<JsonValue> cells;
    if (ReadText(folder / fw::Utf8ToWide(cellsRel), cellsText)) cells = fw::ParseJson(cellsText);
    WorkbookResult r;
    if (!cells) {
      r.source = source;
      r.error = "no cells file " + cellsRel;
    } else {
      r = CompareWorkbook(corpus / fw::Utf8ToWide(source), *cells, source);
    }
    total.Add(r.counts);
    std::printf("%-48s %s\n", r.source.c_str(),
                r.error.empty() ? (std::to_string(r.counts.same) + "/" + std::to_string(r.counts.cells) + " cells (" +
                                   std::to_string(Percent(r.counts.same, r.counts.cells)).substr(0, 5) + " %)" +
                                   (r.counts.widthOnly ? ", " + std::to_string(r.counts.widthOnly) + " by width" : ""))
                                      .c_str()
                                : ("ERROR " + r.error).c_str());
    results.push_back(std::move(r));
  }
  double pct = Percent(total.same, total.cells);
  std::printf("\n%zu workbooks: %zu of %zu visible cells show Excel's text (%.2f %%)\n", results.size(), total.same, total.cells, pct);
  std::printf("  %zu differ only in what the column width decides (\"####\", General shortened): not measured until the grid's widths\n"
              "  %zu matched with a * fill; not compared: %zu hidden, %zu formulas without a cached value, %zu that\n"
              "  Excel recalculated on opening, %zu missing from the model\n",
              total.widthOnly, total.fillSame, total.hidden, total.uncalculated, total.recalculated, total.missing);
  std::vector<const Difference*> shown;
  for (const auto& r : results)
    for (const auto& d : r.differences)
      if (shown.size() < 40) shown.push_back(&d);
  if (!shown.empty()) std::printf("\nFirst differences (Excel | ours | format):\n");
  for (const auto& r : results)
    for (const auto& d : r.differences)
      if (std::find(shown.begin(), shown.end(), &d) != shown.end())
        std::printf("  %s %s!%s: \"%s\" | \"%s\" | %s\n", r.source.c_str(), d.sheet.c_str(), d.ref.c_str(), d.excel.c_str(), d.ours.c_str(),
                    d.format.c_str());

  if (!options.jsonOut.empty()) {
    std::ofstream out(options.jsonOut, std::ios::binary);
    out << "{\n  \"cells\": " << total.cells << ", \"same\": " << total.same << ", \"percent\": " << pct << ", \"widthOnly\": " << total.widthOnly
        << ", \"hidden\": " << total.hidden << ", \"uncalculated\": " << total.uncalculated << ", \"missing\": " << total.missing
        << ",\n  \"workbooks\": [\n";
    for (size_t i = 0; i < results.size(); ++i) {
      const auto& r = results[i];
      out << "    {\"source\": " << JsonString(r.source) << ", \"cells\": " << r.counts.cells << ", \"same\": " << r.counts.same
          << ", \"widthOnly\": " << r.counts.widthOnly << ", \"uncalculated\": " << r.counts.uncalculated;
      if (!r.error.empty()) out << ", \"error\": " << JsonString(r.error);
      out << ", \"differences\": [";
      for (size_t k = 0; k < r.differences.size(); ++k) {
        const auto& d = r.differences[k];
        out << (k ? ", " : "") << "[" << JsonString(d.sheet) << ", " << JsonString(d.ref) << ", " << JsonString(d.excel) << ", "
            << JsonString(d.ours) << ", " << JsonString(d.format) << "]";
      }
      out << "]}" << (i + 1 < results.size() ? "," : "") << "\n";
    }
    out << "  ]\n}\n";
  }
  if (options.minCells >= 0 && pct < options.minCells) {
    std::printf("FAIL: %.2f %% of cells is below %.2f %%\n", pct, options.minCells);
    return 1;
  }
  return 0;
}

}  // namespace fwtool
