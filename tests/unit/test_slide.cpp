// The presentation model and .pptx reader (src/slide, P1): the generated decks, and inheritance
// (slide → layout → master → theme) on decks made by replacing parts of simple.pptx.
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

using namespace fw::slide;

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

std::unique_ptr<Presentation> OpenBytes(std::vector<uint8_t> bytes, fw::pkg::Status* out = nullptr) {
  fw::pkg::Status st;
  auto pk = fw::pkg::Package::Open(fw::pkg::Snapshot::FromBytes(std::move(bytes)), fw::pkg::Limits::Default(), st);
  if (!pk) {
    if (out) *out = st;
    return nullptr;
  }
  auto deck = Read(std::move(pk), st);
  if (out) *out = st;
  return deck;
}

std::unique_ptr<Presentation> OpenGenerated(const char* name) { return OpenBytes(FileBytes(Generated() / name)); }

// simple.pptx with some parts replaced (or added); an empty text removes a part.
std::unique_ptr<Presentation> Modified(const std::map<std::string, std::string>& replace, fw::pkg::Status* st = nullptr) {
  fw::pkg::Status s;
  auto pk = fw::pkg::Package::Open(fw::pkg::Snapshot::FromBytes(FileBytes(Generated() / "simple.pptx")),
                                   fw::pkg::Limits::Default(), s);
  std::vector<std::pair<std::string, std::string>> entries;
  std::map<std::string, std::string> left = replace;
  for (const fw::pkg::Part& part : pk->Parts()) {
    std::string name = part.name.substr(1);
    auto it = left.find(name);
    if (it != left.end()) {
      if (!it->second.empty()) entries.emplace_back(name, it->second);
      left.erase(it);
      continue;
    }
    std::vector<uint8_t> bytes;
    pk->ReadPart(part.name, bytes);
    entries.emplace_back(name, test::Text(bytes));
  }
  for (auto& [name, xml] : left) entries.emplace_back(name, xml);
  return OpenBytes(test::MakeZip(entries), st);
}

constexpr const char* kNs =
    " xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\""
    " xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\""
    " xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\"";

std::string SlideXml(const std::string& shapes, const std::string& extra = "", const std::string& attrs = "") {
  return std::string("<p:sld") + kNs + attrs + "><p:cSld>" + extra +
         "<p:spTree><p:nvGrpSpPr><p:cNvPr id=\"1\" name=\"\"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr><p:grpSpPr/>" +
         shapes + "</p:spTree></p:cSld><p:clrMapOvr><a:masterClrMapping/></p:clrMapOvr></p:sld>";
}

std::string PhShape(int id, const std::string& ph, const std::string& body, const std::string& spPr = "") {
  return "<p:sp><p:nvSpPr><p:cNvPr id=\"" + std::to_string(id) + "\" name=\"P" + std::to_string(id) +
         "\"/><p:cNvSpPr/><p:nvPr>" + ph + "</p:nvPr></p:nvSpPr><p:spPr>" + spPr + "</p:spPr><p:txBody>" + body +
         "</p:txBody></p:sp>";
}

const char* kSlideRels =
    "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
    "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideLayout\""
    " Target=\"../slideLayouts/slideLayout1.xml\"/>"
    "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/hyperlink\""
    " Target=\"https://example.com/\" TargetMode=\"External\"/>"
    "<Relationship Id=\"rId3\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/slide\""
    " Target=\"slide2.xml\"/></Relationships>";

const TextBody* TextOf(Presentation& deck, const Slide& s, uint32_t id) {
  const Shape* shape = s.FindShape(id);
  return shape ? deck.Text(*shape, s) : nullptr;
}

bool Near(draw::Color c, float r, float g, float b) {
  return std::fabs(c.r - r) < 0.01f && std::fabs(c.g - g) < 0.01f && std::fabs(c.b - b) < 0.01f;
}

}  // namespace

