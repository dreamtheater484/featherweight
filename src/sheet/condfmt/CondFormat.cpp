#include "sheet/condfmt/CondFormat.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <iterator>
#include <string>
#include <unordered_map>

#include "core/Strings.h"
#include "sheet/calc/Bind.h"
#include "sheet/model/Workbook.h"
#include "sheet/numfmt/NumFormat.h"
#include "sheet/numfmt/NumLocale.h"
#include "sheet/numfmt/Parse.h"
#include "sheet/xlsx/Internal.h"

namespace fw::sheet::condfmt {

namespace {

using xlsx::Attr;
using xlsx::Child;
using xlsx::Local;

constexpr std::string_view kIconNames[] = {
    "3Arrows", "3ArrowsGray", "3Flags", "3TrafficLights1", "3TrafficLights2", "3Signs", "3Symbols", "3Symbols2",
    "3Stars", "3Triangles", "4Arrows", "4ArrowsGray", "4RedToBlack", "4Rating", "4TrafficLights", "5Arrows",
    "5ArrowsGray", "5Rating", "5Quarters", "5Boxes", "NoIcons"};

enum class Type : uint8_t {
  CellIs, Expression, ColorScale, DataBar, IconSet, Top10, Unique, Duplicate, ContainsText, NotContainsText,
  BeginsWith, EndsWith, Blanks, NoBlanks, Errors, NoErrors, TimePeriod, AboveAverage, Unknown
};

Type ParseType(std::string_view s) {
  static constexpr std::pair<std::string_view, Type> kTypes[] = {
      {"cellIs", Type::CellIs}, {"expression", Type::Expression}, {"colorScale", Type::ColorScale},
      {"dataBar", Type::DataBar}, {"iconSet", Type::IconSet}, {"top10", Type::Top10},
      {"uniqueValues", Type::Unique}, {"duplicateValues", Type::Duplicate}, {"containsText", Type::ContainsText},
      {"notContainsText", Type::NotContainsText}, {"beginsWith", Type::BeginsWith}, {"endsWith", Type::EndsWith},
      {"containsBlanks", Type::Blanks}, {"notContainsBlanks", Type::NoBlanks}, {"containsErrors", Type::Errors},
      {"notContainsErrors", Type::NoErrors}, {"timePeriod", Type::TimePeriod}, {"aboveAverage", Type::AboveAverage}};
  for (const auto& [name, t] : kTypes)
    if (s == name) return t;
  return Type::Unknown;
}

enum Op : uint8_t { kLess, kLessEq, kEqual, kNotEqual, kGreaterEq, kGreater, kBetween, kNotBetween };

Op ParseOp(std::string_view s) {
  static constexpr std::pair<std::string_view, Op> kOps[] = {
      {"lessThan", kLess}, {"lessThanOrEqual", kLessEq}, {"equal", kEqual}, {"notEqual", kNotEqual},
      {"greaterThanOrEqual", kGreaterEq}, {"greaterThan", kGreater}, {"between", kBetween},
      {"notBetween", kNotBetween}};
  for (const auto& [name, op] : kOps)
    if (s == name) return op;
  return kEqual;
}

enum Period : uint8_t { kToday, kYesterday, kTomorrow, kLast7, kThisMonth, kLastMonth, kNextMonth, kThisWeek, kLastWeek, kNextWeek };

Period ParsePeriod(std::string_view s) {
  static constexpr std::string_view kNames[] = {"today", "yesterday", "tomorrow", "last7Days", "thisMonth",
                                                "lastMonth", "nextMonth", "thisWeek", "lastWeek", "nextWeek"};
  for (size_t i = 0; i < std::size(kNames); ++i)
    if (s == kNames[i]) return static_cast<Period>(i);
  return kToday;
}

// A value as rules compare it. Text is borrowed.
struct Item {
  ValueType type = ValueType::Empty;
  double num = 0;  // a number, a boolean as 0/1, an error's code
  std::string_view text;
};

Item FromModel(const Value& v) {
  Item i;
  i.type = v.type;
  i.num = v.type == ValueType::Bool    ? (v.boolean ? 1 : 0)
          : v.type == ValueType::Error ? static_cast<double>(v.error)
                                       : v.number;
  i.text = v.text;
  return i;
}

Item FromCalc(const calc::Value& v) {
  Item i;
  switch (v.type()) {
    case calc::Type::Empty: break;
    case calc::Type::Number: i.type = ValueType::Number, i.num = v.number(); break;
    case calc::Type::Bool: i.type = ValueType::Bool, i.num = v.boolean() ? 1 : 0; break;
    case calc::Type::Text: i.type = ValueType::String, i.text = v.text(); break;
    default: i.type = ValueType::Error; break;
  }
  return i;
}

// Excel's comparison: numbers < text < booleans; an empty value as the other side's 0, "" or
// FALSE. False when either is an error.
bool Compare(Item a, Item b, int& out) {
  if (a.type == ValueType::Error || b.type == ValueType::Error) return false;
  if (a.type == ValueType::Empty) a.type = b.type == ValueType::Empty ? ValueType::Number : b.type;
  if (b.type == ValueType::Empty) b.type = a.type;
  auto rank = [](ValueType t) { return t == ValueType::Number ? 0 : t == ValueType::String ? 1 : 2; };
  if (rank(a.type) != rank(b.type)) {
    out = rank(a.type) < rank(b.type) ? -1 : 1;
  } else if (a.type == ValueType::String) {
    out = calc::CompareText(a.text, b.text);
  } else {
    const double x = calc::Round15(a.num), y = calc::Round15(b.num);
    out = x < y ? -1 : x > y ? 1 : 0;
  }
  return true;
}

bool Truthy(const Item& v) { return (v.type == ValueType::Number || v.type == ValueType::Bool) && v.num != 0; }

// A cell as text functions read it: numbers as General shows them, booleans as TRUE/FALSE.
std::string AsText(const Item& v, char decimal) {
  switch (v.type) {
    case ValueType::Number: return calc::NumberText(v.num, decimal);
    case ValueType::Bool: return v.num != 0 ? "TRUE" : "FALSE";
    case ValueType::String: return std::string(v.text);
    default: return {};
  }
}

// The first (or last) `chars` code points of UTF-8 text; false when it has fewer.
bool Ends(std::string_view s, size_t chars, bool last, std::string_view& out) {
  std::vector<size_t> starts;
  for (size_t i = 0; i < s.size(); ++i)
    if ((static_cast<uint8_t>(s[i]) & 0xC0) != 0x80) starts.push_back(i);
  if (starts.size() < chars) return false;
  out = last ? s.substr(chars ? starts[starts.size() - chars] : s.size()) : s.substr(0, chars < starts.size() ? starts[chars] : s.size());
  return true;
}

size_t CodePoints(std::string_view s) {
  return static_cast<size_t>(std::count_if(s.begin(), s.end(), [](char c) { return (static_cast<uint8_t>(c) & 0xC0) != 0x80; }));
}

double Percentile(const std::vector<double>& sorted, double p) {
  if (sorted.empty()) return 0;
  const double pos = std::clamp(p, 0.0, 1.0) * static_cast<double>(sorted.size() - 1);
  const size_t lo = static_cast<size_t>(pos);
  if (lo + 1 >= sorted.size()) return sorted.back();
  return sorted[lo] + (pos - static_cast<double>(lo)) * (sorted[lo + 1] - sorted[lo]);
}

// a -> b by t (0..1), each channel truncated toward a, as Excel's color scales are.
uint32_t Blend(uint32_t a, uint32_t b, double t) {
  uint32_t out = 0xFF000000u;
  for (int shift = 0; shift < 24; shift += 8) {
    const int x = static_cast<int>((a >> shift) & 0xFF), y = static_cast<int>((b >> shift) & 0xFF);
    out |= static_cast<uint32_t>(x + static_cast<int>((y - x) * t)) << shift;
  }
  return out;
}

// A differential format written inside a rule (Excel 2010's extension list).
Dxf ReadDxf(pugi::xml_node n) {
  Dxf d;
  if (pugi::xml_node f = Child(n, "font")) {
    d.font = xlsx::ReadFont(f);
    d.fontBold = static_cast<bool>(Child(f, "b"));
    d.fontItalic = static_cast<bool>(Child(f, "i"));
    d.fontStrike = static_cast<bool>(Child(f, "strike"));
    d.fontUnderline = static_cast<bool>(Child(f, "u"));
  }
  if (pugi::xml_node f = Child(n, "numFmt")) {
    d.numFmtId = xlsx::UIntAttr(f, "numFmtId", 0);
    d.numFmtCode = Attr(f, "formatCode");
  }
  if (pugi::xml_node f = Child(n, "fill")) d.fill = xlsx::ReadFill(f, true);
  if (pugi::xml_node f = Child(n, "border")) d.border = xlsx::ReadBorder(f);
  return d;
}

// Children of an element with the local name `local`.
template <typename Fn>
void Each(pugi::xml_node n, std::string_view local, Fn fn) {
  for (pugi::xml_node c = n.first_child(); c; c = c.next_sibling())
    if (c.type() == pugi::node_element && Local(c) == local) fn(c);
}

}  // namespace

IconSet ParseIconSet(std::string_view name) {
  for (size_t i = 0; i < std::size(kIconNames); ++i)
    if (name == kIconNames[i]) return static_cast<IconSet>(i);
  return IconSet::TrafficLights3;
}

int IconCount(IconSet s) {
  if (s == IconSet::None) return 0;
  return kIconNames[static_cast<size_t>(s)][0] - '0';
}

struct SheetRules::Impl {
  // A threshold: a color scale's, data bar's or icon set's.
  struct Cfvo {
    enum Kind : uint8_t { Min, Max, Num, Percent, Percentile, Formula, AutoMin, AutoMax } kind = Min;
    std::string val;
    bool gte = true;
    double value = 0;  // worked out with the range's statistics
  };
  // A formula operand: a constant, or compiled (on first use) at the block's base cell.
  struct Operand {
    std::string text;
    enum State : uint8_t { Unread, Constant, Compiled, Bad } state = Unread;
    std::string constText;
    Item constant;
    std::shared_ptr<const calc::Formula> f;
  };
  struct Rule {
    Type type = Type::Unknown;
    int priority = 0;
    uint32_t order = 0;  // as written, for equal priorities
    uint32_t block = 0;
    bool stop = false;
    const Dxf* dxf = nullptr;
    Op op = kEqual;
    std::string text;
    Period period = kToday;
    int rank = 10, stdDev = 0;
    bool percent = false, bottom = false, above = true, equal = false;
    std::vector<Operand> formulas;
    // Color scales, data bars and icon sets.
    std::vector<Cfvo> cfvos;
    std::vector<uint32_t> colors;  // ARGB
    bool showValue = true, reverse = false;
    // Data bars.
    double minLength = 0.1, maxLength = 0.9;
    bool gradient = true, border = false, negSameFill = true, negSameBorder = true;
    uint8_t axisPos = 2;    // 0 automatic, 1 middle, 2 none (Excel 2007's bars)
    uint8_t direction = 0;  // 0 context, 1 left to right, 2 right to left
    uint32_t borderColor = 0, negFill = 0xFFFF0000u, negBorder = 0xFFFF0000u, axisColor = 0xFF000000u;
    // Icon sets.
    IconSet icons = IconSet::TrafficLights3;
    std::vector<std::pair<IconSet, uint8_t>> custom;
    // The range's statistics, worked out on first use.
    bool ready = false, usable = false;
    std::vector<double> sorted;
    double limit = 0;
    struct Key {
      uint8_t kind;  // 0 number, 1 text, 2 boolean, 3 error
      double num;
      std::string text;
      uint32_t count;
    };
    std::vector<Key> keys;
  };
  struct Block {
    std::vector<Range> ranges;
    uint32_t baseRow = kNone, baseCol = kNone;
    std::vector<uint32_t> rules;  // by priority
  };
  struct Entry {
    Range r;
    uint32_t block;
  };

