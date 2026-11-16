#pragma once

// The worksheet functions the engine knows: their names, argument counts and argument classes
// (how each argument is evaluated), and their implementations.

#include <cstdint>
#include <string_view>

namespace fw::sheet::calc {

struct Call;
using FnImpl = void (*)(Call& call);

enum FnFlag : uint8_t {
  kFnVolatile = 1,  // recalculated every time (NOW, RAND, OFFSET, INDIRECT)
  kFnLazy = 2,      // arguments evaluated on demand (IF, IFERROR, CHOOSE)
  kFnDynRef = 4,    // makes references at run time (OFFSET, INDIRECT)
  kFnXlfn = 8,      // files write it with _xlfn. (newer than Excel 2007)
  kFnXlws = 16,     // files write it with _xlfn._xlws.
};

struct FuncInfo {
  const char* name;  // upper case, without prefixes
  uint8_t minArgs, maxArgs;
  uint8_t flags;
  // One class per argument, the last one repeating: 'V' a value (a reference is implicitly
  // intersected outside array formulas, and arrays are taken element by element), 'R' a reference
  // or value as it is, 'A' an array (the argument is evaluated as an array formula).
  const char* params;
  FnImpl impl;  // null: not implemented yet (the formula keeps its cached value)
};

// The function called `name` (case-insensitive, without _xlfn./_xlws.), or -1.
int FindFunction(std::string_view name);
const FuncInfo& Function(int id);
int FunctionCount();
// The class of argument `i` (0-based).
char ParamClass(const FuncInfo& f, int i);

}  // namespace fw::sheet::calc
