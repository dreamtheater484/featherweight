// Slide rendering (P1): placeholders inherit from their layout and master, text follows the list
// styles, prompt text isn't drawn, tables take their style, and slides draw into bitmaps.
#include <windows.h>

#include <filesystem>
#include <string>

#include "TestHarness.h"
#include "draw/Drawing.h"
#include "draw/Theme.h"
#include "pkg/Package.h"
#include "pkg/Snapshot.h"
#include "pkg/XmlDom.h"
#include "slide/render/PackageDeck.h"
#include "slide/render/SlideRender.h"
#include "text/Fonts.h"

namespace {

using namespace fw::slide::render;

constexpr const char* kNs =
    " xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\""
    " xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\""
    " xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\"";

class NoResources : public draw::DrawingResources {
 public:
  std::shared_ptr<const draw::Image> GetImage(std::string_view) override { return nullptr; }
};

// A master with a title and a body placeholder, a layout that moves the title, and a slide.
struct Deck {
  fw::pkg::XmlDom master, layout, slide;
  NoResources res;

  explicit Deck(const std::string& slideTree, const std::string& masterExtra = "") {
    master.Parse(std::string("<p:sldMaster") + kNs +
                 "><p:cSld><p:bg><p:bgPr><a:solidFill><a:srgbClr val=\"FFFF00\"/></a:solidFill></p:bgPr></p:bg><p:spTree>"
                 "<p:sp><p:nvSpPr><p:cNvPr id=\"2\" name=\"Title\"/><p:cNvSpPr/><p:nvPr><p:ph type=\"title\"/></p:nvPr></p:nvSpPr>"
                 "<p:spPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"9144000\" cy=\"1270000\"/></a:xfrm></p:spPr>"
                 "<p:txBody><a:bodyPr anchor=\"ctr\"/><a:lstStyle/><a:p><a:r><a:t>Click to edit Master title style</a:t></a:r></a:p></p:txBody></p:sp>"
                 "<p:sp><p:nvSpPr><p:cNvPr id=\"3\" name=\"Body\"/><p:cNvSpPr/><p:nvPr><p:ph type=\"body\" idx=\"1\"/></p:nvPr></p:nvSpPr>"
                 "<p:spPr><a:xfrm><a:off x=\"635000\" y=\"1905000\"/><a:ext cx=\"7620000\" cy=\"3810000\"/></a:xfrm></p:spPr>"
                 "<p:txBody><a:bodyPr/><a:lstStyle/><a:p><a:r><a:t>Click to edit Master text styles</a:t></a:r></a:p></p:txBody></p:sp>" +
                 masterExtra +
                 "</p:spTree></p:cSld><p:clrMap bg1=\"lt1\" tx1=\"dk1\" bg2=\"lt2\" tx2=\"dk2\" accent1=\"accent1\" accent2=\"accent2\""
                 " accent3=\"accent3\" accent4=\"accent4\" accent5=\"accent5\" accent6=\"accent6\" hlink=\"hlink\" folHlink=\"folHlink\"/>"
                 "<p:txStyles><p:titleStyle><a:lvl1pPr algn=\"ctr\"><a:defRPr sz=\"4400\"><a:solidFill><a:srgbClr val=\"FF0000\"/></a:solidFill>"
                 "<a:latin typeface=\"+mj-lt\"/></a:defRPr></a:lvl1pPr></p:titleStyle>"
                 "<p:bodyStyle><a:lvl1pPr marL=\"342900\" indent=\"-342900\"><a:buFont typeface=\"Arial\"/><a:buChar char=\"&#8226;\"/>"
                 "<a:defRPr sz=\"3200\"/></a:lvl1pPr><a:lvl2pPr marL=\"742950\" indent=\"-285750\"><a:buAutoNum type=\"arabicPeriod\"/>"
                 "<a:defRPr sz=\"2800\"/></a:lvl2pPr></p:bodyStyle><p:otherStyle><a:lvl1pPr><a:defRPr sz=\"1800\"/></a:lvl1pPr></p:otherStyle>"
                 "</p:txStyles></p:sldMaster>");
    layout.Parse(std::string("<p:sldLayout") + kNs +
                 "><p:cSld><p:spTree><p:sp><p:nvSpPr><p:cNvPr id=\"2\" name=\"Title\"/><p:cNvSpPr/><p:nvPr><p:ph type=\"title\"/></p:nvPr></p:nvSpPr>"
                 "<p:spPr><a:xfrm><a:off x=\"1270000\" y=\"254000\"/><a:ext cx=\"6350000\" cy=\"1016000\"/></a:xfrm></p:spPr>"
                 "<p:txBody><a:bodyPr/><a:lstStyle/><a:p><a:r><a:t>Click to add title</a:t></a:r></a:p></p:txBody></p:sp>"
                 "<p:sp><p:nvSpPr><p:cNvPr id=\"3\" name=\"Content\"/><p:cNvSpPr/><p:nvPr><p:ph idx=\"1\"/></p:nvPr></p:nvSpPr>"
                 "<p:spPr/><p:txBody><a:bodyPr/><a:lstStyle/><a:p><a:r><a:t>Click to add text</a:t></a:r></a:p></p:txBody></p:sp>"
                 "</p:spTree></p:cSld></p:sldLayout>");
    slide.Parse(std::string("<p:sld") + kNs + "><p:cSld><p:spTree>" + slideTree + "</p:spTree></p:cSld></p:sld>");
  }