  const Workbook& book;
  const Sheet& sheet;
  size_t sheetIndex;
  Options opt;
  const NumLocale& locale;
  char decimal;
  std::vector<Rule> rules;
  std::vector<Block> blocks;
  std::vector<Entry> entries;
  std::deque<Dxf> ownDxfs;  // written inside Excel 2010 rules
  std::unordered_map<const Dxf*, std::unique_ptr<NumberFormat>> numFmts;
  std::unique_ptr<calc::Evaluator> evaluator;
  bool rtl = false;
  double today = 0;
  // The entries over the row last asked about.
  uint32_t currentRow = kNone;
  std::vector<uint32_t> rowEntries;
  std::vector<std::pair<uint64_t, uint32_t>> hits;  // (priority, order), rule

  Impl(const Workbook& b, size_t s, const Options& o)
      : book(b), sheet(b.GetSheet(s)), sheetIndex(s), opt(o), locale(o.locale ? *o.locale : EnUsLocale()),
        decimal(!locale.decimal.empty() && locale.decimal[0] < 128 ? static_cast<char>(locale.decimal[0]) : '.') {
    if (opt.calc) evaluator = std::make_unique<calc::Evaluator>(*opt.calc, &opt.calc->engine());
    rtl = !sheet.Views().empty() && sheet.Views()[0].rightToLeft;
    today = opt.today;
    if (today == 0) {
      SYSTEMTIME t;
      GetLocalTime(&t);
      calc::DateSerial(t.wYear, t.wMonth, t.wDay, book.Date1904(), today);
    }
    Read();
  }

