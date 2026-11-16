#pragma once

// The bytecode formulas compile to (internal to the engine). Postfix: each operand pushes one
// value, each operator and call pops its operands and pushes its result. Multi-byte operands are
// little-endian and unaligned.

#include <cstdint>
#include <cstring>
#include <string_view>

namespace fw::sheet::calc::bc {

enum Op : uint8_t {
  kEnd = 0,
  kNum,      // f64
  kStr,      // u32 n, n bytes (UTF-8)
  kBool,     // u8
  kErr,      // u8 Err
  kMissing,  // an omitted argument
  kArray,    // u16 rows, u16 cols, then rows*cols items, each kNum/kStr/kBool/kErr with its operand
  kRef,      // u8 flags, [u16 sheet [u16 sheet2]], i32 row, i16 col
  kArea,     // u8 flags, [u16 sheet [u16 sheet2]], i32 r1, i16 c1, i32 r2, i16 c2
  kRefErr,   // u8 flags, [u16 sheet [u16 sheet2]]: #REF! as a reference (Sheet1!#REF!)
  kName,     // u32 slot, u8 flags, [u16 sheet]: a defined name
  kExtern,   // u32 n, n bytes: a reference into another workbook, as written; never resolved
  kTable,    // u8 items, str table, str column1, str column2 (strings: u32 n + bytes)
  kUnary,    // u8 UnaryOp | kArrayMode
  kBinary,   // u8 BinaryOp | kArrayMode
  kCall,     // u16 function, u8 argc, u8 mode
  kLazy,     // u16 function, u8 argc, u8 mode, argc x u32 code length, then each argument's code
  kUnknown,  // u8 argc, u8 mode, u32 n, n bytes: a function the engine doesn't know, as written
  kParen,    // parentheses: nothing when evaluated, kept for printing
  kOpCount,
};

// Reference flags. Absolute parts hold 0-based rows and columns; relative ones the offset from the
// formula's cell.
enum RefFlag : uint8_t {
  kRowAbs = 1,
  kColAbs = 2,
  kRow2Abs = 4,
  kCol2Abs = 8,
  kSheet = 16,      // an explicit sheet (else the formula's own)
  kSheet3D = 32,    // a range of sheets
  kWholeCols = 64,  // A:B (rows 0..max, absolute)
  kWholeRows = 128, // 1:2 (columns 0..max, absolute)
};

enum UnaryOp : uint8_t { kNeg, kPlus, kPercent, kImplicit /* @ */, kSpill /* # */ };
enum BinaryOp : uint8_t {
  kAdd, kSub, kMul, kDiv, kPow, kConcat, kEq, kNe, kLt, kLe, kGt, kGe, kRange, kUnion, kIsect,
};
inline constexpr uint8_t kArrayMode = 0x80;  // the operator runs in an array context

// Table items (structured references).
enum TableItem : uint8_t { kAll = 1, kData = 2, kHeaders = 4, kTotals = 8, kThisRow = 16 };

template <class T>
inline T Read(const uint8_t* p) {
  T v;
  std::memcpy(&v, p, sizeof v);
  return v;
}
inline std::string_view ReadStr(const uint8_t* p, size_t* len) {  // p at the u32 length
  uint32_t n = Read<uint32_t>(p);
  *len = 4 + n;
  return {reinterpret_cast<const char*>(p + 4), n};
}

// The size of a reference's operands after the flags byte.
inline size_t SheetBytes(uint8_t flags) { return (flags & kSheet) ? ((flags & kSheet3D) ? 4 : 2) : 0; }

// The length of the instruction at `p` (op byte included), for walking code without running it.
size_t InstrLength(const uint8_t* p);

}  // namespace fw::sheet::calc::bc
