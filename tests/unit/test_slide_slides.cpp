// Slides in the presentation tab as an editor (P2, p2-slides): the thumbnails' selection, where a
// drag drops and the order a move gives, caches following an edit, slide sizes and how content
// scales, theme colors and themes, backgrounds, and slide numbers, footer and date.

#include <pugixml.hpp>

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "TestHarness.h"
#include "draw/Theme.h"
#include "pkg/Package.h"
#include "pkg/Snapshot.h"
#include "slide/Presentation.h"
#include "slide/edit/Edit.h"
#include "slide/edit/Save.h"
#include "slide/edit/SlideDesignOps.h"
#include "slide/view/NewDeck.h"
#include "slide/view/SlideDesign.h"
#include "slide/view/SlideHeaderFooter.h"
#include "slide/view/SlideOrder.h"

using namespace fw::slide::view;

namespace {

std::vector<ThumbRow> Rows(size_t n) {
  std::vector<ThumbRow> rows;
  for (size_t i = 0; i < n; ++i) {
    ThumbRow r;
    const float top = 10 + 100.0f * static_cast<float>(i);
    r.row = {0, top, 200, top + 90};
    r.slide = {30, top + 5, 190, top + 85};
    r.number = {0, top + 5, 25, top + 25};
    rows.push_back(r);
  }
  return rows;
}

// The fragment parsed with the three prefixes declared, as it would sit in a part.
pugi::xml_node Parse(pugi::xml_document& doc, const std::string& xml) {
  const std::string wrapped =
      "<w xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" "
      "xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\" "
      "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\">" +
      xml + "</w>";
  if (!doc.load_string(wrapped.c_str())) return {};
  return doc.first_child().first_child();
}

std::unique_ptr<fw::slide::Presentation> OpenDeck(const char* name) {
  const std::wstring path =
      (std::filesystem::path(__FILE__).parent_path().parent_path() / "office" / "presentations" / name).wstring();
  fw::pkg::Status status;
  auto snap = fw::pkg::Snapshot::Open(path, {}, status);
  if (!snap) return nullptr;
  auto package = fw::pkg::Package::Open(std::move(snap), fw::pkg::Limits::Default(), status);
  return package ? fw::slide::Read(std::move(package), status) : nullptr;
}

}  // namespace

// ---------------------------------------------------------------------------
// Selection

TEST(SlideSelection_ClickCtrlShift) {
  SlideSelection s;
  s.Reset(10, 2);
  CHECK_EQ(s.Indices(), (std::vector<size_t>{2}));
  // A plain click: that slide alone.
  CHECK_EQ(s.Click(5, false, false), 5u);
  CHECK_EQ(s.Indices(), (std::vector<size_t>{5}));
  // Ctrl+click adds, and shows the slide added.
  CHECK_EQ(s.Click(7, true, false), 7u);
  CHECK_EQ(s.Indices(), (std::vector<size_t>{5, 7}));
  CHECK(s.Many());
  // Ctrl+click on the shown one removes it and shows the nearest selected.
  CHECK_EQ(s.Click(7, true, false), 5u);
  CHECK_EQ(s.Indices(), (std::vector<size_t>{5}));
  // The last selected slide stays.
  CHECK_EQ(s.Click(5, true, false), 5u);
  CHECK_EQ(s.SelectedCount(), 1u);
  // Shift+click: the run from the anchor, either way; the anchor stays.
  s.Click(3, false, false);
  CHECK_EQ(s.Click(6, false, true), 6u);
  CHECK_EQ(s.Indices(), (std::vector<size_t>{3, 4, 5, 6}));
  s.Click(1, false, true);
  CHECK_EQ(s.Indices(), (std::vector<size_t>{1, 2, 3}));
  // Ctrl+Shift adds a run to what's selected.
  s.Click(8, true, false);   // anchor 8
  s.Click(9, true, true);
  CHECK_EQ(s.Indices(), (std::vector<size_t>{1, 2, 3, 8, 9}));
  s.SelectAll();
  CHECK_EQ(s.SelectedCount(), 10u);
  // Out of range clicks change nothing.
  CHECK_EQ(s.Click(42, false, false), 9u);
  CHECK_EQ(s.SelectedCount(), 10u);
}

TEST(SlideSelection_SetAndEmpty) {
  SlideSelection s;
  s.Reset(0, 0);
  CHECK_EQ(s.SelectedCount(), 0u);
  CHECK_EQ(s.Click(0, false, false), 0u);
  s.Reset(4, 9);  // clamped
  CHECK_EQ(s.Current(), 3u);
  s.Set({0, 2, 7}, 2);
  CHECK_EQ(s.Indices(), (std::vector<size_t>{0, 2}));
  CHECK_EQ(s.Current(), 2u);
  s.Set({}, 1);  // the current slide is always selected
  CHECK_EQ(s.Indices(), (std::vector<size_t>{1}));
}

