// The presentation tab's headless parts (P1, p1-view): fitting the slide, the thumbnails list, the
// printed and exported pages (slides, handouts, notes pages), find, and the placeholder deck on the
// generated decks.

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "TestHarness.h"
#include "slide/view/ModelDeck.h"
#include "slide/view/SlideLayout.h"
#include "slide/view/SlidePages.h"
#include "text/Fonts.h"

using namespace fw::slide::view;

namespace {

bool Inside(const draw::Rect& inner, const draw::Rect& outer) {
  return inner.left >= outer.left - 0.01f && inner.top >= outer.top - 0.01f && inner.right <= outer.right + 0.01f &&
         inner.bottom <= outer.bottom + 0.01f;
}

bool Overlap(const draw::Rect& a, const draw::Rect& b) {
  return a.left < b.right - 0.01f && b.left < a.right - 0.01f && a.top < b.bottom - 0.01f && b.top < a.bottom - 0.01f;
}

std::wstring DeckPath(const char* name) {
  return (std::filesystem::path(__FILE__).parent_path().parent_path() / "office" / "generated" / name).wstring();
}

// A painter that counts what's drawn: the slides each page asks for, and text.
class CountingPainter : public draw::Painter {
 public:
  void Save() override { stack_.push_back(m_); }
  void Restore() override {
    if (!stack_.empty()) {
      m_ = stack_.back();
      stack_.pop_back();
    }
  }
  void Transform(const draw::Matrix& m) override { m_ = m * m_; }
  draw::Matrix CurrentTransform() const override { return m_; }
  void ClipRect(const draw::Rect&) override {}
  void ClipPath(const draw::Path&) override {}
  void BeginGroup(float) override { Save(); }
  void FillPath(const draw::Path&, const draw::Brush&) override { ++fills; }
  void StrokePath(const draw::Path&, const draw::Pen&) override { ++strokes; }
  void DrawGlyphs(draw::Point, const draw::GlyphRun& run, const draw::Brush&) override { glyphs += run.count; }
  void DrawBitmap(const draw::Bitmap&, const draw::Rect&, const draw::Rect*, float) override {}
  int fills = 0, strokes = 0;
  uint32_t glyphs = 0;

 private:
  draw::Matrix m_;
  std::vector<draw::Matrix> stack_;
};

// A deck of `n` slides that records which slides were painted, and where.
class FakeDeck : public fw::slide::view::Deck {
 public:
  explicit FakeDeck(size_t n) : n_(n) {}
  size_t SlideCount() const override { return n_; }
  draw::Size SlideSize() const override { return {960, 540}; }
  bool Hidden(size_t i) const override { return i == 1; }
  std::vector<TextBlock> Text(size_t i) const override { return {{L"Slide " + std::to_wstring(i + 1), {0, 0, 960, 100}}}; }
  std::wstring Notes(size_t i) const override { return i == 0 ? L"Say hello.\nThen the agenda." : L""; }
  std::vector<SlideLink> Links(size_t) const override { return {}; }
  void Paint(size_t i, draw::Painter& p) override {
    painted.push_back(i);
    origins.push_back(p.CurrentTransform().Apply({0, 0}));
    scales.push_back(p.CurrentTransform().ScaleX());
  }
  std::vector<size_t> painted;
  std::vector<draw::Point> origins;
  std::vector<float> scales;

 private:
  size_t n_;
};

}  // namespace

TEST(SlideView_FitRectKeepsTheShapeAndCenters) {
  const draw::Rect r = FitRect({960, 540}, {0, 0, 1000, 1000});
  CHECK_NEAR(r.Width(), 1000.0f, 0.01f);
  CHECK_NEAR(r.Height(), 562.5f, 0.01f);
  CHECK_NEAR(r.top, (1000 - 562.5f) / 2, 0.01f);
  const draw::Rect tall = FitRect({720, 540}, {10, 20, 410, 220});  // 4:3 in a wide box
  CHECK_NEAR(tall.Height(), 200.0f, 0.01f);
  CHECK_NEAR(tall.left, 10 + (400 - 266.667f) / 2, 0.01f);
  const draw::Rect capped = FitRect({100, 50}, {0, 0, 1000, 1000}, 2);
  CHECK_NEAR(capped.Width(), 200.0f, 0.01f);
  const draw::Rect none = FitRect({0, 0}, {5, 5, 100, 100});
  CHECK(none.Width() == 0 && none.Height() == 0);
}

