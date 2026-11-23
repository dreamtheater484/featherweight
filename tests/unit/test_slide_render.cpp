// Slide rendering (P1, src/slide/render): slides drawn from the presentation model. Placeholders
// sit where their layout puts them, prompt text isn't drawn, bullets follow the master's styles,
// the slide show's filter leaves shapes out, thumbnails render at idle and then stop, a big
// bullet keeps its line's height and outlined text is stroked.
#include <windows.h>

#include <algorithm>
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

TEST(SlideRender_BulletWithoutHangingIndentIsFollowedStraightOn) {
  // marL 0, indent 0: PowerPoint draws "•text", no tab between; a hanging indent keeps the tab.
  const auto width = [](const char* pPr, const char* bullet = "&#8226;") {
    auto deck = Modified(Slide1(PhShape(3, "<p:ph idx=\"1\"/>",
                                            std::string("<a:p><a:pPr ") + pPr + "><a:buFont typeface=\"Arial\"/><a:buChar char=\"" + bullet + "\"/></a:pPr>"
                                            "<a:r><a:rPr sz=\"1800\"/><a:t>x</a:t></a:r></a:p>"),
                                2));
    if (!deck) return -1.f;
    fw::text::Fonts fonts;
    auto slide = SlideRender::Make(*deck, *deck->GetSlide(0), fonts);
    const std::vector<ParagraphInfo>& p = slide->Paragraphs();
    return p.empty() ? -1.f : p[0].bounds.right - p[0].bounds.left;
  };
  const float glued = width("marL=\"0\" indent=\"0\""), hanging = width("marL=\"685800\" indent=\"-685800\"");
  CHECK(glued > 5 && glued < 25);
  CHECK(hanging > 54);
  // Only a bullet's first character is drawn ("••" in a SmartArt list draws one).
  CHECK_NEAR(width("marL=\"0\" indent=\"0\"", "&#8226;&#8226;"), glued, 0.01f);
}

TEST(SlideRender_MissingFontNamingAnInstalledFamilyUsesIt) {
  // "Adobe Garamond Pro", not installed, is drawn in Garamond (PowerPoint's PDF), not Calibri.
  fw::text::Fonts probe;
  if (!probe.Installed(L"Garamond") || probe.Installed(L"Adobe Garamond Pro")) return;
  const auto width = [](const char* font) {
    auto deck = Modified(Slide1(PhShape(3, "<p:ph idx=\"1\"/>",
                                            std::string("<a:p><a:pPr><a:buNone/></a:pPr><a:r><a:rPr sz=\"2400\"><a:latin typeface=\"") + font +
                                                "\"/></a:rPr><a:t>Petroleum Sites</a:t></a:r></a:p>"),
                                2));
    if (!deck) return -1.f;
    fw::text::Fonts fonts;
    fw::slide::render::UseDeckFonts(fonts, *deck);  // PowerPoint's substitutes
    auto slide = SlideRender::Make(*deck, *deck->GetSlide(0), fonts);
    const std::vector<ParagraphInfo>& p = slide->Paragraphs();
    return p.empty() ? -1.f : p[0].bounds.right - p[0].bounds.left;
  };
  CHECK_NEAR(width("Adobe Garamond Pro"), width("Garamond"), 0.01f);
  CHECK(std::fabs(width("Adobe Garamond Pro") - width("Calibri")) > 1);
}