TEST(SlideOpensGeneratedDecks) {
  for (const char* name : {"simple.pptx", "layouts.pptx", "shapes-table-chart.pptx"}) {
    auto deck = OpenGenerated(name);
    CHECK(deck != nullptr);
    if (!deck) continue;
    deck->LoadAll();
    CHECK(deck->SlideCount() > 0);
    CHECK_EQ(deck->Masters().size(), size_t{1});
    CHECK_EQ(deck->Layouts().size(), size_t{11});
    for (size_t i = 0; i < deck->SlideCount(); ++i) {
      const Slide* s = deck->GetSlide(i);
      CHECK(s && s->layout && s->master && s->theme);
      CHECK(s->background);  // the master's bgRef
      for (const DrawItem& item : deck->DrawList(*s)) {
        CHECK(item.shape->drawNode);
        if (item.shape->textNode) CHECK(deck->Text(*item.shape, *s) != nullptr);
      }
    }
    CHECK(deck->Problems().empty());
    CHECK(deck->Format() == FileFormat::Pptx);
  }
}

TEST(SlideSimpleDeckInherits) {
  auto deck = OpenGenerated("simple.pptx");
  CHECK(deck != nullptr);
  if (!deck) return;
  CHECK_NEAR(deck->SlideSize().width, 720.f, 0.01f);
  CHECK_NEAR(deck->SlideSize().height, 540.f, 0.01f);
  CHECK_EQ(deck->SlideCount(), size_t{2});
  const Slide* s1 = deck->GetSlide(0);
  CHECK_EQ(s1->number, 1);
  // The centered title takes the layout's position and the master's title style.
  const Shape* title = s1->FindPlaceholder(PlaceholderType::CenteredTitle);
  CHECK(title && title->inherits);
  CHECK_NEAR(title->bounds.left, 54.f, 0.01f);
  CHECK_NEAR(title->bounds.top, 2130425 / 12700.f, 0.01f);
  const TextBody* t = deck->Text(*title, *s1);
  CHECK(t && t->paragraphs.size() == 1);
  CHECK(t->paragraphs[0].runs[0].text == L"A simple presentation");
  CHECK_NEAR(t->paragraphs[0].runs[0].props.size, 44.f, 0.01f);
  CHECK(t->paragraphs[0].runs[0].props.latin == L"Calibri");
  CHECK(t->paragraphs[0].props.align == TextAlign::Center);
  CHECK(Near(t->paragraphs[0].runs[0].props.color, 0, 0, 0));

  // Body levels from the master's body style.
  const Slide* s2 = deck->GetSlide(1);
  const Shape* body = s2->FindShape(3);
  CHECK(body && body->placeholder.type == PlaceholderType::Object && body->inherits);
  CHECK_NEAR(body->bounds.top, 1600200 / 12700.f, 0.01f);
  const TextBody* b = deck->Text(*body, *s2);
  CHECK(b && b->paragraphs.size() == 4);
  CHECK_NEAR(b->paragraphs[0].runs[0].props.size, 32.f, 0.01f);
  CHECK(b->paragraphs[0].props.bullet.kind == Bullet::Kind::Char);
  CHECK(b->paragraphs[0].props.bullet.character == L"•");
  CHECK_NEAR(b->paragraphs[0].props.marginLeft, 27.f, 0.01f);
  CHECK_NEAR(b->paragraphs[0].props.indent, -27.f, 0.01f);
  CHECK_EQ(b->paragraphs[1].props.level, 1);
  CHECK_NEAR(b->paragraphs[1].runs[0].props.size, 28.f, 0.01f);
  CHECK(b->paragraphs[1].props.bullet.character == L"–");
  CHECK(b->paragraphs[0].props.spaceBefore.percent);
  CHECK_NEAR(b->paragraphs[0].props.spaceBefore.value, 0.2f, 0.001f);
  // Notes.
  CHECK(s2->notes != nullptr);
  CHECK(s2->NotesText() == L"Speaker notes for the bullet slide.");
  CHECK(deck->NotesMaster() != nullptr);
  // The master's own shapes aren't placeholders here, so only the slide's are drawn.
  CHECK_EQ(deck->DrawList(*s2).size(), s2->shapes.size());
}