// ---------------------------------------------------------------------------
// Dragging thumbnails

TEST(SlideDrag_GapAndMarker) {
  const std::vector<ThumbRow> rows = Rows(4);  // rows at 10-100, 110-200, 210-300, 310-400
  CHECK_EQ(DropGap(rows, -50), 0u);
  CHECK_EQ(DropGap(rows, 54), 0u);    // the top half of the first row
  CHECK_EQ(DropGap(rows, 56), 1u);
  CHECK_EQ(DropGap(rows, 205), 2u);   // between rows: the next gap
  CHECK_EQ(DropGap(rows, 399), 4u);
  CHECK_EQ(DropGap(rows, 5000), 4u);
  CHECK_EQ(DropGap({}, 10), 0u);
  const draw::Rect m0 = DropMarker(rows, 0, 2), m2 = DropMarker(rows, 2, 2), m4 = DropMarker(rows, 4, 2);
  CHECK_NEAR(m0.top, 9.0f, 0.01f);
  CHECK_NEAR(m2.top + 1, 205.0f, 0.01f);  // halfway between 200 and 210
  CHECK_NEAR(m4.bottom - 1, 400.0f, 0.01f);
  CHECK_NEAR(m2.Width(), 200.0f, 0.01f);
}

TEST(SlideDrag_MovedOrder) {
  // One slide down: slide 1 into the gap before slide 4.
  CHECK_EQ(MovedOrder(5, {1}, 4), (std::vector<size_t>{0, 2, 3, 1, 4}));
  CHECK_EQ(MovedTo(5, {1}, 4), 3u);
  // Up to the front.
  CHECK_EQ(MovedOrder(5, {3}, 0), (std::vector<size_t>{3, 0, 1, 2, 4}));
  CHECK_EQ(MovedTo(5, {3}, 0), 0u);
  // To the end.
  CHECK_EQ(MovedOrder(5, {0, 2}, 5), (std::vector<size_t>{1, 3, 4, 0, 2}));
  CHECK_EQ(MovedTo(5, {0, 2}, 5), 3u);
  // Several slides that weren't together end up together, in their order.
  CHECK_EQ(MovedOrder(6, {0, 3, 5}, 2), (std::vector<size_t>{1, 0, 3, 5, 2, 4}));
  CHECK_EQ(MovedTo(6, {0, 3, 5}, 2), 1u);
  // Dropped next to itself: nothing changes.
  CHECK(!MoveChanges(5, {2}, 2));
  CHECK(!MoveChanges(5, {2}, 3));
  CHECK(!MoveChanges(5, {1, 2}, 1));
  CHECK(!MoveChanges(5, {1, 2}, 3));
  CHECK(MoveChanges(5, {1, 2}, 4));
  CHECK(MoveChanges(5, {1, 3}, 2));  // gathers them
  // The order and the new indexes are inverse.
  const std::vector<size_t> order = MovedOrder(6, {0, 3, 5}, 2);
  const std::vector<size_t> where = NewIndexes(order);
  for (size_t i = 0; i < order.size(); ++i) CHECK_EQ(where[order[i]], i);
}

TEST(SlideDrag_CachesFollow) {
  // Ids before: 256..260. Slide 258 deleted, 259 moved first, one new slide (300) at the end.
  const std::vector<uint32_t> before = {256, 257, 258, 259, 260};
  const std::vector<uint32_t> after = {259, 256, 257, 260, 300};
  const std::vector<int> old = OldIndexes(before, after);
  CHECK_EQ(old, (std::vector<int>{3, 0, 1, 4, -1}));
  std::vector<std::string> cache = {"a", "b", "c", "d", "e"};
  FollowSlides(cache, old);
  CHECK_EQ(cache, (std::vector<std::string>{"d", "a", "b", "e", ""}));
  // A duplicated id (a damaged deck) counts once.
  CHECK_EQ(OldIndexes({5, 6}, {5, 5, 6}), (std::vector<int>{0, -1, 1}));
  // Caches of move-only types follow too.
  std::vector<std::unique_ptr<int>> owned;
  owned.push_back(std::make_unique<int>(1));
  owned.push_back(std::make_unique<int>(2));
  FollowSlides(owned, {1, -1, 0});
  CHECK(owned.size() == 3 && *owned[0] == 2 && !owned[1] && *owned[2] == 1);
}

