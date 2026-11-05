// fwtool spelling: what proofing a whole document costs (plan §6.2, §11). Opens each Word
// document, checks every paragraph's spelling with Windows' spell checker as the app does in idle
// time, and counts its words, timing both. The app does this a few milliseconds at a time after
// the first page shows, so it never delays a keystroke; this says how long the idle work lasts.
#include <windows.h>

#include <objbase.h>

#include <chrono>
#include <cstdio>

#include "Common.h"
#include "doc/docx/Docx.h"
#include "doc/proof/Spelling.h"
#include "doc/proof/WordCount.h"
#include "text/SpellCheck.h"

namespace fwtool {

int RunSpelling(const Options& options) {
  using Clock = std::chrono::steady_clock;
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  fw::text::SpellChecker checker;
  const auto load = Clock::now();
  if (!checker.Available() || checker.Check(L"en-US", L"Warm up").size() > 2) {
    std::printf("Windows' spell checker isn't available on this PC.\n");
    return 1;
  }
  std::printf("Loading Windows' spell checker and an English dictionary: %.1f ms\n\n",
              std::chrono::duration<double, std::milli>(Clock::now() - load).count());
  int failed = 0;
  std::printf("%-40s %8s %8s %8s %10s %9s\n", "Document", "paras", "words", "errors", "spell ms", "count ms");
  for (const auto& file : CollectFiles(options.inputs)) {
    fw::pkg::Status st;
    auto pkg = OpenPackage(file, st);
    std::unique_ptr<fw::doc::Document> doc = pkg ? fw::doc::docx::Read(std::move(pkg), st) : nullptr;
    if (!doc) {
      std::printf("%-40s can't open: %s\n", Display(file).c_str(), st.message.c_str());
      ++failed;
      continue;
    }
    const auto t0 = Clock::now();
    fw::doc::proof::Spelling spelling(*doc, checker);
    while (spelling.Continue(8)) {
    }
    const double spellMs = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    const auto t1 = Clock::now();
    const fw::doc::proof::Counts counts = fw::doc::proof::CountDocument(*doc, false);
    const double countMs = std::chrono::duration<double, std::milli>(Clock::now() - t1).count();
    std::printf("%-40s %8zu %8llu %8zu %10.1f %9.1f\n", Display(file).c_str(), doc->Paragraphs().size(),
                static_cast<unsigned long long>(counts.words), spelling.Count(), spellMs, countMs);
  }
  CoUninitialize();
  return failed ? 1 : 0;
}

}  // namespace fwtool
