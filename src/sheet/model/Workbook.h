#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "pkg/Package.h"
#include "pkg/Status.h"
#include "sheet/model/Sheet.h"
#include "sheet/model/Styles.h"
#include "sheet/model/TextPool.h"
#include "sheet/model/Types.h"

namespace draw {
class Theme;
}

// The workbook model (plan §7.3): sheets of cells, the shared strings, the formats, defined names
// and calculation settings. The xlsx reader (sheet/xlsx/Xlsx.h) and the CSV reader fill it; the
// grid view, the number formats and the formula engine read it.
//
// Loading is lazy (plan §4.3 rule 8): opening reads the workbook, the styles, the shared strings
// and the active sheet; Workbook::LoadSheet reads the others when they are needed or at idle.
namespace fw::sheet {

// A run of rich text in a shared string: from `start` (a byte offset into its UTF-8 text) to the
// next run's start, in `font` (an index into StringTable::Fonts()), or the cell's font for kNone.
struct RichRun {
  uint32_t start = 0;
  uint32_t font = kNone;
};

// The workbook's shared strings (xl/sharedStrings.xml): plain and rich text, by id.
class StringTable {
 public:
  uint32_t Size() const { return text_.Size(); }
  std::string_view Text(uint32_t id) const { return text_.Get(id); }
  // The rich text runs of a string; empty for plain text.
  std::span<const RichRun> Runs(uint32_t id) const;
  const std::vector<struct Font>& Fonts() const { return fonts_; }
  // Adds a plain string (CSV, recalculation); returns its id.
  uint32_t Add(std::string_view text);
  size_t Bytes() const;

  // ---- For the reader and writer: how each string was written.
  enum Flag : uint8_t { kPreserve = 1, kRaw = 2 };
  uint8_t Flags(uint32_t id) const { return id < flags_.size() ? flags_[id] : 0; }
  // A string written as something other than <si><t>text</t></si>: its content as written.
  std::string_view RawXml(uint32_t id) const;
  uint32_t AddRead(std::string_view text, uint8_t flags, std::string_view rawXml, std::vector<RichRun> runs);
  uint32_t AddFont(const struct Font& f);
  void Reserve(size_t strings, size_t bytes) { text_.Reserve(strings, bytes); flags_.reserve(strings); }
  void Shrink() { text_.Shrink(); flags_.shrink_to_fit(); }
  bool HasRoom(size_t more) const { return text_.HasRoom(more) && raw_.HasRoom(more); }
  // The part around the strings, as written: the writer puts it back. `kept` holds what sits
  // before the string with that id (or at the end) that isn't a string: comments, extLst.
  struct PartText {
    std::string prolog, rootStart, rootEnd, epilog;
    std::vector<std::pair<uint32_t, std::string>> kept;
  };
  const PartText& Part() const { return part_; }
  PartText& Part() { return part_; }

 private:
  TextPool text_;
  std::vector<uint8_t> flags_;
  struct Rich {
    uint32_t id, raw, firstRun, runCount;
  };
  std::vector<Rich> rich_;  // by id
  std::vector<RichRun> runs_;
  TextPool raw_;
  std::vector<struct Font> fonts_;
  std::unordered_map<std::string, uint32_t> fontIndex_;
  PartText part_;
};

struct DefinedName {
  std::string name;     // "_xlnm.Print_Area" for the built-in names
  int sheet = -1;       // localSheetId: the sheet it belongs to; -1 for the workbook
  std::string formula;  // as written, without "="
  bool hidden = false;
};

enum class CalcMode : uint8_t { Auto, Manual, AutoNoTable };

// calcPr.
struct CalcProps {
  uint32_t calcId = 0;
  CalcMode mode = CalcMode::Auto;
  bool fullCalcOnLoad = false, forceFullCalc = false, iterate = false, fullPrecision = true, calcOnSave = true,
       concurrentCalc = true, r1c1 = false;
  uint32_t iterateCount = 100, concurrentManualCount = 0;
  double iterateDelta = 0.001;
  // The calculation features the workbook's extLst lists (xcalcf:feature), names as written:
  // "microsoft.com:RD", "microsoft.com:Single", ... Read only.
  std::vector<std::string> calcFeatures;
};

enum class FileFormat : uint8_t { Xlsx, Xlsm, Xltx, Xltm, Csv };

class Workbook {
 public:
  Workbook();
  ~Workbook();
  Workbook(const Workbook&) = delete;
  Workbook& operator=(const Workbook&) = delete;

  FileFormat Format() const { return format_; }

  // ---- Sheets, in tab order (hidden ones included)
  size_t SheetCount() const { return sheets_.size(); }
  Sheet& GetSheet(size_t i) { return *sheets_[i]; }
  const Sheet& GetSheet(size_t i) const { return *sheets_[i]; }
  // By name, without case (as Excel compares sheet names); -1 when there is none.
  int FindSheet(std::string_view name) const;
  // A table by its name (displayName, without case), on any sheet; null when there is none. `sheet`
  // receives the sheet's index.
  const Table* FindTable(std::string_view name, size_t* sheet = nullptr) const;
  // The tab shown when the file opens, and the first tab the tab bar shows.
  uint32_t ActiveSheet() const { return activeSheet_; }
  uint32_t FirstVisibleTab() const { return firstTab_; }
  // Reads a sheet that isn't loaded yet; does nothing for a loaded one. Not while another thread
  // reads that sheet.
  pkg::Status LoadSheet(size_t i);
  pkg::Status LoadAll();

