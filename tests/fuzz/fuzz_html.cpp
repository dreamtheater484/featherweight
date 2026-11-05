// Fuzz target: HTML pasted from the clipboard (src/doc/edit/Clip.cpp, FromHtml): CF_HTML from Word,
// browsers and LibreOffice, and Featherweight's own copies (data-fw-*, which name styles and lists
// in the document pasted into). What it reads must write back in both formats and read again, and
// must paste into a document that stays valid (fuzz_paste.h). Seeds: tests/clipboard and
// tests/fuzz/regressions/html.
#include "fuzz_paste.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  using namespace fuzz_paste;
  const auto clip = edit::FromHtml(std::string_view(reinterpret_cast<const char*>(data), size));
  if (!clip) return 0;
  ReadsBack(*clip);
  PasteIntoTarget(*clip);
  return 0;
}