// ---------------------------------------------------------------------------
// Slide size

TEST(SlideSize_ChoicesAndScale) {
  CHECK_EQ(SlideSizes().size(), 2u);
  CHECK_EQ(SlideSizeChoiceOf({960, 540}), 0);
  CHECK_EQ(SlideSizeChoiceOf({720, 540}), 1);
  CHECK_EQ(SlideSizeChoiceOf({720, 405}), -1);
  // 16:9 to 4:3: narrower, so PowerPoint asks.
  CHECK(AsksHowToScale({960, 540}, {720, 540}));
  CHECK(!AsksHowToScale({720, 540}, {960, 540}));
  // Maximize keeps the height (cut at the sides), centered.
  SizeTransform t = ScaleContent({960, 540}, {720, 540}, ContentScale::Maximize);
  CHECK_NEAR(t.scale, 1.0f, 1e-4f);
  CHECK_NEAR(t.dx, -120.0f, 1e-3f);
  CHECK_NEAR(t.dy, 0.0f, 1e-3f);
  // Ensure fit shrinks to the width, centered vertically.
  t = ScaleContent({960, 540}, {720, 540}, ContentScale::EnsureFit);
  CHECK_NEAR(t.scale, 0.75f, 1e-4f);
  CHECK_NEAR(t.dx, 0.0f, 1e-3f);
  CHECK_NEAR(t.dy, 67.5f, 1e-3f);
  // 4:3 to 16:9: the content keeps its size, centered.
  t = ScaleContent({720, 540}, {960, 540}, ContentScale::Maximize);
  CHECK_NEAR(t.scale, 1.0f, 1e-4f);
  CHECK_NEAR(t.dx, 120.0f, 1e-3f);
  CHECK(ScaleContent({960, 540}, {960, 540}, ContentScale::EnsureFit).Identity());
  CHECK(ScaleContent({0, 540}, {960, 540}, ContentScale::EnsureFit).Identity());
}

// ---------------------------------------------------------------------------
// Theme colors, themes, backgrounds

TEST(SlideDesign_ColorSchemeXml) {
  CHECK(ColorSchemes().size() >= 8);
  for (const ColorScheme& scheme : ColorSchemes()) {
    // The scheme put into the default theme reads back as its colors.
    std::string theme =
        "<a:theme xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" name=\"T\"><a:themeElements>" +
        ColorSchemeXml(scheme) + FontSchemeXml("F", "Georgia", "Segoe UI") + "</a:themeElements></a:theme>";
    auto t = draw::Theme::Parse(theme);
    CHECK(t != nullptr);
    if (!t) continue;
    for (int i = 0; i < 12; ++i) {
      const draw::Color want = ColorOf(scheme.colors[i]), got = t->GetColor(static_cast<draw::ThemeColor>(i));
      CHECK_NEAR(got.r, want.r, 0.002f);
      CHECK_NEAR(got.g, want.g, 0.002f);
      CHECK_NEAR(got.b, want.b, 0.002f);
    }
    CHECK_EQ(t->MajorFont().latin, std::string("Georgia"));
    CHECK_EQ(t->MinorFont().latin, std::string("Segoe UI"));
  }
  // Names are escaped; other prefixes are used as given.
  Prefixes ns;
  ns.a = "dml";
  const std::string x = FontSchemeXml("A & B", "x\"y", "z", ns);
  CHECK(x.find("A &amp; B") != std::string::npos);
  CHECK(x.find("<dml:fontScheme") == 0);
  CHECK_EQ(HexColor(0x4472C4), std::string("4472C4"));
}

TEST(SlideDesign_BackgroundXml) {
  Background bg;
  bg.color = 0x112233;
  pugi::xml_document d1;
  pugi::xml_node n = Parse(d1, BackgroundXml(bg));
  CHECK(n && std::string(n.name()) == "p:bg");
  CHECK_EQ(std::string(n.child("p:bgPr").child("a:solidFill").child("a:srgbClr").attribute("val").value()), std::string("112233"));
  CHECK(n.child("p:bgPr").child("a:effectLst"));
  bg.kind = Background::Kind::Gradient;
  bg.color2 = 0xFFFFFF;
  bg.angle = 45;
  pugi::xml_document d2;
  n = Parse(d2, BackgroundXml(bg));
  pugi::xml_node grad = n.child("p:bgPr").child("a:gradFill");
  CHECK(grad);
  CHECK_EQ(std::string(grad.child("a:lin").attribute("ang").value()), std::string("2700000"));
  CHECK_EQ(std::string(grad.child("a:gsLst").last_child().attribute("pos").value()), std::string("100000"));
  bg.kind = Background::Kind::Picture;
  CHECK(BackgroundXml(bg).empty());  // no picture part yet
  pugi::xml_document d3;
  n = Parse(d3, BackgroundXml(bg, "rId7"));
  CHECK_EQ(std::string(n.child("p:bgPr").child("a:blipFill").child("a:blip").attribute("r:embed").value()), std::string("rId7"));
  bg.kind = Background::Kind::Theme;
  bg.themeIndex = 1002;
  pugi::xml_document d4;
  n = Parse(d4, BackgroundXml(bg));
  CHECK_EQ(std::string(n.child("p:bgRef").attribute("idx").value()), std::string("1002"));
  CHECK(!GradientPresets().empty());
}

