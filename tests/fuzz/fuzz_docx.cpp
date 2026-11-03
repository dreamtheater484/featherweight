// Fuzz target: the Word document model (src/doc): the docx reader, everything the layout phase
// asks of the model, and the writer, which must give back what it read.
// Seeds: tests/fuzz/seeds/docx (a small document with every structure the model reads) and
// tests/fuzz/seeds/package (corpus files as text).
//
// An input is a zip, or parts as text as fuzz_check reads them: each part starts on a line
// "@<part name>", and the rest of the input up to the next such line is its content.
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "doc/docx/Docx.h"
#include "pkg/Canonical.h"
#include "pkg/Package.h"
#include "pkg/Snapshot.h"
#include "pkg/Zip.h"

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

// Parts as text, stored in a zip (as in fuzz_check.cpp).
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

// What pagination and the viewer read, for every paragraph and table.
void ReadEverything(const Document& doc) {
  for (uint32_t story = 0; story < doc.Stories().size(); ++story) {
    for (uint32_t para : doc.StoryParagraphs(story)) {
      const std::u16string& text = doc.Para(para).text;
      uint32_t last = 0;
      for (const TextRun& r : doc.Runs(para)) {
        Check(r.start >= last && r.end >= r.start && r.end <= text.size());
        last = r.end;
        if (r.run) doc.ResolvedRun(para, r.run);
        if (r.hyperlink) doc.HyperlinkTarget(r.hyperlink);
      }
      for (const InlineItem& item : doc.Items(para)) Check(item.offset + item.length <= text.size());
      doc.ResolvedPara(para);
      doc.ResolvedMark(para);
      doc.Label(para);
    }
    for (const Field& f : doc.Fields(story)) f.Type();
    doc.Revisions(story);
    // Tables, nested ones included, without recursion.
    std::vector<Block> blocks = doc.Blocks(story);
    for (size_t i = 0; i < blocks.size(); ++i) {
      if (blocks[i].kind != BlockKind::Table) continue;
      Node table = blocks[i].node;
      doc.ResolvedTable(table);
      doc.TableGrid(table);
      for (Node row : doc.TableRows(table)) {
        doc.ResolvedRow(row);
        for (Node cell : doc.RowCells(row)) {
          doc.ResolvedCell(cell);
          doc.CellGridColumn(cell);
          doc.CellRowSpan(cell);
          for (const Block& b : doc.Blocks(cell)) blocks.push_back(b);
        }
      }
    }
  }
  doc.Comments();
  for (const Drawing& d : doc.Drawings()) doc.DrawingXml(d);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const Limits limits = FuzzLimits();
  std::string_view input(reinterpret_cast<const char*>(data), size);
  std::vector<uint8_t> zip = input.starts_with("PK\x03\x04") ? std::vector<uint8_t>(data, data + size) : ZipFromText(input);

  Status st;
  auto pkg = Package::Open(Snapshot::FromBytes(std::move(zip)), limits, st);
  if (!pkg) return 0;
  std::unique_ptr<Document> doc = docx::Read(std::move(pkg), st);
  if (!doc) return 0;
  ReadEverything(*doc);

  // The never-damage bar: every part the model owns, written back, says what it said.
  std::vector<uint8_t> bytes;
  for (uint32_t i = 0; i < doc->Parts().size(); ++i) {
    const auto& part = doc->Parts()[i];
    std::string out, before, after;
    if (!part.writable) continue;
    Check(doc->Package().ReadPart(part.name, bytes).Ok());
    Check(docx::WritePart(*doc, i, out).Ok());
    std::string_view original(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    Check(CanonicalizeXml(original, before, limits, part.name).Ok());
    Check(CanonicalizeXml(out, after, limits, part.name).Ok());
    Check(before == after);
  }
  return 0;
}
