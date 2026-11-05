// Fuzz target: pagination (src/doc/layout): a Word document read into the model and laid out on
// pages with the PC's fonts, then hit-tested and dumped as the document tab and fwtool do.
// Seeds: tests/fuzz/seeds/docx and tests/fuzz/seeds/package, read like fuzz_docx's.
//
// Besides crashes, it aborts when pagination doesn't end, a page has no size, or a hit test or a
// caret lands outside the page or the text.
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string_view>
#include <vector>

#include "doc/docx/Docx.h"
#include "doc/layout/Layout.h"
#include "pkg/Package.h"
#include "pkg/Snapshot.h"
#include "pkg/Zip.h"
#include "text/Fonts.h"

using namespace fw::doc;
using namespace fw::pkg;

namespace {

Limits FuzzLimits() {
  Limits limits;
  limits.maxEntrySize = 16u << 20;
  limits.maxTotalSize = 64u << 20;
  limits.maxEntries = 4096;
  limits.maxStringLength = 1u << 20;
  return limits;
}

std::vector<uint8_t> ZipFromText(std::string_view text) {
  MemorySink sink;
  ZipWriter writer(sink);
  size_t pos = text.starts_with('@') ? 0 : text.find("\n@");
  while (pos != std::string_view::npos) {
    if (text[pos] == '\n') ++pos;
    size_t eol = text.find('\n', pos);
    if (eol == std::string_view::npos) break;
    std::string_view name = text.substr(pos + 1, eol - pos - 1);
    if (!name.empty() && name.back() == '\r') name.remove_suffix(1);
    size_t next = text.find("\n@", eol);
    std::string_view body = text.substr(eol + 1, (next == std::string_view::npos ? text.size() : next) - eol - 1);
    auto bytes = reinterpret_cast<const uint8_t*>(body.data());
    writer.Add(name, std::span<const uint8_t>(bytes, body.size()), nullptr, ZipWriter::Compression::Store);
    pos = next;
  }
  writer.Finish();
  return std::move(sink.bytes);
}

void Check(bool ok) {
  if (!ok) std::abort();
}

// One Fonts for the run, as in the app: faces stay cached.
fw::text::Fonts& SharedFonts() {
  static fw::text::Fonts fonts;
  return fonts;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  std::string_view input(reinterpret_cast<const char*>(data), size);
  std::vector<uint8_t> zip = input.starts_with("PK\x03\x04") ? std::vector<uint8_t>(data, data + size) : ZipFromText(input);
  Status st;
  auto pkg = Package::Open(Snapshot::FromBytes(std::move(zip)), FuzzLimits(), st);
  if (!pkg) return 0;
  std::unique_ptr<Document> doc = docx::Read(std::move(pkg), st);
  if (!doc) return 0;

  layout::Layout layout(*doc, SharedFonts());
  // A page at a time, as the document tab does; pagination guarantees an end.
  const size_t cap = 4 * doc->Paragraphs().size() + 1100;
  while (!layout.Done()) {
    const size_t before = layout.PageCount();
    layout.LayOutPages(before + 1);
    Check(layout.PageCount() > before || layout.Done());
    Check(layout.PageCount() <= cap);
  }
  for (size_t p = 0; p < layout.PageCount(); ++p) {
    const layout::Page& page = layout.GetPage(p);
    Check(page.width > 0 && page.height > 0 && std::isfinite(page.width) && std::isfinite(page.height));
    for (const layout::LineBox& l : page.lines) {
      Check(std::isfinite(l.top) && std::isfinite(l.height) && std::isfinite(l.baseline));
      Check(l.para == kNone || l.para < doc->Paragraphs().size());
    }
    // Where a click lands, and back.
    for (float y : {0.0f, page.height / 2, page.height}) {
      std::optional<layout::TextPos> hit = layout.HitTest(p, page.width / 2, y);
      if (!hit) continue;
      Check(hit->para < doc->Paragraphs().size() && hit->offset <= doc->Para(hit->para).text.size());
      if (std::optional<layout::Layout::Caret> caret = layout.CaretAt(*hit)) {
        Check(caret->page < layout.PageCount());
        Check(std::isfinite(caret->x) && caret->top <= caret->bottom);
      }
      layout.Ranges({hit->para, 0}, *hit);
    }
  }
  layout.Dump("fuzz");
  return 0;
}