TEST(SlideDesign_BuiltInThemes) {
  CHECK(BuiltInThemes().size() >= 5);
  for (const BuiltInTheme& t : BuiltInThemes()) {
    CHECK(t.colors >= 0 && static_cast<size_t>(t.colors) < ColorSchemes().size());
    CHECK(t.major && *t.major && t.minor && *t.minor);
    pugi::xml_document d;
    CHECK(Parse(d, BackgroundXml(t.background)));
  }
  const auto light = ColorMapOf(false), dark = ColorMapOf(true);
  CHECK_EQ(light.size(), 12u);
  CHECK_EQ(std::string(light[0].second), std::string("lt1"));
  CHECK_EQ(std::string(dark[0].second), std::string("dk1"));
  CHECK_EQ(std::string(dark[1].second), std::string("lt1"));
}

// ---------------------------------------------------------------------------
// Slide numbers, footer and date

TEST(SlideHeaderFooter_ShapeXml) {
  HeaderFooterSettings s;
  s.slideNumber = s.footer = s.date = true;
  s.footerText = L"Q3 <review> & plan";
  s.dateFormat = 4;
  const std::string id = NewFieldId();
  CHECK_EQ(id.size(), 38u);
  CHECK(id.front() == '{' && id.back() == '}' && id[15] == '4');
  CHECK(id != NewFieldId());
  pugi::xml_document d1;
  pugi::xml_node sp = Parse(d1, HfShapeXml(HfKind::SlideNumber, 7, 12, "quarter", s, id, "en-US"));
  CHECK(sp && std::string(sp.name()) == "p:sp");
  pugi::xml_node ph = sp.child("p:nvSpPr").child("p:nvPr").child("p:ph");
  CHECK_EQ(std::string(ph.attribute("type").value()), std::string("sldNum"));
  CHECK_EQ(std::string(ph.attribute("idx").value()), std::string("12"));
  CHECK_EQ(std::string(ph.attribute("sz").value()), std::string("quarter"));
  CHECK_EQ(std::string(sp.child("p:nvSpPr").child("p:cNvPr").attribute("id").value()), std::string("7"));
  CHECK(sp.child("p:spPr"));
  pugi::xml_node fld = sp.child("p:txBody").child("a:p").child("a:fld");
  CHECK_EQ(std::string(fld.attribute("type").value()), std::string("slidenum"));
  CHECK_EQ(std::string(fld.attribute("id").value()), id);
  pugi::xml_document d2;
  sp = Parse(d2, HfShapeXml(HfKind::Footer, 8, 11, "", s, id, "nl-NL"));
  CHECK(!sp.child("p:nvSpPr").child("p:nvPr").child("p:ph").attribute("sz"));
  pugi::xml_node r = sp.child("p:txBody").child("a:p").child("a:r");
  CHECK_EQ(std::string(r.child("a:t").text().get()), std::string("Q3 <review> & plan"));
  CHECK_EQ(std::string(r.child("a:rPr").attribute("lang").value()), std::string("nl-NL"));
  pugi::xml_document d3;
  sp = Parse(d3, HfShapeXml(HfKind::Date, 9, 10, "half", s, id, "en-US"));
  fld = sp.child("p:txBody").child("a:p").child("a:fld");
  CHECK_EQ(std::string(fld.attribute("type").value()), std::string("datetime4"));
  CHECK(*fld.child("a:t").text().get());  // today's date
  s.dateAuto = false;
  s.dateFixed = L"1 April";
  pugi::xml_document d4;
  sp = Parse(d4, HfShapeXml(HfKind::Date, 9, 10, "half", s, id, "en-US"));
  CHECK(!sp.child("p:txBody").child("a:p").child("a:fld"));
  CHECK_EQ(std::string(sp.child("p:txBody").child("a:p").child("a:r").child("a:t").text().get()), std::string("1 April"));
  // An empty footer is an empty paragraph.
  s.footerText.clear();
  pugi::xml_document d5;
  sp = Parse(d5, HfShapeXml(HfKind::Footer, 8, 11, "", s, id, "en-US"));
  CHECK(!sp.child("p:txBody").child("a:p").child("a:r"));
  CHECK(sp.child("p:txBody").child("a:p").child("a:endParaRPr"));
}

