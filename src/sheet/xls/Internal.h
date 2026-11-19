#pragma once

#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Shared by the .xls converter (Xls.cpp) and its formula decompiler (Ptg.cpp). Everything here
// reads untrusted bytes: reads past the end give zeros and clear `ok`, never more.
namespace fw::sheet::xls::detail {

// Little-endian reads from a span, bounds-checked.
class Bytes {
 public:
  Bytes() = default;
  explicit Bytes(std::span<const uint8_t> b) : b_(b) {}
  size_t Size() const { return b_.size(); }
  size_t Pos() const { return pos_; }
  size_t Left() const { return pos_ < b_.size() ? b_.size() - pos_ : 0; }
  bool Ok() const { return ok_; }
  void Seek(size_t p) {
    if (p > b_.size()) ok_ = false, p = b_.size();
    pos_ = p;
  }
  bool Skip(size_t n) {
    if (n > Left()) {
      ok_ = false;
      pos_ = b_.size();
      return false;
    }
    pos_ += n;
    return true;
  }
  uint8_t U8() {
    if (Left() < 1) return Fail();
    return b_[pos_++];
  }
  uint16_t U16() {
    if (Left() < 2) return Fail();
    const uint16_t v = static_cast<uint16_t>(b_[pos_] | b_[pos_ + 1] << 8);
    pos_ += 2;
    return v;
  }
  uint32_t U32() {
    if (Left() < 4) return Fail();
    const uint32_t v = b_[pos_] | b_[pos_ + 1] << 8 | b_[pos_ + 2] << 16 | uint32_t{b_[pos_ + 3]} << 24;
    pos_ += 4;
    return v;
  }
  double F64() {
    if (Left() < 8) return Fail();
    double d;
    std::memcpy(&d, b_.data() + pos_, 8);
    pos_ += 8;
    return d;
  }
  std::span<const uint8_t> Take(size_t n) {
    if (n > Left()) {
      ok_ = false;
      n = Left();
    }
    auto s = b_.subspan(pos_, n);
    pos_ += n;
    return s;
  }
  std::span<const uint8_t> Rest() const { return b_.subspan(std::min(pos_, b_.size())); }

 private:
  uint8_t Fail() {
    ok_ = false;
    pos_ = b_.size();
    return 0;
  }
  std::span<const uint8_t> b_;
  size_t pos_ = 0;
  bool ok_ = true;
};

// UTF-8 from UTF-16 code units (lone surrogates become U+FFFD) and from Latin-1 bytes (BIFF8's
// "compressed" strings: the high byte of each character is zero).
void AppendUtf16(std::string& out, const uint16_t* units, size_t n);
void AppendCodeUnit(std::string& out, uint32_t cp);
// A ShortXLUnicodeString (cch is one byte) or an XLUnicodeString (two bytes); false when cut short.
bool ReadShortString(Bytes& b, std::string& out);
bool ReadString(Bytes& b, std::string& out);
// Characters without a count: `cch` of them, with the high-byte flag first.
bool ReadChars(Bytes& b, uint32_t cch, std::string& out);

// The workbook's names and references, for the formulas.
struct FormulaContext {
  // Sheet names by BIFF sheet index (BOUNDSHEET order); empty for a sheet left out (a chart sheet,
  // a VBA module): a reference to it is #REF!.
  std::vector<std::string> sheets;
  struct SupBook {
    enum Kind : uint8_t { Self, AddIn, External, Other } kind = Other;
    std::vector<std::string> names;  // EXTERNNAME records that follow it, in order
  };
  std::vector<SupBook> supBooks;
  struct Xti {
    uint32_t supBook = 0;
    int first = 0, last = 0;  // sheet indexes in that SUPBOOK; negative: special
  };
  std::vector<Xti> xti;
  // Defined names by index (PtgName's is 1-based): the text a formula writes for each; empty for
  // one that can't be referred to (which fails the formula).
  std::vector<std::string> names;
};

// Decompiles a BIFF8 parsed formula (rgce, with its extra data rgcb) into Excel's invariant text,
// without the leading "=". (`row`, `col`): the cell it is in, which relative tokens (PtgRefN,
// PtgAreaN of shared formulas) count from. False when it can't be written faithfully: unknown or
// damaged tokens, references to other workbooks, data tables, macro commands.
bool Decompile(const FormulaContext& ctx, std::span<const uint8_t> rgce, std::span<const uint8_t> rgcb, uint32_t row,
               uint32_t col, std::string& out);

// Cell names: "A1", "$A$1" (rows and columns 0-based).
void AppendCol(std::string& out, uint32_t col);
// A sheet name as formulas write it: quoted when it must be.
std::string QuoteSheet(std::string_view name);
// The shortest text that reads back as `v`.
void AppendNumber(std::string& out, double v);
// An error's text from its BIFF code (#DIV/0! for 0x07); empty for an unknown code.
std::string_view ErrorName(uint8_t code);

}  // namespace fw::sheet::xls::detail
