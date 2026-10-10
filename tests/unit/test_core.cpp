#include "TestHarness.h"

#include "core/Ini.h"
#include "core/Json.h"
#include "core/Layout.h"
#include "core/PageRange.h"
#include "core/SemVer.h"
#include "core/Strings.h"

using namespace fw;

// ---------- Strings ----------

TEST(Strings_Utf8RoundTrip) {
  std::wstring w = L"Caf\u00e9 \u65e5\u672c \U0001F600";
  CHECK(Utf8ToWide(WideToUtf8(w)) == w);
}

TEST(Strings_PathHelpers) {
  CHECK(FileNameOf(L"C:\\a\\b\\file.pdf") == L"file.pdf");
  CHECK(FileNameOf(L"C:/a/b/file.pdf") == L"file.pdf");
  CHECK(DirOf(L"C:\\a\\b\\file.pdf") == L"C:\\a\\b");
  CHECK(EndsWithNoCase(L"Report.PDF", L".pdf"));
}

TEST(Strings_Percent) {
  CHECK(FormatPercent(1.0) == L"100%");
  CHECK(FormatPercent(1.254) == L"125%");
  CHECK(FormatPercent(0.5) == L"50%");
}

TEST(Strings_RelativeTime) {
  CHECK(RelativeTime(10) == L"Just now");
  CHECK(RelativeTime(5 * 60) == L"5 minutes ago");
  CHECK(RelativeTime(26 * 3600) == L"Yesterday");
  CHECK(RelativeTime(3 * 86400) == L"3 days ago");
}

TEST(Strings_HashStable) {
  CHECK(Fnv1a64(L"abc") == Fnv1a64(L"abc"));
  CHECK(Fnv1a64(L"abc") != Fnv1a64(L"abd"));
  CHECK(Hex64(0xABCDEFull).size() == 16);
}

// ---------- INI ----------

TEST(Ini_ParseAndSerialize) {
  Ini ini;
  ini.Parse("\xEF\xBB\xBF; comment\r\n[General]\r\nTheme = dark\r\nInvert=1\r\n\r\n[Recent]\r\n1=C:\\x.pdf\r\n");
  CHECK(ini.Get(L"general", L"theme") == L"dark");
  CHECK(ini.GetBool(L"General", L"Invert", false));
  CHECK(ini.Get(L"Recent", L"1") == L"C:\\x.pdf");
  CHECK(ini.GetInt(L"General", L"Missing", 7) == 7);
  ini.Set(L"General", L"Theme", L"light");
  ini.SetInt(L"New", L"Count", 3);
  Ini again;
  again.Parse(ini.Serialize());
  CHECK(again.Get(L"General", L"Theme") == L"light");
  CHECK(again.GetInt(L"New", L"Count", 0) == 3);
}

TEST(Ini_Unicode) {
  Ini ini;
  ini.Set(L"Recent", L"1", L"D:\\Documenten\\\u00e9t\u00e9.pdf");
  Ini again;
  again.Parse(ini.Serialize());
  CHECK(again.Get(L"Recent", L"1") == L"D:\\Documenten\\\u00e9t\u00e9.pdf");
}

TEST(Ini_ReplaceSection) {
  Ini ini;
  ini.Set(L"State", L"a", L"1");
  ini.ReplaceSection(L"State", {{L"b", L"2"}});
  CHECK(!ini.Has(L"State", L"a"));
  CHECK(ini.Get(L"State", L"b") == L"2");
}

// ---------- JSON ----------

TEST(Json_GitHubRelease) {
  auto v = ParseJson(R"({"tag_name":"v1.2.0","draft":false,"assets":[
      {"name":"Featherweight.exe","browser_download_url":"https://x/y.exe","size":123},
      {"name":"SHA256SUMS.txt","browser_download_url":"https://x/s.txt"}],
      "body":"Line\nTwo \u00e9 \ud83d\ude00"})");
  CHECK(v.has_value());
  CHECK(v->StringOr("tag_name") == "v1.2.0");
  const JsonValue* assets = v->Find("assets");
  CHECK(assets && assets->type == JsonValue::Type::Array && assets->array.size() == 2);
  CHECK(assets->array[0].StringOr("browser_download_url") == "https://x/y.exe");
  CHECK(v->StringOr("body") == "Line\nTwo \xC3\xA9 \xF0\x9F\x98\x80");
}

TEST(Json_RejectsGarbage) {
  CHECK(!ParseJson("{").has_value());
  CHECK(!ParseJson("{\"a\":}").has_value());
  CHECK(!ParseJson("[1,2,]x").has_value());
  CHECK(ParseJson("  [1, 2.5e3, true, null]  ").has_value());
}

// ---------- SemVer ----------

TEST(SemVer_Parse) {
  auto a = ParseSemVer("v1.2.3");
  CHECK(a && a->major == 1 && a->minor == 2 && a->patch == 3);
  CHECK(ParseSemVer("1.2").has_value());
  CHECK(!ParseSemVer("1").has_value());
  CHECK(!ParseSemVer("1.2.3.4").has_value());
  CHECK(!ParseSemVer("1.x.3").has_value());
}