TEST(SlideRender_PatternCellSpansWholePixels) {
  // A 6-pt pattern cell at 100 dpi is 8.33 pixels; the slide view draws it at 9, as a PDF viewer
  // (pdfium) draws the PDF's tiling pattern, so every row repeats every 9 pixels.
  auto deck = Modified(Slide1(
      "<p:sp><p:nvSpPr><p:cNvPr id=\"2\" name=\"P\"/><p:cNvSpPr/><p:nvPr/></p:nvSpPr><p:spPr><a:xfrm><a:off x=\"0\" y=\"0\"/>"
      "<a:ext cx=\"9144000\" cy=\"6858000\"/></a:xfrm><a:prstGeom prst=\"rect\"/><a:pattFill prst=\"dkVert\"><a:fgClr>"
      "<a:srgbClr val=\"000000\"/></a:fgClr><a:bgClr><a:srgbClr val=\"FFFFFF\"/></a:bgClr></a:pattFill><a:ln><a:noFill/></a:ln>"
      "</p:spPr></p:sp>"));
  REQUIRE(deck != nullptr);
  fw::text::Fonts fonts;
  auto slide = SlideRender::Make(*deck, *deck->GetSlide(0), fonts);
  auto bmp = Render(*slide, 1000, 750);
  REQUIRE(bmp != nullptr);
  int apart9 = 0, apart8 = 0;
  for (int x = 200; x < 600; ++x) {
    const auto level = [&](int at) { return static_cast<int>(Pixel(*bmp, at, 375) & 0xFF); };
    apart9 = std::max(apart9, std::abs(level(x) - level(x + 9)));
    apart8 = std::max(apart8, std::abs(level(x) - level(x + 8)));
  }
  CHECK(apart9 <= 2);
  CHECK(apart8 > 40);
}

TEST(SlideRender_TurnedShapesPatternStaysUpright) {
  // A pattern doesn't turn with its shape (PowerPoint's PDFs: upright cells on the page's grid):
  // vertical stripes in a shape turned 90 degrees stay vertical.
  auto deck = Modified(Slide1(
      "<p:sp><p:nvSpPr><p:cNvPr id=\"2\" name=\"P\"/><p:cNvSpPr/><p:nvPr/></p:nvSpPr><p:spPr><a:xfrm rot=\"5400000\">"
      "<a:off x=\"1143000\" y=\"1143000\"/><a:ext cx=\"4572000\" cy=\"4572000\"/></a:xfrm><a:prstGeom prst=\"rect\"/>"
      "<a:pattFill prst=\"dkVert\"><a:fgClr><a:srgbClr val=\"000000\"/></a:fgClr><a:bgClr><a:srgbClr val=\"FFFFFF\"/></a:bgClr>"
      "</a:pattFill><a:ln><a:noFill/></a:ln></p:spPr></p:sp>"));
  REQUIRE(deck != nullptr);
  fw::text::Fonts fonts;
  auto slide = SlideRender::Make(*deck, *deck->GetSlide(0), fonts);
  auto bmp = Render(*slide, 1000, 750);
  REQUIRE(bmp != nullptr);
  // Inside the shape (90 to 450 pt, 125 to 625 pixels): along a row the stripes alternate, down a column not.
  int across = 0, down = 0;
  for (int i = 300; i < 400; ++i) {
    const auto level = [&](int x, int y) { return static_cast<int>(Pixel(*bmp, x, y) & 0xFF); };
    across = std::max(across, std::abs(level(i, 350) - level(i + 1, 350)));
    down = std::max(down, std::abs(level(350, i) - level(350, i + 1)));
  }
  CHECK(across > 40);
  CHECK(down <= 2);
}