TEST(SlideView_ThumbnailRowsGoDownInOrder) {
  const std::vector<ThumbRow> rows = ThumbRows(5, {960, 540}, {196, 1});
  CHECK_EQ(rows.size(), size_t{5});
  for (size_t i = 0; i < rows.size(); ++i) {
    CHECK(rows[i].slide.Width() > 100);
    CHECK_NEAR(rows[i].slide.Height() / rows[i].slide.Width(), 540.0f / 960, 0.01f);
    CHECK(rows[i].number.right <= rows[i].slide.left);  // the number is left of the picture
    if (i) CHECK(rows[i].slide.top > rows[i - 1].slide.bottom);
  }
  CHECK(ThumbListHeight(rows, 1) > rows.back().slide.bottom);
  // At 200 %, everything doubles.
  const std::vector<ThumbRow> big = ThumbRows(1, {960, 540}, {392, 2});
  CHECK_NEAR(big[0].slide.Width(), rows[0].slide.Width() * 2, 1.0f);
  CHECK(ThumbRows(0, {960, 540}, {196, 1}).empty());
}

TEST(SlideView_HandoutSlotsForEachCount) {
  const draw::Size page{540, 720}, slide{960, 540};
  for (int n : kHandoutCounts) {
    const std::vector<PageItem> slots = HandoutSlots(n, page, slide);
    CHECK_EQ(slots.size(), static_cast<size_t>(n));
    for (size_t i = 0; i < slots.size(); ++i) {
      CHECK(Inside(slots[i].rect, {0, 0, page.width, page.height}));
      CHECK_NEAR(slots[i].rect.Height() / slots[i].rect.Width(), 540.0f / 960, 0.01f);
      for (size_t k = 0; k < i; ++k) CHECK(!Overlap(slots[i].rect, slots[k].rect));
      // Reading order: left to right, then down.
      if (i) CHECK(slots[i].rect.top > slots[i - 1].rect.top + 0.5f || slots[i].rect.left > slots[i - 1].rect.left);
      // Only 3 a page has lines beside the slides.
      CHECK((slots[i].notes.Width() > 0) == (n == 3));
      if (n == 3) CHECK(slots[i].notes.left > slots[i].rect.right && Inside(slots[i].notes, {0, 0, page.width, page.height}));
    }
  }
  CHECK(ValidHandoutCount(9) && !ValidHandoutCount(5) && !ValidHandoutCount(0));
  CHECK_EQ(HandoutSlots(5, page, slide).size(), size_t{6});  // an odd count falls back to 6
  // Fewer slides a page means bigger slides.
  CHECK(HandoutSlots(1, page, slide)[0].rect.Width() >= HandoutSlots(2, page, slide)[0].rect.Width());
  CHECK(HandoutSlots(4, page, slide)[0].rect.Width() > HandoutSlots(9, page, slide)[0].rect.Width());
}

TEST(SlideView_NotesPageHasTheSlideAboveTheNotes) {
  const draw::Size page{540, 720};
  const PageItem it = NotesSlots(page, {960, 540});
  CHECK(Inside(it.rect, {0, 0, page.width, page.height}));
  CHECK(Inside(it.notes, {0, 0, page.width, page.height}));
  CHECK(it.rect.bottom <= it.notes.top);
  CHECK_NEAR(it.rect.Width(), 432.0f, 0.5f);  // 6 inches across on a 7.5 inch page
}

TEST(SlideView_LayoutPagesForSlidesHandoutsAndNotes) {
  std::vector<size_t> slides(10);
  for (size_t i = 0; i < slides.size(); ++i) slides[i] = i;
  PrintSettings s;
  s.what = PrintWhat::Slides;
  std::vector<PrintPage> pages = LayoutPages(slides, {960, 540}, {960, 540}, {540, 720}, s);
  CHECK_EQ(pages.size(), size_t{10});
  CHECK_NEAR(pages[3].items[0].rect.Width(), 960.0f, 0.01f);  // a slide fills its own page
  CHECK(pages[3].items[0].slide == 3 && pages[3].number == 4);
  // On Letter paper the slide fits across, centered.
  pages = LayoutPages(slides, {960, 540}, {792, 612}, {612, 792}, s);
  CHECK_NEAR(pages[0].items[0].rect.Width(), 792.0f, 0.01f);
  CHECK_NEAR(pages[0].items[0].rect.top, (612 - 445.5f) / 2, 0.01f);
  s.what = PrintWhat::Handouts;
  s.perPage = 6;
  pages = LayoutPages(slides, {960, 540}, {960, 540}, {540, 720}, s);
  CHECK_EQ(pages.size(), size_t{2});
  CHECK_EQ(pages[0].items.size(), size_t{6});
  CHECK_EQ(pages[1].items.size(), size_t{4});
  CHECK(pages[1].items[0].slide == 6 && pages[1].number == 2);
  s.perPage = 9;
  CHECK_EQ(LayoutPages(slides, {960, 540}, {960, 540}, {540, 720}, s).size(), size_t{2});
  s.what = PrintWhat::Notes;
  pages = LayoutPages(slides, {960, 540}, {960, 540}, {540, 720}, s);
  CHECK_EQ(pages.size(), size_t{10});
  CHECK(pages[9].size.width == 540 && pages[9].items[0].notes.Height() > 0);
  CHECK(LayoutPages({}, {960, 540}, {960, 540}, {540, 720}, s).empty());
}

