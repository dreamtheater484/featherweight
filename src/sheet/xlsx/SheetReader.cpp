// The worksheet reader: sheetData streamed with XmlPull into the model's row blocks, the small
// elements around it (views, columns, merges, hyperlinks, page setup) read from their text, and
// everything around sheetData kept as written for the writer (plan §4.3 rules 1 and 8).
#include <algorithm>
#include <charconv>
#include <cmath>
#include <unordered_map>

#include "sheet/xlsx/Internal.h"

namespace fw::sheet {

using pkg::Status;
using pkg::XmlPull;
using namespace detail;
using xlsx::IsMainNs;

namespace {

bool AllSpace(std::string_view s) {
  for (char c : s)
    if (c != ' ' && c != '\t' && c != '\n' && c != '\r') return false;
  return true;
}

// Digits without a leading zero, as the model writes a number back: true when `s` is one.
bool CanonicalUInt(std::string_view s, uint32_t& out) {
  if (s.empty() || s.size() > 10 || (s[0] == '0' && s.size() > 1)) return false;
  uint64_t v = 0;
  for (char c : s) {
    if (c < '0' || c > '9') return false;
    v = v * 10 + static_cast<uint64_t>(c - '0');
  }
  if (v > 0xFFFFFFFFull) return false;
  out = static_cast<uint32_t>(v);
  return true;
}

// A number, and the style that writes it back as `s` (xlsx/Numbers.cpp); `exact` is false when
// none does.
bool ParseNumber(std::string_view s, double& v, bool& exact, uint8_t& style) {
  exact = false;
  style = 0;
  if (s.empty()) return false;
  // Integers up to 15 digits: the common case, exact by construction.
  size_t i = s[0] == '-' ? 1 : 0;
  size_t digits = s.size() - i;
  if (digits > 0 && digits <= 15 && (s[i] != '0' || digits == 1) && !(i == 1 && s[1] == '0')) {
    int64_t n = 0;
    bool ok = true;
    for (size_t k = i; k < s.size() && ok; ++k) {
      if (s[k] < '0' || s[k] > '9') ok = false;
      else n = n * 10 + (s[k] - '0');
    }
    if (ok) {
      v = static_cast<double>(i ? -n : n);
      exact = true;
      return true;
    }
  }
  auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  if (ec != std::errc() || p != s.data() + s.size() || !std::isfinite(v)) return false;
  // Plain decimals of up to 15 significant digits (12.5, 0.0375): two of those never read as the
  // same number, so the shortest digits are these, and style 0 writes them like this when the
  // exponent is -4 or more. No need to format the number to know.
  size_t dot = s.find('.', i);
  if (dot != std::string_view::npos && dot > i && dot + 1 < s.size() && s.back() != '0' && (s[i] != '0' || dot == i + 1)) {
    size_t sig = 0, zeros = 0;
    bool ok = true, leading = s[i] == '0';
    for (size_t k = i; k < s.size() && ok; ++k) {
      if (k == dot) continue;
      if (s[k] < '0' || s[k] > '9') ok = false;
      else if (leading && s[k] == '0') ++zeros;
      else leading = false, ++sig;
    }
    if (ok && sig <= 15 && zeros <= 4 && !(v == 0)) {
      exact = true;
      return true;
    }
  }
  exact = xlsx::NumberStyle(s, v, style);
  return true;
}

// Days from 1970-01-01 to a civil date (Howard Hinnant's algorithm).
int64_t DaysFromCivil(int64_t y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

// t="d": an ISO 8601 date and time, as a serial number in the workbook's date system.
bool ParseIsoDate(std::string_view s, bool date1904, double& out) {
  int y = 0;
  unsigned mo = 1, d = 1, h = 0, mi = 0;
  double sec = 0;
  auto num = [&](size_t at, size_t len, auto& v) {
    if (at + len > s.size()) return false;
    auto [p, ec] = std::from_chars(s.data() + at, s.data() + at + len, v);
    return ec == std::errc() && p == s.data() + at + len;
  };
  size_t t = s.find('T');
  if (t != 0) {
    if (s.size() < 10 || s[4] != '-' || s[7] != '-' || !num(0, 4, y) || !num(5, 2, mo) || !num(8, 2, d)) return false;
    if (mo < 1 || mo > 12 || d < 1 || d > 31) return false;
  }
  if (t != std::string_view::npos) {
    std::string_view tm = s.substr(t + 1);
    size_t base = t + 1;
    if (tm.size() < 5 || tm[2] != ':' || !num(base, 2, h) || !num(base + 3, 2, mi)) return false;
    if (tm.size() > 6 && tm[5] == ':') {
      size_t end = 6;
      while (end < tm.size() && ((tm[end] >= '0' && tm[end] <= '9') || tm[end] == '.')) ++end;
      if (!num(base + 6, end - 6, sec)) return false;
    }
    if (h > 24 || mi > 59 || sec >= 61) return false;
  } else if (s.size() != 10) {
    return false;
  }
  double days = 0;
  if (t != 0) {
    int64_t epoch = date1904 ? DaysFromCivil(1904, 1, 1) : DaysFromCivil(1899, 12, 30);
    days = static_cast<double>(DaysFromCivil(y, mo, d) - epoch);
    if (!date1904 && days < 61) days -= 1;  // Excel counts 1900-02-29, which never was
  }
  out = days + (h * 3600 + mi * 60 + sec) / 86400.0;
  return true;
}

}  // namespace

class SheetLoader {
 public:
  SheetLoader(Workbook& book, Sheet& sheet) : book_(book), s_(sheet) {}
  Status Load(std::string_view xml, const pkg::Limits& limits, std::string_view label);

 private:
  // A run of text between markup in a container the model writes: kept when it says something.
  struct TextRun {
    std::string raw;
    bool spaceOnly = true;
    void Add(std::string_view r, std::string_view decoded) {
      raw += r;
      spaceOnly = spaceOnly && AllSpace(decoded);
    }
  };

