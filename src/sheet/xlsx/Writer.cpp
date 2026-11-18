// The xlsx writer, for the round-trip tests (plan §10): sheetData and the shared strings from the
// model, and what the reader kept as written around them. Saving comes in S2.
#include <charconv>

#include "pkg/XmlWriter.h"
#include "sheet/xlsx/Internal.h"
#include "sheet/xlsx/Xlsx.h"

namespace fw::sheet::xlsx {

using pkg::Status;
using namespace detail;

namespace {

constexpr std::string_view kDeclaration = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n";

// "x:" for a start tag "<x:sheetData ...>", "" without a prefix.
std::string PrefixOf(std::string_view startTag) {
  if (startTag.empty()) return {};
  size_t end = 1;
  while (end < startTag.size() && startTag[end] != ' ' && startTag[end] != '/' && startTag[end] != '>' &&
         startTag[end] != '\t' && startTag[end] != '\r' && startTag[end] != '\n')
    ++end;
  std::string_view name = startTag.substr(1, end - 1);
  size_t colon = name.find(':');
  return colon == std::string_view::npos ? std::string() : std::string(name.substr(0, colon + 1));
}

void WriteAttrs(std::string& out, const std::vector<pkg::XmlAttr>& attrs) {
  for (const auto& a : attrs) {
    out += ' ';
    out += a.qname;
    out += "=\"";
    pkg::AppendEscapedAttribute(out, a.value);
    out += '"';
  }
}

void WriteUInt(std::string& out, uint64_t v) {
  char buf[24];
  auto [p, ec] = std::to_chars(buf, buf + sizeof buf, v);
  out.append(buf, p);
}

void WriteNumber(std::string& out, double v, uint8_t style) {
  char buf[48];
  out.append(buf, FormatNumber(v, style, buf));
}

void WriteCellName(std::string& out, uint32_t row, uint32_t col) {
  char letters[4];
  size_t n = 0;
  for (uint32_t c = col + 1; c > 0 && n < 4; c = (c - 1) / 26) letters[n++] = static_cast<char>('A' + (c - 1) % 26);
  while (n > 0) out += letters[--n];
  WriteUInt(out, static_cast<uint64_t>(row) + 1);
}

std::string_view TypeName(XmlType t) {
  switch (t) {
    case XmlType::N: return "n";
    case XmlType::S: return "s";
    case XmlType::Str: return "str";
    case XmlType::InlineStr: return "inlineStr";
    case XmlType::B: return "b";
    case XmlType::E: return "e";
    case XmlType::D: return "d";
  }
  return "n";
}

class SheetWriter {
 public:
  SheetWriter(const Sheet& s, std::string& out, const PartText& text)
      : s_(s), out_(out), kept_(s.KeptInData()), text_(text) {}

  void Write() {
    const PartText& t = text_;
    out_ = kDeclaration;
    out_ += t.prolog;
    out_ += t.rootStart;
    if (t.rootEnd.empty()) {  // an empty root element
      out_ += t.epilog;
      return;
    }
    out_ += t.beforeData;
    if (t.hasData) WriteData(t);
    out_ += t.afterData;
    out_ += t.rootEnd;
    out_ += t.epilog;
  }

 private:
  void Open(std::string_view local) {
    out_ += '<';
    out_ += p_;
    out_ += local;
  }
  void Close(std::string_view local) {
    out_ += "</";
    out_ += p_;
    out_ += local;
    out_ += '>';
  }
  // Kept XML anchored at (row, col): kBeforeRow, a cell's position, or anything inside the row.
  void EmitKept(uint32_t row, uint32_t col, bool restOfRow = false) {
    while (k_ < kept_.size() && kept_[k_].row == row &&
           (restOfRow ? kept_[k_].col != Kept::kBeforeRow : kept_[k_].col == col)) {
      out_ += kept_[k_].xml;
      ++k_;
    }
  }

  void WriteData(const PartText& t) {
    p_ = PrefixOf(t.dataStart);
    bool anything = !kept_.empty() || s_.CellCount() > 0;
    for (const auto& b : s_.Blocks()) anything = anything || !b->rows.empty();
    if (t.emptyData && !anything) {
      out_ += t.dataStart;
      return;
    }
    if (t.emptyData) {
      std::string_view tag = t.dataStart;
      tag.remove_suffix(2);  // "/>"
      out_ += tag;
      out_ += '>';
    } else {
      out_ += t.dataStart;
    }
    uint32_t rowOrd = 0;
    for (const auto& block : s_.Blocks()) {
      const RowBlock& b = *block;
      size_t f = 0, side = 0;
      for (const RowRec& r : b.rows) {
        EmitKept(rowOrd, Kept::kBeforeRow);
        Open("row");
        if (!(r.flags & kRowNoRef)) {
          out_ += " r=\"";
          WriteUInt(out_, static_cast<uint64_t>(r.row) + 1);
          out_ += '"';
        }
        WriteAttrs(out_, s_.Attributes(r.attrs));
        bool keptInside = k_ < kept_.size() && kept_[k_].row == rowOrd && kept_[k_].col != Kept::kBeforeRow;
        if (r.count == 0 && !keptInside) {
          out_ += "/>";
        } else {
          out_ += '>';
          for (uint32_t i = 0; i < r.count; ++i) {
            EmitKept(rowOrd, i);
            WriteCell(b, r, r.first + i, f, side);
          }
          EmitKept(rowOrd, 0, true);
          Close("row");
        }
        ++rowOrd;
      }
    }
    while (k_ < kept_.size()) out_ += kept_[k_++].xml;
    Close("sheetData");
  }