TEST(SlideShapesDeck) {
  auto deck = OpenGenerated("shapes-table-chart.pptx");
  CHECK(deck != nullptr);
  if (!deck) return;
  const Slide* s = deck->GetSlide(0);
  CHECK_EQ(s->shapes.size(), size_t{4});
  CHECK(s->shapes[0].kind == Shape::Kind::Shape);
  CHECK(s->shapes[3].kind == Shape::Kind::Picture);
  CHECK(s->shapes[2].textBox);
  CHECK_NEAR(s->shapes[1].rotation, 15.f, 0.001f);
  // The rounded rectangle's text is white from its style's fontRef.
  const TextBody* t = TextOf(*deck, *s, 2);
  CHECK(t && Near(t->paragraphs[0].runs[0].props.color, 1, 1, 1));
  // The text box's link.
  const TextBody* link = TextOf(*deck, *s, 4);
  CHECK(link && link->paragraphs[0].runs[0].props.click.action == Hyperlink::Action::Url);
  CHECK_NEAR(link->paragraphs[0].runs[0].props.size, 20.f, 0.01f);
  CHECK(link->body.autofit == BodyProps::Autofit::ResizeShape);
  CHECK(!link->body.wrap);
  CHECK(deck->GetSlide(1)->shapes[0].kind == Shape::Kind::Table);
  CHECK(deck->GetSlide(2)->shapes[0].kind == Shape::Kind::Chart);
  // fwdraw draws them.
  for (size_t i = 0; i < deck->SlideCount(); ++i) {
    const Slide* slide = deck->GetSlide(i);
    for (const DrawItem& item : deck->DrawList(*slide)) {
      auto res = deck->Resources(*item.owner, *slide);
      auto d = draw::Drawing::FromShapeTree(item.shape->drawNode, *res);
      CHECK(d != nullptr);
      if (!d) continue;
      for (const draw::TextBox& box : d->TextBoxes()) CHECK(item.owner->ShapeOfText(box.content) != nullptr);
    }
  }
  auto res = deck->Resources(*s->backgroundOwner, *s);
  CHECK(draw::Drawing::FromShapeTree(s->background, *res) != nullptr);
}

