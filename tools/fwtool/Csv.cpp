// fwtool csv: opens CSV and TSV files as the app does (plan §7.1) and times it against the budget
// (§11: a 100 MB CSV shows its first screen in < 1 s and is fully loaded in < 5 s); --values checks
// that every field shows as it was written (§7.1).
#include <windows.h>
#include <psapi.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <string>

#include "Common.h"
#include "core/Strings.h"
#include "sheet/csv/Csv.h"
#include "sheet/numfmt/NumFormat.h"
#include "sheet/numfmt/NumLocale.h"

namespace fwtool {

namespace {

double Ms(std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b) {
  return std::chrono::duration<double, std::milli>(b - a).count();
}

// The process's private memory now and at its peak, in MB.
double PrivateMb(bool peak) {
  PROCESS_MEMORY_COUNTERS_EX m{};
  m.cb = sizeof m;
  GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&m), sizeof m);
  return static_cast<double>(peak ? m.PeakPagefileUsage : m.PrivateUsage) / 1048576.0;
}

// The file in memory, as the app reads it: one read into a buffer.
bool ReadAll(const std::filesystem::path& p, std::string& out) {
  HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER size{};
  GetFileSizeEx(h, &size);
  out.resize(static_cast<size_t>(size.QuadPart));
  size_t done = 0;
  while (done < out.size()) {
    DWORD got = 0;
    const DWORD want = static_cast<DWORD>(std::min<size_t>(out.size() - done, 1u << 30));
    if (!ReadFile(h, out.data() + done, want, &got, nullptr) || !got) break;
    done += got;
  }
  CloseHandle(h);
  return done == out.size();
}

// The cell's text as the app shows it in a wide enough column, in its format.
std::string Shown(const fw::sheet::Workbook& book, const fw::sheet::CellInfo& cell, const fw::sheet::NumLocale& locale,
                  std::map<uint32_t, std::unique_ptr<fw::sheet::NumberFormat>>& formats) {
  namespace sh = fw::sheet;
  const sh::Styles& st = book.GetStyles();
  const uint32_t id = st.CellXf(cell.style).numFmt;
  auto& f = formats[id];
  if (!f) {
    const std::string_view code = st.FormatCode(id);
    f = std::make_unique<sh::NumberFormat>(code.empty() ? sh::NumberFormat::Builtin(static_cast<int>(id), locale)
                                                        : sh::NumberFormat(fw::Utf8ToWide(code)));
  }
  sh::FormatContext ctx;
  ctx.locale = &locale;
  ctx.date1904 = book.Date1904();
  sh::Formatted out;
  switch (cell.value.type) {
    case sh::ValueType::Number: f->Format(cell.value.number, ctx, out); break;
    case sh::ValueType::String:
      if (f->IsGeneral()) return std::string(cell.value.text);
      f->Format(fw::Utf8ToWide(cell.value.text), ctx, out);
      break;
    case sh::ValueType::Bool: return cell.value.boolean ? "TRUE" : "FALSE";
    case sh::ValueType::Error: return std::string(sh::ErrorText(cell.value.error));
    default: return {};
  }
  return out.overflow ? "####" : fw::WideToUtf8(out.text);
}

// Every field must show exactly as it was written (plan §7.1: no value changes). Returns how many don't.
size_t CheckValues(const fw::sheet::csv::Reader& reader, const fw::sheet::Workbook& book, const fw::sheet::Sheet& sheet,
                   const fw::sheet::NumLocale& locale) {
  std::map<uint32_t, std::unique_ptr<fw::sheet::NumberFormat>> formats;
  std::vector<std::string> fields;
  size_t checked = 0, changed = 0;
  const size_t rows = std::min<size_t>(reader.RowCount(), 1048576);
  for (size_t r = 0; r < rows; ++r) {
    reader.Fields(r, fields);
    for (size_t c = 0; c < fields.size() && c < 16384; ++c) {
      fw::sheet::CellInfo cell;
      const bool has = sheet.GetCell(static_cast<uint32_t>(r), static_cast<uint32_t>(c), cell);
      if (!has && fields[c].empty()) continue;
      ++checked;
      const std::string shown = has ? Shown(book, cell, locale, formats) : std::string();
      if (shown == fields[c]) continue;
      if (++changed <= 12)
        std::printf("    row %zu, column %zu: \"%s\" shows as \"%s\"\n", r + 1, c + 1, fields[c].substr(0, 60).c_str(),
                    shown.substr(0, 60).c_str());
    }
  }
  std::printf("  values: %zu fields, %zu changed\n", checked, changed);
  return changed;
}

}  // namespace

