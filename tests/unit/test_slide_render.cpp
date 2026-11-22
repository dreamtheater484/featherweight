// Slide rendering (P1, src/slide/render): slides drawn from the presentation model. Placeholders
// sit where their layout puts them, prompt text isn't drawn, bullets follow the master's styles,
// the slide show's filter leaves shapes out, thumbnails render at idle and then stop, a big
// bullet keeps its line's height and outlined text is stroked.
#include <windows.h>

#include <cmath>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "PkgTestUtil.h"
#include "TestHarness.h"
#include "pkg/Package.h"
#include "pkg/Snapshot.h"
#include "slide/Presentation.h"
#include "slide/render/SlideRender.h"
#include "text/Fonts.h"

using namespace fw::slide;
using namespace fw::slide::render;

// Stops the test when a condition it needs doesn't hold.
#define REQUIRE(cond)                                             \
  do {                                                            \
    const bool required_ = static_cast<bool>(cond);               \
    if (!required_) return test::Fail(__FILE__, __LINE__, #cond); \
  } while (0)

namespace {

std::filesystem::path Generated() {
  return std::filesystem::path(__FILE__).parent_path().parent_path() / "office" / "generated";
}

std::vector<uint8_t> FileBytes(const std::filesystem::path& p) {
  fw::pkg::Status st;
  auto snap = fw::pkg::Snapshot::Open(p.wstring(), {}, st);
  if (!snap) return {};
  auto span = snap->Bytes();
  return std::vector<uint8_t>(span.begin(), span.end());
}

std::unique_ptr<Presentation> OpenBytes(std::vector<uint8_t> bytes) {
  fw::pkg::Status st;
  auto pk = fw::pkg::Package::Open(fw::pkg::Snapshot::FromBytes(std::move(bytes)), fw::pkg::Limits::Default(), st);
  return pk ? Read(std::move(pk), st) : nullptr;
}

// simple.pptx with some parts replaced.
std::unique_ptr<Presentation> Modified(const std::map<std::string, std::string>& replace) {
  fw::pkg::Status s;
  auto pk = fw::pkg::Package::Open(fw::pkg::Snapshot::FromBytes(FileBytes(Generated() / "simple.pptx")),
                                   fw::pkg::Limits::Default(), s);
  if (!pk) return nullptr;
  std::vector<std::pair<std::string, std::string>> entries;
  for (const fw::pkg::Part& part : pk->Parts()) {
    const std::string name = part.name.substr(1);
    auto it = replace.find(name);
    if (it != replace.end()) {
      entries.emplace_back(name, it->second);
      continue;
    }
    std::vector<uint8_t> bytes;
    pk->ReadPart(part.name, bytes);
    entries.emplace_back(name, test::Text(bytes));
  }
  return OpenBytes(test::MakeZip(entries));
}

constexpr const char* kNs =
    " xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\""
    " xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\""
    " xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\"";

// Slide 1 with these shapes, on layout `layout` (1: Title Slide, 2: Title and Content).
std::map<std::string, std::string> Slide1(const std::string& shapes, int layout = 1) {
  return {{"ppt/slides/slide1.xml",
           std::string("<p:sld") + kNs +
               "><p:cSld><p:spTree><p:nvGrpSpPr><p:cNvPr id=\"1\" name=\"\"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr><p:grpSpPr/>" +
               shapes + "</p:spTree></p:cSld><p:clrMapOvr><a:masterClrMapping/></p:clrMapOvr></p:sld>"},
          {"ppt/slides/_rels/slide1.xml.rels",
           "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
           "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideLayout\""
           " Target=\"../slideLayouts/slideLayout" +
               std::to_string(layout) + ".xml\"/></Relationships>"}};
}

std::string PhShape(int id, const std::string& ph, const std::string& paragraphs) {
  return "<p:sp><p:nvSpPr><p:cNvPr id=\"" + std::to_string(id) + "\" name=\"P" + std::to_string(id) + "\"/><p:cNvSpPr/><p:nvPr>" + ph +
         "</p:nvPr></p:nvSpPr><p:spPr/><p:txBody><a:bodyPr/><a:lstStyle/>" + paragraphs + "</p:txBody></p:sp>";
}

std::string Rect(int id, int x, int y, int w, int h, const char* rgb) {
  return "<p:sp><p:nvSpPr><p:cNvPr id=\"" + std::to_string(id) + "\" name=\"R\"/><p:cNvSpPr/><p:nvPr/></p:nvSpPr><p:spPr><a:xfrm>"
         "<a:off x=\"" + std::to_string(x * 12700) + "\" y=\"" + std::to_string(y * 12700) + "\"/><a:ext cx=\"" +
         std::to_string(w * 12700) + "\" cy=\"" + std::to_string(h * 12700) +
         "\"/></a:xfrm><a:prstGeom prst=\"rect\"/><a:solidFill><a:srgbClr val=\"" + rgb + "\"/></a:solidFill>"
         "<a:ln><a:noFill/></a:ln></p:spPr></p:sp>";
}

// WIC, under RenderBitmap, needs COM on the thread, as in the app.
std::shared_ptr<const draw::Bitmap> Render(const SlideRender& slide, int w, int h, DrawFilter* filter = nullptr) {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  return RenderBitmap(slide, w, h, filter);
}

uint32_t Pixel(const draw::Bitmap& b, int x, int y) {
  const uint8_t* p = b.Pixels() + (static_cast<size_t>(y) * b.Width() + x) * 4;
  return (uint32_t(p[2]) << 16) | (uint32_t(p[1]) << 8) | p[0];
}

bool AllOf(const draw::Bitmap& b, int x0, int y0, int x1, int y1, uint32_t rgb) {
  for (int y = y0; y < y1; y += 2)
    for (int x = x0; x < x1; x += 2)
      if (Pixel(b, x, y) != rgb) return false;
  return true;
}

}  // namespace

TEST(SlideRender_GeneratedDeckTitleSlide) {
  auto deck = OpenBytes(FileBytes(Generated() / "simple.pptx"));
  CHECK(deck != nullptr);
  if (!deck) return;
  fw::text::Fonts fonts;
  auto slide = SlideRender::Make(*deck, *deck->GetSlide(0), fonts);
  CHECK(slide->Text().find(L"A simple presentation") != std::wstring::npos);
  CHECK(slide->Text().find(L"Click") == std::wstring::npos);
  CHECK_NEAR(slide->Size().width, 720, 0.01);
  // The title where the Title Slide layout puts its centered title.
  const ShapeInfo* title = nullptr;
  for (const ShapeInfo& i : slide->Shapes())
    if (i.shape && i.shape->placeholder.type == PlaceholderType::CenteredTitle) title = &i;
  CHECK(title != nullptr);
  if (!title) return;
  if (const Shape* from = title->shape->inherits) CHECK_NEAR(title->bounds.left, from->bounds.left, 0.01);
  auto bmp = Render(*slide, 720, 540);
  CHECK(bmp != nullptr);
  if (!bmp) return;
  // The master's white background (bgRef bg1), dark title text in its box.
  CHECK_EQ(Pixel(*bmp, 5, 5), 0xFFFFFFu);
  int dark = 0;
  for (int y = static_cast<int>(title->bounds.top); y < static_cast<int>(title->bounds.bottom); ++y)
    for (int x = 0; x < 720; ++x) dark += (Pixel(*bmp, x, y) & 0xFF) < 100;
  CHECK(dark > 300);
}

TEST(SlideRender_PromptTextIsNotDrawn) {
  auto deck = Modified(Slide1(PhShape(2, "<p:ph type=\"ctrTitle\"/>", "<a:p><a:endParaRPr lang=\"en-US\"/></a:p>")));
  CHECK(deck != nullptr);
  if (!deck) return;
  fw::text::Fonts fonts;
  auto slide = SlideRender::Make(*deck, *deck->GetSlide(0), fonts);
  CHECK(slide->Text().empty());
  auto bmp = Render(*slide, 360, 270);
  CHECK(bmp && AllOf(*bmp, 0, 0, 360, 270, 0xFFFFFFu));
}

TEST(SlideRender_BulletsFollowTheMastersBodyStyle) {
  // Title and Content: first-level paragraphs with the master's bullet, a second-level one
  // indented further, each paragraph below the one before.
  auto deck = Modified(Slide1(PhShape(3, "<p:ph idx=\"1\"/>",
                                          "<a:p><a:r><a:t>One</a:t></a:r></a:p><a:p><a:pPr lvl=\"1\"/><a:r><a:t>Sub</a:t></a:r></a:p>"
                                          "<a:p><a:r><a:t>Two</a:t></a:r></a:p>"),
                              2));
  CHECK(deck != nullptr);
  if (!deck) return;
  fw::text::Fonts fonts;
  auto slide = SlideRender::Make(*deck, *deck->GetSlide(0), fonts);
  CHECK(slide->Text() == L"One\nSub\nTwo");
  const std::vector<ParagraphInfo>& p = slide->Paragraphs();
  CHECK_EQ(p.size(), size_t{3});
  if (p.size() != 3) return;
  CHECK(p[1].bounds.left > p[0].bounds.left + 20);
  CHECK(p[1].bounds.top > p[0].bounds.bottom - 1);
  CHECK(p[2].bounds.top > p[1].bounds.bottom - 1);
}

TEST(SlideRender_BigBulletKeepsTheLineHeight) {
  // A bullet at 150 % of the text leaves its line as high as the next (PowerPoint's PDFs).
  auto deck = Modified(Slide1(PhShape(3, "<p:ph idx=\"1\"/>",
                                          "<a:p><a:pPr><a:buSzPct val=\"150000\"/></a:pPr><a:r><a:t>Big</a:t></a:r></a:p>"
                                          "<a:p><a:r><a:t>Plain</a:t></a:r></a:p>"),
                              2));
  REQUIRE(deck != nullptr);
  fw::text::Fonts fonts;
  auto slide = SlideRender::Make(*deck, *deck->GetSlide(0), fonts);
  const std::vector<ParagraphInfo>& p = slide->Paragraphs();
  REQUIRE(p.size() == 2);
  const float big = p[0].bounds.bottom - p[0].bounds.top, plain = p[1].bounds.bottom - p[1].bounds.top;
  CHECK(std::fabs(big - plain) < 0.5f);
}

TEST(SlideRender_LineBreakAloneIsItsOwnSize) {
  // A paragraph starting with an 18-pt a:br in a 40-pt body: the empty first line is 1.2 x 18 high
  // (PowerPoint's PDFs), so the paragraph is as high as one that breaks after 18-pt text.
  auto deck = Modified(Slide1(PhShape(3, "<p:ph idx=\"1\"/>",
                                          "<a:p><a:pPr><a:buNone/></a:pPr><a:br><a:rPr sz=\"1800\"/></a:br><a:r><a:rPr sz=\"1800\"/>"
                                          "<a:t>Text</a:t></a:r><a:endParaRPr sz=\"4000\"/></a:p>"
                                          "<a:p><a:pPr><a:buNone/></a:pPr><a:r><a:rPr sz=\"1800\"/><a:t>X</a:t></a:r><a:br>"
                                          "<a:rPr sz=\"1800\"/></a:br><a:r><a:rPr sz=\"1800\"/><a:t>Text</a:t></a:r>"
                                          "<a:endParaRPr sz=\"4000\"/></a:p>"),
                              2));
  REQUIRE(deck != nullptr);
  fw::text::Fonts fonts;
  auto slide = SlideRender::Make(*deck, *deck->GetSlide(0), fonts);
  const std::vector<ParagraphInfo>& p = slide->Paragraphs();
  REQUIRE(p.size() == 2);
  const float empty = p[0].bounds.bottom - p[0].bounds.top, full = p[1].bounds.bottom - p[1].bounds.top;
  CHECK(std::fabs(empty - full) < 0.5f);
}

TEST(SlideRender_FirstLineStaysInTheTextArea) {
  // marL 0 with a 27-pt hanging indent: PowerPoint starts the first line at the text area's edge.
  auto deck = Modified(Slide1(PhShape(3, "<p:ph idx=\"1\"/>",
                                          "<a:p><a:pPr marL=\"0\" indent=\"-342900\"><a:buNone/></a:pPr><a:r><a:t>Hangs</a:t></a:r></a:p>"
                                          "<a:p><a:pPr marL=\"0\" indent=\"0\"><a:buNone/></a:pPr><a:r><a:t>Hangs</a:t></a:r></a:p>"),
                              2));
  REQUIRE(deck != nullptr);
  fw::text::Fonts fonts;
  auto slide = SlideRender::Make(*deck, *deck->GetSlide(0), fonts);
  const std::vector<ParagraphInfo>& p = slide->Paragraphs();
  REQUIRE(p.size() == 2);
  CHECK(std::fabs(p[0].bounds.left - p[1].bounds.left) < 0.5f);
}

TEST(SlideRender_KernsOnlyByTheKernTable) {
  // kern="1200": Arial (a 'kern' table) is kerned, Georgia (OpenType pairs only) isn't, as in
  // PowerPoint's PDFs.
  auto width = [](const char* font, bool kern) {
    const std::string rPr = std::string("<a:rPr sz=\"4000\"") + (kern ? " kern=\"1200\"" : " kern=\"0\"") + "><a:latin typeface=\"" + font +
                            "\"/></a:rPr>";
    auto deck = Modified(Slide1(PhShape(3, "<p:ph idx=\"1\"/>",
                                            "<a:p><a:pPr><a:buNone/></a:pPr><a:r>" + rPr + "<a:t>AVAVAVAV</a:t></a:r></a:p>"),
                                2));
    if (!deck) return -1.f;
    fw::text::Fonts fonts;
    auto slide = SlideRender::Make(*deck, *deck->GetSlide(0), fonts);
    const std::vector<ParagraphInfo>& p = slide->Paragraphs();
    return p.empty() ? -1.f : p[0].bounds.right - p[0].bounds.left;
  };
  CHECK(width("Arial", true) < width("Arial", false) - 1);
  CHECK(std::fabs(width("Georgia", true) - width("Georgia", false)) < 0.01f);
}

TEST(SlideRender_ShrunkTextKeepsPointSpacing) {
  // normAutofit at 50 %: 12 pt before a paragraph stays 12 pt (cpb deck: PowerPoint's paragraph
  // gaps are the stated points, not shrunk with the text).
  const std::string para = "<a:p><a:pPr><a:spcBef><a:spcPts val=\"1200\"/></a:spcBef><a:spcAft><a:spcPts val=\"0\"/></a:spcAft>"
                           "<a:buNone/></a:pPr><a:r><a:t>Line</a:t></a:r></a:p>";
  std::string shape = PhShape(3, "<p:ph idx=\"1\"/>", para + para);
  const std::string plain = "<a:bodyPr/>";
  shape.replace(shape.find(plain), plain.size(), "<a:bodyPr><a:normAutofit fontScale=\"50000\"/></a:bodyPr>");
  auto deck = Modified(Slide1(shape, 2));
  REQUIRE(deck != nullptr);
  fw::text::Fonts fonts;
  auto slide = SlideRender::Make(*deck, *deck->GetSlide(0), fonts);
  const std::vector<ParagraphInfo>& p = slide->Paragraphs();
  REQUIRE(p.size() == 2);
  CHECK(std::fabs(p[1].bounds.top - p[0].bounds.bottom - 12.f) < 0.05f);
}

TEST(SlideRender_SimulatedBoldKeepsAdvances) {
  // Calibri Light has no bold: PowerPoint emboldens it in place, its advances as the light face's.
  auto width = [](bool bold) {
    const std::string rPr = std::string("<a:rPr sz=\"4400\" kern=\"0\"") + (bold ? " b=\"1\"" : " b=\"0\"") +
                            "><a:latin typeface=\"Calibri Light\"/></a:rPr>";
    auto deck = Modified(Slide1(PhShape(3, "<p:ph idx=\"1\"/>",
                                            "<a:p><a:pPr><a:buNone/></a:pPr><a:r>" + rPr + "<a:t>Geopolitical</a:t></a:r></a:p>"),
                                2));
    if (!deck) return -1.f;
    fw::text::Fonts fonts;
    auto slide = SlideRender::Make(*deck, *deck->GetSlide(0), fonts);
    const std::vector<ParagraphInfo>& p = slide->Paragraphs();
    return p.empty() ? -1.f : p[0].bounds.right - p[0].bounds.left;
  };
  const float regular = width(false), bold = width(true);
  CHECK(regular > 0);
  CHECK(std::fabs(bold - regular) < 0.5f);
}

TEST(SlideRender_OutlinedTextIsStroked) {
  // A run with no fill and a red a:ln: its glyphs' outlines in red.
  auto deck = Modified(Slide1(PhShape(2, "<p:ph type=\"title\"/>",
                                          "<a:p><a:r><a:rPr sz=\"6000\"><a:ln w=\"38100\"><a:solidFill><a:srgbClr val=\"FF0000\"/>"
                                          "</a:solidFill></a:ln><a:noFill/></a:rPr><a:t>HHHH</a:t></a:r></a:p>")));
  REQUIRE(deck != nullptr);
  fw::text::Fonts fonts;
  auto slide = SlideRender::Make(*deck, *deck->GetSlide(0), fonts);
  auto b = Render(*slide, 480, 360);
  REQUIRE(b != nullptr);
  int red = 0;
  for (int y = 0; y < b->Height(); ++y)
    for (int x = 0; x < b->Width(); ++x) {
      const uint32_t c = Pixel(*b, x, y);
      red += (c >> 16) > 200 && ((c >> 8) & 0xFF) < 90 && (c & 0xFF) < 90;
    }
  CHECK(red > 40);
}

TEST(SlideRender_FilterLeavesShapesOut) {
  // A red and a blue rectangle; the show's filter leaves the red one out, then the blue one's body.
  auto deck = Modified(Slide1(Rect(4, 100, 100, 100, 100, "FF0000") + Rect(5, 300, 100, 100, 100, "0000FF")));
  CHECK(deck != nullptr);
  if (!deck) return;
  fw::text::Fonts fonts;
  auto slide = SlideRender::Make(*deck, *deck->GetSlide(0), fonts);
  CHECK_EQ(slide->Shapes().size(), size_t{2});
  if (slide->Shapes().size() != 2) return;
  CHECK_EQ(slide->Shapes()[1].id, 5u);
  CHECK_NEAR(slide->Shapes()[1].bounds.left, 300, 0.01);
  auto all = Render(*slide, 720, 540);
  CHECK(all && Pixel(*all, 150, 150) == 0xFF0000u && Pixel(*all, 350, 150) == 0x0000FFu);
  struct NoRed : DrawFilter {
    bool Shape(uint32_t id) override { return id != 4; }
  } noRed;
  auto some = Render(*slide, 720, 540, &noRed);
  CHECK(some && Pixel(*some, 150, 150) == 0xFFFFFFu && Pixel(*some, 350, 150) == 0x0000FFu);
  struct NoBodies : DrawFilter {
    bool ShapeBody(uint32_t) override { return false; }
    bool Background() override { return false; }
  } noBodies;
  auto none = Render(*slide, 720, 540, &noBodies);
  CHECK(none && Pixel(*none, 350, 150) == 0xFFFFFFu);
}

TEST(SlideRender_ThumbnailsRenderAtIdleThenStop) {
  auto deck = OpenBytes(FileBytes(Generated() / "layouts.pptx"));
  CHECK(deck != nullptr);
  if (!deck || deck->SlideCount() < 5) return;
  fw::text::Fonts fonts;
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  Thumbnails thumbs([&](size_t i) { return SlideRender::Make(*deck, *deck->GetSlide(i), fonts); });
  thumbs.Reset(deck->SlideCount(), 96, 72);
  thumbs.Prefer(3, 4);
  CHECK(thumbs.RenderNext());
  CHECK(thumbs.Get(3) != nullptr);
  CHECK(thumbs.Get(0) == nullptr);
  size_t steps = 1;
  while (thumbs.RenderNext()) ++steps;
  CHECK_EQ(steps + 1, deck->SlideCount());
  CHECK(thumbs.Done());
  CHECK(!thumbs.RenderNext());  // nothing left: the idle handler stops
  for (size_t i = 0; i < deck->SlideCount(); ++i) CHECK(thumbs.Get(i) != nullptr);
}