TEST(SlideListStyleChain) {
  // Layout 1's centered title: its list style makes the title right-aligned and 50 pt; the slide's
  // list style makes it bold; the run is italic.
  std::string layout =
      std::string("<p:sldLayout") + kNs + " type=\"title\"><p:cSld name=\"Title Slide\"><p:spTree>"
      "<p:nvGrpSpPr><p:cNvPr id=\"1\" name=\"\"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr><p:grpSpPr/>" +
      PhShape(2, "<p:ph type=\"ctrTitle\"/>",
                  "<a:bodyPr anchor=\"b\"/><a:lstStyle><a:lvl1pPr algn=\"r\"><a:defRPr sz=\"5000\"/></a:lvl1pPr></a:lstStyle><a:p/>",
                  "<a:xfrm><a:off x=\"127000\" y=\"254000\"/><a:ext cx=\"1270000\" cy=\"635000\"/></a:xfrm>") +
      PhShape(3, "<p:ph sz=\"half\" idx=\"2\"/>", "<a:bodyPr/><a:lstStyle/><a:p/>") +
      "</p:spTree></p:cSld><p:clrMapOvr><a:masterClrMapping/></p:clrMapOvr></p:sldLayout>";
  std::string slide = SlideXml(
      PhShape(2, "<p:ph type=\"ctrTitle\"/>",
                  "<a:bodyPr lIns=\"0\"/><a:lstStyle><a:lvl1pPr><a:defRPr b=\"1\"/></a:lvl1pPr></a:lstStyle>"
                  "<a:p><a:r><a:rPr i=\"1\"/><a:t>Title</a:t></a:r></a:p>") +
      PhShape(3, "<p:ph sz=\"half\" idx=\"2\"/>", "<a:bodyPr/><a:lstStyle/><a:p><a:r><a:t>Second</a:t></a:r></a:p>"));
  auto deck = Modified({{"ppt/slideLayouts/slideLayout1.xml", layout}, {"ppt/slides/slide1.xml", slide},
                        {"ppt/slides/_rels/slide1.xml.rels", kSlideRels}});
  CHECK(deck != nullptr);
  if (!deck) return;
  const Slide* s = deck->GetSlide(0);
  const TextBody* t = TextOf(*deck, *s, 2);
  CHECK(t != nullptr);
  if (!t) return;
  const RunProps& r = t->paragraphs[0].runs[0].props;
  CHECK_NEAR(r.size, 50.f, 0.01f);
  CHECK(r.bold && r.italic);
  CHECK(t->paragraphs[0].props.align == TextAlign::Right);
  // Body properties merge attribute by attribute.
  CHECK(t->body.anchor == BodyProps::Anchor::Bottom);
  CHECK_NEAR(t->body.left, 0.f, 0.001f);
  CHECK_NEAR(t->body.right, 7.2f, 0.001f);
  // Its position is the layout's.
  CHECK_NEAR(s->FindShape(2)->bounds.left, 10.f, 0.01f);
  CHECK_NEAR(s->FindShape(2)->bounds.bottom, 70.f, 0.01f);
  // A second content placeholder (idx 2) inherits from the master's body, not from its date
  // (also idx 2): 32 pt with bullets, not 12 pt.
  const Shape* second = s->FindShape(3);
  CHECK(second && second->inherits && second->inherits->inherits);
  CHECK(second->inherits->inherits->placeholder.type == PlaceholderType::Body);
  const TextBody* b = TextOf(*deck, *s, 3);
  CHECK(b && std::fabs(b->paragraphs[0].runs[0].props.size - 32.f) < 0.01f);
  CHECK(b && b->paragraphs[0].props.bullet.kind == Bullet::Kind::Char);
}

TEST(SlideColorMapOverride) {
  std::string slide = std::string("<p:sld") + kNs +
                      "><p:cSld><p:spTree><p:nvGrpSpPr><p:cNvPr id=\"1\" name=\"\"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>"
                      "<p:grpSpPr/>" +
                      PhShape(2, "<p:ph type=\"ctrTitle\"/>", "<a:bodyPr/><a:lstStyle/><a:p><a:r><a:t>T</a:t></a:r></a:p>") +
                      "</p:spTree></p:cSld><p:clrMapOvr><a:overrideClrMapping bg1=\"dk1\" tx1=\"lt1\" bg2=\"dk2\" "
                      "tx2=\"lt2\" accent1=\"accent1\" accent2=\"accent2\" accent3=\"accent3\" accent4=\"accent4\" "
                      "accent5=\"accent5\" accent6=\"accent6\" hlink=\"hlink\" folHlink=\"folHlink\"/></p:clrMapOvr></p:sld>";
  auto deck = Modified({{"ppt/slides/slide1.xml", slide}, {"ppt/slides/_rels/slide1.xml.rels", kSlideRels}});
  CHECK(deck != nullptr);
  if (!deck) return;
  const Slide* s = deck->GetSlide(0);
  const TextBody* t = TextOf(*deck, *s, 2);
  CHECK(t && Near(t->paragraphs[0].runs[0].props.color, 1, 1, 1));  // tx1 is now light 1
  // The second slide keeps the master's mapping.
  const TextBody* t2 = TextOf(*deck, *deck->GetSlide(1), 2);
  CHECK(t2 && Near(t2->paragraphs[0].runs[0].props.color, 0, 0, 0));
}

