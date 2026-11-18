#pragma once

// Formulas: parsing Excel's formula syntax into compact bytecode, and printing it back (plan §7.3).
//
// The parser reads A1 and R1C1 references, sheets and 3D references, defined names, structured
// references, array constants, errors, every operator (range, union and intersection too) and the
// _xlfn./_xlws. prefixes. References to other workbooks are kept but never resolved.
//
// Relative references are stored as offsets from the formula's cell, as R1C1 writes them, so the
// cells of a shared formula share one compiled Formula.

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fw::sheet::calc {

// What a formula's sheet names and defined names refer to, while it compiles and prints.
class Names {
 public:
  virtual ~Names() = default;
  // The sheet called `name` (case-insensitive), or -1.
  virtual int FindSheet(std::string_view name) const = 0;
  virtual std::string SheetName(int sheet) const = 0;
  // The slot of a defined name: the same name (case-insensitive) always gets the same slot,
  // whether or not it is defined yet.
  virtual uint32_t NameSlot(std::string_view name) = 0;
  virtual std::string SlotName(uint32_t slot) const = 0;
};

struct CompileOptions {
  int sheet = 0;  // the formula's cell, which relative references are relative to
  int32_t row = 0, col = 0;
  bool r1c1 = false;   // R1C1 references instead of A1
  bool array = false;  // an array formula (entered with Ctrl+Shift+Enter, or a dynamic array)
  // Typed in a cell, read as Excel reads typing: R1C1 references in A1 style (=RC is the cell
  // itself), A1:A1048576 as A:A and A1:XFD1 as 1:1, and no unions of values (=(1,2)).
  bool typed = false;
};

class Formula {
 public:
  enum Flags : uint16_t {
    kVolatile = 1,      // NOW, TODAY, RAND, OFFSET, INDIRECT...: recalculated every time
    kDynamicRefs = 2,   // OFFSET, INDIRECT: references made while it runs
    kUnsupported = 4,   // a function the engine lacks, or another workbook: keep the cached value
    kArray = 8,         // compiled as an array formula
    kNames = 16,        // uses defined names
    kTables = 32,       // uses structured references
    kSpills = 64,       // uses # (ANCHORARRAY)
    kSubtotal = 128,    // calls SUBTOTAL or AGGREGATE (SUBTOTALs and some AGGREGATEs leave this cell out)
  };
  std::vector<uint8_t> code;
  uint16_t flags = 0;
};

struct ParseError {
  std::string message;
  size_t pos = 0;  // in the text
};

// Compiles `text` (with or without the leading "="). Null on a syntax error.
std::shared_ptr<const Formula> Compile(std::string_view text, const CompileOptions& at, Names& names,
                                       ParseError* error = nullptr);

// Moves references while printing (structural edits): given a reference's stored parts (offsets
// for relative ones), on `sheet`, changes them, or returns false when it's gone (#REF!).
class RefMap {
 public:
  virtual ~RefMap() = default;
  enum Abs : uint8_t { kRow1 = 1, kCol1 = 2, kRow2 = 4, kCol2 = 8 };
  virtual bool Map(int sheet, int32_t& r1, int32_t& c1, int32_t& r2, int32_t& c2, uint8_t abs, bool wholeRows,
                   bool wholeCols) const = 0;
  // Map, and the sheet the reference is on after the edit (a cut pasted onto another sheet takes
  // references to its cells there): `toSheet` starts as `sheet`. Moved to another sheet, the
  // reference is printed with that sheet's name (quoted as needed), except one without a name that
  // lands on the printed formula's own sheet, as Excel writes them.
  virtual bool MapTo(int sheet, int& toSheet, int32_t& r1, int32_t& c1, int32_t& r2, int32_t& c2, uint8_t abs,
                     bool wholeRows, bool wholeCols) const {
    (void)toSheet;
    return Map(sheet, r1, c1, r2, c2, abs, wholeRows, wholeCols);
  }
  // A sheet prefix's sheets (first and last; the same for one sheet): changed, or false when it's
  // gone (#REF!A1).
  virtual bool MapSheets(int& s1, int& s2) const {
    (void)s1, (void)s2;
    return true;
  }
};

struct PrintOptions {
  bool r1c1 = false;
  bool file = true;  // as files store it (_xlfn. prefixes, _xlfn.SINGLE), else as the formula bar shows it
  // A relative reference that falls off the sheet at the cell printed for is #REF! (a copied
  // formula, as Excel makes it), not wrapped around to the other side (a shared formula).
  bool offSheetRef = false;
  const RefMap* map = nullptr;
  // With a map: the sheet references without a sheet name are on (the formula's sheet before it
  // moved to another sheet); -1: the printed cell's.
  int homeSheet = -1;
};

// The formula's text (A1, as stored) with the references `map` changes written as Print would
// write them for the cell `to`, and everything else as written (spaces, case). 1: a reference
// changed, `out` has the text; 0: none did; -1: the text has something this doesn't read (R1C1,
// typed text, structured, external or 3-D references, #REF!A1): compile and print it instead.
// The map is asked only about A1 references (not names or sheet prefixes).
int MapReferenceText(std::string_view text, const CompileOptions& at, const CompileOptions& to, const Names& names,
                     const RefMap& map, std::string& out);

// The formula as text, without "=", for the cell `at` (a shared formula prints per cell).
std::string Print(const Formula& f, const CompileOptions& at, const Names& names, const PrintOptions& how = {});

// Column letters: 0 -> "A", 16383 -> "XFD".
std::string ColumnName(int32_t col);

}  // namespace fw::sheet::calc
