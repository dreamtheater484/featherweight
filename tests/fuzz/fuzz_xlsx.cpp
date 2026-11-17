// Fuzz target: the workbook model (src/sheet/model) and its .xlsx reader and writer (S1): every
// sheet read, everything the grid and the formula engine ask of the model, a few changes, and the
// writer, which must give back what it read. Also what sits above the cells (src/sheet/objects):
// drawings, charts with their data references, notes and threaded comments, read, placed and
// drawn; and the table styles' formats.
// Seeds: tests/fuzz/seeds/xlsx (a small workbook with every structure the model reads) and
// tests/fuzz/seeds/package (corpus files as text).
//
// An input is a zip, or parts as text as fuzz_check reads them: each part starts on a line
// "@<part name>", and the rest of the input up to the next such line is its content.
#include <d2d1_1.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "pkg/Canonical.h"
#include "pkg/Package.h"
#include "pkg/Snapshot.h"
#include "pkg/Zip.h"
#include "chart/Chart.h"
#include "draw/D2DPainter.h"
#include "sheet/model/TableStyles.h"
#include "sheet/objects/SheetObjects.h"
#include "sheet/xlsx/Xlsx.h"

using namespace fw::pkg;
using namespace fw::sheet;

namespace {

Limits FuzzLimits() {
  Limits limits;
  limits.maxEntrySize = 16u << 20;
  limits.maxTotalSize = 64u << 20;
  limits.maxEntries = 4096;
  limits.maxStringLength = 1u << 20;
  return limits;
}

// Parts as text, stored in a zip (as in fuzz_check.cpp).
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

void Check(bool ok) {
  if (!ok) std::abort();
}

// What the grid, the number formats and the formula engine read.
void ReadEverything(const Workbook& book) {
  const Styles& styles = book.GetStyles();
  for (size_t i = 0; i < book.SheetCount(); ++i) {
    const Sheet& s = book.GetSheet(i);
    size_t cells = 0;
    int64_t lastRow = -1, lastCol = -1;
    s.ForEachCell(Range::All(), [&](const CellInfo& c) {
      ++cells;
      Check(c.row < kMaxRows && c.col < kMaxCols);
      Check(static_cast<int64_t>(c.row) > lastRow || (static_cast<int64_t>(c.row) == lastRow && static_cast<int64_t>(c.col) > lastCol));
      lastRow = c.row, lastCol = c.col;
      CellInfo again;
      Check(s.GetCell(c.row, c.col, again) && again.value.type == c.value.type);
      const Xf& xf = styles.CellXf(c.style);
      styles.Resolve(styles.FontOf(xf).color, 0);
      styles.Resolve(styles.FillOf(xf).fg, 0);
      styles.FormatCode(xf.numFmt);
      s.StyleAt(c.row, c.col);
      s.MergeAt(c.row, c.col);
      if (c.sharedString != kNone) {
        uint32_t last = 0;
        for (const RichRun& r : book.Strings().Runs(c.sharedString)) {
          Check(r.start >= last && r.start <= book.Strings().Text(c.sharedString).size());
          Check(r.font == kNone || r.font < book.Strings().Fonts().size());
          last = r.start;
        }
      }
      FormulaInfo f;
      if (c.hasFormula && s.GetFormula(c.row, c.col, f)) {
        Check(f.origin.row < kMaxRows && f.origin.col < kMaxCols);
        s.FormulaAttribute(f, "r1");
      }
      return true;
    });
    Check(cells == s.CellCount());
    s.ForEachRow(0, kMaxRows - 1, [&](uint32_t row, const RowInfo& r) {
      Check(row < kMaxRows && r.stored);
      return true;
    });
    for (const ColumnInfo& c : s.Columns()) Check(c.first <= c.last && c.last < kMaxCols);
    s.Column(0);
    s.Column(kMaxCols - 1);
    s.UsedRange();
    s.KeptXml();
  }
  for (const DefinedName& n : book.Names()) book.FindName(n.name, n.sheet);
  for (size_t i = 0; i < book.SheetCount(); ++i) Check(book.FindSheet(book.GetSheet(i).Name()) >= 0);
}

// A small software render target, made once.
struct Target {
  Microsoft::WRL::ComPtr<ID2D1Factory1> factory;
  Microsoft::WRL::ComPtr<IWICImagingFactory> wic;
  Microsoft::WRL::ComPtr<IWICBitmap> bitmap;
  Microsoft::WRL::ComPtr<ID2D1RenderTarget> target;
  Microsoft::WRL::ComPtr<ID2D1DeviceContext> dc;
  draw::D2DBitmapCache cache;

