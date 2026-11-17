// What sits on a sheet above its cells (S1 phase 3, src/sheet/objects): anchors, pictures, charts
// from the cells, chart sheets, notes and threaded comments, on tests/office/sheet's workbooks.
#include "TestHarness.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "DrawTestUtil.h"
#include "PkgTestUtil.h"
#include "chart/Chart.h"
#include "draw/D2DPainter.h"
#include "sheet/model/Workbook.h"
#include "sheet/objects/ChartData.h"
#include "sheet/objects/SheetObjects.h"
#include "sheet/xlsx/Xlsx.h"

using namespace fw::sheet;

namespace {

std::unique_ptr<Workbook> OpenSheetFile(const char* name) {
  const auto path = std::filesystem::path(__FILE__).parent_path().parent_path() / name;
  std::ifstream in(path, std::ios::binary);
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  fw::pkg::Status st;
  auto pkg = test::OpenBytes(std::move(bytes), st);
  CHECK(pkg != nullptr);
  if (!pkg) return nullptr;
  auto book = xlsx::Read(std::move(pkg), st, xlsx::ReadOptions{});
  CHECK(book != nullptr);
  return book;
}

// Columns 48 points wide and rows 15 high; column 5 and row 9 hidden.
struct GridLayout : SheetLayout {
  double ColumnLeft(uint32_t col) const override { return 48.0 * col - (col > 5 ? 48 : 0); }
  double RowTop(uint32_t row) const override { return 15.0 * row - (row > 9 ? 15 : 0); }
};

bool Close(const draw::Rect& r, float l, float t, float rt, float b) {
  return std::fabs(r.left - l) < 0.01f && std::fabs(r.top - t) < 0.01f && std::fabs(r.right - rt) < 0.01f &&
         std::fabs(r.bottom - b) < 0.01f;
}

// Excel's default grid: 64-pixel columns, 20-pixel rows.
struct DefaultLayout : SheetLayout {
  double ColumnLeft(uint32_t col) const override { return 48.0 * col; }
  double RowTop(uint32_t row) const override { return 15.0 * row; }
};

void SavePng(drawtest::Surface& s, const std::wstring& path) {
  Microsoft::WRL::ComPtr<IWICStream> stream;
  Microsoft::WRL::ComPtr<IWICBitmapEncoder> encoder;
  Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> frame;
  if (FAILED(s.wic->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) ||
      FAILED(s.wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
      FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
      FAILED(encoder->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr)))
    return;
  frame->SetSize(s.w, s.h);
  WICPixelFormatGUID format = GUID_WICPixelFormat32bppPBGRA;
  frame->SetPixelFormat(&format);
  frame->WriteSource(s.bitmap.Get(), nullptr);
  frame->Commit();
  encoder->Commit();
}

}  // namespace