TEST(SlideView_PaintPageDrawsEachSlideInItsPlace) {
  fw::text::Fonts fonts;
  FakeDeck deck(4);
  PrintSettings s;
  s.what = PrintWhat::Handouts;
  s.perPage = 4;
  const std::vector<PrintPage> pages = LayoutPages({0, 1, 2, 3}, deck.SlideSize(), {960, 540}, {540, 720}, s);
  CHECK_EQ(pages.size(), size_t{1});
  CountingPainter p;
  PaintPage(deck, pages[0], s, fonts, p);
  CHECK((deck.painted == std::vector<size_t>{0, 1, 2, 3}));
  for (size_t i = 0; i < 4; ++i) {
    CHECK_NEAR(deck.origins[i].x, pages[0].items[i].rect.left, 0.01f);
    CHECK_NEAR(deck.origins[i].y, pages[0].items[i].rect.top, 0.01f);
    CHECK_NEAR(deck.scales[i], pages[0].items[i].rect.Width() / 960, 0.001f);
  }
  CHECK(p.strokes >= 4);    // the frames
  CHECK(p.glyphs >= 1);     // the page number
  // A notes page draws the notes' text below the slide; without a frame, no strokes.
  s.what = PrintWhat::Notes;
  s.frame = false;
  CountingPainter notes;
  PaintPage(deck, LayoutPages({0}, deck.SlideSize(), {960, 540}, {540, 720}, s)[0], s, fonts, notes);
  CHECK(notes.glyphs > 20);  // "Say hello." "Then the agenda." and the page number
  CHECK_EQ(notes.strokes, 0);
  // Full page slides: no page number.
  s.what = PrintWhat::Slides;
  CountingPainter slide;
  PaintPage(deck, LayoutPages({2}, deck.SlideSize(), {960, 540}, {540, 720}, s)[0], s, fonts, slide);
  CHECK_EQ(slide.glyphs, 0u);
  CHECK_EQ(deck.painted.back(), size_t{2});
}

TEST(SlideView_FindAll) {
  CHECK((FindAll(L"Sales grew; sales fell", L"sales", false, false) == std::vector<size_t>{0, 12}));
  CHECK((FindAll(L"Sales grew; sales fell", L"sales", true, false) == std::vector<size_t>{12}));
  CHECK((FindAll(L"salesman sales", L"sales", false, true) == std::vector<size_t>{9}));
  CHECK((FindAll(L"aaaa", L"aa", false, false) == std::vector<size_t>{0, 2}));  // not overlapping
  CHECK(FindAll(L"abc", L"", false, false).empty());
  CHECK(FindAll(L"ab", L"abc", false, false).empty());
  CHECK((FindAll(L"ÉCOLE école", L"école", false, false) == std::vector<size_t>{0, 6}));
}

TEST(SlideView_ModelDeckReadsTheGeneratedDecks) {
  std::wstring error;
  std::unique_ptr<fw::slide::view::Deck> deck = OpenDeck(DeckPath("simple.pptx"), error);
  CHECK(deck != nullptr);
  if (!deck) return;
  CHECK_EQ(deck->SlideCount(), size_t{2});
  CHECK_NEAR(deck->SlideSize().width, 720.0f, 0.01f);  // 4:3, 10 x 7.5 inches
  CHECK_NEAR(deck->SlideSize().height, 540.0f, 0.01f);
  CHECK_NEAR(deck->NotesSize().height, 720.0f, 0.01f);
  CHECK(!deck->StartsAsShow());
  const std::vector<TextBlock> first = deck->Text(0);
  CHECK(!first.empty() && first[0].text == L"A simple presentation");
  bool notes = false;
  for (size_t i = 0; i < deck->SlideCount(); ++i)
    if (deck->Notes(i) == L"Speaker notes for the bullet slide.") notes = true;
  CHECK(notes);
  // Painting draws the slide's text.
  CountingPainter p;
  deck->Paint(0, p);
  CHECK(p.fills >= 1 && p.glyphs > 10);

  std::unique_ptr<fw::slide::view::Deck> wide = OpenDeck(DeckPath("layouts.pptx"), error);
  CHECK(wide && wide->SlideCount() == 11);
  if (wide) CHECK_NEAR(wide->SlideSize().width, 960.0f, 0.01f);  // 16:9
  CHECK(OpenDeck(DeckPath("shapes-table-chart.pptx"), error) != nullptr);
  CHECK(OpenDeck(DeckPath("no-such-deck.pptx"), error) == nullptr && !error.empty());
}
