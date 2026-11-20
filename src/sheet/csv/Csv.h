#pragma once

// CSV and TSV files (plan §7.1): opened without changing any value. The encoding and the delimiter
// are detected, Excel's quoting rules apply, and a value becomes a number, date, TRUE/FALSE or error
// only when it shows exactly as written (numfmt's ParseExact); everything else stays text. What
// S2 needs to save the file as it was (encoding, byte-order mark, delimiter, quoting, line ends, where
// each row starts) is recorded.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "sheet/model/Workbook.h"
#include "sheet/numfmt/NumLocale.h"

namespace fw::sheet::csv {

enum class Encoding : uint8_t { Utf8, Utf16LE, Utf16BE, Ansi };
enum class LineEnd : uint8_t { CRLF, LF, CR };

// How the file is written, for saving it the same way.
struct Source {
  Encoding encoding = Encoding::Utf8;
  bool bom = false;
  uint32_t codePage = 65001;   // Ansi: the code page it was read with
  char delimiter = ',';
  bool sepLine = false;        // it starts with Excel's "sep=;" line, which isn't a row
  std::string sepText;         // that line with its line end, as UTF-8
  LineEnd lineEnd = LineEnd::CRLF;  // the most common one
  bool mixedLineEnds = false;
  bool finalLineEnd = true;    // the last row ends with a line end
  bool quotedAll = false;      // every field is quoted
  bool tooManyRows = false;    // rows beyond the sheet's 1,048,576 were left out
  bool tooManyColumns = false; // fields beyond column XFD were left out
  // Where each row starts and ends (without its line end) in the text as UTF-8 (the file's own bytes
  // for UTF-8, after the byte-order mark and sep= line).
  std::vector<uint32_t> rowStart, rowEnd;
};

struct Options {
  const NumLocale* locale = nullptr;  // nullptr: Windows' (UserLocale)
  char delimiter = 0;                  // 0: detect ('\t' first for a .tsv)
  bool tsv = false;                    // the file is .tsv: a tab wins a tie
  bool convert = true;                 // false: every field stays text
  unsigned threads = 0;                // 0: the PC's cores (up to 8)
  uint32_t ansiCodePage = 0;           // a file that isn't UTF-8 or UTF-16; 0: Windows' (SystemAnsiCodePage)
};

// Windows' ANSI code page, which Excel reads such a file in (1252 on a Western PC). Not GetACP():
// the app's manifest makes that UTF-8 (65001).
uint32_t SystemAnsiCodePage();

class Reader {
 public:
  // Decodes the file and finds its rows (one quick pass). `bytes` must outlive the reader.
  Reader(std::string_view bytes, const Options& options);

  const Source& Info() const { return source_; }
  size_t RowCount() const { return source_.rowStart.size(); }
  // The text as UTF-8.
  std::string_view Text() const { return text_; }
  // A row's fields, quotes removed.
  void Fields(size_t row, std::vector<std::string>& out) const;

  // Adds rows [first, last) to the sheet in order, parsed on worker threads (rows already added
  // stay; call with growing ranges, the first screen first). Formats for numbers and dates are added
  // to the workbook's styles.
  void AddRows(Workbook& book, SheetBuilder& builder, size_t first, size_t last);

 private:
  void Decode(std::string_view bytes);
  void FindRows();
  Options options_;
  const NumLocale* locale_;
  Source source_;
  std::string decoded_;
  std::string_view text_;
  std::vector<std::pair<std::string, uint32_t>> styles_;  // format code (UTF-8) -> cell style
  size_t stringBytes_ = 0;                                  // text added to the workbook's strings
};

// The first sheet as the CSV file it was opened from (plan §7.1): `original` is that file, read again
// with the options it was opened with. Its encoding, byte-order mark, sep= line, delimiter, quoting
// and line ends are kept, and a row whose values read as they did is written as it was read (the
// file unchanged gives the same bytes; columns and rows past the sheet's edge are kept).
std::string Write(const Workbook& book, std::string_view original, const Options& options);
// The first sheet as a new CSV file, as Excel writes it from the used range, but UTF-8 with a
// byte-order mark and the region's list separator (`options.delimiter`, else the locale's; a tab
// for `tsv`), CRLF after each row.
std::string WriteNew(const Workbook& book, const Options& options);

// Whether saving as CSV loses something the workbook holds (S2 plan: the "CSV keeps only the values
// of the first sheet" banner): another sheet, a formula, formatting beyond number formats (fonts,
// fills, borders, alignment, cell styles), merged cells, column widths or row heights, hidden rows
// or columns, hyperlinks, a filter, a table, conditional formats or validations, charts, pictures,
// shapes, notes or comments. Number formats aren't: the values are written as shown.
bool Loses(const Workbook& book);

// The sheet a CSV file is read into, as Read makes it: the workbook's format, default styles and
// one sheet named `sheetName` (for reading in steps with Reader::AddRows).
Sheet& NewSheet(Workbook& book, const std::string& sheetName);

// The whole file as a workbook with one sheet named `sheetName` (format Csv).
void Read(std::string_view bytes, const std::string& sheetName, const Options& options, Workbook& book,
          Source& source);

}  // namespace fw::sheet::csv