  void WriteF(const FormulaRec& formula, const TextPool& texts) {
    Open("f");
    WriteAttrs(out_, s_.Attributes(formula.attrs));
    if (formula.text != kNone && !texts.Get(formula.text).empty()) {
      out_ += '>';
      pkg::AppendEscapedText(out_, texts.Get(formula.text));
      Close("f");
    } else {
      out_ += "/>";
    }
  }

  // XML text with the predefined entities read, to compare two writings of an element.
  static std::string Plain(std::string_view s) {
    static constexpr std::pair<std::string_view, char> kEntities[] = {
        {"&lt;", '<'}, {"&gt;", '>'}, {"&amp;", '&'}, {"&quot;", '"'}, {"&apos;", '\''}};
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
      bool done = false;
      if (s[i] == '&')
        for (const auto& [name, ch] : kEntities)
          if (s.substr(i, name.size()) == name) {
            out += ch;
            i += name.size();
            done = true;
            break;
          }
      if (!done) out += s[i++];
    }
    return out;
  }

  // The f element in a cell's content as written: [begin, end).
  static bool FindF(std::string_view s, size_t& begin, size_t& end) {
    auto nameChar = [](char ch) {
      return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' ||
             ch == '.';
    };
    for (size_t at = s.find('<'); at != std::string_view::npos; at = s.find('<', at + 1)) {
      size_t n = at + 1, name = n;
      while (n < s.size() && nameChar(s[n])) ++n;
      if (n < s.size() && s[n] == ':') {
        name = ++n;
        while (n < s.size() && nameChar(s[n])) ++n;
      }
      if (n - name != 1 || s[name] != 'f' || n >= s.size() ||
          !(s[n] == ' ' || s[n] == '>' || s[n] == '/' || s[n] == '\t' || s[n] == '\r' || s[n] == '\n'))
        continue;
      const size_t gt = s.find('>', n);
      if (gt == std::string_view::npos) return false;
      begin = at;
      if (s[gt - 1] == '/') {
        end = gt + 1;
        return true;
      }
      const std::string close = "</" + std::string(s.substr(at + 1, n - at - 1)) + ">";
      const size_t c = s.find(close, gt);
      if (c == std::string_view::npos) return false;
      end = c + close.size();
      return true;
    }
    return false;
  }

