// Fuzz target: RTF pasted from the clipboard (src/doc/edit/Clip.cpp, FromRtf). What it reads must
// write back in both formats and read again, and must paste into a document that stays valid
// (fuzz_paste.h). Seeds: tests/clipboard (what Word and LibreOffice put on the clipboard) and
// tests/fuzz/regressions/rtf.
#include "fuzz_paste.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  using namespace fuzz_paste;
  const auto clip = edit::FromRtf(std::string_view(reinterpret_cast<const char*>(data), size));
  if (!clip) return 0;
  ReadsBack(*clip);
  PasteIntoTarget(*clip);
  return 0;
}
