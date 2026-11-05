// fwtool export: Save as RTF and as plain text (src/doc/edit/Export.h) on real documents. Each Word
// document is written to DIR as <name>.rtf and <name>.txt (UTF-8), as the Word tab's Save As writes
// them, for Word to read back (scripts\word-open-check.ps1) and for a look.
#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "Common.h"
#include "Reference.h"
#include "core/Strings.h"
#include "doc/docx/Docx.h"
#include "doc/edit/Clip.h"
#include "doc/edit/Export.h"

namespace fwtool {

int RunExport(const Options& options) {
  using Clock = std::chrono::steady_clock;
  if (options.outDir.empty()) {
    std::fprintf(stderr, "Give the folder to write to with --out.\n");
    return 2;
  }
  std::vector<std::filesystem::path> files;
  for (const auto& f : CollectFiles(options.inputs))
    if (IsWordDocument(fw::WideToUtf8(f.filename().wstring()))) files.push_back(f);
  if (files.empty()) {
    std::fprintf(stderr, "No Word documents found.\n");
    return 1;
  }
  std::error_code ec;
  std::filesystem::create_directories(options.outDir, ec);
  int failed = 0;
  for (const auto& file : files) {
    const std::string name = Display(file);
    fw::pkg::Status st;
    auto pkg = OpenPackage(file, st);
    auto doc = pkg ? fw::doc::docx::Read(std::move(pkg), st) : nullptr;
    if (!doc) {
      std::printf("%s: not read (%s), skipped\n", name.c_str(), st.message.c_str());
      continue;
    }
    const auto t = Clock::now();
    const std::string rtf = fw::doc::edit::RtfFile(*doc);
    const std::string txt = fw::doc::edit::TextFile(*doc, fw::doc::edit::TextEncoding::Utf8);
    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t).count();
    // The RTF reads back (the clipboard's reader), and each file is written. The reader refuses
    // more than a paste may hold (10,000 paragraphs): bigger documents aren't read back.
    size_t paragraphs = 0;
    for (const char* mark : {"\\par\r\n", "\\cell"})
      for (size_t at = rtf.find(mark); at != std::string::npos; at = rtf.find(mark, at + 1)) ++paragraphs;
    const bool big = paragraphs >= 10000;
    const bool reads = big || fw::doc::edit::FromRtf(rtf).has_value();
    bool written = true;
    for (const auto& [ext, bytes] : {std::pair<const wchar_t*, const std::string*>{L".rtf", &rtf}, {L".txt", &txt}}) {
      std::filesystem::path out = options.outDir / file.stem();
      out += ext;
      std::ofstream f(out, std::ios::binary);
      f.write(bytes->data(), static_cast<std::streamsize>(bytes->size()));
      written = written && f.good();
    }
    if (!reads || !written) ++failed;
    std::printf("%s: RTF %zu bytes, text %zu bytes, %.0f ms%s%s%s\n", name.c_str(), rtf.size(), txt.size(), ms,
                big ? " (too big to read back as a paste)" : "", reads ? "" : ", the RTF doesn't read back",
                written ? "" : ", not written");
  }
  std::printf("\n%zu documents, %d failed\n", files.size(), failed);
  return failed ? 1 : 0;
}

}  // namespace fwtool
