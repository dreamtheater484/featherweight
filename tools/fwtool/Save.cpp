// fwtool save: Word documents saved the way Featherweight's Save does (plan §6.3 "Saving", D2):
// through docx::Serialize and safe saving, into a folder, then checked.
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>

#include "Common.h"
#include "core/Strings.h"
#include "doc/docx/Docx.h"
#include "doc/docx/Save.h"
#include "pkg/Check.h"
#include "pkg/SafeFile.h"

namespace fwtool {
namespace {

using fw::pkg::Status;

struct Result {
  bool ok = true;
  std::string detail;
  double ms = 0;
};

Result SaveFile(const std::filesystem::path& file, const std::filesystem::path& outDir, bool edited) {
  Result r;
  auto fail = [&](std::string why) {
    r.ok = false;
    r.detail = std::move(why);
    return r;
  };
  std::vector<uint8_t> original;
  {
    Status st;
    auto pkg = OpenPackage(file, st);
    if (!pkg) return fail(st.message);
    if (fw::pkg::FormatOf(*pkg) != fw::pkg::DocFormat::Word) {
      r.detail = "not a Word document, skipped";
      return r;
    }
    const auto& snap = pkg->GetSnapshot();
    original.resize(static_cast<size_t>(snap.Size()));
    if (!snap.ReadAt(0, original.data(), original.size())) return fail("the snapshot couldn't be read");
  }
  Status st;
  auto doc = fw::doc::docx::Read(OpenPackage(file, st), st);
  if (!doc) return fail(st.message);
  const std::vector<std::string> knownErrors = fw::pkg::CheckPackage(doc->Package()).errors;

  fw::doc::docx::SaveOptions options;
  if (edited) {
    // As after an edit in every part: each part the model can write is rewritten.
    for (uint32_t i = 0; i < doc->Parts().size(); ++i)
      if (doc->Parts()[i].writable && doc->Parts()[i].Editable()) options.changedParts.push_back(i);
    options.edited = true;
    options.author = "fwtool";
  }
  const auto start = std::chrono::steady_clock::now();
  std::vector<uint8_t> bytes;
  if (!(st = fw::doc::docx::Serialize(*doc, options, bytes)).Ok()) return fail("saving failed: " + st.message);
  const std::filesystem::path target = outDir / file.filename();
  fw::pkg::SaveResult saved = fw::pkg::SafeWrite(target.wstring(), bytes);
  r.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  if (!saved.Ok()) return fail(saved.message);

  // The saved file, read back from the disk as the app would open it.
  auto again = fw::doc::docx::Read(OpenPackage(target, st), st);
  if (!again) return fail("the saved file doesn't open: " + st.message);
  if (!edited) {
    std::vector<uint8_t> onDisk(static_cast<size_t>(again->Package().GetSnapshot().Size()));
    again->Package().GetSnapshot().ReadAt(0, onDisk.data(), onDisk.size());
    if (onDisk != original) return fail("saved with no edits, but the file isn't identical to the original");
  }
  for (const auto& e : fw::pkg::CheckPackage(again->Package()).errors)
    if (std::find(knownErrors.begin(), knownErrors.end(), e) == knownErrors.end())
      return fail("the saved file fails fwtool check: " + e);
  if (again->Paragraphs().size() != doc->Paragraphs().size())
    return fail("the saved file has " + std::to_string(again->Paragraphs().size()) + " paragraphs, not " +
                std::to_string(doc->Paragraphs().size()));
  char buf[96];
  snprintf(buf, sizeof(buf), "%s in %.1f ms", edited ? "edited and saved" : "identical", r.ms);
  r.detail = buf;
  if (edited) r.detail += ", " + std::to_string(options.changedParts.size()) + " parts written from the model";
  if (!knownErrors.empty()) r.detail += " (" + std::to_string(knownErrors.size()) + " check errors in the original)";
  return r;
}

}  // namespace

int RunSave(const Options& options) {
  auto files = CollectFiles(options.inputs);
  if (files.empty()) {
    std::fprintf(stderr, "No Office files found.\n");
    return 1;
  }
  std::filesystem::path outDir = options.outDir;
  std::error_code ec;
  if (outDir.empty())
    outDir = std::filesystem::temp_directory_path(ec) / (L"fwtool-save-" + std::to_wstring(GetCurrentProcessId()));
  std::filesystem::create_directories(outDir, ec);
  int failed = 0;
  double slowest = 0;
  std::string slowestName;
  for (const auto& file : files) {
    Result r = SaveFile(file, outDir, options.edited);
    std::printf("%s %s: %s\n", r.ok ? "[ ok ]" : "[FAIL]", Display(file).c_str(), r.detail.c_str());
    if (!r.ok) ++failed;
    if (r.ms > slowest) {
      slowest = r.ms;
      slowestName = Display(file);
    }
  }
  if (options.outDir.empty()) std::filesystem::remove_all(outDir, ec);
  std::printf("\n%zu files, %d failed; slowest save %.1f ms (%s)\n", files.size(), failed, slowest, slowestName.c_str());
  return failed == 0 ? 0 : 1;
}

}  // namespace fwtool