TEST(SemVer_Compare) {
  auto c = [](const char* a, const char* b) { return CompareSemVer(*ParseSemVer(a), *ParseSemVer(b)); };
  CHECK(c("1.0.0", "1.0.1") < 0);
  CHECK(c("1.10.0", "1.9.9") > 0);
  CHECK(c("2.0.0", "2.0.0") == 0);
  CHECK(c("1.0.0-beta", "1.0.0") < 0);
  CHECK(c("v1.1.0", "1.0.9") > 0);
}

// ---------- Page ranges ----------

TEST(PageRange_Basic) {
  auto r = ParsePageRanges(L"1-3, 7", 10);
  CHECK(r && *r == std::vector<int>({0, 1, 2, 6}));
  r = ParsePageRanges(L" 9- ", 10);
  CHECK(r && *r == std::vector<int>({8, 9}));
  r = ParsePageRanges(L"3-1", 10);
  CHECK(r && *r == std::vector<int>({2, 1, 0}));
}

TEST(PageRange_Invalid) {
  CHECK(!ParsePageRanges(L"0", 10));
  CHECK(!ParsePageRanges(L"11", 10));
  CHECK(!ParsePageRanges(L"a-b", 10));
  CHECK(!ParsePageRanges(L"", 10));
}

// ---------- Layout ----------

static std::vector<PageSizePt> Letter(int n) { return std::vector<PageSizePt>(n, PageSizePt{612, 792}); }

TEST(Layout_ContinuousStacksPages) {
  LayoutInput in;
  in.pages = Letter(3);
  in.scale = 1.0;
  in.viewportW = 1000;
  Layout L = ComputeLayout(in);
  CHECK_EQ(L.boxes.size(), 3u);
  CHECK_EQ(L.width, 1000);
  CHECK_EQ(L.boxes[0].y, in.margin);
  CHECK_EQ(L.boxes[1].y, in.margin + 792 + in.gap);
  CHECK_EQ(L.height, in.margin * 2 + 792 * 3 + in.gap * 2);
  CHECK_EQ(L.boxes[0].x, 500 - 306);
}

TEST(Layout_FacingCoverAlone) {
  LayoutInput in;
  in.pages = Letter(4);
  in.mode = ViewMode::Facing;
  in.coverAlone = true;
  in.scale = 1.0;
  in.viewportW = 2000;
  Layout L = ComputeLayout(in);
  CHECK_EQ(L.rows.size(), 3u);  // [0] [1 2] [3]
  CHECK(L.boxes[0].x > 1000);   // cover on the right
  CHECK(L.boxes[1].Right() < 1000 && L.boxes[2].x > 1000);
  CHECK(L.boxes[3].Right() < 1000);  // trailing page on the left
  CHECK_EQ(L.rowOfPage[2], 1);
  CHECK_EQ(FacingRowStart(2, true), 1);
  CHECK_EQ(FacingRowStart(3, false), 2);
}

TEST(Layout_SingleModeRowsShareY) {
  LayoutInput in;
  in.pages = Letter(3);
  in.mode = ViewMode::Single;
  in.viewportW = 800;
  Layout L = ComputeLayout(in);
  CHECK_EQ(L.boxes[0].y, L.boxes[2].y);
  CHECK_EQ(L.height, 792 + 2 * in.margin);
}

TEST(Layout_RotationSwapsSize) {
  LayoutInput in;
  in.pages = Letter(1);
  in.rotation = 1;
  in.viewportW = 100;
  Layout L = ComputeLayout(in);
  CHECK_EQ(L.boxes[0].w, 792);
  CHECK_EQ(L.boxes[0].h, 612);
  CHECK(L.width >= 792);
}

TEST(Layout_VisibleRows) {
  LayoutInput in;
  in.pages = Letter(10);
  in.viewportW = 800;
  Layout L = ComputeLayout(in);
  int first, last;
  L.VisibleRows(L.rows[3].y + 10, L.rows[3].y + 900, first, last);
  CHECK_EQ(first, 3);
  CHECK_EQ(last, 4);
}

TEST(Layout_FitScales) {
  auto pages = Letter(2);
  FitInput f;
  f.pages = &pages;
  f.viewportW = 612 + 32;
  f.viewportH = 792 + 32;
  f.margin = 16;
  f.dpi = 72;
  CHECK_NEAR(ComputeFitScale(ZoomMode::FitWidth, f), 1.0, 1e-9);
  CHECK_NEAR(ComputeFitScale(ZoomMode::FitPage, f), 1.0, 1e-9);
  CHECK_NEAR(ComputeFitScale(ZoomMode::ActualSize, f), 1.0, 1e-9);
  f.viewportW = 5000;  // wide screen: automatic caps at 125 %
  CHECK_NEAR(ComputeFitScale(ZoomMode::Automatic, f), 1.25, 1e-9);
  f.dpi = 144;
  CHECK_NEAR(ComputeFitScale(ZoomMode::ActualSize, f), 2.0, 1e-9);
}

TEST(Layout_ZoomSteps) {
  CHECK_NEAR(NextZoomStep(1.0, true), 1.25, 1e-9);
  CHECK_NEAR(NextZoomStep(1.0, false), 0.75, 1e-9);
  CHECK_NEAR(NextZoomStep(1.1, true), 1.25, 1e-9);
  CHECK_NEAR(NextZoomStep(16.0, true), 16.0, 1e-9);
  CHECK_NEAR(ClampZoom(100.0), kMaxZoom, 1e-9);
}