  void Reset();
  Status Fail() const {
    return pull_->GetStatus().Ok() ? Status::Fail(Status::Code::Damaged, "The sheet " + s_.name_ + " is damaged.")
                                   : pull_->GetStatus();
  }
  std::string_view Since(size_t before) const { return src_.substr(before, pull_->Offset() - before); }
  void Keep(uint32_t row, uint32_t col, std::string_view xml) { s_.kept_.push_back({row, col, std::string(xml)}); }
  void Flush(TextRun& run, bool atEnd, bool hasMarkup, uint32_t row, uint32_t col) {
    if (run.raw.empty()) return;
    if (!run.spaceOnly || (atEnd && !hasMarkup)) Keep(row, col, run.raw);
    run.raw.clear();
    run.spaceOnly = true;
  }
  pkg::AttrSetId Intern(std::vector<pkg::XmlAttr>& attrs) {
    pkg::AttrSetId id = s_.store_.Intern(std::move(attrs));
    attrs.clear();
    return id;
  }

  bool ReadSheetData();
  bool ReadRow(size_t before);
  bool ReadCell(size_t before, uint32_t row, uint32_t rowOrd, uint32_t cellOrd);
  // The text of the element just started (f, v, t), through its end. False on an error; sets
  // `odd` when it holds markup.
  bool ReadText(std::string& out, bool& odd);
  bool ReadInline(std::string& out, bool& preserve, bool& odd);
  void ReadTop(std::string_view local, std::string_view xml);