TEST(SlideBackgrounds) {
  std::string own = SlideXml("", "<p:bg><p:bgPr><a:solidFill><a:srgbClr val=\"FF0000\"/></a:solidFill><a:effectLst/></p:bgPr></p:bg>");
  auto deck = Modified({{"ppt/slides/slide1.xml", own}, {"ppt/slides/_rels/slide1.xml.rels", kSlideRels}});
  CHECK(deck != nullptr);
  if (!deck) return;
  const Slide* s1 = deck->GetSlide(0);
  CHECK(s1->background && s1->backgroundOwner == s1);
  const Slide* s2 = deck->GetSlide(1);
  CHECK(s2->background && s2->backgroundOwner == s2->master);
  // Both draw as a rectangle filling the slide.
  for (const Slide* s : {s1, s2}) {
    auto res = deck->Resources(*s->backgroundOwner, *s);
    auto d = draw::Drawing::FromShapeTree(s->background, *res);
    CHECK(d && std::fabs(d->Bounds().Width() - 720.f) < 0.01f);
  }
}

TEST(SlideFontRefAndNumbering) {
  std::string shape =
      "<p:sp><p:nvSpPr><p:cNvPr id=\"5\" name=\"S\"/><p:cNvSpPr/><p:nvPr/></p:nvSpPr><p:spPr><a:xfrm><a:off x=\"0\" "
      "y=\"0\"/><a:ext cx=\"100\" cy=\"100\"/></a:xfrm><a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></p:spPr>"
      "<p:style><a:lnRef idx=\"1\"><a:schemeClr val=\"accent1\"/></a:lnRef><a:fillRef idx=\"3\"><a:schemeClr "
      "val=\"accent1\"/></a:fillRef><a:effectRef idx=\"2\"><a:schemeClr val=\"accent1\"/></a:effectRef><a:fontRef "
      "idx=\"major\"><a:schemeClr val=\"accent2\"/></a:fontRef></p:style><p:txBody><a:bodyPr/><a:lstStyle/>"
      "<a:p><a:pPr><a:buAutoNum type=\"arabicPeriod\"/></a:pPr><a:r><a:t>one</a:t></a:r></a:p>"
      "<a:p><a:pPr lvl=\"1\"><a:buAutoNum type=\"alphaLcParenR\"/></a:pPr><a:r><a:t>a</a:t></a:r></a:p>"
      "<a:p><a:pPr><a:buAutoNum type=\"arabicPeriod\"/></a:pPr><a:r><a:t>two</a:t></a:r></a:p>"
      "<a:p><a:pPr><a:buNone/></a:pPr><a:r><a:t>break</a:t></a:r></a:p>"
      "<a:p><a:pPr><a:buAutoNum type=\"arabicPeriod\" startAt=\"5\"/></a:pPr><a:r><a:t>five</a:t></a:r></a:p>"
      "</p:txBody></p:sp>";
  auto deck = Modified({{"ppt/slides/slide1.xml", SlideXml(shape)}, {"ppt/slides/_rels/slide1.xml.rels", kSlideRels}});
  CHECK(deck != nullptr);
  if (!deck) return;
  const TextBody* t = TextOf(*deck, *deck->GetSlide(0), 5);
  CHECK(t && t->paragraphs.size() == 5);
  if (!t || t->paragraphs.size() != 5) return;
  CHECK(Near(t->paragraphs[0].runs[0].props.color, 0xC0 / 255.f, 0x50 / 255.f, 0x4D / 255.f));
  CHECK(t->paragraphs[0].runs[0].props.latin == L"Calibri");
  CHECK_EQ(t->paragraphs[0].number, 1);
  CHECK_EQ(t->paragraphs[1].number, 1);
  CHECK_EQ(t->paragraphs[2].number, 2);
  CHECK_EQ(t->paragraphs[3].number, 0);
  CHECK_EQ(t->paragraphs[4].number, 5);
  // A text box (no placeholder, no style): the default text style, 18 pt.
  CHECK_NEAR(t->paragraphs[0].runs[0].props.size, 18.f, 0.01f);
}

