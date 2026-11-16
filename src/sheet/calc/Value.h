#pragma once

// Values of the formula engine (plan §7.3): numbers, text, booleans, errors, arrays and references.
// A Value is 24 bytes; text and arrays are shared, reference-counted and immutable, so copies are
// cheap. The engine is single-threaded (it runs time-sliced on the UI thread), so the counts are
// plain integers.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fw::sheet::calc {

inline constexpr int32_t kMaxRows = 1048576;  // rows 0 .. kMaxRows-1
inline constexpr int32_t kMaxCols = 16384;    // columns 0 .. kMaxCols-1 (A .. XFD)

// Excel's error values, in the order of their ERROR.TYPE numbers (1 = #NULL!).
enum class Err : uint8_t {
  None, Null, Div0, Value, Ref, Name, Num, NA, GettingData, Spill, Calc, Field, Blocked, Connect, Busy,
  Unknown, Python,
};
inline constexpr int kErrCount = 17;
std::string_view ErrText(Err e);
// The error literal at the start of `s` (#DIV/0!, #n/a, ...), ignoring case; its length in `len`.
Err ParseErr(std::string_view s, size_t* len = nullptr);

// Cells on one sheet or a range of sheets: rows and columns 0-based, inclusive.
struct Area {
  int32_t r1 = 0, r2 = 0;
  int16_t c1 = 0, c2 = 0;
  uint16_t s1 = 0, s2 = 0;
  bool operator==(const Area&) const = default;
  int32_t Rows() const { return r2 - r1 + 1; }
  int32_t Cols() const { return c2 - c1 + 1; }
  bool Single() const { return r1 == r2 && c1 == c2 && s1 == s2; }
  bool Contains(int sheet, int32_t r, int32_t c) const {
    return sheet >= s1 && sheet <= s2 && r >= r1 && r <= r2 && c >= c1 && c <= c2;
  }
};

enum class Type : uint8_t { Empty, Number, Bool, Text, Error, Array, Ref, RefList, Missing };

class Value;

// A rows x cols block of scalar values (never references or nested arrays).
class Array {
 public:
  static Array* Make(int32_t rows, int32_t cols);  // filled with Empty, one reference held
  int32_t rows() const { return rows_; }
  int32_t cols() const { return cols_; }
  Value& at(int32_t r, int32_t c);
  const Value& at(int32_t r, int32_t c) const;
  Value* data() { return reinterpret_cast<Value*>(this + 1); }
  const Value* data() const { return reinterpret_cast<const Value*>(this + 1); }
  int32_t size() const { return rows_ * cols_; }

 private:
  friend class Value;
  Array() = default;
  static void Release(Array* a);
  uint32_t refs_ = 1;
  int32_t rows_ = 0, cols_ = 0;
  uint32_t pad_ = 0;
};

// The areas of a union reference: (A1:B2,D4).
struct RefList {
  uint32_t refs = 1;
  std::vector<Area> areas;
};

class Value {
 public:
  Value() : n_(0) {}
  ~Value() { Release(); }
  Value(const Value& o) { CopyFrom(o); }
  Value(Value&& o) noexcept { MoveFrom(o); }
  Value& operator=(const Value& o) {
    if (this != &o) {
      Release();
      CopyFrom(o);
    }
    return *this;
  }
  Value& operator=(Value&& o) noexcept {
    if (this != &o) {
      Release();
      MoveFrom(o);
    }
    return *this;
  }

  static Value Number(double d) {
    Value v;
    v.t_ = Type::Number;
    v.n_ = d;
    return v;
  }
  static Value Bool(bool b) {
    Value v;
    v.t_ = Type::Bool;
    v.n_ = b ? 1 : 0;
    return v;
  }
  static Value Error(Err e) {
    Value v;
    v.t_ = Type::Error;
    v.e_ = e;
    return v;
  }
  static Value Missing() {
    Value v;
    v.t_ = Type::Missing;
    return v;
  }
  static Value Text(std::string_view s);
  // Takes over the one reference Array::Make gave.
  static Value Adopt(Array* a) {
    Value v;
    v.t_ = Type::Array;
    v.a_ = a;
    return v;
  }
  // `dynamic`: made at run time (OFFSET, INDIRECT), so not among the formula's static precedents.
  static Value Ref(const Area& a, bool dynamic = false) {
    Value v;
    v.t_ = Type::Ref;
    v.area_ = a;
    v.dyn_ = dynamic;
    return v;
  }
  static Value Refs(std::vector<Area> areas, bool dynamic = false);

