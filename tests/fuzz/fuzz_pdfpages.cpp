// Fuzz target (F1, f1-pdf): the PDF object reader under signing (src/pdf/sign/PdfObjects) and the
// signature's incremental update (pdfsign::Prepare). PDFium itself, which the page tools and OCR
// go through, is fuzzed upstream; this is the code of ours that reads a PDF's structure.
// Seeds: tests/fuzz/seeds/pdf and tests/pdfs.
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

#include "pdf/sign/PdfObjects.h"
#include "pdf/sign/Sign.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const std::string_view bytes(reinterpret_cast<const char*>(data), size);
  // Objects alone, from any offset the input starts with.
  {
    size_t at = 0;
    pdfobj::Obj o;
    if (pdfobj::ParseObject(bytes, at, o)) {
      // What it reads it writes back, and that reads again as the same.
      const std::string written = pdfobj::Write(o);
      size_t again = 0;
      pdfobj::Obj back;
      if (o.type != pdfobj::Obj::Stream && (!pdfobj::ParseObject(written, again, back) || pdfobj::Write(back) != written))
        std::abort();
      // And compares as the same object (how signed PDFs' updates keep only what changed).
      pdfobj::File none;
      if (o.type != pdfobj::Obj::Stream && !pdfobj::SameObject(none, o, none, back)) std::abort();
    }
  }
  pdfobj::File f;
  if (!f.Open(bytes)) return 0;
  const int first = f.FirstPage();
  for (int n = 0; n < f.Size() && n < 64; ++n) {
    pdfobj::Obj o;
    f.Get(n, o);
  }
  // A signature's update after it: the result must read again, with the same catalog and page,
  // and its byte range must leave out exactly the signature's contents.
  pdfsign::Prepared p;
  pdfsign::Details d;
  d.name = L"Fuzz";
  if (pdfsign::Prepare(bytes, d, 64, p) != pdfsign::Error::None) return 0;
  if (p.bytes.compare(0, size, bytes) != 0) std::abort();
  if (p.range[0] != 0 || p.range[1] != p.contentsAt || p.range[2] != p.contentsAt + p.contentsHex + 2 ||
      p.range[2] + p.range[3] != p.bytes.size() || p.bytes[p.contentsAt] != '<' ||
      p.bytes[p.contentsAt + p.contentsHex + 1] != '>')
    std::abort();
  pdfobj::File g;
  if (!g.Open(p.bytes) || g.Size() <= f.Size() || g.FirstPage() != first) std::abort();
  const pdfobj::Obj* root = g.Trailer().Get("Root");
  pdfobj::Obj catalog, form;
  if (!root || !g.Get(root->num, catalog) || !catalog.Get("AcroForm") || !g.Resolve(*catalog.Get("AcroForm"), form) ||
      !form.Get("SigFlags"))
    std::abort();
  return 0;
}