// With FW_SHEET_PNG set to a folder: every sheet's objects of tests/office/sheet's workbooks drawn
// there on Excel's default grid, a point to a pixel, to hold up against Excel's reference PDFs.
// With FW_SHEET_CORPUS set to a folder: every workbook in it (and below) opened, and every sheet's
// objects read, placed and drawn (the real-world corpus: no crash, no hang). Prints a summary.
TEST(sheet_objects_corpus) {
  wchar_t root[MAX_PATH];
  if (!GetEnvironmentVariableW(L"FW_SHEET_CORPUS", root, MAX_PATH)) return;
  fw::chart::Install();
  size_t files = 0, opened = 0, objects = 0, charts = 0, notes = 0;
  for (const auto& e : std::filesystem::recursive_directory_iterator(root)) {
    const std::wstring ext = e.path().extension().wstring();
    if (!e.is_regular_file() || (ext != L".xlsx" && ext != L".xlsm")) continue;
    ++files;
    std::ifstream in(e.path(), std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    fw::pkg::Status st;
    auto pkg = test::OpenBytes(std::move(bytes), st);
    auto book = pkg ? xlsx::Read(std::move(pkg), st, xlsx::ReadOptions{true}) : nullptr;
    if (!book) {
      std::printf("    not opened: %ls: %s\n", e.path().c_str(), st.message.c_str());
      continue;
    }
    ++opened;
    for (size_t i = 0; i < book->SheetCount(); ++i) {
      auto o = SheetObjects::Load(*book, i);
      DefaultLayout layout;
      o->Place(layout);
      for (const auto& x : o->Objects()) ++objects, charts += x.chart ? 1 : 0;
      notes += o->Notes().size();
      drawtest::Surface s(160, 120);
      {
        draw::D2DPainter p(s.dc.Get());
        p.Transform(draw::Matrix::Scale(0.1f, 0.1f));
        if (o->IsChartSheet()) o->DrawChartSheet(p, {0, 0, 1600, 1200});
        o->Draw(p, {0, 0, 1600, 1200}, {});
      }
      s.End();
    }
  }
  std::printf("    %zu workbooks, %zu opened; %zu objects (%zu charts), %zu notes\n", files, opened, objects, charts, notes);
  CHECK_EQ(opened, files);
}

TEST(sheet_objects_render) {
  wchar_t dir[MAX_PATH];
  if (!GetEnvironmentVariableW(L"FW_SHEET_PNG", dir, MAX_PATH)) return;
  fw::chart::Install();
  for (const char* name : {"office/sheet/charts.xlsx", "office/sheet/notes.xlsx", "office/sheet/pictures-shapes.xlsx", "sheet/objects.xlsx"}) {
    auto book = OpenSheetFile(name);
    if (!book) continue;
    for (size_t i = 0; i < book->SheetCount(); ++i) {
      auto objects = SheetObjects::Load(*book, i);
      DefaultLayout layout;
      objects->Place(layout);
      drawtest::Surface s(1300, 1100);
      {
        draw::D2DPainter p(s.dc.Get());
        if (objects->IsChartSheet()) objects->DrawChartSheet(p, {0, 0, 700, 500});
        objects->Draw(p, {0, 0, 1300, 1100}, {});
        for (const CellNote& n : objects->Notes()) {
          const draw::Size size = objects->NoteSize(n);
          const float x = static_cast<float>(layout.ColumnLeft(n.col + 1)) + 11, y = static_cast<float>(layout.RowTop(n.row + 20));
          objects->DrawNote(p, n, {x, y, x + size.width, y + size.height});
        }
      }
      CHECK(s.End());
      SavePng(s, std::filesystem::path(dir) / (std::filesystem::path(name).stem().wstring() + L"-" + std::to_wstring(i) + L".png"));
    }
  }
  // Every chart of tests/office/excel/charts.xlsx printed on its own at 100 dpi, to hold up
  // against the same chart cut from Excel's PDF (reference/excel/charts.xlsx.pdf).
  auto book = OpenSheetFile("office/excel/charts.xlsx");
  if (!book) return;
  const float scale = 100.0f / 72;
  // Columns and rows wide enough that no chart reaches the next one.
  struct Spaced : SheetLayout {
    double ColumnLeft(uint32_t col) const override { return 60.0 * col; }
    double RowTop(uint32_t row) const override { return 20.0 * row; }
  };
  for (size_t i = 0; i < book->SheetCount(); ++i) {
    auto objects = SheetObjects::Load(*book, i);
    Spaced layout;
    objects->Place(layout);
    const auto placed = objects->Objects();
    for (size_t k = 0; k < placed.size(); ++k) {
      const draw::Rect& r = placed[k].rect;
      drawtest::Surface s(static_cast<int>(std::ceil(r.Width() * scale)), static_cast<int>(std::ceil(r.Height() * scale)));
      {
        draw::D2DPainter p(s.dc.Get());
        p.Transform(draw::Matrix::Translate(-r.left, -r.top) * draw::Matrix::Scale(scale, scale));
        SheetObjects::DrawOptions options;
        options.printing = true;
        objects->Draw(p, r, options);
      }
      CHECK(s.End());
      SavePng(s, std::filesystem::path(dir) / (L"chart-" + std::to_wstring(i) + L"-" + std::to_wstring(k + 1) + L".png"));
    }
    if (objects->IsChartSheet()) {
      drawtest::Surface s(static_cast<int>(11 * 100), static_cast<int>(8.5 * 100));
      {
        draw::D2DPainter p(s.dc.Get());
        p.Transform(draw::Matrix::Scale(scale, scale));
        objects->DrawChartSheet(p, {0, 0, 11 * 72, 8.5f * 72});
      }
      CHECK(s.End());
      SavePng(s, std::filesystem::path(dir) / (L"chart-" + std::to_wstring(i) + L"-sheet.png"));
    }
  }
}

TEST(sheet_objects_pictures) {
  auto book = OpenSheetFile("office/sheet/pictures-shapes.xlsx");
  if (!book) return;
  auto objects = SheetObjects::Load(*book, 0);
  GridLayout layout;
  objects->Place(layout);
  const auto placed = objects->Objects();
  CHECK_EQ(placed.size(), 3u);
  if (placed.size() != 3) return;
  // Two cells (E3 to I13, over the hidden column F), then one cell (B3, 120 x 75 points).
  CHECK(Close(placed[0].rect, 192, 30, 336, 165));
  CHECK(Close(placed[1].rect, 48, 30, 168, 105));
  CHECK(!placed[0].chart && placed[0].name == "Image 2");
  drawtest::Surface s(400, 300);
  {
    draw::D2DPainter p(s.dc.Get());
    objects->Draw(p, {0, 0, 400, 300}, {});
  }
  CHECK(s.End());
  CHECK(s.At(100, 60) != 0xFFFFFF);  // the one-cell picture
  CHECK(s.At(390, 290) == 0xFFFFFF);

  // A right-to-left sheet 400 points wide: the one-cell picture moves to 232-352.
  drawtest::Surface rtl(400, 300);
  {
    draw::D2DPainter p(rtl.dc.Get());
    SheetObjects::DrawOptions options;
    options.mirrorX = 400;
    objects->Draw(p, {0, 0, 400, 300}, options);
  }
  CHECK(rtl.End());
  CHECK(rtl.At(300, 60) != 0xFFFFFF && rtl.At(60, 60) == 0xFFFFFF);
}

TEST(sheet_objects_charts) {
  fw::chart::Install();
  auto book = OpenSheetFile("office/sheet/charts.xlsx");
  if (!book) return;
  // The charts' data comes from the cells (the file caches none).
  objects::WorkbookData data(*book, 0);
  std::vector<std::string> texts;
  std::vector<double> numbers;
  std::string format;
  CHECK(data.Cells("'Charts'!$B$2:$B$7", texts, numbers, &format));
  CHECK(texts.size() == 6 && numbers.size() == 6 && numbers[0] == numbers[0] && format == "General");
  CHECK(data.Cells("(Charts!$A$2:$A$3,Charts!$A$5)", texts, numbers, nullptr) && texts.size() == 3);
  CHECK(!data.Cells("[1]Charts!$A$2", texts, numbers, nullptr));
  CHECK(!data.Cells("Nowhere!$A$2", texts, numbers, nullptr));

  auto objects = SheetObjects::Load(*book, 0);
  GridLayout layout;
  objects->Place(layout);
  const auto placed = objects->Objects();
  CHECK_EQ(placed.size(), 12u);
  // Every one, the combo chart and the radar chart included.
  size_t charts = 0;
  for (const auto& o : placed) charts += o.chart ? 1 : 0;
  CHECK_EQ(charts, 12u);
  drawtest::Surface s(420, 240);
  {
    draw::D2DPainter p(s.dc.Get());
    p.Transform(draw::Matrix::Translate(0, -120));  // the first chart, at A9
    objects->Draw(p, {0, 120, 420, 360}, {});
  }
  CHECK(s.End());
  int ink = 0;
  for (int y = 0; y < 200; y += 4)
    for (int x = 0; x < 390; x += 4) ink += s.At(x, y) != 0xFFFFFF ? 1 : 0;
  CHECK(ink > 100);

  // The chart sheet fills what it's given.
  size_t chartSheet = book->SheetCount();
  for (size_t i = 0; i < book->SheetCount(); ++i)
    if (book->GetSheet(i).Kind() == SheetKind::Chartsheet) chartSheet = i;
  CHECK(chartSheet < book->SheetCount());
  if (chartSheet >= book->SheetCount()) return;
  auto sheet = SheetObjects::Load(*book, chartSheet);
  CHECK(sheet->IsChartSheet());
  drawtest::Surface c(300, 200);
  {
    draw::D2DPainter p(c.dc.Get());
    sheet->DrawChartSheet(p, {0, 0, 300, 200});
  }
  CHECK(c.End());
  ink = 0;
  for (int y = 0; y < 200; y += 4)
    for (int x = 0; x < 300; x += 4) ink += c.At(x, y) != 0xFFFFFF ? 1 : 0;
  CHECK(ink > 50);
}

TEST(sheet_objects_notes) {
  auto book = OpenSheetFile("office/sheet/notes.xlsx");
  if (!book) return;
  auto notes = SheetObjects::Load(*book, 0);
  CHECK(notes->Notes().size() == 4);
  const CellNote* b2 = notes->NoteAt(1, 1);
  CHECK(b2 && !b2->threaded && b2->text == L"A short note." && b2->author == L"Featherweight tests");
  CHECK(notes->NoteAt(1, 2) == nullptr);
  if (b2) {
    const draw::Size size = notes->NoteSize(*b2);
    CHECK(std::fabs(size.width - 108) < 0.5f && std::fabs(size.height - 59.25f) < 0.5f);  // 144 x 79 px
  }
  GridLayout layout;
  notes->Place(layout);
  drawtest::Surface s(200, 100);
  {
    draw::D2DPainter p(s.dc.Get());
    notes->Draw(p, {0, 0, 200, 100}, {});
  }
  CHECK(s.End());
  CHECK(drawtest::Red(s.At(94, 16)) > 200 && drawtest::Green(s.At(94, 16)) < 80);  // B2's red corner
  CHECK(s.At(60, 25) == 0xFFFFFF);

  auto threads = SheetObjects::Load(*book, 1);
  const CellNote* t = threads->NoteAt(1, 1);
  CHECK(t && t->threaded && !t->resolved && t->author == L"Featherweight tests");
  CHECK(t && t->text == L"Featherweight tests: A threaded comment.\nFeatherweight tests: A reply to it.");
  const CellNote* d5 = threads->NoteAt(4, 3);
  CHECK(d5 && d5->threaded && d5->resolved);
  CHECK(t && t->legacyText.starts_with(L"[Threaded comment]") && t->legacyText.find(L"Reply:") != std::wstring::npos);
  CHECK(b2 && b2->legacyText.empty());
  if (t) {
    const draw::Size size = threads->NoteSize(*t);
    CHECK(size.width > 100 && size.height > 20);
    drawtest::Surface card(300, 200);
    {
      draw::D2DPainter p(card.dc.Get());
      threads->DrawNote(p, *t, {0, 0, size.width, size.height});
    }
    CHECK(card.End());
  }
}

// tests/sheet/objects.xlsx, as Excel writes shapes, text boxes, connectors, groups, pictures, a
// combo chart, a chart sheet and a shown note (tests/sheet/excel_objects.py).
TEST(sheet_objects_excel) {
  fw::chart::Install();
  auto book = OpenSheetFile("sheet/objects.xlsx");
  if (!book) return;
  const int sheet = book->FindSheet("Objects"), charts = book->FindSheet("Chart sheet");
  CHECK(sheet >= 0 && charts >= 0);
  if (sheet < 0 || charts < 0) return;
  auto objects = SheetObjects::Load(*book, static_cast<size_t>(sheet));
  DefaultLayout layout;
  objects->Place(layout);
  const auto placed = objects->Objects();
  auto find = [&](const char* name) -> const SheetObjects::Object* {
    for (const auto& o : placed)
      if (o.name == name) return &o;
    return nullptr;
  };
  for (const char* name : {"Rectangle text", "Rounded bold", "Text box", "Arrow", "Rotated", "Group", "Picture"})
    CHECK(find(name) != nullptr);
  size_t chartCount = 0;
  for (const auto& o : placed) chartCount += o.chart ? 1 : 0;
  CHECK_EQ(chartCount, 2u);
  if (const auto* r = find("Rectangle text")) CHECK(Close(r->rect, 300, 20, 420, 70));
  const CellNote* shown = objects->NoteAt(9, 5);
  CHECK(shown && shown->shown && shown->text == L"Excel: a note that stays shown.");
  CHECK(objects->NoteAt(13, 5) && !objects->NoteAt(13, 5)->shown);
  drawtest::Surface s(700, 480);
  {
    draw::D2DPainter p(s.dc.Get());
    objects->Draw(p, {0, 0, 700, 480}, {});
  }
  CHECK(s.End());
  CHECK(s.At(380, 180) == 0xFFFFE1);                                       // the shown note, over the group
  CHECK(drawtest::Blue(s.At(310, 60)) > 150 && drawtest::Red(s.At(310, 60)) < 120);  // the blue rectangle
  // Printed, the shown note stays off the page: the page setup doesn't print notes.
  drawtest::Surface printed(700, 480);
  {
    draw::D2DPainter p(printed.dc.Get());
    SheetObjects::DrawOptions options;
    options.printing = true;
    objects->Draw(p, {0, 0, 700, 480}, options);
  }
  CHECK(printed.End());
  CHECK(printed.At(380, 180) != 0xFFFFE1);
  for (const auto& o : placed) CHECK(o.prints);
  auto chartSheet = SheetObjects::Load(*book, static_cast<size_t>(charts));
  CHECK(chartSheet->IsChartSheet());
}