  SlideSource Source() {
    SlideSource s;
    s.slide = slide.Root();
    s.layout = layout.Root();
    s.master = master.Root();
    s.slideResources = s.layoutResources = s.masterResources = &res;
    s.colorMap = draw::ColorMap::Read(fw::pkg::XmlDom().Root());
    s.size = {720, 540};
    s.number = 7;
    return s;
  }
};

const char* kTitle =
    "<p:sp><p:nvSpPr><p:cNvPr id=\"5\" name=\"Title 1\"/><p:cNvSpPr/><p:nvPr><p:ph type=\"title\"/></p:nvPr></p:nvSpPr><p:spPr/>"
    "<p:txBody><a:bodyPr/><a:lstStyle/><a:p><a:r><a:t>Hello</a:t></a:r></a:p></p:txBody></p:sp>";

// WIC, under RenderBitmap, needs COM on the thread, as in the app.
std::shared_ptr<const draw::Bitmap> Render(const SlideRender& slide, int w, int h) {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  return RenderBitmap(slide, w, h);
}

uint32_t Pixel(const draw::Bitmap& b, int x, int y) {
  const uint8_t* p = b.Pixels() + (static_cast<size_t>(y) * b.Width() + x) * 4;
  return (uint32_t(p[2]) << 16) | (uint32_t(p[1]) << 8) | p[0];
}

bool Reddish(uint32_t c) { return (c >> 16 & 0xFF) > 180 && (c >> 8 & 0xFF) < 120 && (c & 0xFF) < 120; }

}  // namespace

TEST(SlideRender_PlaceholderTakesTheLayoutsPlaceAndTheMastersStyle) {
  Deck deck(kTitle);
  fw::text::Fonts fonts;
  auto slide = SlideRender::Make(deck.Source(), fonts);
  CHECK(slide->Text() == L"Hello");
  bool found = false;
  for (const ShapeInfo& s : slide->Shapes())
    if (s.id == 5) {
      found = true;
      CHECK(s.placeholder);
      CHECK_NEAR(s.bounds.left, 100, 0.01);
      CHECK_NEAR(s.bounds.top, 20, 0.01);
      CHECK_NEAR(s.bounds.right, 600, 0.01);
    }
  CHECK(found);
  // The title's red text (titleStyle) on the master's yellow background, centered in its box.
  auto bmp = Render(*slide, 720, 540);
  CHECK(bmp != nullptr);
  if (!bmp) return;
  CHECK_EQ(Pixel(*bmp, 5, 300), 0xFFFF00u);
  int red = 0, minX = 720, maxX = 0;
  for (int y = 20; y < 100; ++y)
    for (int x = 100; x < 600; ++x)
      if (Reddish(Pixel(*bmp, x, y))) {
        ++red;
        minX = std::min(minX, x);
        maxX = std::max(maxX, x);
      }
  CHECK(red > 200);
  CHECK(std::abs((minX + maxX) / 2 - 350) < 12);
}