TEST(SlideRender_ShapesOwnStyleBeatsTheLayoutsNoFill) {
  // The layout's title says noFill; the slide's title has an empty spPr and a p:style whose
  // fillRef is red: PowerPoint fills it red (poi customgeo deck).
  std::map<std::string, std::string> parts = Slide1(
      "<p:sp><p:nvSpPr><p:cNvPr id=\"2\" name=\"T\"/><p:cNvSpPr/><p:nvPr><p:ph type=\"title\"/></p:nvPr></p:nvSpPr><p:spPr/>"
      "<p:style><a:lnRef idx=\"0\"><a:srgbClr val=\"000000\"/></a:lnRef><a:fillRef idx=\"1\"><a:srgbClr val=\"FF0000\"/></a:fillRef>"
      "<a:effectRef idx=\"0\"><a:srgbClr val=\"000000\"/></a:effectRef><a:fontRef idx=\"minor\"/></p:style>"
      "<p:txBody><a:bodyPr/><a:lstStyle/><a:p><a:endParaRPr lang=\"en-US\"/></a:p></p:txBody></p:sp>",
      2);
  parts["ppt/slideLayouts/slideLayout2.xml"] =
      std::string("<p:sldLayout") + kNs +
      "><p:cSld name=\"T\"><p:spTree><p:nvGrpSpPr><p:cNvPr id=\"1\" name=\"\"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr><p:grpSpPr/>"
      "<p:sp><p:nvSpPr><p:cNvPr id=\"2\" name=\"Title 1\"/><p:cNvSpPr/><p:nvPr><p:ph type=\"title\"/></p:nvPr></p:nvSpPr><p:spPr>"
      "<a:xfrm><a:off x=\"1270000\" y=\"1270000\"/><a:ext cx=\"3810000\" cy=\"1270000\"/></a:xfrm><a:noFill/><a:ln><a:noFill/></a:ln>"
      "</p:spPr></p:sp></p:spTree></p:cSld><p:clrMapOvr><a:masterClrMapping/></p:clrMapOvr></p:sldLayout>";
  auto deck = Modified(parts);
  REQUIRE(deck != nullptr);
  fw::text::Fonts fonts;
  auto slide = SlideRender::Make(*deck, *deck->GetSlide(0), fonts);
  auto bmp = Render(*slide, 720, 540);
  REQUIRE(bmp != nullptr);
  CHECK_EQ(Pixel(*bmp, 250, 150), 0xFF0000u);  // inside the title's 100..400 x 100..200 points
  CHECK_EQ(Pixel(*bmp, 250, 250), 0xFFFFFFu);  // below it
}

TEST(SlideRender_DirectWriteMetricsAgreeWithGdisForCalibri) {
  // Slide text takes DirectWrite's line metrics (a font's typo metrics when its USE_TYPO_METRICS
  // bit asks, as PowerPoint does: embedded Public Sans); for Calibri both say the same, so slides
  // in it don't move.
  fw::text::Fonts fonts;
  const fw::text::Face* calibri = fonts.Get(L"Calibri", DWRITE_FONT_WEIGHT_NORMAL, false);
  if (!calibri) return;
  const fw::text::FontMetrics gdi = calibri->Metrics(18), dw = calibri->Metrics(18, true);
  CHECK_NEAR(dw.ascent, gdi.ascent, 0.01f);
  CHECK_NEAR(dw.descent, gdi.descent, 0.01f);
}

TEST(SlideRender_KernsOnlyByTheKernTable) {
  // kern="1200": Arial (a 'kern' table) is kerned, Georgia (OpenType pairs only) isn't, as in
  // PowerPoint's PDFs; Franklin Gothic Medium (a 'kern' table, its GPOS without pairs, so
  // DirectWrite alone wouldn't kern it) is kerned by the table (rlf-webinar decks).
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
  const float franklin = width("Franklin Gothic Medium", false);
  if (franklin > 0) CHECK(width("Franklin Gothic Medium", true) < franklin - 1);
}

