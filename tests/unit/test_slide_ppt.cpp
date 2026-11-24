// The .ppt reader (src/slide/ppt, P3 p3-ppt): PowerPoint 97-2003 records built here by hand,
// converted into a .pptx package and read into the slide model.
#include "TestHarness.h"

#include <windows.h>

#include <cstdlib>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "pkg/Check.h"
#include "pkg/CompoundFile.h"
#include "pkg/Package.h"
#include "pkg/Snapshot.h"
#include "slide/Presentation.h"
#include "slide/ppt/Ppt.h"

using namespace fw;
using namespace fw::slide;

namespace {

using Bytes = std::vector<uint8_t>;

void Le(Bytes& b, uint64_t v, int n) {
  for (int i = 0; i < n; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
Bytes Rec(uint16_t ver, uint16_t inst, uint16_t type, const Bytes& body) {
  Bytes b;
  Le(b, ver | inst << 4, 2);
  Le(b, type, 2);
  Le(b, body.size(), 4);
  b.insert(b.end(), body.begin(), body.end());
  return b;
}
Bytes Cat(std::initializer_list<Bytes> parts) {
  Bytes b;
  for (const Bytes& p : parts) b.insert(b.end(), p.begin(), p.end());
  return b;
}
Bytes Utf16(std::u16string_view s) {
  Bytes b;
  for (char16_t c : s) Le(b, c, 2);
  return b;
}
Bytes SlideAtom(uint32_t master, uint16_t flags) {
  Bytes b;
  Le(b, 0, 4);  // geom
  b.insert(b.end(), 8, 0);
  Le(b, master, 4);
  Le(b, 0, 4);
  Le(b, flags, 2);
  Le(b, 0, 2);
  return Rec(2, 0, 0x03EF, b);
}
// A drawing: the patriarch group and `shapes`.
Bytes Drawing(const Bytes& shapes) {
  Bytes fspgr(16, 0), fsp;
  Le(fsp, 1024, 4);
  Le(fsp, 5, 4);
  const Bytes patriarch = Rec(0xF, 0, 0xF004, Cat({Rec(1, 0, 0xF009, fspgr), Rec(2, 0, 0xF00A, fsp)}));
  Bytes fdg;
  Le(fdg, 2, 4);
  Le(fdg, 1025, 4);
  return Rec(0xF, 0, 0x040C, Rec(0xF, 0, 0xF002, Cat({Rec(0, 1, 0xF008, fdg), Rec(0xF, 0, 0xF003, Cat({patriarch, shapes}))})));
}

struct Built {
  Bytes document, user, pictures;
};

// A one-slide presentation: a master with a title style (Arial 44 pt, centered), and a slide whose
// title placeholder has a red fill and the text "Hello" (bold) and "World".
Built OneSlide() {
  // The document.
  Bytes docAtom;
  for (int v : {5760, 4320, 4320, 5760, 1, 2, 0, 0}) Le(docAtom, static_cast<uint32_t>(v), 4);
  Le(docAtom, 1, 2);  // first slide number
  Le(docAtom, 0, 2);
  Le(docAtom, 0, 4);
  Bytes font = Utf16(u"Arial");
  font.resize(64, 0);
  font.insert(font.end(), {0, 0, 0, 0x22});
  Bytes cf;  // TextCFException: size 18, font 0
  Le(cf, (1u << 16) | (1u << 17), 4);
  Le(cf, 0, 2);
  Le(cf, 18, 2);
  const Bytes environment = Rec(0xF, 0, 0x03F2, Cat({Rec(0xF, 0, 0x07D5, Rec(0, 0, 0x0FB7, font)), Rec(0, 0, 0x0FA4, cf)}));
  const auto persistAtom = [](uint32_t persist, uint32_t id) {
    Bytes b;
    for (uint32_t v : {persist, 0u, 0u, id, 0u}) Le(b, v, 4);
    return Rec(0, 0, 0x03F3, b);
  };
  const Bytes document = Rec(0xF, 0, 0x03E8, Cat({Rec(1, 0, 0x03E9, docAtom), environment, Rec(0xF, 1, 0x0FF0, persistAtom(2, 0x80000000)),
                                                  Rec(0xF, 0, 0x0FF0, persistAtom(3, 256)), Rec(0, 0, 0x03EA, {})}));
  // The master: a white/black scheme and the title style.
  Bytes scheme;
  for (uint32_t c : {0xFFFFFFu, 0u, 0x808080u, 0u, 0xBBE0E3u, 0x333399u, 0x009999u, 0x99CC00u}) Le(scheme, c, 4);
  Bytes title;
  Le(title, 1, 2);                 // one level
  Le(title, 1u << 11, 4);          // PF: alignment
  Le(title, 1, 2);                 // centered
  Le(title, (1u << 16) | (1u << 17), 4);  // CF: typeface, size
  Le(title, 0, 2);
  Le(title, 44, 2);
  const Bytes master = Rec(0xF, 0, 0x03F8, Cat({SlideAtom(0, 0), Rec(0, 1, 0x07F0, scheme), Rec(0, 0, 0x0FA3, title), Drawing({})}));
  // The slide: one title placeholder with text.
  Bytes fsp;
  Le(fsp, 1025, 4);
  Le(fsp, 0xA00, 4);
  Bytes opt;
  Le(opt, 0x0181, 2);
  Le(opt, 0x0000FF, 4);  // fill: red
  Bytes anchor;
  for (int v : {576, 576, 2880, 1152}) Le(anchor, static_cast<uint16_t>(v), 2);  // top, left, right, bottom
  Bytes placeholder;
  Le(placeholder, 0, 4);
  placeholder.insert(placeholder.end(), {0x0D, 0, 0, 0});
  Bytes header;
  Le(header, 0, 4);  // title text
  Bytes style;       // one paragraph run over all 12 characters; bold "Hello", then the rest
  Le(style, 12, 4);
  Le(style, 0, 2);
  Le(style, 0, 4);
  Le(style, 5, 4);
  Le(style, 1u << 0, 4);
  Le(style, 1, 2);
  Le(style, 7, 4);
  Le(style, 0, 4);
  const Bytes textbox = Cat({Rec(0, 0, 0x0F9F, header), Rec(0, 0, 0x0FA0, Utf16(u"Hello\rWorld")), Rec(0, 0, 0x0FA1, style)});
  const Bytes shape = Rec(0xF, 0, 0xF004, Cat({Rec(2, 1, 0xF00A, fsp), Rec(3, 1, 0xF00B, opt), Rec(0, 0, 0xF010, anchor),
                                                Rec(0xF, 0, 0xF011, Rec(0, 0, 0x0BC3, placeholder)), Rec(0xF, 0, 0xF00D, textbox)}));
  const Bytes slide = Rec(0xF, 0, 0x03EE, Cat({SlideAtom(0x80000000, 7), Drawing(shape)}));

  Built out;
  out.document = document;
  const uint32_t masterAt = static_cast<uint32_t>(out.document.size());
  out.document.insert(out.document.end(), master.begin(), master.end());
  const uint32_t slideAt = static_cast<uint32_t>(out.document.size());
  out.document.insert(out.document.end(), slide.begin(), slide.end());
  const uint32_t dirAt = static_cast<uint32_t>(out.document.size());
  Bytes dir;
  Le(dir, 1 | 3u << 20, 4);
  for (uint32_t v : {0u, masterAt, slideAt}) Le(dir, v, 4);
  const Bytes directory = Rec(0, 0, 0x1772, dir);
  out.document.insert(out.document.end(), directory.begin(), directory.end());
  const uint32_t editAt = static_cast<uint32_t>(out.document.size());
  Bytes edit;
  Le(edit, 256, 4);
  Le(edit, 0x0DBC, 2);
  edit.insert(edit.end(), {0, 3});
  for (uint32_t v : {0u, dirAt, 1u, 4u}) Le(edit, v, 4);
  Le(edit, 1, 2);
  Le(edit, 0, 2);
  const Bytes userEdit = Rec(0, 0, 0x0FF5, edit);
  out.document.insert(out.document.end(), userEdit.begin(), userEdit.end());
  Bytes user;
  for (uint32_t v : {0x14u, 0xE391C05Fu, editAt}) Le(user, v, 4);
  Le(user, 0, 2);
  Le(user, 0x03F4, 2);
  user.insert(user.end(), {3, 0, 0, 0});
  Le(user, 8, 4);
  out.user = Rec(0, 0, 0x0FF6, user);
  return out;
}

ppt::StreamFn StreamsOf(const Built& b) {
  return [b](std::string_view name) -> std::optional<std::vector<uint8_t>> {
    if (name == "PowerPoint Document") return b.document;
    if (name == "Current User") return b.user;
    if (name == "Pictures" && !b.pictures.empty()) return b.pictures;
    return std::nullopt;
  };
}

std::unique_ptr<Presentation> Open(const Bytes& pptx) {
  pkg::Status st;
  auto pk = pkg::Package::Open(pkg::Snapshot::FromBytes(Bytes(pptx)), pkg::Limits::Default(), st);
  CHECK(pk != nullptr);
  if (!pk) return nullptr;
  CHECK(pkg::CheckPackage(*pk).Ok());
  auto deck = Read(std::move(pk), st);
  CHECK(deck != nullptr);
  return deck;
}

}  // namespace

TEST(ppt_one_slide) {
  const Built built = OneSlide();
  const ppt::StreamFn streams = StreamsOf(built);
  CHECK(ppt::IsPresentation(streams));
  CHECK(!ppt::NeedsPassword(streams));
  pkg::Status st;
  const Bytes pptx = ppt::ToPptx(streams, st);
  CHECK(st.Ok());
  auto deck = Open(pptx);
  if (!deck) return;
  CHECK_NEAR(deck->SlideSize().width, 720.f, 0.01f);
  CHECK_NEAR(deck->SlideSize().height, 540.f, 0.01f);
  CHECK_EQ(deck->SlideCount(), size_t{1});
  const Slide* s = deck->GetSlide(0);
  const Shape* title = s ? s->FindPlaceholder(PlaceholderType::Title) : nullptr;
  CHECK(title != nullptr);
  if (!title) return;
  CHECK_NEAR(title->bounds.left, 72.f, 0.01f);
  CHECK_NEAR(title->bounds.top, 72.f, 0.01f);
  CHECK_NEAR(title->bounds.right, 360.f, 0.01f);
  CHECK_NEAR(title->bounds.bottom, 144.f, 0.01f);
  const TextBody* t = deck->Text(*title, *s);
  CHECK(t && t->paragraphs.size() == 2);
  if (!t || t->paragraphs.size() != 2) return;
  CHECK(t->paragraphs[0].runs[0].text == L"Hello");
  CHECK(t->paragraphs[0].runs[0].props.bold);
  CHECK_NEAR(t->paragraphs[0].runs[0].props.size, 44.f, 0.01f);
  CHECK(t->paragraphs[0].runs[0].props.latin == L"Arial");
  CHECK(t->paragraphs[0].props.align == TextAlign::Center);
  CHECK(t->paragraphs[1].runs[0].text == L"World");
  CHECK(!t->paragraphs[1].runs[0].props.bold);
  // Inside a compound file, as PowerPoint writes it.
  const Bytes cfb = pkg::WriteCompoundFile({{"PowerPoint Document", built.document}, {"Current User", built.user}});
  auto cf = pkg::CompoundFile::Open(cfb);
  CHECK(cf != nullptr);
  if (cf) {
    pkg::Status st2;
    const Bytes again = ppt::ToPptx([&cf](std::string_view n) { return cf->Stream(n); }, st2);
    CHECK(st2.Ok() && again == pptx);
  }
  // The fuzzer's seed: FW_PPT_SEED=<file> fw_tests writes this presentation there.
  char seed[MAX_PATH];
  if (GetEnvironmentVariableA("FW_PPT_SEED", seed, MAX_PATH) > 0) {
    std::ofstream out(seed, std::ios::binary);
    out.write(reinterpret_cast<const char*>(cfb.data()), static_cast<std::streamsize>(cfb.size()));
  }
}

TEST(ppt_damaged_input) {
  // Not a presentation, a truncated one, and one whose persist directory points nowhere: plain failures.
  pkg::Status st;
  CHECK(ppt::ToPptx([](std::string_view) { return std::nullopt; }, st).empty() && !st.Ok());
  Built built = OneSlide();
  for (size_t cut : {size_t{0}, size_t{9}, built.document.size() / 2, built.document.size() - 3}) {
    Built b = built;
    b.document.resize(cut);
    pkg::Status s2;
    const Bytes pptx = ppt::ToPptx(StreamsOf(b), s2);
    CHECK(pptx.empty() == !s2.Ok());
  }
}
