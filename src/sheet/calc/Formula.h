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

struct PrintOptions {
  bool r1c1 = false;
  bool file = true;  // as files store it (_xlfn. prefixes, _xlfn.SINGLE), else as the formula bar shows it
};

// The formula as text, without "=", for the cell `at` (a shared formula prints per cell).
std::string Print(const Formula& f, const CompileOptions& at, const Names& names, const PrintOptions& how = {});

// Column letters: 0 -> "A", 16383 -> "XFD".
std::string ColumnName(int32_t col);

}  // namespace fw::sheet::calc