  // ---- Workbook-wide
  bool Date1904() const { return date1904_; }
  const CalcProps& Calc() const { return calc_; }
  const std::vector<DefinedName>& Names() const { return names_; }
  // Protect Workbook: the workbookProtection element as the file has it or an edit made it (without
  // children); empty when there's none. sheet/edit/Protection.h reads it.
  const std::string& ProtectionXml() const { return protectionXml_; }
  // A defined name for a sheet (sheet -1: the workbook's), without case; null when there is none.
  // A sheet's own name hides the workbook's of the same name.
  const DefinedName* FindName(std::string_view name, int sheet = -1) const;
  const Styles& GetStyles() const { return styles_; }
  Styles& GetStyles() { return styles_; }
  const StringTable& Strings() const { return strings_; }
  StringTable& Strings() { return strings_; }
  // The theme (fonts and colors); Office's when the file has none.
  const std::shared_ptr<const draw::Theme>& Theme() const;
  // The error a cell's value metadata (vm, from 1) stands for: Excel writes #SPILL!, #CALC! and the
  // other newer errors as <v>#VALUE!</v> with a rich value (xl/metadata.xml, xl/richData) that says
  // which. Nothing when that vm isn't an error.
  std::optional<ErrorCode> ValueError(uint32_t vm) const;
  // Whether a cell's cell metadata (cm, from 1) makes its array formula a dynamic array (XLDAPR
  // with fDynamic="1"). Sheet::GetFormula says it as FormulaInfo::dynamic.
  bool DynamicArray(uint32_t cm) const { return cm >= 1 && cm <= dynamicArrays_.size() && dynamicArrays_[cm - 1]; }

  // ---- Building (CSV, tests)
  Sheet& AddSheet(std::string name);
  // A CSV file read again in full on a worker (S4: its first rows were shown meanwhile): the
  // first sheet's cells, the strings and the styles become `other`'s, and `other` gets these.
  void SwapCells(Workbook& other);
  void SetFormat(FileFormat f) { format_ = f; }
  void SetDate1904(bool b) { date1904_ = b; }

  // ---- The file behind it (xlsx), for the parts other pieces read and for the writer.
  const pkg::Package* GetPackage() const { return package_.get(); }
  pkg::Package* GetPackage() { return package_.get(); }
  const std::string& WorkbookPart() const { return workbookPart_; }  // "/xl/workbook.xml"
  const std::string& StylesPart() const { return stylesPart_; }
  const std::string& StringsPart() const { return stringsPart_; }
  // The relationship target of a part's relationship id, as a part name ("" when it's missing).
  std::string ResolveRel(std::string_view sourcePart, std::string_view relId) const;

  size_t ModelBytes() const;

 private:
  friend class WorkbookLoader;
  friend class edit::Access;
  FileFormat format_ = FileFormat::Xlsx;
  std::vector<std::unique_ptr<Sheet>> sheets_;
  uint32_t activeSheet_ = 0, firstTab_ = 0;
  bool date1904_ = false;
  CalcProps calc_;
  std::vector<DefinedName> names_;
  std::string protectionXml_;
  std::vector<uint8_t> valueErrors_;  // by vm - 1: an ErrorCode, or 0xFF
  std::vector<uint8_t> dynamicArrays_;  // by cm - 1: 1 for a dynamic array
  Styles styles_;
  StringTable strings_;
  mutable std::shared_ptr<const draw::Theme> theme_;
  std::unique_ptr<pkg::Package> package_;
  std::string workbookPart_, stylesPart_, stringsPart_, themePart_;
  std::function<pkg::Status(Workbook&, size_t)> loader_;
};

// Gives the readers access to a workbook's insides.
class WorkbookLoader {
 public:
  explicit WorkbookLoader(Workbook& b) : b_(b) {}
  void SetPackage(std::unique_ptr<pkg::Package> p) { b_.package_ = std::move(p); }
  void SetParts(std::string workbook, std::string styles, std::string strings, std::string theme) {
    b_.workbookPart_ = std::move(workbook), b_.stylesPart_ = std::move(styles), b_.stringsPart_ = std::move(strings),
    b_.themePart_ = std::move(theme);
  }
  void SetActive(uint32_t active, uint32_t first) { b_.activeSheet_ = active, b_.firstTab_ = first; }
  CalcProps& Calc() { return b_.calc_; }
  std::vector<DefinedName>& Names() { return b_.names_; }
  std::string& ProtectionXml() { return b_.protectionXml_; }
  std::vector<uint8_t>& ValueErrors() { return b_.valueErrors_; }
  std::vector<uint8_t>& DynamicArrays() { return b_.dynamicArrays_; }
  void SetLoader(std::function<pkg::Status(Workbook&, size_t)> f) { b_.loader_ = std::move(f); }
  void SetTheme(std::shared_ptr<const draw::Theme> t) { b_.theme_ = std::move(t); }
  static void SetSheetInfo(Sheet& s, SheetKind kind, SheetState state, uint32_t sheetId, std::string part,
                           std::string relId);
  static void MarkLoaded(Sheet& s, bool loaded);
  static std::vector<Table>& Tables(Sheet& s);

 private:
  Workbook& b_;
};

}  // namespace fw::sheet
