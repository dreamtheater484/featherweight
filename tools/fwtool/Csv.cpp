// fwtool csv: opens CSV and TSV files as the app does (plan §7.1) and times it against the budget
// (§11: a 100 MB CSV shows its first screen in < 1 s and is fully loaded in < 5 s, in < 150 MB).
#include <windows.h>
#include <psapi.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <algorithm>
#include <string>
#include <vector>

#include "Common.h"
#include "core/Strings.h"
#include "sheet/csv/Csv.h"
#include "sheet/model/Sheet.h"
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

}  // namespace

int RunCsv(const Options& options) {
  int failed = 0;
  // Files, and the CSV and TSV files under folders, in a stable order.
  std::vector<std::filesystem::path> files;
  for (const auto& in : options.inputs) {
    std::error_code ec;
    if (!std::filesystem::is_directory(in, ec)) {
      files.push_back(in);
      continue;
    }
    std::vector<std::filesystem::path> found;
    for (const auto& e : std::filesystem::recursive_directory_iterator(in, ec)) {
      std::wstring ext = e.path().extension().wstring();
      for (auto& ch : ext) ch = static_cast<wchar_t>(towlower(ch));
      if (e.is_regular_file() && (ext == L".csv" || ext == L".tsv")) found.push_back(e.path());
    }
    std::sort(found.begin(), found.end());
    files.insert(files.end(), found.begin(), found.end());
  }
  size_t allFields = 0, allSame = 0, filesDiffering = 0;
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
    o.tsv = file.extension() == L".tsv";
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
    if (!options.values) continue;
    // Every field shows exactly as written: what the cell shows (its value through the format the
    // reader gave it, in the locale it read with) against the field's own text.
    const fw::sheet::NumLocale& locale = fw::sheet::UserLocale();
    CellText show(book, locale);
    std::vector<std::string> fields;
    size_t count = 0, same = 0, listed = 0;
    for (size_t r = 0; r < reader.RowCount() && r < 1048576; ++r) {
      reader.Fields(r, fields);
      for (size_t c = 0; c < fields.size() && c < 16384; ++c) {
        fw::sheet::CellInfo info;
        std::string ours = sheet.GetCell(static_cast<uint32_t>(r), static_cast<uint32_t>(c), info) ? show(info) : std::string();
        if (fields[c].empty() && ours.empty()) continue;
        ++count;
        if (ours == fields[c]) {
          ++same;
        } else if (listed++ < 10) {
          std::printf("  row %zu, field %zu: the file has \"%s\", Featherweight shows \"%s\"\n", r + 1, c + 1,
                      fields[c].substr(0, 80).c_str(), ours.substr(0, 80).c_str());
        }
      }
    }
    std::printf("  values: %zu of %zu fields show as written%s\n", same, count, same == count ? "" : "  <-- differs");
    allFields += count;
    allSame += same;
    if (same != count) {
      ++filesDiffering;
      ++failed;
    }
  }
  if (options.values)
    std::printf("%zu files: %zu of %zu fields show as written; %zu files differ\n", files.size(), allSame, allFields, filesDiffering);
  return failed ? 1 : 0;
}

}  // namespace fwtool