TEST(SlideRender_SymbolCharactersTakeTheRunsSymFont) {
  // A run with a:sym Wingdings: its U+F0FC (a tick) in Wingdings as PowerPoint draws it, the rest
  // in the run's own font (letters-home's "(SP)" after a tick).
  auto width = [](const std::string& rPr, const char* text) {
    auto deck = Modified(Slide1(PhShape(3, "<p:ph idx=\"1\"/>",
                                            "<a:p><a:pPr><a:buNone/></a:pPr><a:r>" + rPr + "<a:t>" + text + "</a:t></a:r></a:p>"),
                                2));
    if (!deck) return -1.f;
    fw::text::Fonts fonts;
    auto slide = SlideRender::Make(*deck, *deck->GetSlide(0), fonts);
    const std::vector<ParagraphInfo>& p = slide->Paragraphs();
    return p.empty() ? -1.f : p[0].bounds.right - p[0].bounds.left;
  };
  const std::string sym = "<a:rPr sz=\"4000\"><a:latin typeface=\"Arial\"/><a:sym typeface=\"Wingdings\"/></a:rPr>";
  const std::string wingdings = "<a:rPr sz=\"4000\"><a:latin typeface=\"Wingdings\"/></a:rPr>";
  const std::string arial = "<a:rPr sz=\"4000\"><a:latin typeface=\"Arial\"/></a:rPr>";
  const float tick = width(wingdings, "&#xF0FC;");
  CHECK(tick > 0);
  CHECK(std::fabs(width(sym, "&#xF0FC;") - tick) < 0.01f);
  CHECK(std::fabs(width(sym, "&#xF0FC;(SP)") - tick - width(arial, "(SP)")) < 0.5f);
}