TEST(SlideHeaderFooter_Rules) {
  HeaderFooterSettings s;
  s.slideNumber = true;
  s.notOnTitle = true;
  CHECK(HfWanted(s, HfKind::SlideNumber, false));
  CHECK(!HfWanted(s, HfKind::SlideNumber, true));
  CHECK(!HfWanted(s, HfKind::Footer, false));
  CHECK(!HfWanted(s, HfKind::Date, false));
  CHECK_EQ(std::string(HfTypeName(HfKind::Date)), std::string("dt"));
  const std::vector<std::wstring> samples = DateFormatSamples("en-US");
  CHECK_EQ(samples.size(), 13u);
  for (const std::wstring& x : samples) CHECK(!x.empty());
  CHECK(samples[1].size() > samples[0].size());  // the long date
}

TEST(SlideHeaderFooter_ReadFromDeck) {
  auto deck = OpenDeck("layouts-placeholders.pptx");
  CHECK(deck != nullptr);
  if (!deck || deck->SlideCount() < 13) return;
  // Slide 13 shows the date (datetime1), a footer and the slide number.
  const fw::slide::Slide* slide = deck->GetSlide(12);
  const HeaderFooterSettings s = ReadHeaderFooter(*slide);
  CHECK(s.date && s.dateAuto && s.dateFormat == 1);
  CHECK(s.footer);
  CHECK(s.footerText == L"Featherweight public set");
  CHECK(s.slideNumber);
  CHECK_EQ(s.lang, std::string("en-US"));
  // Slide 1 shows none of them.
  const HeaderFooterSettings none = ReadHeaderFooter(*deck->GetSlide(0));
  CHECK(!none.date && !none.footer && !none.slideNumber);
  // Its layout: a title slide layout when slide 1 uses one.
  const fw::slide::Slide* first = deck->GetSlide(0);
  if (first->layout && first->layout->type == "title") CHECK(IsTitleLayout(first->layout));
  CHECK(!IsTitleLayout(nullptr));
}

// ---------------------------------------------------------------------------
// A new presentation

TEST(SlideNew_BlankPresentation) {
  std::vector<uint8_t> bytes = BlankPresentation();
  CHECK(bytes.size() > 4000);
  // FW_BLANK_PPTX=<path> writes it out, for the validator and PowerPoint's open check.
  char out[MAX_PATH];
  if (GetEnvironmentVariableA("FW_BLANK_PPTX", out, MAX_PATH)) {
    std::ofstream f(out, std::ios::binary);
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  }
  fw::pkg::Status status;
  auto package = fw::pkg::Package::Open(fw::pkg::Snapshot::FromBytes(bytes), fw::pkg::Limits::Default(), status);
  CHECK(package != nullptr);
  if (!package) return;
  auto deck = fw::slide::Read(std::move(package), status);
  CHECK(deck != nullptr);
  if (!deck) return;
  CHECK(deck->Problems().empty());
  CHECK_EQ(deck->SlideCount(), 1u);
  CHECK_NEAR(deck->SlideSize().width, 960.0f, 0.01f);
  CHECK_NEAR(deck->SlideSize().height, 540.0f, 0.01f);
  CHECK_EQ(deck->Masters().size(), 1u);
  CHECK_EQ(deck->Layouts().size(), 11u);
  const char* names[] = {"Title Slide", "Title and Content", "Section Header", "Two Content", "Comparison", "Title Only",
                         "Blank", "Content with Caption", "Picture with Caption", "Title and Vertical Text",
                         "Vertical Title and Text"};
  for (size_t i = 0; i < 11; ++i) CHECK_EQ(deck->Masters()[0]->layouts[i]->name, std::string(names[i]));
  const fw::slide::Slide* s = deck->GetSlide(0);
  CHECK(s && s->layout && s->layout->type == "title");
  CHECK(IsTitleLayout(s->layout));
  // The slide's placeholders inherit the layout's positions and the master's styles.
  const fw::slide::Shape* title = s->FindPlaceholder(fw::slide::PlaceholderType::CenteredTitle);
  CHECK(title && title->inherits);
  if (title) CHECK_NEAR(title->bounds.left, 120.0f, 0.01f);  // 1524000 EMU
  const fw::slide::Master& m = *deck->Masters()[0];
  for (auto t : {fw::slide::PlaceholderType::Title, fw::slide::PlaceholderType::Body, fw::slide::PlaceholderType::Date,
                 fw::slide::PlaceholderType::Footer, fw::slide::PlaceholderType::SlideNumber})
    CHECK(m.FindPlaceholder(t) != nullptr);
  CHECK(m.titleStyle && m.bodyStyle && m.otherStyle);
  CHECK_EQ(m.theme->Name(), std::string("Office Theme"));
  // Every layout has the date, footer and slide number placeholders new slides copy.
  for (const auto& l : deck->Layouts())
    for (auto t : {fw::slide::PlaceholderType::Date, fw::slide::PlaceholderType::Footer, fw::slide::PlaceholderType::SlideNumber})
      CHECK(l->FindPlaceholder(t) != nullptr);
  std::wstring error;
  CHECK(OpenDeckFromBytes(bytes, error) != nullptr);
  CHECK(OpenDeckFromBytes({}, error) == nullptr && !error.empty());
}

