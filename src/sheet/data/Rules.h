#pragma once

// Conditional formats and data validation (s3-data): a sheet's rules read from its XML into lists
// that Conditional Formatting, Manage Rules and Data Validation edit, and written back as Excel
// writes them, for Editor::SetSheetRules. A rule the user doesn't edit keeps its XML as written
// (only its priority follows the order). Formulas are kept in the invariant form, without "=".

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "sheet/model/Styles.h"
#include "sheet/model/Types.h"

namespace fw::sheet {
struct Table;
namespace detail {
struct PartText;
}
namespace calc {
struct FormulaLocale;
}
}  // namespace fw::sheet

namespace fw::sheet::data {

// ---- Sqrefs: space-separated areas ("A1:B5 D2").
std::vector<Range> ParseAreas(std::string_view sqref);
std::string JoinAreas(const std::vector<Range>& areas);
bool AreasContain(const std::vector<Range>& areas, uint32_t row, uint32_t col);
// `areas` without the cells of `cut`, as few rectangles as it takes.
std::vector<Range> CutAreas(const std::vector<Range>& areas, const Range& cut);

// ---- Conditional formats

struct CfRule {
  std::string sqref;  // the cells it applies to
  std::string type;   // Excel's: cellIs, expression, containsText, timePeriod, top10, dataBar...
  bool stopIfTrue = false;
  std::string xml;    // the cfRule element (its priority and stopIfTrue are written from the list); empty: x14 only
  std::string ext;    // its x14:cfRule (a data bar's extras, an x14 icon set); empty: none
};

// The sheet's rules, the one that applies first first.
std::vector<CfRule> ReadCfRules(const detail::PartText& t);
// The XML for Editor::SetSheetRules (ConditionalFormats) on the sheet whose text `t` is; priorities
// follow the list's order.
void WriteCfRules(const std::vector<CfRule>& rules, const detail::PartText& t, std::string& xml, std::string& ext);

// Excel's formats for highlight rules (its "Format cells that..." list).
inline constexpr const wchar_t* kCfPresets[] = {
    L"Light Red Fill with Dark Red Text", L"Yellow Fill with Dark Yellow Text", L"Green Fill with Dark Green Text",
    L"Light Red Fill", L"Red Text", L"Bold"};
Dxf CfPreset(int preset);

// New rules as Excel writes them; `dxf` is the format's index (Editor::AddDxf). Formulas and values
// are invariant, without "=". Relative references are from the sqref's top-left cell.
enum class CfOp : uint8_t { Between, NotBetween, Equal, NotEqual, Greater, Less, GreaterEqual, LessEqual };
CfRule CellIsRule(const std::string& sqref, CfOp op, const std::string& f1, const std::string& f2, uint32_t dxf);
CfRule ExpressionRule(const std::string& sqref, const std::string& formula, uint32_t dxf);
enum class CfText : uint8_t { Contains, NotContains, BeginsWith, EndsWith };
CfRule TextRule(const std::string& sqref, CfText kind, const std::string& text, uint32_t dxf);
// Excel's periods, in its order: yesterday, today, tomorrow, last 7 days, last week, this week,
// next week, last month, this month, next month.
inline constexpr const wchar_t* kCfPeriods[] = {L"Yesterday", L"Today", L"Tomorrow", L"In the last 7 days",
                                                L"Last week", L"This week", L"Next week", L"Last month",
                                                L"This month", L"Next month"};
CfRule DateRule(const std::string& sqref, int period, uint32_t dxf);
CfRule DuplicateRule(const std::string& sqref, bool unique, uint32_t dxf);
CfRule TopRule(const std::string& sqref, uint32_t rank, bool percent, bool bottom, uint32_t dxf);
CfRule AverageRule(const std::string& sqref, bool above, uint32_t dxf);
// Excel's galleries.
inline constexpr const wchar_t* kColorScales[] = {
    L"Green - Yellow - Red", L"Red - Yellow - Green", L"Green - White - Red", L"Red - White - Green",
    L"Blue - White - Red",   L"Red - White - Blue",   L"White - Red",         L"Red - White",
    L"Green - White",        L"White - Green",        L"Green - Yellow",      L"Yellow - Green"};
CfRule ColorScaleRule(const std::string& sqref, int preset);
inline constexpr const wchar_t* kDataBars[] = {L"Blue",       L"Green",          L"Red",          L"Orange",
                                               L"Light Blue", L"Purple",         L"Blue (solid)", L"Green (solid)",
                                               L"Red (solid)", L"Orange (solid)", L"Light Blue (solid)",
                                               L"Purple (solid)"};
// `id`: the GUID that ties the rule to its x14 part ("{...}"); empty makes one.
CfRule DataBarRule(const std::string& sqref, int preset, std::string id = {});
inline constexpr const char* kIconSets[] = {"3Arrows",  "3ArrowsGray",    "4Arrows",        "4ArrowsGray", "5Arrows",
                                            "5ArrowsGray", "3Triangles",  "3TrafficLights1", "3TrafficLights2",
                                            "3Signs",   "4TrafficLights", "4RedToBlack",    "3Symbols",    "3Symbols2",
                                            "3Flags",   "3Stars",         "4Rating",        "5Quarters",   "5Rating",
                                            "5Boxes"};
CfRule IconSetRule(const std::string& sqref, int preset, std::string id = {});
// For Manage Rules: what the rule does, in Excel's words, with formulas in the region's form.
std::wstring DescribeCf(const CfRule& rule, const calc::FormulaLocale& region);
// Clear Rules from the cells of `cut`: each rule's cells without them; rules left with none go.
void ClearCfRules(std::vector<CfRule>& rules, const Range& cut);
// A copy of the rule applying to `sqref` (Applies to).
void SetCfSqref(CfRule& rule, const std::string& sqref);
// A rule copied to other cells (pasting): its cells `sqref`, each formula through `move` (invariant,
// without "="; relative references re-based on the new top-left cell), a data bar's or x14 rule's id new.
using MoveFormula = std::string (*)(void* ctx, std::string_view formula);
CfRule CopyCfRule(const CfRule& rule, const std::string& sqref, MoveFormula move, void* ctx);

// ---- Tables

// A formula (invariant, without "=") at `row` of a sheet with its references to table `t` (on the
// sheet `sheetName`) written as cells, as Excel's Convert to Range writes them: Sales[Amount] ->
// Sheet1!$B$2:$B$4, [@Amount] -> Sheet1!$B2, Sales[[#Totals],[Amount]] -> Sheet1!$B$5. `inside`:
// the formula is in the table, where [Amount] without the name is its. Unchanged when it has none.
std::string Unstructure(std::string_view formula, const Table& t, const std::string& sheetName, uint32_t row, bool inside);

// ---- Data validation

struct Validation {
  enum class Type : uint8_t { Any, Whole, Decimal, List, Date, Time, TextLength, Custom };
  enum class Alert : uint8_t { Stop, Warning, Information };
  Type type = Type::Any;
  CfOp op = CfOp::Between;
  std::string f1, f2;  // invariant, without "="; a typed list as Excel stores it: "\"a,b,c\""
  bool allowBlank = true, dropDown = true, showInput = true, showError = true;
  Alert alert = Alert::Stop;
  std::string promptTitle, prompt, errorTitle, error;
  std::string sqref;
  std::string xml;  // as written; empty: write from the fields (a new or edited one)
  bool x14 = false; // read from Excel's x14 extension (a list or formula on another sheet)
};

std::vector<Validation> ReadValidations(const detail::PartText& t);
// The XML for Editor::SetSheetRules (Validations): rules whose formulas refer to another sheet go
// to the x14 extension, as Excel writes them.
void WriteValidations(const std::vector<Validation>& rules, const detail::PartText& t, std::string& xml,
                      std::string& ext);
// The rule for a cell (the first whose cells hold it), or null.
const Validation* ValidationAt(const std::vector<Validation>& rules, uint32_t row, uint32_t col);
// A typed list's items ("\"a,b,c\"" -> a, b, c); empty when the list is a reference or formula.
std::vector<std::string> ListItems(const Validation& v);
// Whether a value (as General shows it) is one of a typed list's items, as Excel 2021 checks it:
// with case, spaces around the item or the value ignored.
bool InList(const std::vector<std::string>& items, std::string_view value);
// Clear All from the cells of `cut`.
void ClearValidations(std::vector<Validation>& rules, const Range& cut);
// A rule copied to other cells (pasting), as CopyCfRule.
Validation CopyValidation(const Validation& v, const std::string& sqref, MoveFormula move, void* ctx);

}  // namespace fw::sheet::data