int RunCsv(const Options& options) {
  int failed = 0;
  const fw::sheet::NumLocale& locale = options.locale == L"en-NL"   ? fw::sheet::EnNlLocale()
                                       : options.locale == L"en-US" ? fw::sheet::EnUsLocale()
                                                                    : fw::sheet::UserLocale();
  // Files as given, and a folder's .csv, .tsv and .txt files (sorted).
  std::vector<std::filesystem::path> files;
  auto isText = [](const std::filesystem::path& f) {
    const std::wstring ext = f.extension().wstring();
    return _wcsicmp(ext.c_str(), L".csv") == 0 || _wcsicmp(ext.c_str(), L".tsv") == 0 || _wcsicmp(ext.c_str(), L".txt") == 0;
  };
  for (const auto& in : options.inputs) {
    std::error_code ec;
    if (!std::filesystem::is_directory(in, ec)) {
      files.push_back(in);
      continue;
    }
    std::vector<std::filesystem::path> found;
    for (auto it = std::filesystem::recursive_directory_iterator(in, ec); !ec && it != std::filesystem::end(it); it.increment(ec))
      if (it->is_regular_file(ec) && isText(it->path())) found.push_back(it->path());
    std::sort(found.begin(), found.end());
    files.insert(files.end(), found.begin(), found.end());
  }
  size_t changedFiles = 0;
  for (const auto& file : files) {
    const auto t0 = std::chrono::steady_clock::now();
    std::string bytes;
    if (!ReadAll(file, bytes)) {
      std::fwprintf(stderr, L"%ls: can't read it\n", file.c_str());
      ++failed;
      continue;
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double mbBytes = PrivateMb(false);
    fw::sheet::csv::Options o;
    o.locale = &locale;
    o.tsv = _wcsicmp(file.extension().c_str(), L".tsv") == 0;
    fw::sheet::csv::Reader reader(bytes, o);
    const auto t2 = std::chrono::steady_clock::now();
    const double mbRows = PrivateMb(false);
    fw::sheet::Workbook book;
    book.SetFormat(fw::sheet::FileFormat::Csv);
    fw::sheet::Sheet& sheet = book.AddSheet(fw::WideToUtf8(file.stem().wstring()));
    std::chrono::steady_clock::time_point t3, t4;
    {
      fw::sheet::SheetBuilder builder(sheet);
      reader.AddRows(book, builder, 0, 100);  // the first screen
      t3 = std::chrono::steady_clock::now();
      reader.AddRows(book, builder, 100, reader.RowCount());
    }
    t4 = std::chrono::steady_clock::now();
    const double mbLoaded = PrivateMb(false), mbPeak = PrivateMb(true);
    const auto& src = reader.Info();
    const char* enc[] = {"UTF-8", "UTF-16 LE", "UTF-16 BE", "ANSI"};
    std::printf("%s: %.1f MB, %zu rows, %zu cells, %s%s, delimiter '%s'%s%s\n", fw::WideToUtf8(file.filename().wstring()).c_str(),
                bytes.size() / 1048576.0, reader.RowCount(), sheet.CellCount(), enc[static_cast<int>(src.encoding)],
                src.bom ? " with BOM" : "", src.delimiter == '\t' ? "tab" : std::string(1, src.delimiter).c_str(),
                src.tooManyRows ? ", rows beyond the sheet left out" : "", src.tooManyColumns ? ", columns beyond XFD left out" : "");
    std::printf("  read %.0f ms, rows found %.0f ms, first screen %.0f ms (total %.0f), fully loaded %.0f ms\n",
                Ms(t0, t1), Ms(t1, t2), Ms(t2, t3), Ms(t0, t3), Ms(t0, t4));
    std::printf("  memory (private): file read %.0f MB, rows found %.0f MB, loaded %.0f MB, peak %.0f MB\n", mbBytes,
                mbRows, mbLoaded, mbPeak);
    std::printf("  the sheet: %zu strings in %.0f MB, cells %.0f MB (16 bytes each)\n",
                static_cast<size_t>(book.Strings().Size()), book.Strings().Bytes() / 1048576.0,
                static_cast<double>(sheet.CellCount()) * 16 / 1048576.0);
    if (options.values && CheckValues(reader, book, sheet, locale)) ++changedFiles;
  }
  if (options.values) std::printf("\n%zu files, %zu with a changed value\n", files.size(), changedFiles);
  return failed || changedFiles ? 1 : 0;
}

}  // namespace fwtool