  Workbook& book_;
  Sheet& s_;
  XmlPull* pull_ = nullptr;
  std::string_view src_;
  SheetBuilder* b_ = nullptr;
  uint32_t rowOrdinal_ = 0;
  int64_t lastRow_ = -1;
  std::string key_, vText_, fText_, isText_;
  std::vector<pkg::XmlAttr> attrs_;
  std::unordered_map<std::string, pkg::AttrSetId> rowSets_;
  std::unordered_map<pkg::AttrSetId, uint32_t> textless_;  // f attributes -> formula, for cells sharing one
};

void SheetLoader::Reset() {
  s_.blocks_.clear();
  s_.blockFirstRow_.clear();
  s_.cellCount_ = s_.formulaCells_ = 0;
  s_.formulas_.clear();
  s_.sharedMasters_.clear();
  s_.texts_ = TextPool();
  s_.store_ = pkg::RoundTripStore();
  s_.kept_.clear();
  s_.text_ = PartText();
  s_.dimension_.reset();
  s_.cols_.clear();
  s_.format_ = SheetFormat();
  s_.merges_.clear();
  s_.mergeMaxHeight_ = 0;
  s_.views_.clear();
  s_.props_ = SheetProps();
  s_.links_.clear();
  s_.page_ = PageSetup();
  s_.headerFooter_ = HeaderFooter();
  s_.rowBreaks_.clear();
  s_.colBreaks_.clear();
  s_.drawingRel_.clear();
  s_.legacyDrawingRel_.clear();
  s_.tableRels_.clear();
}

Status SheetLoader::Load(std::string_view xml, const pkg::Limits& limits, std::string_view label) {
  Reset();
  XmlPull pull(xml, limits, label);
  pull_ = &pull;
  src_ = pull.Source();
  PartText& text = s_.text_;
  size_t declEnd = xlsx::DeclarationEnd(src_);
  XmlPull::Event e;
  do {
    e = pull.Next();
  } while (e == XmlPull::Event::Comment || e == XmlPull::Event::ProcessingInstruction || e == XmlPull::Event::Text);
  if (e != XmlPull::Event::StartElement) return Fail();
  size_t rootBegin = xlsx::TagBegin(src_, pull.Offset());
  text.prolog = std::string(src_.substr(declEnd, rootBegin > declEnd ? rootBegin - declEnd : 0));
  text.rootStart = std::string(src_.substr(rootBegin, pull.Offset() - rootBegin));
  bool rootEmpty = pull.IsEmptyElement();
  size_t contentStart = pull.Offset(), afterStart = contentStart;
  for (;;) {
    size_t before = pull.Offset();
    e = pull.Next();
    if (e == XmlPull::Event::Error || e == XmlPull::Event::EndDocument) return Fail();
    if (e == XmlPull::Event::EndElement && pull.Depth() == 1) {
      size_t endBegin = rootEmpty ? pull.Offset() : xlsx::TagBegin(src_, pull.Offset());
      if (endBegin < afterStart) endBegin = afterStart;
      (text.hasData ? text.afterData : text.beforeData) = std::string(src_.substr(afterStart, endBegin - afterStart));
      text.rootEnd = std::string(src_.substr(endBegin, pull.Offset() - endBegin));
      break;
    }
    if (e != XmlPull::Event::StartElement) continue;
    std::string_view local = pull.LocalName();
    bool main = IsMainNs(pull.NamespaceUri());
    size_t tagBegin = xlsx::TagBegin(src_, pull.Offset());
    if (tagBegin < before) tagBegin = before;
    if (main && local == "sheetData" && !text.hasData) {
      text.beforeData = std::string(src_.substr(contentStart, tagBegin - contentStart));
      text.dataStart = std::string(src_.substr(tagBegin, pull.Offset() - tagBegin));
      text.hasData = true;
      text.emptyData = pull.IsEmptyElement();
      if (!ReadSheetData()) return Fail();
      afterStart = pull.Offset();
      continue;
    }
    static constexpr std::string_view kRead[] = {"sheetPr",    "dimension",   "sheetViews", "sheetFormatPr", "cols",
                                                 "mergeCells", "hyperlinks",  "printOptions", "pageMargins", "pageSetup",
                                                 "headerFooter", "rowBreaks", "colBreaks",  "drawing",       "legacyDrawing",
                                                 "tableParts"};
    bool read = main && std::find(std::begin(kRead), std::end(kRead), local) != std::end(kRead);
    std::string name(read ? local : std::string_view());
    if (!xlsx::SkipElement(pull)) return Fail();
    if (read) ReadTop(name, src_.substr(tagBegin, pull.Offset() - tagBegin));
  }
  size_t epilogStart = pull.Offset();
  for (;;) {
    e = pull.Next();
    if (e == XmlPull::Event::Error) return Fail();
    if (e == XmlPull::Event::EndDocument) break;
  }
  text.epilog = std::string(src_.substr(epilogStart));

  // Shared formulas by si; the first master of each wins.
  std::stable_sort(s_.sharedMasters_.begin(), s_.sharedMasters_.end(),
                   [](const auto& a, const auto& b) { return a.first < b.first; });
  s_.sharedMasters_.erase(std::unique(s_.sharedMasters_.begin(), s_.sharedMasters_.end(),
                                      [](const auto& a, const auto& b) { return a.first == b.first; }),
                          s_.sharedMasters_.end());
  s_.formulas_.shrink_to_fit();
  s_.texts_.Shrink();
  return {};
}

bool SheetLoader::ReadSheetData() {
  XmlPull& pull = *pull_;
  if (pull.IsEmptyElement()) return pull.Next() == XmlPull::Event::EndElement;
  SheetBuilder builder(s_);
  b_ = &builder;
  TextRun run;
  bool hasMarkup = false;
  for (;;) {
    size_t before = pull.Offset();
    XmlPull::Event e = pull.Next();
    switch (e) {
      case XmlPull::Event::Text:
      case XmlPull::Event::CData:
        run.Add(Since(before), pull.Text());
        break;
      case XmlPull::Event::EndElement:
        Flush(run, true, hasMarkup, rowOrdinal_, Kept::kBeforeRow);
        builder.Finish();
        b_ = nullptr;
        return true;
      case XmlPull::Event::StartElement:
        Flush(run, false, hasMarkup, rowOrdinal_, Kept::kBeforeRow);
        hasMarkup = true;
        if (pull.LocalName() == "row" && IsMainNs(pull.NamespaceUri())) {
          if (!ReadRow(before)) return false;
        } else {
          if (!xlsx::SkipElement(pull)) return false;
          Keep(rowOrdinal_, Kept::kBeforeRow, Since(before));
        }
        break;
      case XmlPull::Event::Comment:
      case XmlPull::Event::ProcessingInstruction:
        Flush(run, false, hasMarkup, rowOrdinal_, Kept::kBeforeRow);
        hasMarkup = true;
        Keep(rowOrdinal_, Kept::kBeforeRow, Since(before));
        break;
      default:
        return false;
    }
  }
}

bool SheetLoader::ReadRow(size_t before) {
  XmlPull& pull = *pull_;
  RowInfo info;
  std::string_view rText;
  bool hasR = false;
  key_.clear();
  for (const auto& a : pull.Attributes()) {
    if (a.prefix.empty()) {
      std::string_view k = a.local;
      if (k == "r") {
        rText = a.value;
        hasR = true;
        continue;
      }
      double d = 0;
      uint32_t u = 0;
      if (k == "ht" && xlsx::ParseDouble(a.value, d)) info.height = std::clamp(d, 0.0, 409.5);
      else if (k == "s" && xlsx::ParseUInt(a.value, u)) info.style = u;
      else if (k == "customFormat") info.customFormat = xlsx::ParseBool(a.value, false);
      else if (k == "hidden") info.hidden = xlsx::ParseBool(a.value, false);
      else if (k == "customHeight") info.customHeight = xlsx::ParseBool(a.value, false);
      else if (k == "outlineLevel" && xlsx::ParseUInt(a.value, u)) info.outlineLevel = static_cast<uint8_t>(std::min(u, 7u));
      else if (k == "collapsed") info.collapsed = xlsx::ParseBool(a.value, false);
      else if (k == "thickTop") info.thickTop = xlsx::ParseBool(a.value, false);
      else if (k == "thickBot") info.thickBottom = xlsx::ParseBool(a.value, false);
    }
    key_ += a.qname;
    key_ += '\1';
    key_ += a.nsUri;
    key_ += '\1';
    key_ += a.value;
    key_ += '\2';
  }
  uint32_t n = 0;
  bool canonicalR = hasR && CanonicalUInt(rText, n) && n >= 1 && n <= kMaxRows;
  uint32_t row = canonicalR ? n - 1 : static_cast<uint32_t>(lastRow_ + 1);
  if (hasR && !canonicalR) {
    if (xlsx::ParseUInt(rText, n) && n >= 1 && n <= kMaxRows) row = n - 1;
    key_ += "r\1\1";
    key_ += rText;
  }
  pkg::AttrSetId set = 0;
  if (!key_.empty()) {
    auto it = rowSets_.find(key_);
    if (it != rowSets_.end()) {
      set = it->second;
    } else {
      for (const auto& a : pull.Attributes())
        if (!(a.prefix.empty() && a.local == "r") || !canonicalR)
          attrs_.push_back({std::string(a.qname), std::string(a.nsUri), std::string(a.value)});
      set = Intern(attrs_);
      rowSets_.emplace(key_, set);
    }
  }
  if (!b_->StartRow(row, info)) {
    if (!xlsx::SkipElement(pull)) return false;
    Keep(rowOrdinal_, Kept::kBeforeRow, Since(before));
    return true;
  }
  RowRec* rec = b_->CurrentRow();
  rec->attrs = set;
  if (!canonicalR) rec->flags |= kRowNoRef;
  uint32_t rowOrd = rowOrdinal_++;
  lastRow_ = row;
  if (pull.IsEmptyElement()) return pull.Next() == XmlPull::Event::EndElement;
  uint32_t cellOrd = 0;
  TextRun run;
  bool hasMarkup = false;
  for (;;) {
    size_t at = pull.Offset();
    XmlPull::Event e = pull.Next();
    switch (e) {
      case XmlPull::Event::Text:
      case XmlPull::Event::CData:
        run.Add(Since(at), pull.Text());
        break;
      case XmlPull::Event::EndElement:
        Flush(run, true, hasMarkup, rowOrd, cellOrd);
        return true;
      case XmlPull::Event::StartElement:
        Flush(run, false, hasMarkup, rowOrd, cellOrd);
        hasMarkup = true;
        if (pull.LocalName() == "c" && IsMainNs(pull.NamespaceUri())) {
          size_t kept = s_.kept_.size();
          if (!ReadCell(at, row, rowOrd, cellOrd)) return false;
          if (s_.kept_.size() == kept) ++cellOrd;  // stored, not kept as written
        } else {
          if (!xlsx::SkipElement(pull)) return false;
          Keep(rowOrd, cellOrd, Since(at));
        }
        break;
      case XmlPull::Event::Comment:
      case XmlPull::Event::ProcessingInstruction:
        Flush(run, false, hasMarkup, rowOrd, cellOrd);
        hasMarkup = true;
        Keep(rowOrd, cellOrd, Since(at));
        break;
      default:
        return false;
    }
  }
}

bool SheetLoader::ReadText(std::string& out, bool& odd) {
  XmlPull& pull = *pull_;
  out.clear();
  if (pull.IsEmptyElement()) return pull.Next() == XmlPull::Event::EndElement;
  for (;;) {
    XmlPull::Event e = pull.Next();
    switch (e) {
      case XmlPull::Event::Text:
      case XmlPull::Event::CData:
        out += pull.Text();
        break;
      case XmlPull::Event::EndElement:
        return true;
      case XmlPull::Event::StartElement:
        odd = true;
        if (!xlsx::SkipElement(pull)) return false;
        break;
      case XmlPull::Event::Comment:
      case XmlPull::Event::ProcessingInstruction:
        odd = true;
        break;
      default:
        return false;
    }
  }
}

// is: a t, or rich runs (r with rPr and t; rPh, phoneticPr), whose text is joined.
bool SheetLoader::ReadInline(std::string& out, bool& preserve, bool& odd) {
  XmlPull& pull = *pull_;
  out.clear();
  if (!pull.Attributes().empty()) odd = true;
  if (pull.IsEmptyElement()) {
    odd = true;
    return pull.Next() == XmlPull::Event::EndElement;
  }
  int depth = pull.Depth();
  int ts = 0;
  bool markup = false, spaceText = false;
  std::string piece;
  for (;;) {
    XmlPull::Event e = pull.Next();
    switch (e) {
      case XmlPull::Event::Text:
      case XmlPull::Event::CData:
        if (AllSpace(pull.Text())) spaceText = true;
        else odd = true;
        break;
      case XmlPull::Event::EndElement:
        if (pull.Depth() == depth) {
          if (ts != 1 || (spaceText && !markup)) odd = true;
          return true;
        }
        break;
      case XmlPull::Event::StartElement: {
        markup = true;
        std::string_view local = pull.LocalName();
        bool main = IsMainNs(pull.NamespaceUri());
        if (main && local == "t" && pull.Depth() == depth + 1) {
          ++ts;
          for (const auto& a : pull.Attributes()) {
            if (a.nsUri == pkg::kXmlNamespace && a.local == "space" && a.value == "preserve") preserve = true;
            else odd = true;
          }
          if (!ReadText(piece, odd)) return false;
          out += piece;
        } else if (main && local == "r" && pull.Depth() == depth + 1) {
          odd = true;  // runs: read their text, keep them as written
        } else if (main && local == "t" && pull.Depth() == depth + 2) {
          if (!ReadText(piece, odd)) return false;
          out += piece;
        } else {
          odd = true;
          if (!xlsx::SkipElement(pull)) return false;
        }
        break;
      }
      case XmlPull::Event::Comment:
      case XmlPull::Event::ProcessingInstruction:
        markup = odd = true;
        break;
      default:
        return false;
    }
  }
}

bool SheetLoader::ReadCell(size_t before, uint32_t row, uint32_t rowOrd, uint32_t cellOrd) {
  XmlPull& pull = *pull_;
  std::string_view rText, sText, tText, vmText;
  bool hasR = false, hasS = false, hasT = false;
  attrs_.clear();
  for (const auto& a : pull.Attributes()) {
    if (a.prefix.empty()) {
      if (a.local == "vm") vmText = a.value;  // kept with the other attributes too
      if (a.local == "r") {
        rText = a.value, hasR = true;
        continue;
      }
      if (a.local == "s") {
        sText = a.value, hasS = true;
        continue;
      }
      if (a.local == "t") {
        tText = a.value, hasT = true;
        continue;
      }
    }
    attrs_.push_back({std::string(a.qname), std::string(a.nsUri), std::string(a.value)});
  }
  // r: letters and the row's number, as the model writes it.
  const int64_t lastCol = b_->LastCol();
  bool noRef = !hasR;
  uint32_t col = 0;
  if (hasR) {
    size_t i = 0;
    uint32_t c = 0;
    while (i < rText.size() && rText[i] >= 'A' && rText[i] <= 'Z' && i < 3) c = c * 26 + static_cast<uint32_t>(rText[i++] - 'A' + 1);
    uint32_t n = 0;
    bool canonical = i > 0 && c >= 1 && c <= kMaxCols && CanonicalUInt(rText.substr(i), n) && n == row + 1;
    if (canonical) {
      col = c - 1;
    } else {
      CellAddr a;
      col = ParseA1(rText, a) ? a.col : static_cast<uint32_t>(lastCol + 1);
      attrs_.push_back({"r", "", std::string(rText)});
      noRef = true;
    }
  } else {
    col = static_cast<uint32_t>(lastCol + 1);
  }
  uint32_t style = 0;
  bool styleWritten = false;
  if (hasS) {
    if (CanonicalUInt(sText, style)) {
      styleWritten = true;
    } else {
      xlsx::ParseUInt(sText, style);
      attrs_.push_back({"s", "", std::string(sText)});
    }
  }
  XmlType type = XmlType::N;
  bool typeWritten = false;
  if (hasT) {
    typeWritten = true;
    if (tText == "n") type = XmlType::N;
    else if (tText == "s") type = XmlType::S;
    else if (tText == "str") type = XmlType::Str;
    else if (tText == "inlineStr") type = XmlType::InlineStr;
    else if (tText == "b") type = XmlType::B;
    else if (tText == "e") type = XmlType::E;
    else if (tText == "d") type = XmlType::D;
    else {
      typeWritten = false;
      attrs_.push_back({"t", "", std::string(tText)});
    }
  }
  CellRec* c = b_->AddCell(col);
  if (!c) {  // out of order: kept as written
    attrs_.clear();
    if (!xlsx::SkipElement(pull)) return false;
    Keep(rowOrd, cellOrd, Since(before));
    return true;
  }
  c->style = style;
  c->type = type;
  c->flags = static_cast<uint8_t>((styleWritten ? kStylePresent : 0) | (typeWritten ? kTypePresent : 0));
  if (!attrs_.empty() || noRef) {
    CellSide& side = b_->Side();
    side.noRef = noRef;
    side.attrs = Intern(attrs_);
  }

  // Children: f, v, is in that order, each once; anything else keeps the content as written.
  size_t contentStart = pull.Offset(), contentEnd = contentStart;
  bool odd = false, hasF = false, hasV = false, hasIs = false, preserve = false;
  pkg::AttrSetId fSet = 0;
  FormulaRec frec;
  if (pull.IsEmptyElement()) {
    if (pull.Next() != XmlPull::Event::EndElement) return false;
  } else {
    int order = 0;
    bool markup = false, spaceText = false;
    for (;;) {
      XmlPull::Event e = pull.Next();
      if (e == XmlPull::Event::EndElement) break;
      switch (e) {
        case XmlPull::Event::Text:
        case XmlPull::Event::CData:
          if (AllSpace(pull.Text())) spaceText = true;
          else odd = true;
          break;
        case XmlPull::Event::StartElement: {
          markup = true;
          std::string_view local = pull.LocalName();
          bool main = IsMainNs(pull.NamespaceUri());
          if (main && local == "f" && order < 1) {
            order = 1;
            hasF = true;
            for (const auto& a : pull.Attributes()) {
              attrs_.push_back({std::string(a.qname), std::string(a.nsUri), std::string(a.value)});
              if (!a.prefix.empty()) continue;
              uint32_t u = 0;
              if (a.local == "t") {
                frec.kind = a.value == "shared"      ? FormulaKind::Shared
                            : a.value == "array"     ? FormulaKind::Array
                            : a.value == "dataTable" ? FormulaKind::DataTable
                                                     : FormulaKind::Normal;
              } else if (a.local == "ref") {
                frec.hasRef = ParseRange(a.value, frec.ref);
              } else if (a.local == "si" && xlsx::ParseUInt(a.value, u)) {
                frec.sharedIndex = u;
              } else if (a.local == "ca") {
                frec.alwaysCalc = xlsx::ParseBool(a.value, false);
              }
            }
            fSet = Intern(attrs_);
            if (!ReadText(fText_, odd)) return false;
          } else if (main && local == "v" && order < 2) {
            order = 2;
            hasV = true;
            if (!pull.Attributes().empty()) odd = true;
            if (!ReadText(vText_, odd)) return false;
          } else if (main && local == "is" && order < 3) {
            order = 3;
            hasIs = true;
            if (!ReadInline(isText_, preserve, odd)) return false;
          } else {
            odd = true;
            if (!xlsx::SkipElement(pull)) return false;
          }
          break;
        }
        case XmlPull::Event::Comment:
        case XmlPull::Event::ProcessingInstruction:
          markup = odd = true;
          break;
        default:
          return false;
      }
    }
    contentEnd = xlsx::TagBegin(src_, pull.Offset());
    if (spaceText && !markup) odd = true;
  }
  if ((hasIs && type != XmlType::InlineStr) || (hasV && type == XmlType::InlineStr)) odd = true;
  TextPool& texts = s_.texts_;
  if (odd && contentEnd > contentStart) {
    CellSide& side = b_->Side();
    side.inner = texts.Add(src_.substr(contentStart, contentEnd - contentStart));
  }

  // The value.
  auto keepText = [&](std::string_view t) { b_->Side().valueText = texts.Add(t); };
  if (type == XmlType::InlineStr) {
    if (hasIs) {
      c->v.index = texts.Add(isText_);
      c->flags |= kHasValue;
      if (preserve) c->flags |= kPreserve;
    } else if (hasV) {
      c->v.index = texts.Add(vText_);
      c->flags |= kHasValue;
    }
  } else if (hasV) {
    switch (type) {
      case XmlType::N: {
        bool exact = false;
        uint8_t numStyle = 0;
        if (ParseNumber(vText_, c->v.number, exact, numStyle)) c->flags |= kHasValue;
        c->numStyle = numStyle;
        if (!exact) keepText(vText_);
        break;
      }
      case XmlType::S: {
        uint32_t id = 0;
        bool canonical = CanonicalUInt(vText_, id);
        if (canonical || xlsx::ParseUInt(vText_, id)) {
          if (id < book_.Strings().Size()) {
            c->v.index = id;
            c->flags |= kHasValue;
          }
        }
        if (!canonical || !(c->flags & kHasValue)) keepText(vText_);
        break;
      }
      case XmlType::Str:
        c->v.index = texts.Add(vText_);
        c->flags |= kHasValue;
        break;
      case XmlType::B:
        if (vText_ == "0" || vText_ == "1") {
          c->v.index = vText_ == "1";
        } else {
          c->v.index = vText_ == "true" ? 1 : 0;
          keepText(vText_);
        }
        c->flags |= kHasValue;
        break;
      case XmlType::E: {
        ErrorCode err = ErrorCode::Unknown;
        const bool known = ParseError(vText_, err);
        // #SPILL!, #CALC!...: written as #VALUE! with a rich value; the text stays as written.
        uint32_t vm = 0;
        const std::optional<ErrorCode> rich = !vmText.empty() && xlsx::ParseUInt(vmText, vm) ? book_.ValueError(vm) : std::nullopt;
        if (!known || (rich && *rich != err)) keepText(vText_);
        if (rich) err = *rich;
        c->v.index = static_cast<uint32_t>(err);
        c->flags |= kHasValue;
        break;
      }
      case XmlType::D:
        if (ParseIsoDate(vText_, book_.Date1904(), c->v.number)) c->flags |= kHasValue;
        keepText(vText_);
        break;
      default:
        break;
    }
  }

  // The formula: cells sharing one have the same attributes and no text, so they share a record.
  if (hasF) {
    uint32_t id = kNone;
    if (fText_.empty()) {
      auto it = textless_.find(fSet);
      if (it != textless_.end()) id = it->second;
    }
    if (id == kNone) {
      frec.attrs = fSet;
      if (!fText_.empty()) frec.text = texts.Add(fText_);
      id = static_cast<uint32_t>(s_.formulas_.size());
      s_.formulas_.push_back(frec);
      if (fText_.empty()) textless_.emplace(fSet, id);
      if (frec.kind == FormulaKind::Shared && !fText_.empty() && frec.sharedIndex != kNone)
        s_.sharedMasters_.emplace_back(frec.sharedIndex, CellAddr{row, col});
    }
    b_->SetFormula(id);
  }
  return true;
}

// ---- The elements around sheetData

namespace {

using namespace xlsx;

PaneId ParsePane(std::string_view s) {
  return s == "bottomRight" ? PaneId::BottomRight
         : s == "topRight"  ? PaneId::TopRight
         : s == "bottomLeft" ? PaneId::BottomLeft
                             : PaneId::TopLeft;
}

void ReadSqref(std::string_view s, std::vector<Range>& out) {
  size_t i = 0;
  while (i < s.size() && out.size() < 100000) {
    while (i < s.size() && s[i] == ' ') ++i;
    size_t j = s.find(' ', i);
    if (j == std::string_view::npos) j = s.size();
    Range r;
    if (j > i && ParseRange(s.substr(i, j - i), r)) out.push_back(r);
    i = j;
  }
}

void ReadBreaks(pugi::xml_node n, std::vector<PageBreak>& out) {
  for (pugi::xml_node b = n.first_child(); b; b = b.next_sibling()) {
    if (b.type() != pugi::node_element || Local(b) != "brk") continue;
    PageBreak pb;
    pb.id = UIntAttr(b, "id", 0);
    pb.min = UIntAttr(b, "min", 0);
    pb.max = UIntAttr(b, "max", 0);
    pb.manual = BoolAttr(b, "man", false);
    pb.pivot = BoolAttr(b, "pt", false);
    out.push_back(pb);
  }
}

}  // namespace

void SheetLoader::ReadTop(std::string_view local, std::string_view xml) {
  pugi::xml_document doc;
  if (!ParseSlice(doc, xml)) return;
  pugi::xml_node n = doc.document_element();
  if (local == "sheetPr") {
    SheetProps& p = s_.props_;
    p.codeName = Attr(n, "codeName");
    p.filterMode = BoolAttr(n, "filterMode", false);
    p.published = BoolAttr(n, "published", true);
    p.tabColor = ReadColor(Child(n, "tabColor"));
    if (pugi::xml_node o = Child(n, "outlinePr")) {
      p.summaryBelow = BoolAttr(o, "summaryBelow", true);
      p.summaryRight = BoolAttr(o, "summaryRight", true);
      p.applyStyles = BoolAttr(o, "applyStyles", false);
      p.showOutlineSymbols = BoolAttr(o, "showOutlineSymbols", true);
    }
    if (pugi::xml_node o = Child(n, "pageSetUpPr")) {
      p.fitToPage = BoolAttr(o, "fitToPage", false);
      p.autoPageBreaks = BoolAttr(o, "autoPageBreaks", true);
    }
  } else if (local == "dimension") {
    Range r;
    if (ParseRange(Attr(n, "ref"), r)) s_.dimension_ = r;
  } else if (local == "sheetViews") {
    for (pugi::xml_node v = n.first_child(); v; v = v.next_sibling()) {
      if (v.type() != pugi::node_element || Local(v) != "sheetView") continue;
      SheetView sv;
      sv.tabSelected = BoolAttr(v, "tabSelected", false);
      sv.showGridLines = BoolAttr(v, "showGridLines", true);
      sv.showRowColHeaders = BoolAttr(v, "showRowColHeaders", true);
      sv.showZeros = BoolAttr(v, "showZeros", true);
      sv.rightToLeft = BoolAttr(v, "rightToLeft", false);
      sv.showFormulas = BoolAttr(v, "showFormulas", false);
      sv.showOutlineSymbols = BoolAttr(v, "showOutlineSymbols", true);
      sv.showRuler = BoolAttr(v, "showRuler", true);
      sv.showWhiteSpace = BoolAttr(v, "showWhiteSpace", true);
      sv.defaultGridColor = BoolAttr(v, "defaultGridColor", true);
      sv.windowProtection = BoolAttr(v, "windowProtection", false);
      std::string_view view = Attr(v, "view");
      sv.view = view == "pageBreakPreview" ? ViewMode::PageBreakPreview : view == "pageLayout" ? ViewMode::PageLayout : ViewMode::Normal;
      sv.zoomScale = std::clamp(UIntAttr(v, "zoomScale", 100), 10u, 400u);
      sv.zoomScaleNormal = UIntAttr(v, "zoomScaleNormal", 0);
      sv.zoomScalePageLayoutView = UIntAttr(v, "zoomScalePageLayoutView", 0);
      sv.zoomScaleSheetLayoutView = UIntAttr(v, "zoomScaleSheetLayoutView", 0);
      ParseA1(Attr(v, "topLeftCell"), sv.topLeft);
      sv.colorId = UIntAttr(v, "colorId", 64);
      sv.workbookViewId = UIntAttr(v, "workbookViewId", 0);
      if (pugi::xml_node p = Child(v, "pane")) {
        std::string_view state = Attr(p, "state");
        sv.paneState = state == "frozen" ? PaneState::Frozen : state == "frozenSplit" ? PaneState::FrozenSplit : PaneState::Split;
        sv.xSplit = std::max(0.0, NumAttr(p, "xSplit", 0));
        sv.ySplit = std::max(0.0, NumAttr(p, "ySplit", 0));
        ParseA1(Attr(p, "topLeftCell"), sv.paneTopLeft);
        sv.activePane = ParsePane(Attr(p, "activePane"));
        if (sv.xSplit == 0 && sv.ySplit == 0) sv.paneState = PaneState::None;
      }
      for (pugi::xml_node s = v.first_child(); s; s = s.next_sibling()) {
        if (s.type() != pugi::node_element || Local(s) != "selection") continue;
        Selection sel;
        sel.pane = ParsePane(Attr(s, "pane"));
        ParseA1(Attr(s, "activeCell"), sel.active);
        sel.activeRange = UIntAttr(s, "activeCellId", 0);
        std::string_view sq = Attr(s, "sqref");
        if (sq.empty()) sel.ranges.push_back(Range::Cell(sel.active.row, sel.active.col));
        else ReadSqref(sq, sel.ranges);
        sv.selections.push_back(std::move(sel));
      }
      s_.views_.push_back(std::move(sv));
    }
  } else if (local == "sheetFormatPr") {
    SheetFormat& f = s_.format_;
    f.baseColWidth = std::clamp(NumAttr(n, "baseColWidth", 8), 0.0, 255.0);
    f.defaultColWidth = std::clamp(NumAttr(n, "defaultColWidth", 0), 0.0, 255.0);
    f.defaultRowHeight = std::clamp(NumAttr(n, "defaultRowHeight", 15), 0.0, 409.5);
    f.customHeight = BoolAttr(n, "customHeight", false);
    f.zeroHeight = BoolAttr(n, "zeroHeight", false);
    f.thickTop = BoolAttr(n, "thickTop", false);
    f.thickBottom = BoolAttr(n, "thickBottom", false);
    f.outlineLevelRow = static_cast<uint8_t>(std::min(UIntAttr(n, "outlineLevelRow", 0), 7u));
    f.outlineLevelCol = static_cast<uint8_t>(std::min(UIntAttr(n, "outlineLevelCol", 0), 7u));
  } else if (local == "cols") {
    for (pugi::xml_node c = n.first_child(); c; c = c.next_sibling()) {
      if (c.type() != pugi::node_element || Local(c) != "col") continue;
      ColumnInfo ci;
      uint32_t lo = UIntAttr(c, "min", 0), hi = UIntAttr(c, "max", 0);
      if (lo < 1 || hi < lo || lo > kMaxCols) continue;
      ci.first = lo - 1;
      ci.last = std::min(hi, kMaxCols) - 1;
      ci.width = std::clamp(NumAttr(c, "width", 0), 0.0, 255.0);
      ci.style = UIntAttr(c, "style", 0);
      ci.hidden = BoolAttr(c, "hidden", false);
      ci.customWidth = BoolAttr(c, "customWidth", false);
      ci.bestFit = BoolAttr(c, "bestFit", false);
      ci.collapsed = BoolAttr(c, "collapsed", false);
      ci.phonetic = BoolAttr(c, "phonetic", false);
      ci.outlineLevel = static_cast<uint8_t>(std::min(UIntAttr(c, "outlineLevel", 0), 7u));
      s_.cols_.push_back(ci);
    }
    // By first column, without overlaps (a later range gives way to an earlier one).
    std::stable_sort(s_.cols_.begin(), s_.cols_.end(), [](const ColumnInfo& a, const ColumnInfo& b) { return a.first < b.first; });
    std::vector<ColumnInfo> clean;
    for (ColumnInfo ci : s_.cols_) {
      if (!clean.empty() && ci.first <= clean.back().last) {
        if (ci.last <= clean.back().last) continue;
        ci.first = clean.back().last + 1;
      }
      clean.push_back(ci);
    }
    s_.cols_ = std::move(clean);
  } else if (local == "mergeCells") {
    for (pugi::xml_node m = n.first_child(); m; m = m.next_sibling()) {
      Range r;
      if (m.type() == pugi::node_element && Local(m) == "mergeCell" && ParseRange(Attr(m, "ref"), r) &&
          !(r.firstRow == r.lastRow && r.firstCol == r.lastCol))
        s_.merges_.push_back(r);
    }
    std::sort(s_.merges_.begin(), s_.merges_.end(),
              [](const Range& a, const Range& b) { return a.firstRow != b.firstRow ? a.firstRow < b.firstRow : a.firstCol < b.firstCol; });
    for (const Range& r : s_.merges_) s_.mergeMaxHeight_ = std::max(s_.mergeMaxHeight_, r.lastRow - r.firstRow + 1);
  } else if (local == "hyperlinks") {
    const pkg::Package* pkg = book_.GetPackage();
    const pkg::RelationshipSet* rels = pkg ? pkg->RelationshipsOf(s_.part_) : nullptr;
    for (pugi::xml_node h = n.first_child(); h; h = h.next_sibling()) {
      if (h.type() != pugi::node_element || Local(h) != "hyperlink") continue;
      Hyperlink link;
      if (!ParseRange(Attr(h, "ref"), link.ref)) continue;
      link.relId = RelIdAttr(h);
      link.location = Attr(h, "location");
      link.display = Attr(h, "display");
      link.tooltip = Attr(h, "tooltip");
      if (const pkg::Relationship* r = rels && !link.relId.empty() ? rels->Find(link.relId) : nullptr)
        link.target = r->external ? r->target : r->targetPart;
      s_.links_.push_back(std::move(link));
    }
  } else if (local == "printOptions") {
    PageSetup& p = s_.page_;
    p.horizontalCentered = BoolAttr(n, "horizontalCentered", false);
    p.verticalCentered = BoolAttr(n, "verticalCentered", false);
    p.headings = BoolAttr(n, "headings", false);
    p.gridLines = BoolAttr(n, "gridLines", false);
    p.gridLinesSet = BoolAttr(n, "gridLinesSet", true);
  } else if (local == "pageMargins") {
    PageMargins& m = s_.page_.margins;
    auto margin = [&](const char* name, double def) { return std::clamp(NumAttr(n, name, def), 0.0, 49.0); };
    m.left = margin("left", m.left);
    m.right = margin("right", m.right);
    m.top = margin("top", m.top);
    m.bottom = margin("bottom", m.bottom);
    m.header = margin("header", m.header);
    m.footer = margin("footer", m.footer);
  } else if (local == "pageSetup") {
    PageSetup& p = s_.page_;
    p.paperSize = UIntAttr(n, "paperSize", 1);
    p.paperWidth = Attr(n, "paperWidth");
    p.paperHeight = Attr(n, "paperHeight");
    p.scale = std::clamp(UIntAttr(n, "scale", 100), 10u, 400u);
    p.firstPageNumber = IntAttr(n, "firstPageNumber", 1);
    p.useFirstPageNumber = BoolAttr(n, "useFirstPageNumber", false);
    p.fitToWidth = UIntAttr(n, "fitToWidth", 1);
    p.fitToHeight = UIntAttr(n, "fitToHeight", 1);
    p.pageOrder = Attr(n, "pageOrder") == "overThenDown" ? PageOrder::OverThenDown : PageOrder::DownThenOver;
    std::string_view o = Attr(n, "orientation");
    p.orientation = o == "portrait" ? Orientation::Portrait : o == "landscape" ? Orientation::Landscape : Orientation::Default;
    p.usePrinterDefaults = BoolAttr(n, "usePrinterDefaults", true);
    p.blackAndWhite = BoolAttr(n, "blackAndWhite", false);
    p.draft = BoolAttr(n, "draft", false);
    std::string_view cc = Attr(n, "cellComments");
    p.cellComments = cc == "asDisplayed" ? CellComments::AsDisplayed : cc == "atEnd" ? CellComments::AtEnd : CellComments::None;
    std::string_view er = Attr(n, "errors");
    p.errors = er == "blank" ? PrintErrors::Blank : er == "dash" ? PrintErrors::Dash : er == "NA" ? PrintErrors::NA : PrintErrors::Displayed;
    p.horizontalDpi = UIntAttr(n, "horizontalDpi", 600);
    p.verticalDpi = UIntAttr(n, "verticalDpi", 600);
    p.copies = UIntAttr(n, "copies", 1);
  } else if (local == "headerFooter") {
    HeaderFooter& h = s_.headerFooter_;
    h.differentOddEven = BoolAttr(n, "differentOddEven", false);
    h.differentFirst = BoolAttr(n, "differentFirst", false);
    h.scaleWithDoc = BoolAttr(n, "scaleWithDoc", true);
    h.alignWithMargins = BoolAttr(n, "alignWithMargins", true);
    for (pugi::xml_node c = n.first_child(); c; c = c.next_sibling()) {
      if (c.type() != pugi::node_element) continue;
      std::string_view k = Local(c);
      std::string* to = k == "oddHeader"     ? &h.oddHeader
                        : k == "oddFooter"   ? &h.oddFooter
                        : k == "evenHeader"  ? &h.evenHeader
                        : k == "evenFooter"  ? &h.evenFooter
                        : k == "firstHeader" ? &h.firstHeader
                        : k == "firstFooter" ? &h.firstFooter
                                             : nullptr;
      if (to) *to = TextOf(c);
    }
  } else if (local == "rowBreaks") {
    ReadBreaks(n, s_.rowBreaks_);
  } else if (local == "colBreaks") {
    ReadBreaks(n, s_.colBreaks_);
  } else if (local == "drawing") {
    s_.drawingRel_ = RelIdAttr(n);
  } else if (local == "legacyDrawing") {
    s_.legacyDrawingRel_ = RelIdAttr(n);
  } else if (local == "tableParts") {
    for (pugi::xml_node t = n.first_child(); t; t = t.next_sibling())
      if (t.type() == pugi::node_element && Local(t) == "tablePart") s_.tableRels_.emplace_back(RelIdAttr(t));
  }
}

namespace xlsx {

Status LoadSheetPart(Workbook& book, size_t index) {
  Sheet& sheet = book.GetSheet(index);
  const pkg::Package* pkg = book.GetPackage();
  if (sheet.Loaded() || !pkg || sheet.PartName().empty()) return {};
  std::vector<uint8_t> bytes;
  Status st = pkg->ReadPart(sheet.PartName(), bytes);
  if (!st.Ok()) return st;
  std::string_view xml(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  SheetLoader loader(book, sheet);
  st = loader.Load(xml, pkg->GetLimits(), sheet.PartName().substr(1));
  WorkbookLoader::MarkLoaded(sheet, st.Ok());
  return st;
}

}  // namespace xlsx
}  // namespace fw::sheet