  void WriteCell(const RowBlock& b, const RowRec& r, uint32_t index, size_t& f, size_t& side) {
    const CellRec& c = b.cells[index];
    while (side < b.sides.size() && b.sides[side].cell < index) ++side;
    const CellSide* sd = side < b.sides.size() && b.sides[side].cell == index ? &b.sides[side] : nullptr;
    while (f < b.formulas.size() && b.formulas[f].first < index) ++f;
    const FormulaRec* formula = nullptr;
    if ((c.flags & kHasFormula) && f < b.formulas.size() && b.formulas[f].first == index &&
        b.formulas[f].second < s_.FormulaRecs().size())
      formula = &s_.FormulaRecs()[b.formulas[f].second];
    const TextPool& texts = s_.Texts();

    Open("c");
    if (!(sd && sd->noRef)) {
      out_ += " r=\"";
      WriteCellName(out_, r.row, c.col);
      out_ += '"';
    }
    if (c.flags & kStylePresent) {
      out_ += " s=\"";
      WriteUInt(out_, c.style);
      out_ += '"';
    }
    if (c.type != XmlType::N || (c.flags & kTypePresent)) {
      out_ += " t=\"";
      out_ += TypeName(c.type);
      out_ += '"';
    }
    if (sd) WriteAttrs(out_, s_.Attributes(sd->attrs));
    if (sd && sd->inner != kNone) {
      // The content as written, but the formula from the model: edits rewrite formulas (references
      // moved, a shared group split) without writing the cell again.
      out_ += '>';
      std::string_view inner = texts.Get(sd->inner);
      size_t fb = 0, fe = 0;
      if (formula && FindF(inner, fb, fe)) {
        const size_t at = out_.size();
        out_.append(inner.substr(0, fb));
        const size_t f0 = out_.size();
        WriteF(*formula, texts);
        // Unchanged (escaped differently at most): as written.
        if (Plain(std::string_view(out_).substr(f0)) == Plain(inner.substr(fb, fe - fb))) {
          out_.resize(at);
          out_.append(inner);
        } else {
          out_.append(inner.substr(fe));
        }
      } else {
        out_.append(inner);
      }
      Close("c");
      return;
    }
    bool hasValue = (c.flags & kHasValue) != 0;
    bool inlineStr = c.type == XmlType::InlineStr && hasValue;
    bool v = (hasValue && !inlineStr) || (sd && sd->valueText != kNone);
    if (!formula && !v && !inlineStr) {
      out_ += "/>";
      return;
    }
    out_ += '>';
    if (formula) WriteF(*formula, texts);
    if (v) {
      Open("v");
      out_ += '>';
      if (sd && sd->valueText != kNone) {
        pkg::AppendEscapedText(out_, texts.Get(sd->valueText));
      } else {
        switch (c.type) {
          case XmlType::N:
          case XmlType::D:
            WriteNumber(out_, c.v.number, c.numStyle);
            break;
          case XmlType::S:
            WriteUInt(out_, c.v.index);
            break;
          case XmlType::Str:
          case XmlType::InlineStr:
            pkg::AppendEscapedText(out_, texts.Get(c.v.index));
            break;
          case XmlType::B:
            out_ += c.v.index ? '1' : '0';
            break;
          case XmlType::E: {
            // Errors newer than the format (#SPILL!, #CALC!...) as Excel stores them without their
            // rich value: #VALUE! (Excel repairs a file with <v>#SPILL!</v>).
            ErrorCode e = static_cast<ErrorCode>(c.v.index);
            out_ += ErrorText(e > ErrorCode::GettingData ? ErrorCode::Value : e);
            break;
          }
        }
      }
      Close("v");
    }
    if (inlineStr) {
      Open("is");
      out_ += '>';
      Open("t");
      if (c.flags & kPreserve) out_ += " xml:space=\"preserve\"";
      out_ += '>';
      pkg::AppendEscapedText(out_, texts.Get(c.v.index));
      Close("t");
      Close("is");
    }
    Close("c");
  }

  const Sheet& s_;
  std::string& out_;
  const std::vector<Kept>& kept_;
  const PartText& text_;
  size_t k_ = 0;
  std::string p_;
};

void WriteStrings(const StringTable& table, std::string& out) {
  const StringTable::PartText& part = table.Part();
  out = kDeclaration;
  out += part.prolog;
  if (part.rootEnd.empty() && table.Size() == 0) {
    out += part.rootStart;
    out += part.epilog;
    return;
  }
  std::string_view start = part.rootStart;
  if (part.rootEnd.empty() && start.ends_with("/>")) {  // <sst/>, and strings added since
    start.remove_suffix(2);
    out += start;
    out += '>';
  } else {
    out += start;
  }
  std::string p = PrefixOf(part.rootStart);
  size_t k = 0;
  for (uint32_t id = 0; id < table.Size(); ++id) {
    while (k < part.kept.size() && part.kept[k].first == id) out += part.kept[k++].second;
    if (table.Flags(id) & StringTable::kRaw) {
      out += table.RawXml(id);
      continue;
    }
    out += '<' + p + "si><" + p + 't';
    if (table.Flags(id) & StringTable::kPreserve) out += " xml:space=\"preserve\"";
    out += '>';
    pkg::AppendEscapedText(out, table.Text(id));
    out += "</" + p + "t></" + p + "si>";
  }
  while (k < part.kept.size()) out += part.kept[k++].second;
  out += part.rootEnd.empty() ? "</" + p + "sst>" : part.rootEnd;
  out += part.epilog;
}

}  // namespace

void WriteSheet(const Sheet& sheet, const PartText& text, std::string& out) { SheetWriter(sheet, out, text).Write(); }
void WriteStringsPart(const StringTable& table, std::string& out) { WriteStrings(table, out); }

Status WritePart(const Workbook& book, std::string_view partName, std::string& out) {
  out.clear();
  if (!book.StringsPart().empty() && partName == book.StringsPart()) {
    WriteStrings(book.Strings(), out);
    return {};
  }
  for (size_t i = 0; i < book.SheetCount(); ++i) {
    const Sheet& s = book.GetSheet(i);
    if (s.PartName() != partName) continue;
    if (!s.Loaded() || s.Text().rootStart.empty())
      return Status::Fail(Status::Code::NotFound, "The sheet " + s.Name() + " isn't loaded.");
    SheetWriter(s, out, s.Text()).Write();
    return {};
  }
  return Status::Fail(Status::Code::NotFound, "The model doesn't write " + std::string(partName) + ".");
}

}  // namespace fw::sheet::xlsx