TEST(SlideDesign_ReadBackground) {
  auto deck = OpenDeck("backgrounds.pptx");
  CHECK(deck != nullptr);
  if (!deck || deck->SlideCount() < 5) return;
  // Slide 2: its layout's own solid color; slide 3: its own solid color; slide 5: a picture.
  const Background two = ReadBackground(*deck->GetSlide(1));
  CHECK(two.kind == Background::Kind::Solid);
  CHECK(two.color != 0xFFFFFF);
  const Background three = ReadBackground(*deck->GetSlide(2));
  CHECK(three.kind == Background::Kind::Solid);
  CHECK(three.color != two.color);
  CHECK(ReadBackground(*deck->GetSlide(4)).kind == Background::Kind::Picture);
  // themes.pptx uses Office's colors on its first slide.
  auto themes = OpenDeck("themes.pptx");
  CHECK(themes != nullptr);
  if (themes) CHECK(ColorSchemeOf(*themes->GetSlide(0)) >= -1);
  // The blank presentation's theme is the Office scheme.
  fw::pkg::Status status;
  auto blank = fw::slide::Read(fw::pkg::Package::Open(fw::pkg::Snapshot::FromBytes(BlankPresentation()), fw::pkg::Limits::Default(), status), status);
  CHECK(blank != nullptr);
  if (blank) {
    CHECK_EQ(ColorSchemeOf(*blank->GetSlide(0)), 0);
    CHECK(ReadBackground(*blank->GetSlide(0)).kind == Background::Kind::Theme);
  }
}

// ---------------------------------------------------------------------------
// Header & footer through the edit layer (src/slide/edit/HeaderFooter.cpp)

TEST(SlideHeaderFooter_EditAndUndo) {
  fw::pkg::Status status;
  auto deck = fw::slide::Read(fw::pkg::Package::Open(fw::pkg::Snapshot::FromBytes(BlankPresentation()), fw::pkg::Limits::Default(), status), status);
  CHECK(deck != nullptr);
  if (!deck) return;
  fw::slide::edit::Editor ed(*deck);
  CHECK(ed.NewSlide(1, 1));  // a Title and Content slide after the title slide
  CHECK_EQ(deck->SlideCount(), 2u);
  fw::slide::edit::HeaderFooter hf;
  hf.slideNumber = hf.footer = hf.date = true;
  hf.footerText = L"Quarterly review";
  hf.dateFormat = "datetime4";
  hf.notOnTitle = true;
  CHECK(ed.SetHeaderFooter({}, hf));
  // The title slide shows none; the other all three, as its layout places them.
  const HeaderFooterSettings title = ReadHeaderFooter(*deck->GetSlide(0));
  CHECK(!title.date && !title.footer && !title.slideNumber);
  const HeaderFooterSettings two = ReadHeaderFooter(*deck->GetSlide(1));
  CHECK(two.date && two.dateAuto && two.dateFormat == 4);
  CHECK(two.footer && two.footerText == L"Quarterly review");
  CHECK(two.slideNumber);
  // Shown: the number is the slide's.
  const fw::slide::Slide* s = deck->GetSlide(1);
  const fw::slide::Shape* num = s->FindPlaceholder(fw::slide::PlaceholderType::SlideNumber);
  CHECK(num != nullptr);
  if (num) {
    const fw::slide::TextBody* body = deck->Text(*num, *s);
    CHECK(body && !body->paragraphs.empty() && !body->paragraphs[0].runs.empty() && body->paragraphs[0].runs[0].text == L"2");
  }
  // Fixed date, no footer, on slide 2 only; then the title slide too.
  hf.dateFixed = true;
  hf.dateText = L"1 April";
  hf.footer = false;
  CHECK(ed.SetHeaderFooter({1}, hf));
  const HeaderFooterSettings fixed = ReadHeaderFooter(*deck->GetSlide(1));
  CHECK(fixed.date && !fixed.dateAuto && fixed.dateFixed == L"1 April" && !fixed.footer && fixed.slideNumber);
  hf.notOnTitle = false;
  CHECK(ed.SetHeaderFooter({0}, hf));
  CHECK(ReadHeaderFooter(*deck->GetSlide(0)).slideNumber);
  // Undo, step by step, back to none.
  CHECK(ed.Undo(3));
  CHECK(!ReadHeaderFooter(*deck->GetSlide(1)).footer && !ReadHeaderFooter(*deck->GetSlide(1)).slideNumber);
  CHECK(ed.Redo(1));
  CHECK(ReadHeaderFooter(*deck->GetSlide(1)).footerText == L"Quarterly review");
  CHECK(!ed.SetHeaderFooter({7}, hf));  // no such slide: refused, nothing changed
}

