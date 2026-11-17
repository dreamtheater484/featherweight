#include "sheet/csv/Csv.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <thread>
#include <unordered_map>

#include "core/Strings.h"
#include "sheet/numfmt/Parse.h"

namespace fw::sheet::csv {

namespace {

constexpr size_t kMaxRows = 1048576, kMaxCols = 16384;
constexpr size_t kBatchRows = 65536;  // rows parsed before they go to the model

bool ValidUtf8(std::string_view s) {
  const auto* p = reinterpret_cast<const unsigned char*>(s.data());
  const auto* e = p + s.size();
  while (p < e) {
    if (e - p >= 8) {
      uint64_t w;
      std::memcpy(&w, p, 8);
      if (!(w & 0x8080808080808080ull)) {
        p += 8;
        continue;
      }
    }
    const unsigned c = *p;
    if (c < 0x80) {
      ++p;
      continue;
    }
    int n;
    unsigned min;
    if ((c & 0xE0) == 0xC0) n = 1, min = 0x80;
    else if ((c & 0xF0) == 0xE0) n = 2, min = 0x800;
    else if ((c & 0xF8) == 0xF0) n = 3, min = 0x10000;
    else return false;
    if (e - p < n + 1) return false;
    unsigned cp = c & (0x3Fu >> n);
    for (int k = 1; k <= n; ++k) {
      if ((p[k] & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (p[k] & 0x3F);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
    p += n + 1;
  }
  return true;
}

std::string WideToUtf8Big(const wchar_t* w, size_t n) {
  std::string out;
  if (!n || n > 0x7FFFFFFF) return out;
  const int len = WideCharToMultiByte(CP_UTF8, 0, w, static_cast<int>(n), nullptr, 0, nullptr, nullptr);
  out.resize(static_cast<size_t>(len));
  WideCharToMultiByte(CP_UTF8, 0, w, static_cast<int>(n), out.data(), len, nullptr, nullptr);
  return out;
}

// Fields of one row (without its line end), quotes removed, as Excel reads them: a field that starts
// with a quote runs to the closing quote ("" is a quote) and on to the delimiter; a quote anywhere
// else is a character.
template <typename Fn>
void SplitRow(std::string_view row, char d, std::string& scratch, Fn&& emit) {
  size_t i = 0;
  for (;;) {
    size_t e;
    if (i < row.size() && row[i] == '"') {
      scratch.clear();
      for (++i; i < row.size(); ++i) {
        if (row[i] != '"') {
          scratch += row[i];
        } else if (i + 1 < row.size() && row[i + 1] == '"') {
          scratch += '"';
          ++i;
        } else {
          ++i;
          break;
        }
      }
      e = std::min(row.find(d, i), row.size());
      scratch.append(row.substr(i, e - i));
      emit(std::string_view(scratch), true);
    } else {
      e = std::min(row.find(d, i), row.size());
      emit(row.substr(i, e - i), false);
    }
    if (e >= row.size()) return;
    i = e + 1;
  }
}

// Field counts per line for a delimiter: the most common count and how many lines have it.
void Consistency(std::string_view t, char d, size_t& mode, size_t& agree) {
  std::vector<size_t> counts;
  size_t i = 0;
  while (i < t.size() && counts.size() < 50) {
    size_t fields = 1;
    bool start = true, quote = false;
    for (; i < t.size(); ++i) {
      const char c = t[i];
      if (quote) {
        if (c == '"') {
          if (i + 1 < t.size() && t[i + 1] == '"') ++i;
          else quote = false;
        }
        continue;
      }
      if (start && c == '"') {
        quote = true;
        start = false;
        continue;
      }
      start = false;
      if (c == d) ++fields, start = true;
      else if (c == '\n' || c == '\r') break;
    }
    if (i >= t.size() && counts.size() > 1 && t.size() >= 65536) break;  // a sample's cut-off line
    counts.push_back(fields);
    if (i < t.size() && t[i] == '\r' && i + 1 < t.size() && t[i + 1] == '\n') ++i;
    ++i;
  }
  mode = agree = 0;
  for (size_t c : counts) {
    const size_t n = static_cast<size_t>(std::count(counts.begin(), counts.end(), c));
    if (n > agree || (n == agree && c > mode)) mode = c, agree = n;
  }
}

char DetectDelimiter(std::string_view t, bool tsv) {
  const std::string_view sample = t.substr(0, 65536);
  char best = ',';
  size_t bestAgree = 0, bestMode = 0;
  const char tsvOrder[] = {'\t', ',', ';', '|'}, csvOrder[] = {',', ';', '\t', '|'};
  for (char d : tsv ? tsvOrder : csvOrder) {
    size_t mode, agree;
    Consistency(sample, d, mode, agree);
    if (mode <= 1) continue;
    if (agree > bestAgree || (agree == bestAgree && mode > bestMode)) best = d, bestAgree = agree, bestMode = mode;
  }
  return best;
}

bool IsDigit(char c) { return c >= '0' && c <= '9'; }

// A value as the sheet stores it.
struct Cell {
  double number;
  uint32_t row, col;
  uint32_t code;    // index into the chunk's codes (+1; 0: General)
  uint32_t offset;  // text: in the chunk's arena, or (inText) in the file's text
  uint32_t length;
  uint32_t hash;    // text: for sharing repeated strings
  enum Kind : uint8_t { Text, Number, Bool, Error } kind;
  uint8_t error;
  bool inText;
};

uint32_t HashText(std::string_view text) {
  uint32_t h = 2166136261u;  // FNV-1a
  for (unsigned char ch : text) h = (h ^ ch) * 16777619u;
  return h ^ (h >> 16);
}

struct Chunk {
  std::vector<Cell> cells;
  std::string arena;
  std::vector<std::string> codes;
  bool tooManyColumns = false;
};

// Reads one field into a cell: a number, date, TRUE/FALSE or error only when it shows as written.
void Classify(std::string_view f, const NumLocale& loc, bool convert, Chunk& chunk, Cell& cell,
              std::unordered_map<std::wstring, uint32_t>& codeIndex, std::wstring& wide) {
  cell.kind = Cell::Text;
  if (!convert || f.size() > 64) return;
  const char c = f[0];
  if (c == '#') {
    ErrorCode e;
    if (ParseError(f, e)) cell.kind = Cell::Error, cell.error = static_cast<uint8_t>(e);
    return;
  }
  // Plain integers, the common case: no leading zero, at most 15 digits.
  const size_t sign = c == '-' ? 1 : 0;
  size_t k = sign;
  while (k < f.size() && IsDigit(f[k])) ++k;
  if (k == f.size() && k > sign && k - sign <= 15 && (f[sign] != '0' || k - sign == 1) && !(sign && f[1] == '0')) {
    double v = 0;
    for (size_t j = sign; j < k; ++j) v = v * 10 + (f[j] - '0');
    cell.kind = Cell::Number;
    cell.number = sign ? -v : v;
    cell.code = 0;
    if (k - sign > 11) {  // General would show it in scientific notation
      auto it = codeIndex.try_emplace(L"0", static_cast<uint32_t>(chunk.codes.size() + 1)).first;
      if (it->second == chunk.codes.size() + 1) chunk.codes.push_back("0");
      cell.code = it->second;
    }
    return;
  }
  // Anything else that could be a value: digits somewhere, or TRUE/FALSE.
  bool digit = false;
  for (char x : f) digit = digit || IsDigit(x);
  if (!digit && f != "TRUE" && f != "FALSE") return;
  wide.clear();
  bool ascii = true;
  for (char x : f) ascii = ascii && static_cast<unsigned char>(x) < 0x80;
  if (ascii) {
    for (char x : f) wide += static_cast<wchar_t>(x);
  } else {
    wide = Utf8ToWide(f);
  }
  ParsedValue p;
  if (!ParseExact(wide, loc, false, p)) return;
  if (p.kind == ParsedValue::Bool) {
    cell.kind = Cell::Bool;
    cell.number = p.number;
    return;
  }
  cell.kind = Cell::Number;
  cell.number = p.number;
  cell.code = 0;
  if (!p.code.empty()) {
    auto it = codeIndex.try_emplace(p.code, static_cast<uint32_t>(chunk.codes.size() + 1)).first;
    if (it->second == chunk.codes.size() + 1) chunk.codes.push_back(WideToUtf8(p.code));
    cell.code = it->second;
  }
}

// A text cell's string id: a string seen recently (one slot per hash, so a column of repeated
// values such as a status or a country is stored once) is shared, anything else is added.
constexpr size_t kRecentStrings = 65536;
constexpr uint32_t kNoString = 0xFFFFFFFF;
uint32_t StringId(StringTable& strings, std::vector<uint32_t>& recent, std::string_view text, uint32_t hash,
                  size_t& bytesAdded) {
  uint32_t& slot = recent[hash & (kRecentStrings - 1)];
  if (slot != kNoString && strings.Text(slot) == text) return slot;
  slot = strings.Add(text);
  bytesAdded += text.size();
  return slot;
}

// Rows [first, last) parsed into `threads` chunks, one worker thread each.
void ParseRows(std::string_view text, const Source& source, const NumLocale& locale, bool convert,
               std::vector<Chunk>& chunks, unsigned threads, size_t first, size_t last) {
  const size_t per = (last - first + threads - 1) / threads;
  auto work = [&](unsigned k) {
    Chunk& chunk = chunks[k];
    std::unordered_map<std::wstring, uint32_t> codeIndex;
    std::string scratch;
    std::wstring wide;
    const size_t from = std::min(last, first + k * per), to = std::min(last, from + per);
    for (size_t r = from; r < to; ++r) {
      const std::string_view row =
          text.substr(source.rowStart[r], source.rowEnd[r] - source.rowStart[r]);
      uint32_t col = 0;
      SplitRow(row, source.delimiter, scratch, [&](std::string_view f, bool quoted) {
        const uint32_t c = col++;
        if (f.empty()) return;
        if (c >= kMaxCols) {
          chunk.tooManyColumns = true;
          return;
        }
        Cell cell{};
        cell.row = static_cast<uint32_t>(r);
        cell.col = c;
        Classify(f, locale, convert, chunk, cell, codeIndex, wide);
        if (cell.kind == Cell::Text) {
          if (quoted) {
            cell.offset = static_cast<uint32_t>(chunk.arena.size());
            chunk.arena.append(f);
          } else {
            cell.offset = static_cast<uint32_t>(f.data() - text.data());
            cell.inText = true;
          }
          cell.length = static_cast<uint32_t>(f.size());
          cell.hash = HashText(f);
        }
        chunk.cells.push_back(cell);
      });
    }
  };
  if (threads == 1) {
    work(0);
  } else {
    std::vector<std::thread> pool;
    for (unsigned k = 0; k < threads; ++k) pool.emplace_back(work, k);
    for (auto& t : pool) t.join();
  }
}

}  // namespace

uint32_t SystemAnsiCodePage() {
  DWORD cp = 0;
  if (GetLocaleInfoEx(LOCALE_NAME_SYSTEM_DEFAULT, LOCALE_IDEFAULTANSICODEPAGE | LOCALE_RETURN_NUMBER,
                      reinterpret_cast<LPWSTR>(&cp), sizeof cp / sizeof(wchar_t)) == 0 ||
      cp == 0 || cp == CP_UTF8)
    cp = 1252;
  return cp;
}

Reader::Reader(std::string_view bytes, const Options& options)
    : options_(options), locale_(options.locale ? options.locale : &UserLocale()) {
  Decode(bytes);
  // Excel's "sep=;" line names the delimiter and isn't a row.
  if (text_.size() >= 5 && text_.substr(0, 4) == "sep=" &&
      (text_.size() == 5 || text_[5] == '\r' || text_[5] == '\n')) {
    source_.sepLine = true;
    source_.delimiter = text_[4];
    size_t skip = 5;
    if (skip < text_.size() && text_[skip] == '\r') ++skip;
    if (skip < text_.size() && text_[skip] == '\n') ++skip;
    source_.sepText = std::string(text_.substr(0, skip));
    text_.remove_prefix(skip);
  } else {
    source_.delimiter = options.delimiter ? options.delimiter : DetectDelimiter(text_, options.tsv);
  }
  FindRows();
}

void Reader::Decode(std::string_view bytes) {
  auto starts = [&](std::string_view bom) { return bytes.substr(0, bom.size()) == bom; };
  if (starts("\xEF\xBB\xBF")) {
    source_.bom = true;
    text_ = bytes.substr(3);
  } else if (starts("\xFF\xFE") || starts("\xFE\xFF")) {
    const bool be = bytes[0] == '\xFE';
    source_.bom = true;
    source_.encoding = be ? Encoding::Utf16BE : Encoding::Utf16LE;
    std::wstring w((bytes.size() - 2) / 2, L'\0');
    for (size_t k = 0; k < w.size(); ++k) {
      const auto lo = static_cast<unsigned char>(bytes[2 + 2 * k + (be ? 1 : 0)]);
      const auto hi = static_cast<unsigned char>(bytes[2 + 2 * k + (be ? 0 : 1)]);
      w[k] = static_cast<wchar_t>(lo | (hi << 8));
    }
    decoded_ = WideToUtf8Big(w.data(), w.size());
    text_ = decoded_;
  } else if (ValidUtf8(bytes)) {
    text_ = bytes;
  } else {
    source_.encoding = Encoding::Ansi;
    source_.codePage = options_.ansiCodePage ? options_.ansiCodePage : SystemAnsiCodePage();
    if (bytes.size() < 0x7FFFFFFF) {
      int n = MultiByteToWideChar(source_.codePage, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
      if (n <= 0) {  // a code page Windows can't convert: Western
        source_.codePage = 1252;
        n = MultiByteToWideChar(source_.codePage, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
      }
      std::wstring w(static_cast<size_t>(n), L'\0');
      MultiByteToWideChar(source_.codePage, 0, bytes.data(), static_cast<int>(bytes.size()), w.data(), n);
      decoded_ = WideToUtf8Big(w.data(), w.size());
    }
    text_ = decoded_;
  }
  if (text_.size() > 0xFFFFFFF0u) text_ = text_.substr(0, 0xFFFFFFF0u);  // offsets are 32-bit
}

void Reader::FindRows() {
  const std::string_view t = text_;
  const char d = source_.delimiter;
  const size_t n = t.size();
  size_t crlf = 0, lf = 0, cr = 0, fields = 0, quoted = 0;
  size_t i = 0;
  source_.finalLineEnd = true;
  while (i < n) {
    source_.rowStart.push_back(static_cast<uint32_t>(i));
    bool start = true, quote = false;
    size_t end = n, next = n;
    for (; i < n; ++i) {
      const char c = t[i];
      if (quote) {
        if (c == '"') {
          if (i + 1 < n && t[i + 1] == '"') ++i;
          else quote = false;
        }
        continue;
      }
      if (start) {
        ++fields;
        start = false;
        if (c == '"') {
          ++quoted;
          quote = true;
          continue;
        }
      }
      if (c == d) {
        start = true;
      } else if (c == '\n' || c == '\r') {
        end = i;
        if (c == '\r' && i + 1 < n && t[i + 1] == '\n') ++crlf, next = i + 2;
        else (c == '\n' ? lf : cr)++, next = i + 1;
        break;
      }
    }
    if (end == n) source_.finalLineEnd = false;
    source_.rowEnd.push_back(static_cast<uint32_t>(end));
    i = next;
  }
  source_.lineEnd = lf > crlf && lf >= cr ? LineEnd::LF : cr > crlf && cr > lf ? LineEnd::CR : LineEnd::CRLF;
  source_.mixedLineEnds = (crlf > 0) + (lf > 0) + (cr > 0) > 1;
  source_.quotedAll = fields > 0 && quoted == fields;
  source_.tooManyRows = source_.rowStart.size() > kMaxRows;
}

void Reader::Fields(size_t row, std::vector<std::string>& out) const {
  out.clear();
  if (row >= RowCount()) return;
  std::string scratch;
  const std::string_view r = text_.substr(source_.rowStart[row], source_.rowEnd[row] - source_.rowStart[row]);
  SplitRow(r, source_.delimiter, scratch, [&](std::string_view f, bool) { out.emplace_back(f); });
}

void Reader::AddRows(Workbook& book, SheetBuilder& builder, size_t first, size_t last) {
  last = std::min({last, RowCount(), kMaxRows});
  if (first >= last) return;
  const unsigned most =
      options_.threads ? options_.threads : std::min(8u, std::max(1u, std::thread::hardware_concurrency()));
  std::vector<Chunk> chunks(most);
  // Styles for the formats values were read with.
  StylesBuilder sb(book.GetStyles());
  if (sb.CellXfs().empty()) sb.CellXfs().push_back(Xf{});
  auto styleOf = [&](const std::string& code) {
    for (const auto& s : styles_)
      if (s.first == code) return s.second;
    const uint32_t id = 164 + static_cast<uint32_t>(sb.NumFmts().size());
    sb.NumFmts()[id] = code;
    Xf xf;
    xf.numFmt = id;
    xf.applyNumFmt = true;
    sb.CellXfs().push_back(xf);
    styles_.emplace_back(code, static_cast<uint32_t>(sb.CellXfs().size() - 1));
    return styles_.back().second;
  };
  // A batch of rows at a time: parsed in parallel, then handed to the model in order, so only one
  // batch's cells are held (all of them at once doubled the peak memory of a 100 MB file).
  int64_t currentRow = -1;
  std::vector<uint32_t> recent(kRecentStrings, kNoString);
  const size_t strings0 = book.Strings().Size(), bytes0 = stringBytes_;
  for (size_t batchFirst = first; batchFirst < last; batchFirst += kBatchRows) {
    const size_t batchLast = std::min(last, batchFirst + kBatchRows);
    const unsigned threads = batchLast - batchFirst < 4096 ? 1 : most;
    for (Chunk& c : chunks) {
      c.cells.clear();
      c.arena.clear();
      c.codes.clear();
    }
    ParseRows(text_, source_, *locale_, options_.convert, chunks, threads, batchFirst, batchLast);
    for (unsigned k = 0; k < threads; ++k) {
      const Chunk& chunk = chunks[k];
      if (chunk.tooManyColumns) source_.tooManyColumns = true;
      std::vector<uint32_t> styles(chunk.codes.size() + 1, 0);
      for (size_t j = 0; j < chunk.codes.size(); ++j) styles[j + 1] = styleOf(chunk.codes[j]);
      for (const Cell& c : chunk.cells) {
        if (c.row != currentRow) {
          builder.StartRow(c.row);
          currentRow = c.row;
        }
        switch (c.kind) {
          case Cell::Number: builder.AddNumber(c.col, c.number, styles[c.code]); break;
          case Cell::Bool: builder.AddBool(c.col, c.number != 0); break;
          case Cell::Error: builder.AddError(c.col, static_cast<ErrorCode>(c.error)); break;
          default: {
            const std::string_view text = c.inText ? text_.substr(c.offset, c.length)
                                                   : std::string_view(chunk.arena).substr(c.offset, c.length);
            builder.AddSharedString(c.col, StringId(book.Strings(), recent, text, c.hash, stringBytes_));
            break;
          }
        }
      }
    }
    // After the first batch, room for the rest of the strings at the same rate: growing the pool
    // as it fills copies it each time (40 MB more at the peak for a 100 MB file).
    if (batchFirst == first && batchLast < last) {
      StringTable& pool = book.Strings();
      const double more = static_cast<double>(last - batchLast) / static_cast<double>(batchLast - first) * 1.05;
      pool.Reserve(pool.Size() + static_cast<size_t>((pool.Size() - strings0) * more),
                   stringBytes_ + static_cast<size_t>(static_cast<double>(stringBytes_ - bytes0) * more));
    }
  }
}

void Read(std::string_view bytes, const std::string& sheetName, const Options& options, Workbook& book,
          Source& source) {
  Reader reader(bytes, options);
  book.SetFormat(FileFormat::Csv);
  Sheet& sheet = book.AddSheet(sheetName);
  {
    SheetBuilder builder(sheet);
    reader.AddRows(book, builder, 0, reader.RowCount());
  }
  source = reader.Info();
}

}  // namespace fw::sheet::csv
