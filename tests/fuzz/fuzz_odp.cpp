// Fuzz target: OpenDocument presentation import (src/slide/odp, P3). Whatever the .odp, the
// converter either refuses it or gives a presentation that opens, passes fwtool check, and is read
// by the model with every slide, layout and master, every shape drawn by fwdraw and every text
// resolved, without a problem the reader reports: an imported file is never a damaged one.
// Seeds: tests/fuzz/seeds/odp (small presentations with every structure the converter reads).
//
// An input is a zip, or parts as text as fuzz_check reads them: each part starts on a line
// "@<part name>", and the rest of the input up to the next such line is its content.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "draw/Drawing.h"
#include "pkg/Check.h"
#include "pkg/Package.h"
#include "pkg/Snapshot.h"
#include "pkg/Zip.h"
#include "slide/Presentation.h"
#include "slide/odp/Odp.h"

using namespace fw::pkg;
using namespace fw::slide;

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

void Check(bool ok, const char* what, const std::string& why = {}) {
  if (ok) return;
  std::fprintf(stderr, "%s %s\n", what, why.c_str());
  std::abort();
}

void CheckShapes(Presentation& deck, const std::vector<Shape>& shapes, const SlideBase& shownOn, int depth) {
  Check(depth <= 40, "groups nested too deep");
  for (const Shape& s : shapes) {
    Check(s.node && s.drawNode, "a shape without its element");
    if (s.textNode) Check(deck.Text(s, shownOn) != nullptr, "a shape's text isn't resolved");
    deck.GetTable(s, shownOn);
    CheckShapes(deck, s.children, shownOn, depth + 1);
  }
}

// fwdraw reads every drawNode of the part, and its background.
void DrawPart(Presentation& deck, const SlideBase& part) {
  if (part.background && part.backgroundOwner) {
    auto res = deck.Resources(*part.backgroundOwner, part);
    draw::Drawing::FromShapeTree(part.background, *res);
  }
  for (const DrawItem& item : deck.DrawList(part)) {
    Check(item.shape && item.owner, "a draw item without its shape");
    auto res = deck.Resources(*item.owner, part);
    draw::Drawing::FromShapeTree(item.shape->drawNode, *res);
  }
  CheckShapes(deck, part.shapes, part, 0);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const Limits limits = FuzzLimits();
  std::string_view input(reinterpret_cast<const char*>(data), size);
  std::vector<uint8_t> odp = input.starts_with("PK\x03\x04") ? std::vector<uint8_t>(data, data + size) : ZipFromText(input);

  std::vector<uint8_t> bytes;
  if (!odp::OdpToPptx(odp, bytes, limits).Ok()) return 0;
  Status st;
  auto pkg = Package::Open(Snapshot::FromBytes(std::move(bytes)), limits, st);
  Check(pkg != nullptr, "the converted presentation doesn't open:", st.message);
  const CheckReport report = CheckPackage(*pkg);
  for (const auto& e : report.errors) std::fprintf(stderr, "check: %s\n", e.c_str());
  Check(report.Ok(), "the converted presentation fails the check");
  std::unique_ptr<Presentation> deck = Read(std::move(pkg), st);
  Check(deck != nullptr, "the model refuses the converted presentation:", st.message);
  deck->LoadAll();
  for (size_t i = 0; i < deck->SlideCount(); ++i) {
    const Slide* s = deck->GetSlide(i);
    Check(s != nullptr && s->layout != nullptr, "a slide isn't read with its layout");
    DrawPart(*deck, *s);
    if (s->notes) DrawPart(*deck, *s->notes);
    s->NotesText();
  }
  for (const auto& l : deck->Layouts()) DrawPart(*deck, *l);
  for (const auto& m : deck->Masters()) DrawPart(*deck, *m);
  for (const std::string& p : deck->Problems()) std::fprintf(stderr, "problem: %s\n", p.c_str());
  Check(deck->Problems().empty(), "the reader reports problems in the converted presentation");
  return 0;
}