TEST(SlideDesign_ApplyThemeAndUndo) {
  fw::pkg::Status status;
  auto deck = fw::slide::Read(fw::pkg::Package::Open(fw::pkg::Snapshot::FromBytes(BlankPresentation()), fw::pkg::Limits::Default(), status), status);
  CHECK(deck != nullptr);
  if (!deck) return;
  fw::slide::edit::Editor ed(*deck);
  CHECK(ed.NewSlide(1, 1));
  const BuiltInTheme* dark = nullptr;
  for (const BuiltInTheme& t : BuiltInThemes())
    if (t.dark) {
      dark = &t;
      break;
    }
  CHECK(dark != nullptr);
  if (!dark) return;
  const int schemeBefore = ColorSchemeOf(*deck->GetSlide(1));
  const std::string eaBefore = deck->GetSlide(1)->theme->MajorFont().eastAsian;
  const size_t scriptsBefore = deck->GetSlide(1)->theme->MajorFont().scripts.size();
  fw::slide::edit::ThemeDesign d;
  d.name = "Dark test";
  d.colorScheme = ColorSchemeXml(ColorSchemes()[dark->colors]);
  d.major = dark->major;
  d.minor = dark->minor;
  d.dark = 1;
  d.background = BackgroundXml(dark->background);
  CHECK(ApplyThemeDesign(ed, 0, d));
  // Colors, fonts (East Asian and script fonts kept), light text on dark, the master's background.
  const fw::slide::Slide* s = deck->GetSlide(1);
  CHECK_EQ(ColorSchemeOf(*s), dark->colors);
  CHECK_EQ(s->theme->Name(), std::string("Dark test"));
  CHECK_EQ(s->theme->MajorFont().latin, std::string(dark->major));
  CHECK_EQ(s->theme->MinorFont().latin, std::string(dark->minor));
  CHECK_EQ(s->theme->MajorFont().eastAsian, eaBefore);
  CHECK_EQ(s->theme->MajorFont().scripts.size(), scriptsBefore);
  CHECK(s->colorMap.map[0] == draw::ThemeColor::Dark1);
  const Background bg = ReadBackground(*s);
  CHECK(bg.kind == dark->background.kind);
  if (bg.kind == Background::Kind::Solid) CHECK_EQ(bg.color, dark->background.color);
  // Hide background graphics on slide 2; a slide background as the panel writes it.
  CHECK(SetShowMasterShapes(ed, {1}, false));
  CHECK(!deck->GetSlide(1)->showMasterShapes);
  CHECK(deck->GetSlide(0)->showMasterShapes);
  Background solid;
  solid.color = 0x336699;
  CHECK(ed.SetBackground({0}, BackgroundXml(solid)));
  CHECK_EQ(ReadBackground(*deck->GetSlide(0)).color, 0x336699u);
  // Slide numbers, footer and an updating date everywhere (a fourth step).
  fw::slide::edit::HeaderFooter hf;
  hf.slideNumber = hf.footer = hf.date = true;
  hf.footerText = L"Footer & <text>";
  hf.dateFormat = "datetime2";
  CHECK(ed.SetHeaderFooter({}, hf));
  // FW_DESIGN_PPTX=<path>: the deck as these edits leave it, for fwtool check, the Open XML
  // validator and PowerPoint.
  char path[MAX_PATH] = {};
  if (GetEnvironmentVariableA("FW_DESIGN_PPTX", path, MAX_PATH) > 0) {
    std::vector<uint8_t> bytes;
    CHECK(fw::slide::edit::SaveToBytes(ed, bytes).Ok());
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  }
  // Four steps back: as it was.
  CHECK(ed.Undo(4));
  s = deck->GetSlide(1);
  CHECK(s->showMasterShapes);
  CHECK(s->colorMap.map[0] == draw::ThemeColor::Light1);
  CHECK_EQ(ColorSchemeOf(*s), schemeBefore);
  CHECK(s->theme->MajorFont().latin != std::string(dark->major));
  CHECK(!SetShowMasterShapes(ed, {9}, false));
  CHECK(!ApplyThemeDesign(ed, 3, d));
}