  Target() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    D2D1_FACTORY_OPTIONS opts{};
    Check(SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &opts,
                                      reinterpret_cast<void**>(factory.GetAddressOf()))));
    Check(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))));
    Check(SUCCEEDED(wic->CreateBitmap(64, 64, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap)));
    const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    Check(SUCCEEDED(factory->CreateWicBitmapRenderTarget(bitmap.Get(), props, &target)));
    Check(SUCCEEDED(target.As(&dc)));
    fw::chart::Install();
  }
};

bool Finite(const draw::Rect& r) {
  return std::isfinite(r.left) && std::isfinite(r.top) && std::isfinite(r.right) && std::isfinite(r.bottom);
}

// Every sheet's objects read, placed on a grid with a hidden column and row, and drawn; the table
// styles' formats for the first cells of each table.
void DrawObjects(Workbook& book) {
  struct Layout : SheetLayout {
    double ColumnLeft(uint32_t c) const override { return 48.0 * c - (c > 2 ? 48 : 0); }
    double RowTop(uint32_t r) const override { return 15.0 * r - (r > 3 ? 15 : 0); }
  } layout;
  static Target t;
  for (size_t i = 0; i < book.SheetCount(); ++i) {
    auto objects = SheetObjects::Load(book, i);
    objects->Place(layout);
    for (const SheetObjects::Object& o : objects->Objects()) Check(Finite(o.rect));
    for (const CellNote& n : objects->Notes()) Check(objects->NoteAt(n.row, n.col) != nullptr);
    t.target->BeginDraw();
    {
      draw::D2DPainter painter(t.dc.Get(), &t.cache);
      painter.Transform(draw::Matrix::Scale(0.1f, 0.1f));
      objects->Draw(painter, {0, 0, 640, 640}, {});
      if (!objects->Notes().empty()) {
        const CellNote& n = objects->Notes().front();
        const draw::Size size = objects->NoteSize(n);
        Check(std::isfinite(size.width) && std::isfinite(size.height));
        objects->DrawNote(painter, n, {0, 0, std::min(size.width, 640.f), std::min(size.height, 640.f)});
      }
      if (objects->IsChartSheet()) objects->DrawChartSheet(painter, {0, 0, 640, 640});
    }
    t.target->EndDraw();  // a failure (a device that can't draw something) is fine; a crash isn't
    t.cache.Clear();
    const Sheet& s = book.GetSheet(i);
    for (const Table& table : s.Tables()) {
      table.FindColumn("Amount");
      for (uint32_t r = table.ref.firstRow; r <= table.ref.lastRow && r < table.ref.firstRow + 6; ++r)
        for (uint32_t c = table.ref.firstCol; c <= table.ref.lastCol && c < table.ref.firstCol + 6; ++c)
          TableFormatAt(book.GetStyles(), s, r, c);
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
  std::unique_ptr<Workbook> book = xlsx::Read(std::move(pkg), st, {.loadAll = true});
  if (!book) return 0;
  ReadEverything(*book);
  DrawObjects(*book);

  // The never-damage bar: every part the model writes, written back, says what it said.
  std::vector<uint8_t> bytes;
  for (const std::string& name : xlsx::ModelParts(*book)) {
    std::string out, before, after;
    Check(book->GetPackage()->ReadPart(name, bytes).Ok());
    Check(xlsx::WritePart(*book, name, out).Ok());
    std::string_view original(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    Check(CanonicalizeXml(original, before, limits, name).Ok());
    Check(CanonicalizeXml(out, after, limits, name).Ok());
    if (before != after) {
      size_t at = 0;
      while (at < before.size() && at < after.size() && before[at] == after[at]) ++at;
      const size_t from = at > 80 ? at - 80 : 0;
      std::fprintf(stderr, "%s differs after canonicalization at %zu:\n  read:    %s\n  written: %s\n", name.c_str(), at,
                   before.substr(from, 200).c_str(), after.substr(from, 200).c_str());
      std::abort();
    }
  }

  // Changes, as recalculation makes them: each sheet's first cells, and a new one.
  for (size_t i = 0; i < book->SheetCount(); ++i) {
    Sheet& s = book->GetSheet(i);
    size_t before = s.CellCount();
    auto snapshot = s.Blocks();
    std::vector<CellAddr> first;
    s.ForEachCell(Range::All(), [&](const CellInfo& c) {
      first.push_back({c.row, c.col});
      return first.size() < 4;
    });
    for (const CellAddr& a : first) s.SetValue(a.row, a.col, Value::Text("changed"));
    s.SetValue(7, 3, Value::Number(1));
    Check(s.GetValue(7, 3).type == ValueType::Number);
    for (const CellAddr& a : first) Check(s.GetValue(a.row, a.col).text == "changed");
    Check(s.CellCount() == before || s.CellCount() == before + 1);
    std::string out;
    if (!s.Text().rootStart.empty()) Check(xlsx::WritePart(*book, s.PartName(), out).Ok());
  }
  return 0;
}