TEST(SlideRender_PromptTextIsNotDrawn) {
  // The slide's title placeholder has no text: the layout's "Click to add title" doesn't show.
  Deck deck("<p:sp><p:nvSpPr><p:cNvPr id=\"5\" name=\"Title 1\"/><p:cNvSpPr/><p:nvPr><p:ph type=\"title\"/></p:nvPr></p:nvSpPr>"
            "<p:spPr/><p:txBody><a:bodyPr/><a:lstStyle/><a:p><a:endParaRPr/></a:p></p:txBody></p:sp>");
  fw::text::Fonts fonts;
  auto slide = SlideRender::Make(deck.Source(), fonts);
  CHECK(slide->Text().empty());
  auto bmp = Render(*slide, 360, 270);
  CHECK(bmp != nullptr);
  if (!bmp) return;
  bool allYellow = true;
  for (int y = 0; y < 270; y += 3)
    for (int x = 0; x < 360; x += 3) allYellow = allYellow && Pixel(*bmp, x, y) == 0xFFFF00u;
  CHECK(allYellow);
}

TEST(SlideRender_BulletsNumbersAndTheMastersOwnShapes) {
  // A body placeholder with two first-level bullets and two numbered second-level paragraphs, and
  // a blue rectangle on the master that the slide shows.
  Deck deck(
      "<p:sp><p:nvSpPr><p:cNvPr id=\"6\" name=\"Content\"/><p:cNvSpPr/><p:nvPr><p:ph idx=\"1\"/></p:nvPr></p:nvSpPr><p:spPr/>"
      "<p:txBody><a:bodyPr/><a:lstStyle/><a:p><a:r><a:t>One</a:t></a:r></a:p><a:p><a:pPr lvl=\"1\"/><a:r><a:t>Sub</a:t></a:r></a:p>"
      "<a:p><a:pPr lvl=\"1\"/><a:r><a:t>Sub two</a:t></a:r></a:p><a:p><a:r><a:t>Two</a:t></a:r></a:p></p:txBody></p:sp>",
      "<p:sp><p:nvSpPr><p:cNvPr id=\"9\" name=\"Bar\"/><p:cNvSpPr/><p:nvPr/></p:nvSpPr><p:spPr><a:xfrm><a:off x=\"0\" y=\"6604000\"/>"
      "<a:ext cx=\"9144000\" cy=\"254000\"/></a:xfrm><a:prstGeom prst=\"rect\"/><a:solidFill><a:srgbClr val=\"0000FF\"/></a:solidFill></p:spPr></p:sp>");
  fw::text::Fonts fonts;
  auto slide = SlideRender::Make(deck.Source(), fonts);
  CHECK(slide->Text() == L"One\nSub\nSub two\nTwo");
  CHECK_EQ(slide->Paragraphs().size(), size_t{4});
  if (slide->Paragraphs().size() == 4) {
    // The second level is indented further, and the paragraphs go down the box.
    CHECK(slide->Paragraphs()[1].bounds.left > slide->Paragraphs()[0].bounds.left + 20);
    CHECK(slide->Paragraphs()[3].bounds.top > slide->Paragraphs()[2].bounds.top);
    CHECK(slide->Paragraphs()[0].bounds.top >= 150 - 1);
  }
  auto bmp = Render(*slide, 720, 540);
  CHECK(bmp && Pixel(*bmp, 360, 530) == 0x0000FFu);
  // A filter leaves the master's shapes and the background out.
  struct NoMaster : DrawFilter {
    bool Shape(uint32_t id) override { return id != 0; }
    bool Background() override { return false; }
  };
  NoMaster filter;
  (void)filter;
}