  // ---- Reading

  void Read() {
    const std::string xml = sheet.KeptXml();
    if (xml.find("conditionalFormatting") == std::string::npos) return;
    pugi::xml_document doc;
    if (!xlsx::ParseSlice(doc, xml)) return;
    pugi::xml_node root = doc.document_element();
    // Excel 2010's rules, and its settings for data bars that Excel 2007's rules point to by id.
    std::unordered_map<std::string, pugi::xml_node> bars;
    std::vector<std::pair<pugi::xml_node, pugi::xml_node>> later;  // (conditionalFormatting, cfRule)
    Each(Child(root, "extLst"), "ext", [&](pugi::xml_node ext) {
      Each(Child(ext, "conditionalFormattings"), "conditionalFormatting", [&](pugi::xml_node cf) {
        Each(cf, "cfRule", [&](pugi::xml_node r) {
          if (Attr(r, "type") == "dataBar" && !Attr(r, "id").empty()) bars[std::string(Attr(r, "id"))] = r;
          else later.push_back({cf, r});
        });
      });
    });
    Each(root, "conditionalFormatting", [&](pugi::xml_node cf) {
      const uint32_t b = AddBlock(Attr(cf, "sqref"));
      if (b == kNone) return;
      Each(cf, "cfRule", [&](pugi::xml_node r) {
        pugi::xml_node bar;
        if (Attr(r, "type") == "dataBar") {
          Each(Child(r, "extLst"), "ext", [&](pugi::xml_node ext) {
            if (auto it = bars.find(xlsx::TextOf(Child(ext, "id"))); it != bars.end()) {
              bar = it->second;
              bars.erase(it);
            }
          });
        }
        AddRule(b, r, bar);
      });
    });
    for (const auto& [cf, r] : later) {
      const uint32_t b = AddBlock(xlsx::TextOf(Child(cf, "sqref")));
      if (b != kNone) AddRule(b, r, {});
    }
    for (Block& b : blocks)
      std::stable_sort(b.rules.begin(), b.rules.end(), [&](uint32_t x, uint32_t y) { return rules[x].priority < rules[y].priority; });
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.r.firstRow < b.r.firstRow; });
  }

  uint32_t AddBlock(std::string_view sqref) {
    Block b;
    size_t i = 0;
    while (i < sqref.size() && b.ranges.size() < 4096) {
      while (i < sqref.size() && sqref[i] == ' ') ++i;
      size_t j = sqref.find(' ', i);
      if (j == std::string_view::npos) j = sqref.size();
      Range r;
      if (j > i && ParseRange(sqref.substr(i, j - i), r)) {
        b.ranges.push_back(r);
        b.baseRow = std::min(b.baseRow, r.firstRow);
        b.baseCol = std::min(b.baseCol, r.firstCol);
      }
      i = j;
    }
    if (b.ranges.empty()) return kNone;
    const uint32_t index = static_cast<uint32_t>(blocks.size());
    for (const Range& r : b.ranges) entries.push_back({r, index});
    blocks.push_back(std::move(b));
    return index;
  }

  uint32_t Color(pugi::xml_node n, uint32_t fallback) const {
    return n ? book.GetStyles().Resolve(xlsx::ReadColor(n), fallback) : fallback;
  }

  static Cfvo ReadCfvo(pugi::xml_node n) {
    static constexpr std::string_view kKinds[] = {"min", "max", "num", "percent", "percentile", "formula", "autoMin", "autoMax"};
    Cfvo c;
    const std::string_view kind = Attr(n, "type");
    for (size_t i = 0; i < std::size(kKinds); ++i)
      if (kind == kKinds[i]) c.kind = static_cast<Cfvo::Kind>(i);
    c.val = Attr(n, "val");
    if (pugi::xml_node f = Child(n, "f")) c.val = xlsx::TextOf(f);
    c.gte = xlsx::BoolAttr(n, "gte", true);
    return c;
  }

  void AddRule(uint32_t block, pugi::xml_node n, pugi::xml_node bar) {
    if (rules.size() >= 65536) return;
    Rule r;
    r.type = ParseType(Attr(n, "type"));
    if (r.type == Type::Unknown) return;
    r.block = block;
    r.order = static_cast<uint32_t>(rules.size());
    r.priority = xlsx::IntAttr(n, "priority", bar ? xlsx::IntAttr(bar, "priority", 0) : 0);
    r.stop = xlsx::BoolAttr(n, "stopIfTrue", false);
    const auto& dxfs = book.GetStyles().Dxfs();
    if (!Attr(n, "dxfId").empty()) {
      const uint32_t id = xlsx::UIntAttr(n, "dxfId", kNone);
      if (id < dxfs.size()) r.dxf = &dxfs[id];
    } else if (pugi::xml_node d = Child(n, "dxf")) {
      ownDxfs.push_back(ReadDxf(d));
      r.dxf = &ownDxfs.back();
    }
    r.op = ParseOp(Attr(n, "operator"));
    r.text = Attr(n, "text");
    r.period = ParsePeriod(Attr(n, "timePeriod"));
    r.rank = xlsx::IntAttr(n, "rank", 10);
    r.percent = xlsx::BoolAttr(n, "percent", false);
    r.bottom = xlsx::BoolAttr(n, "bottom", false);
    r.above = xlsx::BoolAttr(n, "aboveAverage", true);
    r.equal = xlsx::BoolAttr(n, "equalAverage", false);
    r.stdDev = std::clamp(xlsx::IntAttr(n, "stdDev", 0), 0, 3);
    for (pugi::xml_node c = n.first_child(); c && r.formulas.size() < 2; c = c.next_sibling())
      if (c.type() == pugi::node_element && (Local(c) == "formula" || Local(c) == "f")) r.formulas.push_back({xlsx::TextOf(c)});
    if (pugi::xml_node scale = Child(n, "colorScale")) {
      Each(scale, "cfvo", [&](pugi::xml_node c) { r.cfvos.push_back(ReadCfvo(c)); });
      Each(scale, "color", [&](pugi::xml_node c) { r.colors.push_back(Color(c, 0xFFFFFFFFu)); });
      if (r.cfvos.size() < 2 || r.cfvos.size() > 3 || r.colors.size() != r.cfvos.size()) return;
    } else if (r.type == Type::DataBar) {
      pugi::xml_node d = Child(n, "dataBar");
      pugi::xml_node x = bar ? Child(bar, "dataBar") : pugi::xml_node();
      if (!d && !x) return;
      Each(x ? x : d, "cfvo", [&](pugi::xml_node c) { r.cfvos.push_back(ReadCfvo(c)); });
      if (r.cfvos.size() != 2) return;
      r.colors.push_back(Color(Child(x, "fillColor") ? Child(x, "fillColor") : Child(d, "color"), 0xFF638EC6u));
      r.showValue = xlsx::BoolAttr(d ? d : x, "showValue", true);
      pugi::xml_node lengths = x ? x : d;
      r.minLength = std::clamp(xlsx::IntAttr(lengths, "minLength", 10), 0, 100) / 100.0;
      r.maxLength = std::clamp(xlsx::IntAttr(lengths, "maxLength", 90), 0, 100) / 100.0;
      if (r.maxLength < r.minLength) r.maxLength = r.minLength;
      if (x) {
        r.gradient = xlsx::BoolAttr(x, "gradient", true);
        r.border = xlsx::BoolAttr(x, "border", false);
        r.negSameFill = xlsx::BoolAttr(x, "negativeBarColorSameAsPositive", false);
        r.negSameBorder = xlsx::BoolAttr(x, "negativeBarBorderColorSameAsPositive", true);
        const std::string_view axis = Attr(x, "axisPosition");
        r.axisPos = axis == "middle" ? 1 : axis == "none" ? 2 : 0;
        const std::string_view dir = Attr(x, "direction");
        r.direction = dir == "leftToRight" ? 1 : dir == "rightToLeft" ? 2 : 0;
        r.borderColor = Color(Child(x, "borderColor"), r.colors[0]);
        r.negFill = Color(Child(x, "negativeFillColor"), 0xFFFF0000u);
        r.negBorder = Color(Child(x, "negativeBorderColor"), 0xFFFF0000u);
        r.axisColor = Color(Child(x, "axisColor"), 0xFF000000u);
      }
    } else if (r.type == Type::IconSet) {
      pugi::xml_node s = Child(n, "iconSet");
      if (!s) return;
      r.icons = ParseIconSet(Attr(s, "iconSet").empty() ? "3TrafficLights1" : Attr(s, "iconSet"));
      r.showValue = xlsx::BoolAttr(s, "showValue", true);
      r.reverse = xlsx::BoolAttr(s, "reverse", false);
      Each(s, "cfvo", [&](pugi::xml_node c) { r.cfvos.push_back(ReadCfvo(c)); });
      if (xlsx::BoolAttr(s, "custom", false))
        Each(s, "cfIcon", [&](pugi::xml_node c) {
          r.custom.push_back({ParseIconSet(Attr(c, "iconSet")), static_cast<uint8_t>(std::min<uint32_t>(xlsx::UIntAttr(c, "iconId", 0), 4))});
        });
      if (r.cfvos.empty() || r.cfvos.size() > 5) return;
    }
    blocks[block].rules.push_back(static_cast<uint32_t>(rules.size()));
    rules.push_back(std::move(r));
  }

  // ---- Formulas

  // A number, "text" or TRUE/FALSE written as the whole formula.
  static bool ReadConstant(Operand& o) {
    std::string_view t = o.text;
    while (!t.empty() && t.front() == ' ') t.remove_prefix(1);
    while (!t.empty() && t.back() == ' ') t.remove_suffix(1);
    if (!t.empty() && t.front() == '=') t.remove_prefix(1);
    if (t.size() >= 2 && t.front() == '"' && t.back() == '"') {
      std::string s;
      for (size_t i = 1; i + 1 < t.size(); ++i) {
        if (t[i] == '"') {
          if (t[i + 1] != '"' || i + 2 >= t.size()) return false;
          ++i;
        }
        s += t[i];
      }
      o.constText = std::move(s);
      o.constant.type = ValueType::String;
      return true;
    }
    if (calc::EqualText(t, "TRUE") || calc::EqualText(t, "FALSE")) {
      o.constant.type = ValueType::Bool;
      o.constant.num = t.size() == 4 ? 1 : 0;
      return true;
    }
    if (t.empty() || t.size() > 64 || t.find_first_not_of("0123456789.eE+-") != std::string_view::npos) return false;
    std::string s(t);
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (end != s.c_str() + s.size() || !std::isfinite(v)) return false;
    o.constant.type = ValueType::Number;
    o.constant.num = v;
    return true;
  }

  // The operand's value at (row, col); `hold` keeps a calculated value's text alive.
  bool Evaluate(Operand& o, const Block& b, uint32_t r, uint32_t c, calc::Value& hold, Item& out) {
    if (o.state == Operand::Unread) {
      if (ReadConstant(o)) {
        o.state = Operand::Constant;
      } else if (opt.calc) {
        o.f = opt.calc->engine().CompileAt(o.text, calc::Cell{static_cast<int>(sheetIndex), static_cast<int32_t>(b.baseRow),
                                                              static_cast<int32_t>(b.baseCol)});
        o.state = o.f ? Operand::Compiled : Operand::Bad;
      } else {
        o.state = Operand::Bad;
      }
    }
    if (o.state == Operand::Constant) {
      out = o.constant;
      if (out.type == ValueType::String) out.text = o.constText;
      return true;
    }
    if (o.state != Operand::Compiled) return false;
    if (evaluator->Evaluate(*o.f, calc::Cell{static_cast<int>(sheetIndex), static_cast<int32_t>(r), static_cast<int32_t>(c)}, hold) !=
        calc::EvalResult::kDone)
      return false;
    if (hold.IsArray() && hold.array().size() > 0) {
      calc::Value first = hold.array().at(0, 0);
      hold = first;
    }
    out = FromCalc(hold);
    return true;
  }

  // ---- Statistics

  // Each value cell of the block once (overlapping ranges counted once); returns the cells' count.
  template <typename Fn>
  uint64_t ForEachValue(const Block& b, Fn fn) const {
    uint64_t area = 0;
    for (size_t i = 0; i < b.ranges.size(); ++i) {
      const Range& rg = b.ranges[i];
      area += static_cast<uint64_t>(rg.lastRow - rg.firstRow + 1) * (rg.lastCol - rg.firstCol + 1);
      sheet.ForEachCell(rg, [&](const CellInfo& ci) {
        for (size_t k = 0; k < i; ++k)
          if (b.ranges[k].Contains(ci.row, ci.col)) return true;
        if (ci.value.type != ValueType::Empty) fn(ci.value);
        return true;
      });
    }
    return area;
  }

  // A duplicate rule's key for a value: text that reads as a number in the region is that number.
  Rule::Key KeyOf(const Item& v) const {
    Rule::Key k{1, 0, {}, 0};
    switch (v.type) {
      case ValueType::Number: k.kind = 0, k.num = calc::Round15(v.num); break;
      case ValueType::Bool: k.kind = 2, k.num = v.num; break;
      case ValueType::Error: k.kind = 3, k.num = v.num; break;
      case ValueType::String: {
        ParsedValue p;
        if (!v.text.empty() && ParseValue(Utf8ToWide(v.text), locale, book.Date1904(), p) && p.kind == ParsedValue::Number) {
          k.kind = 0, k.num = calc::Round15(p.number);
        } else {
          k.text = v.text;
        }
        break;
      }
      default: break;
    }
    return k;
  }

  static bool KeyLess(const Rule::Key& a, const Rule::Key& b) {
    if (a.kind != b.kind) return a.kind < b.kind;
    if (a.kind == 1) return calc::CompareText(a.text, b.text) < 0;
    return a.num < b.num;
  }

  double CfvoValue(Rule& r, Cfvo& c, bool& ok) {
    const std::vector<double>& s = r.sorted;
    const double lo = s.front(), hi = s.back();
    double p = 0;
    if (c.kind == Cfvo::Percent || c.kind == Cfvo::Percentile || c.kind == Cfvo::Num || c.kind == Cfvo::Formula) {
      Operand o{c.val};
      calc::Value hold;
      Item v;
      if (!Evaluate(o, blocks[r.block], blocks[r.block].baseRow, blocks[r.block].baseCol, hold, v) ||
          v.type != ValueType::Number) {
        ok = false;
        return 0;
      }
      p = v.num;
    }
    switch (c.kind) {
      case Cfvo::Min: return lo;
      case Cfvo::Max: return hi;
      case Cfvo::AutoMin: return std::min(lo, 0.0);
      case Cfvo::AutoMax: return std::max(hi, 0.0);
      case Cfvo::Percent: return lo + (hi - lo) * p / 100;
      case Cfvo::Percentile: return Percentile(s, p / 100);
      default: return p;
    }
  }

  void Prepare(Rule& r) {
    r.ready = true;
    r.usable = false;
    r.sorted.clear();
    r.keys.clear();
    const Block& b = blocks[r.block];
    switch (r.type) {
      case Type::Top10: case Type::AboveAverage: case Type::ColorScale: case Type::DataBar: case Type::IconSet: {
        ForEachValue(b, [&](const Value& v) {
          if (v.type == ValueType::Number) r.sorted.push_back(v.number);
        });
        if (r.sorted.empty()) return;
        std::sort(r.sorted.begin(), r.sorted.end());
        const size_t n = r.sorted.size();
        if (r.type == Type::Top10) {
          size_t k = r.percent ? std::max<size_t>(1, n * static_cast<size_t>(std::max(r.rank, 0)) / 100)
                               : static_cast<size_t>(std::max(r.rank, 0));
          if (k == 0) return;
          k = std::min(k, n);
          r.limit = r.bottom ? r.sorted[k - 1] : r.sorted[n - k];
        } else if (r.type == Type::AboveAverage) {
          double sum = 0, squares = 0;
          for (double x : r.sorted) sum += x;
          const double mean = sum / static_cast<double>(n);
          for (double x : r.sorted) squares += (x - mean) * (x - mean);
          const double sd = std::sqrt(squares / static_cast<double>(n));
          r.limit = r.above ? mean + r.stdDev * sd : mean - r.stdDev * sd;
        } else {
          bool ok = true;
          for (Cfvo& c : r.cfvos) c.value = CfvoValue(r, c, ok);
          if (!ok) return;
        }
        r.usable = true;
        return;
      }
      case Type::Unique: case Type::Duplicate: {
        uint64_t filled = 0;
        const uint64_t area = ForEachValue(b, [&](const Value& v) {
          ++filled;
          Rule::Key k = KeyOf(FromModel(v));
          r.keys.push_back(std::move(k));
        });
        if (area > filled) r.keys.push_back({1, 0, {}, 0});  // the empty cells, as ""
        std::sort(r.keys.begin(), r.keys.end(), KeyLess);
        // One key per value, with its count (empty cells count once at most: enough for "").
        std::vector<Rule::Key> grouped;
        for (Rule::Key& k : r.keys) {
          if (!grouped.empty() && !KeyLess(grouped.back(), k)) {
            ++grouped.back().count;
          } else {
            k.count = 1;
            grouped.push_back(std::move(k));
          }
        }
        r.keys = std::move(grouped);
        r.usable = true;
        return;
      }
      default: r.usable = true; return;
    }
  }

  // ---- Evaluating

  bool TimeHolds(Period p, double v) const {
    const double d = std::floor(v), t = std::floor(today);
    const bool d1904 = book.Date1904();
    const int w = static_cast<int>(std::fmod(t + (d1904 ? 5 : 6), 7.0)) + 1;  // WEEKDAY(TODAY()): Sunday 1
    switch (p) {
      case kToday: return d == t;
      case kYesterday: return d == t - 1;
      case kTomorrow: return d == t + 1;
      case kLast7: return t - d <= 6 && d <= t;
      case kThisWeek: return t - d <= w - 1 && d - t <= 7 - w;
      case kLastWeek: return t - d >= w && t - d < w + 7;
      case kNextWeek: return d - t > 7 - w && d - t < 15 - w;
      default: {
        int y, m, dd, ty, tm, td;
        if (!calc::SerialDate(d, d1904, y, m, dd) || !calc::SerialDate(t, d1904, ty, tm, td)) return false;
        int month = ty * 12 + tm - 1 + (p == kLastMonth ? -1 : p == kNextMonth ? 1 : 0);
        return y * 12 + m - 1 == month;
      }
    }
  }

  bool TextHolds(const Rule& r, const Item& v) const {
    if (v.type == ValueType::Error) return r.type == Type::NotContainsText;
    const std::string s = AsText(v, decimal);
    std::string_view part;
    switch (r.type) {
      case Type::ContainsText: return calc::WildcardMatch("*" + r.text + "*", s);
      case Type::NotContainsText: return !calc::WildcardMatch("*" + r.text + "*", s);
      case Type::BeginsWith: return Ends(s, CodePoints(r.text), false, part) && calc::EqualText(part, r.text);
      default: return Ends(s, CodePoints(r.text), true, part) && calc::EqualText(part, r.text);
    }
  }

  void Bar(const Rule& r, double v, Style& out) const {
    const double lo = r.cfvos[0].value, hi = std::max(r.cfvos[1].value, lo);
    DataBar& bar = out.bar;
    auto fraction = [](double x, double a, double b) { return b > a ? std::clamp((x - a) / (b - a), 0.0, 1.0) : (x >= b ? 1.0 : 0.0); };
    double from = 0, to = 0, axis = -1;
    if (r.axisPos == 2 || (r.axisPos == 0 && lo >= 0)) {
      to = r.minLength + fraction(v, lo, hi) * (r.maxLength - r.minLength);
    } else if (r.axisPos == 0 && hi <= 0) {  // all negative: from the right edge
      axis = 1;
      from = 1 - (r.minLength + fraction(-v, -hi, -lo) * (r.maxLength - r.minLength));
      to = 1;
    } else {
      const double m = std::max(std::fabs(lo), std::fabs(hi));
      axis = r.axisPos == 1 ? 0.5 : -lo / (hi - lo);
      if (v >= 0) {
        from = axis;
        to = axis + (1 - axis) * fraction(v, 0, r.axisPos == 1 ? m : hi);
      } else {
        from = axis - axis * fraction(-v, 0, r.axisPos == 1 ? m : -lo);
        to = axis;
      }
    }
    bar.from = static_cast<float>(std::clamp(from, 0.0, 1.0));
    bar.to = static_cast<float>(std::clamp(to, 0.0, 1.0));
    bar.axis = static_cast<float>(axis);
    bar.negative = v < 0 && r.axisPos != 2;
    bar.fill = bar.negative && !r.negSameFill ? r.negFill : r.colors[0];
    bar.border = !r.border ? 0 : bar.negative && !r.negSameBorder ? r.negBorder : r.borderColor;
    bar.axisColor = r.axisColor;
    bar.gradient = r.gradient;
    bar.rightToLeft = r.direction == 2 || (r.direction == 0 && rtl);
    out.set |= Style::kBar;
  }

  // Whether the rule holds at the cell; color scales, data bars and icons write their part.
  bool Holds(Rule& r, uint32_t row, uint32_t col, const Item& v, Style& out) {
    const bool number = v.type == ValueType::Number;
    switch (r.type) {
      case Type::CellIs: {
        if (r.formulas.empty()) return false;
        calc::Value h0, h1;
        Item a, b;
        const Block& bl = blocks[r.block];
        if (!Evaluate(r.formulas[0], bl, row, col, h0, a)) return false;
        int c0 = 0, c1 = 0;
        if (r.op == kBetween || r.op == kNotBetween) {
          if (r.formulas.size() < 2 || !Evaluate(r.formulas[1], bl, row, col, h1, b)) return false;
          int order = 0;
          if (!Compare(a, b, order)) return false;
          if (order > 0) std::swap(a, b);
          if (!Compare(v, a, c0) || !Compare(v, b, c1)) return false;
          return r.op == kBetween ? c0 >= 0 && c1 <= 0 : c0 < 0 || c1 > 0;
        }
        if (!Compare(v, a, c0)) return false;
        switch (r.op) {
          case kLess: return c0 < 0;
          case kLessEq: return c0 <= 0;
          case kNotEqual: return c0 != 0;
          case kGreaterEq: return c0 >= 0;
          case kGreater: return c0 > 0;
          default: return c0 == 0;
        }
      }
      case Type::Expression: {
        calc::Value h;
        Item x;
        return !r.formulas.empty() && Evaluate(r.formulas[0], blocks[r.block], row, col, h, x) && Truthy(x);
      }
      case Type::ContainsText: case Type::NotContainsText: case Type::BeginsWith: case Type::EndsWith:
        return TextHolds(r, v);
      case Type::Blanks: case Type::NoBlanks: {
        const bool blank = v.type == ValueType::Empty ||
                           (v.type == ValueType::String && v.text.find_first_not_of(' ') == std::string_view::npos);
        return blank == (r.type == Type::Blanks);
      }
      case Type::Errors: return v.type == ValueType::Error;
      case Type::NoErrors: return v.type != ValueType::Error;
      case Type::TimePeriod: return number && TimeHolds(r.period, v.num);
      default: break;
    }
    // The rules that read the whole range.
    if (!r.ready) Prepare(r);
    if (!r.usable) return false;
    if (r.type == Type::Unique || r.type == Type::Duplicate) {
      if (v.type == ValueType::Empty) return false;
      const Rule::Key k = KeyOf(v);
      auto it = std::lower_bound(r.keys.begin(), r.keys.end(), k, KeyLess);
      const uint32_t count = it != r.keys.end() && !KeyLess(k, *it) ? it->count : 0;
      return r.type == Type::Duplicate ? count >= 2 : count == 1;
    }
    if (!number) return false;
    const double x = v.num;
    switch (r.type) {
      case Type::Top10: return r.bottom ? x <= r.limit : x >= r.limit;
      case Type::AboveAverage:
        return r.above ? (r.equal ? x >= r.limit : x > r.limit) : (r.equal ? x <= r.limit : x < r.limit);
      case Type::ColorScale: {
        if (out.set & Style::kFill) return true;
        const auto& c = r.cfvos;
        const size_t last = c.size() - 1;
        uint32_t color;
        if (x >= c[last].value) color = r.colors[last];
        else if (x <= c[0].value) color = r.colors[0];
        else if (last == 2 && x >= c[1].value)
          color = c[2].value > c[1].value ? Blend(r.colors[1], r.colors[2], (x - c[1].value) / (c[2].value - c[1].value)) : r.colors[2];
        else
          color = c[1].value > c[0].value ? Blend(r.colors[0], r.colors[1], (x - c[0].value) / (c[1].value - c[0].value)) : r.colors[1];
        out.pattern = PatternType::Solid;
        out.fillColor = color;
        out.set |= Style::kFill;
        return true;
      }
      case Type::DataBar:
        if (!(out.set & Style::kBar)) {
          Bar(r, x, out);
          if (!r.showValue) out.set |= Style::kHideValue;
        }
        return true;
      case Type::IconSet: {
        if (out.set & Style::kIcon) return true;
        const int n = static_cast<int>(r.cfvos.size());
        int i = 0;
        for (int k = 1; k < n; ++k)
          if (r.cfvos[k].gte ? x >= r.cfvos[k].value : x > r.cfvos[k].value) i = k;
        if (r.reverse) i = n - 1 - i;
        IconSet set = r.icons;
        int icon = i;
        if (static_cast<size_t>(i) < r.custom.size()) set = r.custom[i].first, icon = r.custom[i].second;
        if (set != IconSet::None && icon < IconCount(set)) {
          out.iconSet = set;
          out.icon = static_cast<uint8_t>(icon);
          out.set |= Style::kIcon;
        }
        if (!r.showValue) out.set |= Style::kHideValue;
        return true;
      }
      default: return false;
    }
  }

  void Apply(const Dxf& d, Style& out) {
    const Styles& st = book.GetStyles();
    if (d.font) {
      const Font& f = *d.font;
      if (f.color.IsSet() && !(out.set & Style::kFontColor))
        out.fontColor = st.Resolve(f.color, 0xFF000000u), out.set |= Style::kFontColor;
      if (d.fontBold && !(out.set & Style::kBold)) out.bold = f.bold, out.set |= Style::kBold;
      if (d.fontItalic && !(out.set & Style::kItalic)) out.italic = f.italic, out.set |= Style::kItalic;
      if (d.fontStrike && !(out.set & Style::kStrike)) out.strike = f.strike, out.set |= Style::kStrike;
      if (d.fontUnderline && !(out.set & Style::kUnderline)) out.underline = f.underline, out.set |= Style::kUnderline;
    }
    if (d.fill && !(out.set & Style::kFill)) {
      const Fill& f = *d.fill;
      if (f.gradient) {
        out.gradient = &f;
        out.set |= Style::kFill;
      } else if (f.pattern == PatternType::Solid) {
        out.pattern = PatternType::Solid;
        out.fillColor = st.Resolve(f.bg.IsSet() ? f.bg : f.fg, 0xFFFFFFFFu);
        out.set |= Style::kFill;
      } else if (f.pattern != PatternType::None) {
        out.pattern = f.pattern;
        out.fillColor = st.Resolve(f.bg, 0xFFFFFFFFu);
        out.patternColor = st.Resolve(f.fg, 0xFF000000u);
        out.set |= Style::kFill;
      }
    }
    if (d.border) {
      auto side = [&](const BorderSide& from, Style::Side& to) {
        if (from.style == BorderStyle::None || to.style != BorderStyle::None) return;
        to.style = from.style;
        to.color = st.Resolve(from.color, 0xFF000000u);
        out.set |= Style::kBorder;
      };
      side(d.border->left, out.left);
      side(d.border->right, out.right);
      side(d.border->top, out.top);
      side(d.border->bottom, out.bottom);
    }
    if ((d.numFmtId || !d.numFmtCode.empty()) && !(out.set & Style::kNumFmt)) {
      std::unique_ptr<NumberFormat>& f = numFmts[&d];
      if (!f) {
        f = d.numFmtCode.empty() ? std::make_unique<NumberFormat>(NumberFormat::Builtin(static_cast<int>(d.numFmtId.value_or(0)), locale))
                                 : std::make_unique<NumberFormat>(Utf8ToWide(d.numFmtCode));
      }
      out.numFmt = f.get();
      out.set |= Style::kNumFmt;
    }
  }

  bool At(uint32_t r, uint32_t c, Style& out) {
    out.Clear();
    if (rules.empty()) return false;
    if (r != currentRow) {
      currentRow = r;
      rowEntries.clear();
      for (uint32_t i = 0; i < entries.size() && entries[i].r.firstRow <= r; ++i)
        if (entries[i].r.lastRow >= r) rowEntries.push_back(i);
    }
    hits.clear();
    uint32_t lastBlock = kNone;
    for (uint32_t i : rowEntries) {
      const Entry& e = entries[i];
      if (c < e.r.firstCol || c > e.r.lastCol || e.block == lastBlock) continue;
      bool seen = false;
      for (const auto& h : hits) seen = seen || rules[h.second].block == e.block;
      if (seen) continue;
      lastBlock = e.block;
      for (uint32_t k : blocks[e.block].rules) {
        const Rule& ru = rules[k];
        hits.push_back({(static_cast<uint64_t>(static_cast<uint32_t>(ru.priority) ^ 0x80000000u) << 32) | ru.order, k});
      }
    }
    if (hits.empty()) return false;
    std::sort(hits.begin(), hits.end());
    const Item v = FromModel(sheet.GetValue(r, c));
    for (const auto& h : hits) {
      Rule& ru = rules[h.second];
      if (!Holds(ru, r, c, v, out)) continue;
      if (ru.dxf) Apply(*ru.dxf, out);
      if (ru.stop) break;
    }
    return out.set != 0;
  }
};

SheetRules::SheetRules(const Workbook& book, size_t sheet, const Options& options)
    : d_(std::make_unique<Impl>(book, sheet, options)) {}
SheetRules::~SheetRules() = default;

bool SheetRules::Empty() const { return d_->rules.empty(); }
size_t SheetRules::RuleCount() const { return d_->rules.size(); }
bool SheetRules::At(uint32_t row, uint32_t col, Style& out) { return d_->At(row, col, out); }

void SheetRules::Changed() {
  for (auto& r : d_->rules) r.ready = false;
}

}  // namespace fw::sheet::condfmt
