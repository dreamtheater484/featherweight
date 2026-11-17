// Fuzz target: CSV and TSV (src/sheet/csv, S1). The input is a file; it's read with the delimiter
// detected and given, with and without values converted, on one thread and several. Rows must lie in
// order inside the text, and every cell must show exactly its field (a value is never changed).
// Seeds: tests/office/csv.
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "core/Strings.h"
#include "sheet/csv/Csv.h"
#include "sheet/numfmt/NumFormat.h"

using namespace fw::sheet;

namespace {

std::string Shown(const Workbook& book, const CellInfo& info, const NumLocale& loc) {
  switch (info.value.type) {
    case ValueType::String: return std::string(info.value.text);
    case ValueType::Bool: return info.value.boolean ? "TRUE" : "FALSE";
    case ValueType::Error: return std::string(ErrorText(info.value.error));
    case ValueType::Number: {
      const Styles& st = book.GetStyles();
      const uint32_t id = st.CellXf(info.style).numFmt;
      const std::string_view code = st.FormatCode(id);
      const NumberFormat f = code.empty() ? NumberFormat::Builtin(static_cast<int>(id), loc) : NumberFormat(fw::Utf8ToWide(code));
      FormatContext ctx;
      ctx.locale = &loc;
      Formatted out;
      f.Format(info.value.number, ctx, out);
      return fw::WideToUtf8(out.text);
    }
    default: return {};
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const std::string_view bytes(reinterpret_cast<const char*>(data), size);
  const NumLocale* locales[] = {&EnUsLocale(), &EnNlLocale()};
  size_t cells = SIZE_MAX;
  for (int pass = 0; pass < 4; ++pass) {
    csv::Options o;
    o.locale = locales[pass & 1];
    o.delimiter = pass == 2 ? ';' : 0;
    o.convert = pass != 3;
    o.threads = pass == 1 ? 3 : 1;
    csv::Reader reader(bytes, o);
    const csv::Source& src = reader.Info();
    if (src.rowStart.size() != src.rowEnd.size()) std::abort();
    for (size_t r = 0; r < src.rowStart.size(); ++r) {
      if (src.rowStart[r] > src.rowEnd[r] || src.rowEnd[r] > reader.Text().size()) std::abort();
      if (r + 1 < src.rowStart.size() && src.rowEnd[r] >= src.rowStart[r + 1] && src.rowStart[r + 1] != src.rowEnd[r])
        std::abort();
    }
    Workbook book;
    Sheet& sheet = book.AddSheet("S");
    {
      SheetBuilder builder(sheet);
      reader.AddRows(book, builder, 0, 5);
      reader.AddRows(book, builder, 5, reader.RowCount());
    }
    if (pass < 2) {  // the same file gives the same cells on one thread or several
      if (cells != SIZE_MAX && cells != sheet.CellCount()) std::abort();
      cells = sheet.CellCount();
    }
    std::vector<std::string> fields;
    for (size_t r = 0; r < reader.RowCount() && r < 64; ++r) {
      reader.Fields(r, fields);
      for (size_t c = 0; c < fields.size() && c < 64; ++c) {
        CellInfo info;
        const bool has = sheet.GetCell(static_cast<uint32_t>(r), static_cast<uint32_t>(c), info);
        if (fields[c].empty() != !has) std::abort();
        if (has && Shown(book, info, *o.locale) != fields[c]) std::abort();
      }
    }
  }
  return 0;
}