TEST(SlideHyperlinks) {
  std::string shapes =
      "<p:sp><p:nvSpPr><p:cNvPr id=\"5\" name=\"A\"><a:hlinkClick r:id=\"\" "
      "action=\"ppaction://hlinkshowjump?jump=nextslide\"/></p:cNvPr><p:cNvSpPr/><p:nvPr/></p:nvSpPr><p:spPr/></p:sp>"
      "<p:sp><p:nvSpPr><p:cNvPr id=\"6\" name=\"B\"><a:hlinkClick r:id=\"rId3\" action=\"ppaction://hlinksldjump\"/>"
      "<a:hlinkHover r:id=\"rId2\"/></p:cNvPr><p:cNvSpPr/><p:nvPr/></p:nvSpPr><p:spPr/></p:sp>"
      "<p:grpSp><p:nvGrpSpPr><p:cNvPr id=\"7\" name=\"G\"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr><p:grpSpPr/>"
      "<p:sp><p:nvSpPr><p:cNvPr id=\"8\" name=\"C\"><a:hlinkClick r:id=\"\" action=\"ppaction://customshow?id=3&amp;return=true\"/>"
      "</p:cNvPr><p:cNvSpPr/><p:nvPr/></p:nvSpPr><p:spPr/></p:sp>"
      "<p:sp><p:nvSpPr><p:cNvPr id=\"9\" name=\"D\"><a:hlinkClick r:id=\"\" action=\"ppaction://macro?name=Evil\"/>"
      "</p:cNvPr><p:cNvSpPr/><p:nvPr/></p:nvSpPr><p:spPr/></p:sp></p:grpSp>";
  auto deck = Modified({{"ppt/slides/slide1.xml", SlideXml(shapes, "", " show=\"0\"")},
                        {"ppt/slides/_rels/slide1.xml.rels", kSlideRels}});
  CHECK(deck != nullptr);
  if (!deck) return;
  const Slide* s = deck->GetSlide(0);
  CHECK(s->hidden);
  CHECK(!deck->GetSlide(1)->hidden);
  CHECK(s->FindShape(5)->click.action == Hyperlink::Action::NextSlide);
  CHECK(s->FindShape(6)->click.action == Hyperlink::Action::Slide);
  CHECK_EQ(s->FindShape(6)->click.slide, 1);
  CHECK(s->FindShape(6)->hover.action == Hyperlink::Action::Url);
  CHECK(s->FindShape(6)->hover.url == "https://example.com/");
  CHECK(s->FindShape(8)->click.action == Hyperlink::Action::CustomShow);
  CHECK_EQ(s->FindShape(8)->click.customShow, uint32_t{3});
  CHECK(s->FindShape(8)->click.returns);
  CHECK(s->FindShape(9)->click.action == Hyperlink::Action::Macro);
  CHECK(s->FindShape(7)->kind == Shape::Kind::Group && s->FindShape(7)->children.size() == 2);
}

