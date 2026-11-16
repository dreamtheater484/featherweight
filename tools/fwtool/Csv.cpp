// fwtool csv: opens CSV and TSV files as the app does (plan §7.1) and times it against the budget
// (§11: a 100 MB CSV shows its first screen in < 1 s and is fully loaded in < 5 s).
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>

#include "Common.h"
#include "core/Strings.h"
#include "sheet/csv/Csv.h"

namespace fwtool {

namespace {

double Ms(std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b) {
  return std::chrono::duration<double, std::milli>(b - a).count();
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
  for (const auto& in : options.inputs) {
    const std::filesystem::path file(in);
    const auto t0 = std::chrono::steady_clock::now();
    std::string bytes;
    if (!ReadAll(file, bytes)) {
      std::fwprintf(stderr, L"%ls: can't read it\n", file.c_str());
      ++failed;
      continue;
    }
    const auto t1 = std::chrono::steady_clock::now();
    fw::sheet::csv::Options o;
    o.tsv = file.extension() == L".tsv";
    fw::sheet::csv::Reader reader(bytes, o);
    const auto t2 = std::chrono::steady_clock::now();
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
    const auto& src = reader.Info();
    const char* enc[] = {"UTF-8", "UTF-16 LE", "UTF-16 BE", "ANSI"};
    std::printf("%s: %.1f MB, %zu rows, %zu cells, %s%s, delimiter '%s'%s%s\n", fw::WideToUtf8(file.filename().wstring()).c_str(),
                bytes.size() / 1048576.0, reader.RowCount(), sheet.CellCount(), enc[static_cast<int>(src.encoding)],
                src.bom ? " with BOM" : "", src.delimiter == '\t' ? "tab" : std::string(1, src.delimiter).c_str(),
                src.tooManyRows ? ", rows beyond the sheet left out" : "", src.tooManyColumns ? ", columns beyond XFD left out" : "");
    std::printf("  read %.0f ms, rows found %.0f ms, first screen %.0f ms (total %.0f), fully loaded %.0f ms\n",
                Ms(t0, t1), Ms(t1, t2), Ms(t2, t3), Ms(t0, t3), Ms(t0, t4));
  }
  return failed ? 1 : 0;
}

}  // namespace fwtool
