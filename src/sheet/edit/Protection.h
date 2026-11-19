#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "sheet/model/Workbook.h"

// Protect Sheet and Protect Workbook (S3, s3-review): the settings as Excel stores them
// (sheetProtection and workbookProtection, ECMA-376 Part 1 §18.3.1.85 and §18.2.29), the password's
// hash, and what Excel 2021 refuses on a protected sheet or workbook. The refusals were measured in
// Excel through automation; the edit layer applies them to every edit (Editor's Guard), and the
// grid to what isn't an edit (selecting locked cells, showing hidden formulas).
namespace fw::sheet::edit {

// A protection password as Excel keeps it: a salted hash, or, in files from Excel 2010 and
// earlier, the old 16-bit one.
struct ProtectionPassword {
  std::string algorithm, hash, salt;  // algorithmName, hashValue, saltValue: "SHA-512", base64
  uint32_t spinCount = 0;
  std::string legacy;                 // password: the 16-bit hash in hex ("CC1A")
  bool Set() const { return !hash.empty() || !legacy.empty(); }
  // True when `typed` is the password; anything is when none is set.
  bool Matches(std::wstring_view typed) const;
  // `password` hashed as Excel 2021 hashes it: SHA-512, a new salt, 100,000 spins. None for "".
  static ProtectionPassword Make(std::wstring_view password);
  bool operator==(const ProtectionPassword&) const = default;
};

// sheetProtection. Each flag is the file's attribute: true protects that part (the action is
// refused). A SheetProtection{} is what Excel's Protect Sheet dialog sets by default; read from a
// file, an attribute left out takes the schema's default instead.
struct SheetProtection {
  bool sheet = true;  // the locked cells: an element without it doesn't protect the sheet
  bool objects = true, scenarios = true;
  bool formatCells = true, formatColumns = true, formatRows = true, insertColumns = true, insertRows = true,
       insertHyperlinks = true, deleteColumns = true, deleteRows = true, selectLockedCells = false, sort = true,
       autoFilter = true, pivotTables = true, selectUnlockedCells = false;
  ProtectionPassword password;
  bool operator==(const SheetProtection&) const = default;
};

// workbookProtection.
struct WorkbookProtection {
  bool lockStructure = true, lockWindows = false, lockRevision = false;
  ProtectionPassword password;  // workbookAlgorithmName, workbookHashValue... or workbookPassword
  std::string other;            // attributes it doesn't edit (the revisions password), as written
  bool operator==(const WorkbookProtection&) const = default;
};

// Read from the element's XML (any prefix), and written as Excel writes it with `prefix` ("x:" or "").
std::optional<SheetProtection> ReadSheetProtection(std::string_view element);
std::string SheetProtectionXml(const SheetProtection& p, std::string_view prefix = {});
std::optional<WorkbookProtection> ReadWorkbookProtection(std::string_view element);
std::string WorkbookProtectionXml(const WorkbookProtection& p, std::string_view prefix = {});

// The sheet's protection, as its part has it; nullopt when it has none. The sheet is protected
// when it's there with `sheet` set.
std::optional<SheetProtection> SheetProtectionOf(const Sheet& s);
bool SheetProtected(const Sheet& s);
std::optional<WorkbookProtection> WorkbookProtectionOf(const Workbook& b);
bool StructureProtected(const Workbook& b);

// A cell's format says it's locked (Format Cells > Protection; Excel's default), or its formula
// hidden. Either takes effect once the sheet is protected.
bool CellLocked(const Sheet& s, const Styles& st, uint32_t row, uint32_t col);
bool FormulaHidden(const Sheet& s, const Styles& st, uint32_t row, uint32_t col);
// No cell of `r` is locked, the empty ones included (as their row's or column's format says).
bool AllUnlocked(const Sheet& s, const Styles& st, const Range& r);

// What an edit does, for Excel's refusals.
enum class ProtectAct : uint8_t {
  Content,        // values, formulas, clearing, cutting: every cell unlocked
  FormatCells,    // cell formats, conditional formats
  FormatColumns,  // widths, hiding
  FormatRows,     // heights, hiding (a filter's rows too, with Use AutoFilter)
  InsertColumns,
  InsertRows,
  DeleteColumns,  // and every cell in them unlocked
  DeleteRows,
  InsertLink,     // links, in unlocked cells
  Objects,        // notes, comments, charts, pictures, shapes
  Sort,           // sorting, over unlocked cells
  Filter,         // an existing filter's criteria
  Never,          // refused on any protected sheet: merging, inserting or deleting cells, grouping,
                  // turning a filter on or off, tables, data validation
  Structure,      // the workbook's sheets: adding, deleting, moving, renaming, hiding, tab colors
};
// Why Excel refuses `act` over `r` on sheet `sheet` (Structure: the workbook), in Excel's words;
// empty when it doesn't.
std::u16string ProtectionRefusal(const Workbook& b, int sheet, ProtectAct act, const Range& r = {});
// Excel's message for a change to a protected sheet's cells.
const char16_t* LockedCellMessage();

}  // namespace fw::sheet::edit