TEST(SlideSectionsShowsAndFormats) {
  std::string pres =
      std::string("<p:presentation") + kNs +
      " firstSlideNum=\"0\"><p:sldMasterIdLst><p:sldMasterId id=\"2147483648\" r:id=\"rId1\"/></p:sldMasterIdLst>"
      "<p:notesMasterIdLst><p:notesMasterId r:id=\"rId9\"/></p:notesMasterIdLst>"
      "<p:sldIdLst><p:sldId id=\"256\" r:id=\"rId7\"/><p:sldId id=\"257\" r:id=\"rId8\"/></p:sldIdLst>"
      "<p:sldSz cx=\"12192000\" cy=\"6858000\"/><p:notesSz cx=\"6858000\" cy=\"9144000\"/>"
      "<p:custShowLst><p:custShow name=\"Short\" id=\"0\"><p:sldLst><p:sld r:id=\"rId8\"/></p:sldLst></p:custShow></p:custShowLst>"
      "<p:extLst><p:ext uri=\"{521415D9-36F7-43E2-AB2F-B90AF26B5E84}\"><p14:sectionLst "
      "xmlns:p14=\"http://schemas.microsoft.com/office/powerpoint/2010/main\"><p14:section name=\"One\" "
      "id=\"{1}\"><p14:sldIdLst><p14:sldId id=\"256\"/></p14:sldIdLst></p14:section><p14:section name=\"Two\" "
      "id=\"{2}\"><p14:sldIdLst><p14:sldId id=\"257\"/></p14:sldIdLst></p14:section></p14:sectionLst></p:ext></p:extLst>"
      "</p:presentation>";
  auto bytes = FileBytes(Generated() / "simple.pptx");
  fw::pkg::Status st;
  auto pk = fw::pkg::Package::Open(fw::pkg::Snapshot::FromBytes(bytes), fw::pkg::Limits::Default(), st);
  std::string types;
  {
    std::vector<uint8_t> b;
    pk->ReadPart("/[Content_Types].xml", b);
    types = test::Text(b);
    std::string from = "presentationml.presentation.main+xml";
    types.replace(types.find(from), from.size(), "presentationml.slideshow.main+xml");
  }
  auto deck = Modified({{"ppt/presentation.xml", pres}, {"[Content_Types].xml", types}});
  CHECK(deck != nullptr);
  if (!deck) return;
  CHECK(deck->StartsInShow());
  CHECK(deck->Format() == FileFormat::Ppsx);
  CHECK_NEAR(deck->SlideSize().width, 960.f, 0.01f);
  CHECK_EQ(deck->FirstSlideNumber(), 0);
  CHECK_EQ(deck->GetSlide(1)->number, 1);
  CHECK_EQ(deck->Sections().size(), size_t{2});
  CHECK(deck->Sections()[1].name == "Two" && deck->Sections()[1].slides == std::vector<size_t>{1});
  CHECK_EQ(deck->CustomShows().size(), size_t{1});
  CHECK(deck->CustomShows()[0].slides == std::vector<size_t>{1});
  CHECK_EQ(deck->SlideIndexOfId(257), 1);
}

TEST(SlideDamagedPartsAreSkipped) {
  // A slide that isn't well-formed is an empty slide; a slide naming no layout uses the first.
  auto deck = Modified({{"ppt/slides/slide1.xml", "<p:sld"}, {"ppt/slides/_rels/slide2.xml.rels", ""}});
  CHECK(deck != nullptr);
  if (!deck) return;
  const Slide* s1 = deck->GetSlide(0);
  CHECK(s1 && s1->shapes.empty() && s1->layout);
  const Slide* s2 = deck->GetSlide(1);
  CHECK(s2 && s2->layout && !s2->shapes.empty() && !s2->notes);
  CHECK(!deck->Problems().empty());
  // Not a presentation at all.
  fw::pkg::Status st;
  auto none = Modified({{"ppt/presentation.xml", "<x/>"}}, &st);
  CHECK(none == nullptr && !st.Ok());
}

TEST(SlideTableStyleApplied) {
  // shapes-table-chart.pptx names Medium Style 2 - Accent 1 without defining it, with a header row
  // and banded rows.
  auto deck = OpenGenerated("shapes-table-chart.pptx");
  CHECK(deck != nullptr);
  if (!deck) return;
  const Slide* s = deck->GetSlide(1);
  const Table* t = deck->GetTable(s->shapes[0], *s);
  CHECK(t != nullptr);
  if (!t) return;
  CHECK_EQ(t->rows.size(), size_t{4});
  CHECK_EQ(t->columns.size(), size_t{3});
  CHECK_NEAR(t->columns[0], 144.f, 0.01f);
  CHECK_NEAR(t->rows[0], 36.f, 0.01f);
  const TableCell* head = t->Cell(0, 1);
  CHECK(head && head->fill.kind == CellFill::Kind::Solid && Near(head->fill.color, 0x4F / 255.f, 0x81 / 255.f, 0xBD / 255.f));
  CHECK(head && head->text && head->text->paragraphs[0].runs[0].props.bold);
  CHECK(head && Near(head->text->paragraphs[0].runs[0].props.color, 1, 1, 1));
  CHECK(head && head->borders[TableCell::Bottom].visible && std::fabs(head->borders[TableCell::Bottom].width - 3.f) < 0.01f);
  CHECK(head && head->borders[TableCell::Left].visible && std::fabs(head->borders[TableCell::Left].width - 1.f) < 0.01f);
  const TableCell* band1 = t->Cell(1, 0);
  const TableCell* band2 = t->Cell(2, 0);
  CHECK(band1 && band2 && band1->fill.kind == CellFill::Kind::Solid && band2->fill.kind == CellFill::Kind::Solid);
  // Band 1 is the 40 % tint, the others the table's 20 %: lighter.
  CHECK(band1 && band2 && band2->fill.color.r > band1->fill.color.r && band1->fill.color.r > 0x4F / 255.f);
  CHECK(band1 && band1->text && !band1->text->paragraphs[0].runs[0].props.bold);
  CHECK(band1 && Near(band1->text->paragraphs[0].runs[0].props.color, 0, 0, 0));
  CHECK(band1 && band1->text->paragraphs[0].runs[0].text == L"R2C1");
  CHECK(deck->GetTable(deck->GetSlide(0)->shapes[0], *s) == nullptr);  // not a table
}