TEST(SlideRender_TextReflectionIsDrawnUnderTheText) {
  // a:reflection on a run (rlf-webinar-iv's titles): the text mirrored under its baseline, fading out.
  auto render = [](bool reflect) -> std::shared_ptr<const draw::Bitmap> {
    const std::string effects = reflect ? "<a:effectLst><a:reflection blurRad=\"12700\" stA=\"48000\" endA=\"300\" endPos=\"55000\" "
                                          "dir=\"5400000\" sy=\"-90000\" algn=\"bl\" rotWithShape=\"0\"/></a:effectLst>"
                                        : "";
    auto deck = Modified(Slide1(PhShape(2, "<p:ph type=\"title\"/>",
                                            "<a:p><a:r><a:rPr sz=\"4000\"><a:solidFill><a:srgbClr val=\"000000\"/></a:solidFill>" + effects +
                                                "</a:rPr><a:t>HHHH</a:t></a:r></a:p>")));
    if (!deck) return nullptr;
    fw::text::Fonts fonts;
    auto slide = SlideRender::Make(*deck, *deck->GetSlide(0), fonts);
    return Render(*slide, 480, 360);
  };
  auto plain = render(false), reflected = render(true);
  REQUIRE(plain != nullptr && reflected != nullptr);
  // The plain text's lowest inked row; the reflection's pixels all lie below it, lighter than the text.
  auto dark = [](uint32_t c) { return ((c >> 16) & 0xFF) < 250; };
  int bottom = -1;
  for (int y = 0; y < plain->Height(); ++y)
    for (int x = 0; x < plain->Width(); ++x)
      if (dark(Pixel(*plain, x, y))) bottom = std::max(bottom, y);
  REQUIRE(bottom > 0);
  int below = 0, above = 0, darkest = 255;
  for (int y = 0; y < reflected->Height(); ++y)
    for (int x = 0; x < reflected->Width(); ++x) {
      const uint32_t c = Pixel(*reflected, x, y);
      if (dark(c) == dark(Pixel(*plain, x, y))) continue;
      if (y > bottom) ++below;
      else if (y < bottom - 1) ++above;  // the mirror starts on the baseline, which the text's last rows touch
      if (y > bottom + 1) darkest = std::min(darkest, static_cast<int>((c >> 16) & 0xFF));
    }
  CHECK(below > 40);
  CHECK(above == 0);
  CHECK(darkest > 90);  // stA 48 %: never as dark as the text
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

TEST(SlideRender_TightSpacingComesOffTheTop) {
  // 90 % of a 48-pt single line: the text rises the whole 4.8 pt (PowerPoint keeps the part below
  // the baseline), not the 3.7 pt of the line's ascent share.
  auto inkTop = [](const char* spacing) {
    auto deck = Modified(Slide1(PhShape(3, "<p:ph idx=\"1\"/>",
                                            std::string("<a:p><a:pPr><a:lnSpc><a:spcPct val=\"") + spacing +
                                                "\"/></a:lnSpc><a:buNone/></a:pPr><a:r><a:rPr sz=\"4000\"/><a:t>HHHH</a:t></a:r></a:p>"),
                                2));
    if (!deck) return -1;
    fw::text::Fonts fonts;
    auto slide = SlideRender::Make(*deck, *deck->GetSlide(0), fonts);
    const float scale = 4;  // pixels per point
    auto b = Render(*slide, static_cast<int>(slide->Size().width * scale), static_cast<int>(slide->Size().height * scale));
    if (!b) return -1;
    for (int y = 0; y < b->Height(); ++y)
      for (int x = 0; x < b->Width(); ++x)
        if ((Pixel(*b, x, y) & 0xFF) < 100) return y;
    return -1;
  };
  const int single = inkTop("100000"), tight = inkTop("90000");
  CHECK(single > 0 && tight > 0);
  CHECK(std::abs(single - tight - 19) <= 2);  // 4.8 pt at 4 px per point
}

TEST(SlideRender_WideSpacingPutsTheBaselineAtThreeQuartersOfTheLine) {
  // 150 % of a 48-pt single line (72 pt) in Arial: PowerPoint's baseline sits 54 pt down the line
  // whatever the font (era-systems' Trebuchet, govau's Public Sans), 15.1 pt below where it is at
  // single (Arial's ascent share of 48), not the ascent share of 72 pt (19.4 pt below).
  auto inkTop = [](const char* spacing) {
    auto deck = Modified(Slide1(PhShape(3, "<p:ph idx=\"1\"/>",
                                            std::string("<a:p><a:pPr><a:lnSpc><a:spcPct val=\"") + spacing +
                                                "\"/></a:lnSpc><a:buNone/></a:pPr><a:r><a:rPr sz=\"4000\"><a:latin "
                                                "typeface=\"Arial\"/></a:rPr><a:t>HHHH</a:t></a:r></a:p>"),
                                2));
    if (!deck) return -1;
    fw::text::Fonts fonts;
    auto slide = SlideRender::Make(*deck, *deck->GetSlide(0), fonts);
    const float scale = 4;  // pixels per point
    auto b = Render(*slide, static_cast<int>(slide->Size().width * scale), static_cast<int>(slide->Size().height * scale));
    if (!b) return -1;
    for (int y = 0; y < b->Height(); ++y)
      for (int x = 0; x < b->Width(); ++x)
        if ((Pixel(*b, x, y) & 0xFF) < 100) return y;
    return -1;
  };
  const int single = inkTop("100000"), wide = inkTop("150000");
  CHECK(single > 0 && wide > 0);
  CHECK(std::abs(wide - single - 60) <= 3);  // 15.1 pt at 4 px per point
}

TEST(SlideRender_ColumnsBreakWhereTheTextWouldPassTheBottom) {
  // Two columns in a box 100 points high, 20-point Arial lines 24 points apart. A line that starts
  // 77 points down stays in the first column: its text (Arial's 22.35 points of ascent and descent)
  // ends above the bottom, though its line passes it (ilo 24). An empty paragraph that starts above
  // the bottom stays too, so the second column starts with the text after it (ilo 18).
  auto columns = [](const std::vector<std::pair<std::string, int>>& paras) {
    std::string xml;
    for (const auto& [text, before] : paras)
      xml += "<a:p><a:pPr><a:lnSpc><a:spcPts val=\"2400\"/></a:lnSpc><a:spcBef><a:spcPts val=\"" + std::to_string(before * 100) +
             "\"/></a:spcBef><a:spcAft><a:spcPts val=\"0\"/></a:spcAft><a:buNone/></a:pPr>" +
             (text.empty() ? std::string() : "<a:r><a:rPr sz=\"2000\"><a:latin typeface=\"Arial\"/></a:rPr><a:t>" + text + "</a:t></a:r>") +
             "<a:endParaRPr sz=\"2000\"><a:latin typeface=\"Arial\"/></a:endParaRPr></a:p>";
    const std::string shape =
        "<p:sp><p:nvSpPr><p:cNvPr id=\"4\" name=\"T\"/><p:cNvSpPr txBox=\"1\"/><p:nvPr/></p:nvSpPr><p:spPr><a:xfrm><a:off x=\"0\" y=\"0\"/>"
        "<a:ext cx=\"" + std::to_string(400 * 12700) + "\" cy=\"" + std::to_string(100 * 12700) +
        "\"/></a:xfrm><a:prstGeom prst=\"rect\"/></p:spPr><p:txBody><a:bodyPr wrap=\"square\" lIns=\"0\" tIns=\"0\" rIns=\"0\" bIns=\"0\""
        " numCol=\"2\" spcCol=\"0\"><a:noAutofit/></a:bodyPr><a:lstStyle/>" + xml + "</p:txBody></p:sp>";
    std::vector<draw::Rect> out;
    auto deck = Modified(Slide1(shape, 2));
    if (!deck) return out;
    fw::text::Fonts fonts;
    auto slide = SlideRender::Make(*deck, *deck->GetSlide(0), fonts);
    out.resize(paras.size());
    for (const ParagraphInfo& p : slide->Paragraphs())
      if (p.shape == 4 && p.index >= 0 && static_cast<size_t>(p.index) < out.size()) out[p.index] = p.bounds;
    return out;
  };
  // "Four" 77 points down: in the first column; "Five" after it, at the second's top.
  std::vector<draw::Rect> b = columns({{"One", 0}, {"Two", 0}, {"Three", 0}, {"Four", 5}, {"Five", 0}});
  REQUIRE(b.size() == 5u);
  CHECK(b[3].left < 200 && b[4].left >= 200);
  CHECK(b[4].top < 5);
  // An empty paragraph 90 points down stays in the first column; "Next" starts the second.
  b = columns({{"One", 0}, {"Two", 0}, {"Three", 0}, {"", 18}, {"Next", 0}});
  REQUIRE(b.size() == 5u);
  CHECK(b[2].left < 200 && b[4].left >= 200);
  CHECK(b[4].top < 5);
}

TEST(SlideRender_BottomAnchorIgnoresWideSpacingBelow) {
  // Bottom-anchored text at 150 %: its last line ends where it would at single spacing (cpb's
  // title), the wider spacing's share below the line not counted.
  auto inkBottom = [](const char* spacing) {
    std::string shape = PhShape(3, "<p:ph idx=\"1\"/>",
                                std::string("<a:p><a:pPr><a:lnSpc><a:spcPct val=\"") + spacing +
                                    "\"/></a:lnSpc><a:buNone/></a:pPr><a:r><a:rPr sz=\"4000\"/><a:t>HHHH</a:t></a:r></a:p>");
    const std::string plain = "<a:bodyPr/>";
    shape.replace(shape.find(plain), plain.size(), "<a:bodyPr anchor=\"b\"><a:noAutofit/></a:bodyPr>");
    auto deck = Modified(Slide1(shape, 2));
    if (!deck) return -1;
    fw::text::Fonts fonts;
    auto slide = SlideRender::Make(*deck, *deck->GetSlide(0), fonts);
    const float scale = 4;
    auto b = Render(*slide, static_cast<int>(slide->Size().width * scale), static_cast<int>(slide->Size().height * scale));
    if (!b) return -1;
    for (int y = b->Height() - 1; y >= 0; --y)
      for (int x = 0; x < b->Width(); ++x)
        if ((Pixel(*b, x, y) & 0xFF) < 100) return y;
    return -1;
  };
  const int single = inkBottom("100000"), wide = inkBottom("150000");
  CHECK(single > 0 && wide > 0);
  CHECK(std::abs(single - wide) <= 2);
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
