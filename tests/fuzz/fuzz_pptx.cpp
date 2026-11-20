// Fuzz target: the presentation model (src/slide, P1): the .pptx reader with every slide, its
// layout, master and notes, and the inheritance the renderer asks for: each shape's drawNode read
// by fwdraw, the text boxes Drawing finds mapped back to their shapes, every text resolved for the
// slide it shows on, tables with their styles, backgrounds, hyperlinks and the embedded fonts.
// Seeds: tests/fuzz/seeds/pptx (the generated decks and a deck with every structure the model
// reads, as text; python tests/fuzz/make_pptx_seeds.py) and tests/fuzz/seeds/package.
//
// An input is a zip, or parts as text as fuzz_check reads them: each part starts on a line
// "@<part name>", and the rest of the input up to the next such line is its content.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "draw/Drawing.h"
#include "pkg/Package.h"
#include "pkg/Snapshot.h"
#include "pkg/Zip.h"
#include "slide/Presentation.h"

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

void CheckAt(bool ok, int line) {
  if (!ok) {
    std::fprintf(stderr, "fuzz_pptx.cpp:%d: check failed\n", line);
    std::abort();
  }
}
#define Check(ok) CheckAt((ok), __LINE__)

bool Finite(float v) { return std::isfinite(v); }
bool Finite(const draw::Rect& r) { return Finite(r.left) && Finite(r.top) && Finite(r.right) && Finite(r.bottom); }
bool Finite(const draw::Color& c) {
  return Finite(c.r) && Finite(c.g) && Finite(c.b) && Finite(c.a) && c.a >= 0 && c.a <= 1;
}

void CheckLink(const Presentation& deck, const Hyperlink& h) {
  if (h.action == Hyperlink::Action::Slide) Check(h.slide >= -1 && h.slide < static_cast<int>(deck.SlideCount()));
}

void CheckText(const Presentation& deck, const TextBody& t) {
  Check(t.body.columns >= 1 && t.body.columns <= 16);
  Check(t.body.fontScale > 0 && t.body.fontScale <= 1);
  Check(Finite(t.body.left) && Finite(t.body.top) && Finite(t.body.right) && Finite(t.body.bottom));
  for (const Paragraph& p : t.paragraphs) {
    Check(p.props.level >= 0 && p.props.level <= 8);
    Check(p.number >= 0);
    Check(Finite(p.props.marginLeft) && Finite(p.props.indent) && Finite(p.props.lineSpacing.value));
    Check(p.props.tabs.size() <= 64);
    Check(p.endProps.size >= 1 && p.endProps.size <= 4000);
    for (const TextRun& r : p.runs) {
      Check(r.props.size >= 1 && r.props.size <= 4000);
      Check(Finite(r.props.baseline) && Finite(r.props.spacing) && Finite(r.props.kern));
      Check(Finite(r.props.color));
      Check(r.node);
      CheckLink(deck, r.props.click);
      CheckLink(deck, r.props.hover);
    }
  }
}

void CheckShape(Presentation& deck, const Shape& s, const SlideBase& owner, const SlideBase& shownOn, int depth) {
  Check(depth <= 40);
  Check(s.owner == &owner);
  Check(s.node && s.drawNode);
  Check(Finite(s.bounds) && Finite(s.rotation));
  Check(owner.FindShape(s.id) != nullptr);
  CheckLink(deck, s.click);
  CheckLink(deck, s.hover);
  if (s.textNode) {
    Check(owner.ShapeOfText(s.textNode) == &s);
    const TextBody* t = deck.Text(s, shownOn);
    Check(t != nullptr);
    CheckText(deck, *t);
    Check(deck.Text(s, shownOn) == t);  // cached
  }
  if (const Table* t = deck.GetTable(s, shownOn)) {
    Check(t->cells.size() == t->rows.size() * t->columns.size());
    for (const TableCell& c : t->cells) {
      Check(c.row >= 0 && c.column >= 0 && c.rowSpan >= 1 && c.columnSpan >= 1);
      Check(c.row + c.rowSpan <= static_cast<int>(t->rows.size()));
      Check(c.column + c.columnSpan <= static_cast<int>(t->columns.size()));
      Check(t->Cell(c.row, c.column) == &c);
      Check(Finite(c.fill.color));
      for (const CellBorder& b : c.borders) Check(Finite(b.width) && b.width >= 0 && Finite(b.color));
      if (c.text) CheckText(deck, *c.text);
    }
  }
  for (const Shape& c : s.children) CheckShape(deck, c, owner, shownOn, depth + 1);
}

// fwdraw reads each drawNode, and every text box it finds belongs to a shape of the part.
void DrawPart(Presentation& deck, const SlideBase& shownOn) {
  if (shownOn.background) {
    Check(shownOn.backgroundOwner != nullptr);
    auto res = deck.Resources(*shownOn.backgroundOwner, shownOn);
    draw::Drawing::FromShapeTree(shownOn.background, *res);
  }
  for (const DrawItem& item : deck.DrawList(shownOn)) {
    Check(item.shape && item.owner);
    CheckShape(deck, *item.shape, *item.owner, shownOn, 0);
    auto res = deck.Resources(*item.owner, shownOn);
    auto d = draw::Drawing::FromShapeTree(item.shape->drawNode, *res);
    if (!d) continue;
    Check(Finite(d->Bounds()));
    for (const draw::TextBox& box : d->TextBoxes()) {
      const Shape* t = item.owner->ShapeOfText(box.content);
      if (t) Check(deck.Text(*t, shownOn) != nullptr);
    }
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const Limits limits = FuzzLimits();
  std::string_view input(reinterpret_cast<const char*>(data), size);
  std::vector<uint8_t> zip = input.starts_with("PK\x03\x04") ? std::vector<uint8_t>(data, data + size) : ZipFromText(input);
  Status st;
  auto pkg = Package::Open(Snapshot::FromBytes(std::move(zip)), limits, st);
  if (!pkg) return 0;
  std::unique_ptr<Presentation> deck = Read(std::move(pkg), st);
  if (!deck) {
    Check(!st.Ok() && !st.message.empty());
    return 0;
  }
  Check(deck->SlideSize().width >= 72 && deck->SlideSize().height >= 72);
  deck->LoadAll();
  for (size_t i = 0; i < deck->SlideCount(); ++i) {
    const Slide* s = deck->GetSlide(i);
    Check(s != nullptr && s->index == i && s->theme);
    Check(deck->SlideIndexOfId(s->id) >= 0);
    DrawPart(*deck, *s);
    if (s->notes) DrawPart(*deck, *s->notes);
    s->NotesText();
  }
  for (const auto& l : deck->Layouts()) DrawPart(*deck, *l);
  for (const auto& m : deck->Masters()) DrawPart(*deck, *m);
  for (const Section& sec : deck->Sections())
    for (size_t i : sec.slides) Check(i < deck->SlideCount());
  for (const CustomShow& cs : deck->CustomShows())
    for (size_t i : cs.slides) Check(i < deck->SlideCount());
  for (const EmbeddedFont& f : deck->EmbeddedFonts()) Check(!f.sfnt.empty());
  deck->Problems();
  return 0;
}