TEST(SlideCopy_BetweenDecks) {
  // As the tab copies slides: the source saved to memory, read back, its slides inserted into
  // another deck in that deck's design.
  fw::pkg::Status status;
  auto a = fw::slide::Read(fw::pkg::Package::Open(fw::pkg::Snapshot::FromBytes(BlankPresentation()), fw::pkg::Limits::Default(), status), status);
  auto b = fw::slide::Read(fw::pkg::Package::Open(fw::pkg::Snapshot::FromBytes(BlankPresentation()), fw::pkg::Limits::Default(), status), status);
  CHECK(a != nullptr && b != nullptr);
  if (!a || !b) return;
  fw::slide::edit::Editor ea(*a), eb(*b);
  CHECK(ea.NewSlide(1, 1));
  fw::slide::edit::HeaderFooter hf;
  hf.footer = hf.slideNumber = true;
  hf.footerText = L"From deck one";
  CHECK(ea.SetHeaderFooter({1}, hf));
  CHECK(ea.SetNotesText(1, L"copied notes"));
  std::vector<uint8_t> clip;
  CHECK(fw::slide::edit::SaveToBytes(ea, clip).Ok());
  auto source = fw::slide::Read(fw::pkg::Package::Open(fw::pkg::Snapshot::FromBytes(clip), fw::pkg::Limits::Default(), status), status);
  CHECK(source != nullptr);
  if (!source) return;
  std::vector<uint32_t> ids;
  CHECK(eb.InsertSlides(*source, {0, 1}, 1, false, &ids));
  CHECK_EQ(b->SlideCount(), 3u);
  CHECK_EQ(ids.size(), 2u);
  const HeaderFooterSettings pasted = ReadHeaderFooter(*b->GetSlide(2));
  CHECK(pasted.footer && pasted.footerText == L"From deck one" && pasted.slideNumber);
  CHECK(b->GetSlide(2)->NotesText() == L"copied notes");
  CHECK(eb.Undo());
  CHECK_EQ(b->SlideCount(), 1u);
}

TEST(SlideHeaderFooter_LineBreaksAndControlCharacters) {
  // fuzz_pptx finding 4: a footer with a line break ('\v') or a character XML can't hold must leave a
  // slide that parses; the break is an a:br, the rest dropped.
  fw::pkg::Status status;
  auto deck = fw::slide::Read(fw::pkg::Package::Open(fw::pkg::Snapshot::FromBytes(BlankPresentation()), fw::pkg::Limits::Default(), status), status);
  CHECK(deck != nullptr);
  if (!deck) return;
  fw::slide::edit::Editor ed(*deck);
  CHECK(ed.NewSlide(1, 1));
  fw::slide::edit::HeaderFooter hf;
  hf.footer = hf.date = true;
  hf.footerText = std::wstring(L"line\vbreak\x01") + L"\xFFFE end";
  hf.dateFixed = true;
  hf.dateText = L"one\ntwo";
  CHECK(ed.SetHeaderFooter({1}, hf));
  std::vector<uint8_t> bytes;
  CHECK(fw::slide::edit::SaveToBytes(ed, bytes).Ok());
  auto again = fw::slide::Read(fw::pkg::Package::Open(fw::pkg::Snapshot::FromBytes(bytes), fw::pkg::Limits::Default(), status), status);
  CHECK(again != nullptr);
  if (!again) return;
  const fw::slide::Slide* s = again->GetSlide(1);
  CHECK(s != nullptr);
  if (!s) return;
  const HeaderFooterSettings read = ReadHeaderFooter(*s);
  CHECK(read.footer && read.date);
  CHECK(read.footerText.find(L"line") != std::wstring::npos && read.footerText.find(L"break") != std::wstring::npos);
  CHECK(read.footerText.find(L'\x01') == std::wstring::npos && read.footerText.find(L'\xFFFE') == std::wstring::npos);
  const fw::slide::Shape* footer = s->FindPlaceholder(fw::slide::PlaceholderType::Footer);
  CHECK(footer != nullptr);
}