  Type type() const { return t_; }
  bool IsEmpty() const { return t_ == Type::Empty; }
  bool IsNumber() const { return t_ == Type::Number; }
  bool IsBool() const { return t_ == Type::Bool; }
  bool IsText() const { return t_ == Type::Text; }
  bool IsError() const { return t_ == Type::Error; }
  bool IsArray() const { return t_ == Type::Array; }
  bool IsRef() const { return t_ == Type::Ref || t_ == Type::RefList; }
  bool IsMissing() const { return t_ == Type::Missing; }
  bool dynamic() const { return dyn_; }

  double number() const { return n_; }
  bool boolean() const { return n_ != 0; }
  Err error() const { return e_; }
  std::string_view text() const;
  const Array& array() const { return *a_; }
  Array& mutable_array() { return *a_; }  // only while this is its one holder (building it)
  const Area& area() const { return area_; }
  const std::vector<Area>& areas() const { return l_->areas; }

 private:
  struct Str {
    uint32_t refs;
    uint32_t size;
    const char* data() const { return reinterpret_cast<const char*>(this + 1); }
  };
  void Release();
  void CopyFrom(const Value& o);
  void MoveFrom(Value& o) {
    t_ = o.t_;
    e_ = o.e_;
    dyn_ = o.dyn_;
    area_ = o.area_;
    o.t_ = Type::Empty;
  }

  union {
    double n_;
    Str* s_;
    Array* a_;
    RefList* l_;
    Area area_;
  };
  Type t_ = Type::Empty;
  Err e_ = Err::None;
  bool dyn_ = false;
};

inline Value& Array::at(int32_t r, int32_t c) { return data()[r * cols_ + c]; }
inline const Value& Array::at(int32_t r, int32_t c) const { return data()[r * cols_ + c]; }

// --- Conversions as Excel makes them ---

// The region's conventions for converting between text and numbers in formulas (Excel follows
// Windows' regional settings): "0.3"&"" is "0,3" with a decimal comma.
struct ConvLocale {
  char decimal = '.', group = ',';
  char dateOrder = 'M';      // typed dates: 'M' m/d/y, 'D' d/m/y, 'Y' y/m/d
  std::string currency = "$";  // UTF-8
};
const ConvLocale& EnUsConv();

// A number as text: up to 15 significant digits, as `&` and General show it ("0.3", "1E+20").
std::string NumberText(double d, char decimal = '.');
// Excel keeps 15 significant digits: rounds `d` to them.
double Round15(double d);
// Text as a number, as arithmetic converts it: " 12 ", "1e3", "1,234.5", "$12", "50%", "(3)",
// dates and times in en-US forms ("1/2/2024", "2024-01-02", "Jan 2, 2024", "12:30 PM").
// `date1904` picks the date system for dates.
bool TextToNumber(std::string_view s, double& out, bool date1904 = false, const ConvLocale& loc = EnUsConv());

// The value's text for TEXTJOIN, CONCAT and `&` (booleans as TRUE/FALSE, empty as "").
std::string ValueText(const Value& v, char decimal = '.');

// Case-insensitive comparison of text as Excel orders it: -1, 0 or 1.
int CompareText(std::string_view a, std::string_view b);
bool EqualText(std::string_view a, std::string_view b);
// Wildcard match (* ? and ~ escapes), case-insensitive, of the whole text.
bool WildcardMatch(std::string_view pattern, std::string_view text);

// --- Dates (serial numbers) ---

// Days since the epoch: 1900 system (1 = 1900-01-01, 60 = the 1900-02-29 that never was) or 1904.
bool DateSerial(int year, int month, int day, bool date1904, double& serial);
// Year, month and day of a serial; false when it is out of Excel's range.
bool SerialDate(double serial, bool date1904, int& year, int& month, int& day);
int DaysInMonth(int year, int month);

}  // namespace fw::sheet::calc