TEST(SlideTableSpansAndOwnProperties) {
  std::string frame =
      "<p:graphicFrame><p:nvGraphicFramePr><p:cNvPr id=\"4\" name=\"T\"/><p:cNvGraphicFramePr/><p:nvPr/>"
      "</p:nvGraphicFramePr><p:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"254000\" cy=\"254000\"/></p:xfrm><a:graphic>"
      "<a:graphicData uri=\"http://schemas.openxmlformats.org/drawingml/2006/table\"><a:tbl><a:tblPr/><a:tblGrid>"
      "<a:gridCol w=\"127000\"/><a:gridCol w=\"127000\"/></a:tblGrid>"
      "<a:tr h=\"127000\"><a:tc gridSpan=\"2\"><a:txBody><a:bodyPr/><a:p><a:r><a:t>wide</a:t></a:r></a:p></a:txBody>"
      "<a:tcPr marL=\"0\" anchor=\"ctr\"><a:lnB w=\"25400\"><a:solidFill><a:srgbClr val=\"00FF00\"/></a:solidFill>"
      "<a:prstDash val=\"dash\"/></a:lnB><a:solidFill><a:srgbClr val=\"FF0000\"/></a:solidFill></a:tcPr></a:tc>"
      "<a:tc hMerge=\"1\"><a:txBody><a:bodyPr/><a:p/></a:txBody><a:tcPr/></a:tc></a:tr>"
      "<a:tr h=\"127000\"><a:tc><a:txBody><a:bodyPr/><a:p/></a:txBody><a:tcPr/></a:tc></a:tr>"
      "</a:tbl></a:graphicData></a:graphic></p:graphicFrame>";
  auto deck = Modified({{"ppt/slides/slide1.xml", SlideXml(frame)}, {"ppt/slides/_rels/slide1.xml.rels", kSlideRels}});
  CHECK(deck != nullptr);
  if (!deck) return;
  const Slide* s = deck->GetSlide(0);
  const Table* t = deck->GetTable(*s->FindShape(4), *s);
  CHECK(t && t->cells.size() == 4);
  if (!t || t->cells.size() != 4) return;
  const TableCell* wide = t->Cell(0, 0);
  CHECK(wide->columnSpan == 2 && !wide->covered && t->Cell(0, 1)->covered);
  CHECK(wide->fill.kind == CellFill::Kind::Solid && Near(wide->fill.color, 1, 0, 0));
  CHECK(wide->borders[TableCell::Bottom].visible && wide->borders[TableCell::Bottom].dash == "dash");
  CHECK_NEAR(wide->borders[TableCell::Bottom].width, 2.f, 0.01f);
  CHECK_NEAR(wide->marginLeft, 0.f, 0.001f);
  CHECK(wide->anchor == BodyProps::Anchor::Middle);
  CHECK(!wide->borders[TableCell::Top].visible);  // no style: no borders
  CHECK(t->Cell(1, 1) && !t->Cell(1, 1)->node);   // a missing cell is an empty one
  CHECK(wide->text && wide->text->paragraphs[0].runs[0].text == L"wide");
}