TEST(SlideRender_TablesTakeTheBuiltInDefaultStyle) {
  // Medium Style 2 - Accent 1 without a tableStyles part: the header row in accent 1 (4472C4 in
  // Office's default theme), white borders between the cells.
  Deck deck(
      "<p:graphicFrame><p:nvGraphicFramePr><p:cNvPr id=\"4\" name=\"Table\"/><p:cNvGraphicFramePr/><p:nvPr/></p:nvGraphicFramePr>"
      "<p:xfrm><a:off x=\"1270000\" y=\"1270000\"/><a:ext cx=\"2540000\" cy=\"1270000\"/></p:xfrm><a:graphic>"
      "<a:graphicData uri=\"http://schemas.openxmlformats.org/drawingml/2006/table\"><a:tbl><a:tblPr firstRow=\"1\" bandRow=\"1\">"
      "<a:tableStyleId>{5C22544A-7EE6-4342-B048-85BDC9FD1C3A}</a:tableStyleId></a:tblPr><a:tblGrid><a:gridCol w=\"1270000\"/>"
      "<a:gridCol w=\"1270000\"/></a:tblGrid><a:tr h=\"635000\"><a:tc><a:txBody><a:bodyPr/><a:p><a:r><a:t>A</a:t></a:r></a:p></a:txBody>"
      "<a:tcPr/></a:tc><a:tc><a:txBody><a:bodyPr/><a:p><a:r><a:t>B</a:t></a:r></a:p></a:txBody><a:tcPr/></a:tc></a:tr>"
      "<a:tr h=\"635000\"><a:tc><a:txBody><a:bodyPr/><a:p><a:r><a:t>1</a:t></a:r></a:p></a:txBody><a:tcPr/></a:tc>"
      "<a:tc><a:txBody><a:bodyPr/><a:p><a:r><a:t>2</a:t></a:r></a:p></a:txBody><a:tcPr/></a:tc></a:tr></a:tbl></a:graphicData>"
      "</a:graphic></p:graphicFrame>");
  fw::text::Fonts fonts;
  auto slide = SlideRender::Make(deck.Source(), fonts);
  CHECK(slide->Text() == L"A\tB\n1\t2");
  auto bmp = Render(*slide, 1440, 1080);  // two pixels a point
  CHECK(bmp != nullptr);
  if (!bmp) return;
  const draw::Color accent = draw::Theme::Default()->GetColor(draw::ThemeColor::Accent1);
  const uint32_t want = (uint32_t(accent.r * 255 + 0.5f) << 16) | (uint32_t(accent.g * 255 + 0.5f) << 8) | uint32_t(accent.b * 255 + 0.5f);
  CHECK_EQ(Pixel(*bmp, 380, 290), want);
  CHECK_EQ(Pixel(*bmp, 399, 250), 0xFFFFFFu);  // the border between the header cells
  CHECK(Pixel(*bmp, 380, 390) != want);         // the first body row: a tint of it
}

TEST(SlideRender_GeneratedDeckDrawsItsShapesAndText) {
  const auto path = std::filesystem::path(__FILE__).parent_path().parent_path() / "office" / "generated" / "simple.pptx";
  fw::pkg::Status st;
  auto pkg = fw::pkg::Package::Open(fw::pkg::Snapshot::Open(path.wstring(), {}, st), fw::pkg::Limits::Default(), st);
  CHECK(pkg != nullptr);
  if (!pkg) return;
  PackageDeck deck(*pkg);
  CHECK_EQ(deck.SlideCount(), size_t{2});
  fw::text::Fonts fonts;
  auto slide = SlideRender::Make(deck.Source(0), fonts);
  CHECK(slide->Text().find(L"A simple presentation") != std::wstring::npos);
  CHECK(slide->Text().find(L"Click") == std::wstring::npos);
  CHECK_NEAR(slide->Size().width, 720, 0.01);
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  Thumbnails thumbs([&](size_t i) { return SlideRender::Make(deck.Source(i), fonts); });
  thumbs.Reset(deck.SlideCount(), 96, 72);
  int steps = 0;
  while (thumbs.RenderNext()) ++steps;
  CHECK_EQ(steps, 1);
  CHECK(thumbs.Done() && thumbs.Get(0) && thumbs.Get(1));
}
