// Fuzz target: the workbook model (src/sheet/model) and its .xlsx reader and writer (S1): every
// sheet read, everything the grid and the formula engine ask of the model, a few changes, and the
// writer, which must give back what it read. Then edits through the edit API (S2), undo, redo, the
// crash-recovery journal replayed, and a save that must open again. Also what sits above the cells (src/sheet/objects):
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

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "pkg/Canonical.h"
#include "pkg/Check.h"
#include "pkg/Package.h"
#include "pkg/Snapshot.h"
#include "pkg/Zip.h"
#include "chart/Chart.h"
#include "draw/D2DPainter.h"
#include "sheet/data/Rules.h"
#include "sheet/data/Shapes.h"
#include "sheet/edit/Edit.h"
#include "sheet/edit/Journal.h"
#include "sheet/model/TableStyles.h"
#include "sheet/objects/SheetObjects.h"
#include "sheet/xlsx/Internal.h"
#include "sheet/xlsx/Xlsx.h"

using namespace fw::pkg;
using namespace fw::sheet;
using edit::CellInput;
using edit::Editor;

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

void CheckAt(bool ok, int line) {
  if (!ok) {
    std::fprintf(stderr, "fuzz_xlsx.cpp:%d: check failed\n", line);
    std::abort();
  }
}
#define Check(ok) CheckAt((ok), __LINE__)

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

// What the conditional formatting, validation, table and Format panels read (s3-data): every
// sheet's rules and validations, written back as the panels write them; each table's filter from
// its part; each drawing object's format, written back with every part changed; each picture
// part's type.
void ReadRulesAndObjects(const Workbook& book) {
  const Package* package = book.GetPackage();
  std::vector<uint8_t> bytes;
  for (size_t i = 0; i < book.SheetCount(); ++i) {
    const Sheet& s = book.GetSheet(i);
    std::string xml, ext;
    const std::vector<data::CfRule> rules = data::ReadCfRules(s.Text());
    for (const data::CfRule& r : rules) data::ParseAreas(r.sqref);
    data::WriteCfRules(rules, s.Text(), xml, ext);
    const std::vector<data::Validation> valid = data::ReadValidations(s.Text());
    for (const data::Validation& v : valid) {
      data::ListItems(v);
      data::ParseAreas(v.sqref);
    }
    data::ValidationAt(valid, 1, 1);
    data::WriteValidations(valid, s.Text(), xml, ext);
    for (const Table& t : s.Tables()) {
      SheetFilter f;
      if (package && !t.part.empty() && package->ReadPart(t.part, bytes).Ok())
        xlsx::ReadTableFilter(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), f);
    }
  }
  if (!package) return;
  for (const auto& part : package->Parts()) {
    if (part.deleted || !package->ReadPart(part.name, bytes).Ok()) continue;
    const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (part.name.starts_with("/xl/media/")) {
      std::string extension, type;
      data::PictureType(text, extension, type);
    } else if (part.name.starts_with("/xl/drawings/") && part.name.ends_with(".xml") && part.name.find("/_rels/") == std::string::npos) {
      for (const edit::drawing::Object& o : edit::drawing::Objects(text)) {
        const std::string_view anchor = text.substr(o.begin, o.end - o.begin);
        data::ObjectFormat f = data::ReadObjectFormat(anchor);
        f.fill = data::ObjectFormat::Paint::Solid;
        f.fillRgb = 0x123456;
        f.line = data::ObjectFormat::Paint::None;
        f.lineWidth = 1.5;
        f.text = "a\nb & <c>";
        f.altText = "alt \"x\"";
        data::FormatObject(anchor, f, data::kFormatFill | data::kFormatLine | data::kFormatText | data::kFormatAltText);
      }
    }
  }
}

// A 1 x 1 PNG, for Insert > Pictures.
const uint8_t kPng[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52, 0x00,
                        0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4, 0x89, 0x00,
                        0x00, 0x00, 0x0A, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0x00, 0x01, 0x00, 0x00, 0x05, 0x00, 0x01,
                        0x0D, 0x0A, 0x2D, 0xB4, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};

// ---- Edits (S2): a program a hash of the input picks, made through the edit API as the app makes
// them, undo and redo among them. The journal replayed on the workbook read again must make every
// edit again and save the same parts; the save must open again (and pass the package check when
// the input did); undoing everything gives the cells back as they were read.

struct Rng {
  uint64_t s;
  uint64_t Next() {
    uint64_t z = (s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }
  uint32_t Below(uint32_t n) { return n ? static_cast<uint32_t>(Next() % n) : 0; }
  bool Chance(uint32_t percent) { return Below(100) < percent; }
};

uint64_t HashOf(const uint8_t* data, size_t size) {
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < size; ++i) h = (h ^ data[i]) * 1099511628211ull;
  return h;
}

// Every stored cell of every sheet, as text: what undoing everything must give back.
std::string CellsOf(const Workbook& book) {
  std::string out;
  for (size_t i = 0; i < book.SheetCount(); ++i) {
    const Sheet& s = book.GetSheet(i);
    out += "#" + s.Name() + "\n";
    s.ForEachCell(Range::All(), [&](const CellInfo& c) {
      out += std::to_string(c.row) + "," + std::to_string(c.col) + ":" + std::to_string(static_cast<int>(c.value.type)) +
             "/" + std::to_string(c.style) + "/";
      if (c.value.type == ValueType::Number) out += std::to_string(c.value.number);
      if (c.value.type == ValueType::String) out += std::string(c.value.text);
      FormulaInfo f;
      if (c.hasFormula && s.GetFormula(c.row, c.col, f)) out += "=" + std::string(f.text);
      out += "\n";
      return true;
    });
  }
  return out;
}

const char* const kFormulas[] = {"A1+1", "SUM(A1:B4)", "Sheet2!A1*2", "'Sheet 1'!B2", "A1+", "#REF!", "T[b]",
                                 "TRANSPOSE(A1:A3)", "Total*2", "Sheet1:Sheet2!A1", "B2:B4", "IF(A1>1,\"x\",1)"};
constexpr uint32_t kFormulaCount = static_cast<uint32_t>(std::size(kFormulas));

void EditOnce(Editor& ed, const Workbook& book, Rng& rng, int& groups, bool hadSheets) {
  const int sheets = static_cast<int>(book.SheetCount());
  const int sheet = rng.Chance(5) ? sheets : static_cast<int>(rng.Below(static_cast<uint32_t>(sheets)));
  auto row = [&] { return rng.Below(rng.Chance(3) ? kMaxRows : 40); };
  auto col = [&] { return rng.Below(rng.Chance(3) ? kMaxCols : 12); };
  auto range = [&] {
    uint32_t r = row(), c = col();
    return Range{r, c, std::min<uint32_t>(r + rng.Below(5), kMaxRows - 1), std::min<uint32_t>(c + rng.Below(4), kMaxCols - 1)};
  };
  auto input = [&] {
    switch (rng.Below(5)) {
      case 0: return CellInput::Number(static_cast<double>(rng.Below(1000)) / 8);
      case 1: return CellInput::Text(rng.Chance(20) ? std::string() : "t" + std::to_string(rng.Below(50)));
      case 2: return CellInput::Bool(rng.Chance(50));
      case 3: return CellInput::Error(static_cast<ErrorCode>(rng.Below(8)));
      default: return CellInput::Formula(kFormulas[rng.Below(kFormulaCount)]);
    }
  };
  const Styles& styles = book.GetStyles();
  const uint32_t pick = rng.Below(36);
  // FUZZ_XLSX_SKIP=n leaves out edit n (narrowing a finding down by hand).
  if (const char* skip = std::getenv("FUZZ_XLSX_SKIP"); skip && std::atoi(skip) == static_cast<int>(pick)) return;
  switch (pick) {
    case 28: {  // a table (s3-data): added over a range, renamed, a total row, resized, filtered, converted
      static const std::vector<Table> none;
      const std::vector<Table>& tables = sheet < sheets ? book.GetSheet(static_cast<size_t>(sheet)).Tables() : none;
      if (tables.empty() || rng.Chance(30)) {
        Table t;
        t.ref = range();
        t.autoFilter = rng.Chance(80);
        t.style = TableStyleInfo{rng.Chance(50) ? "TableStyleMedium2" : "TableStyleLight9", false, false, true, rng.Chance(30)};
        for (uint32_t c = t.ref.firstCol; c <= t.ref.lastCol; ++c) t.columns.push_back(TableColumn{0, "C" + std::to_string(c - t.ref.firstCol + 1)});
        ed.AddTable(sheet, t);
        break;
      }
      const size_t i = rng.Below(static_cast<uint32_t>(tables.size()));
      Table t = tables[i];
      const uint32_t op = rng.Below(5);
      if (const char* skip = std::getenv("FUZZ_XLSX_SKIP_TABLE"); skip && std::atoi(skip) == static_cast<int>(op)) break;
      switch (op) {
        case 0: t.name = t.displayName = "T" + std::to_string(rng.Below(5)); ed.SetTable(sheet, i, t); break;
        case 1:
          if (t.totalsRows) {
            t.totalsRows = 0;
            if (t.ref.lastRow > t.ref.firstRow + 1) --t.ref.lastRow;
          } else if (t.ref.lastRow + 1 < kMaxRows && !t.columns.empty()) {
            t.totalsRows = 1;
            ++t.ref.lastRow;
            t.columns.front().totalsLabel = "Total";
            t.columns.back().totalsFunction = rng.Chance(50) ? "sum" : "count";
          }
          ed.SetTable(sheet, i, t);
          break;
        case 2:
          t.ref.lastRow = std::min<uint32_t>(t.ref.firstRow + 1 + rng.Below(8), kMaxRows - 1);
          if (rng.Chance(50) && t.ref.lastCol + 1 < kMaxCols) {
            ++t.ref.lastCol;
            t.columns.push_back(TableColumn{0, "N" + std::to_string(rng.Below(9))});
          } else if (t.columns.size() > 1) {
            --t.ref.lastCol;
            t.columns.pop_back();
          }
          ed.SetTable(sheet, i, t);
          break;
        case 3: {
          SheetFilter f;
          f.ref = t.ref;
          if (rng.Chance(70) && !t.columns.empty()) {
            FilterColumn c;
            c.colId = rng.Below(static_cast<uint32_t>(t.columns.size()));
            c.kind = static_cast<FilterColumn::Kind>(rng.Below(5));
            c.values = {"t" + std::to_string(rng.Below(9))};
            c.conditions = {{"greaterThan", std::to_string(rng.Below(100))}};
            c.count = 1 + rng.Below(20);
            f.columns.push_back(std::move(c));
          }
          ed.SetTableFilter(sheet, i, f);
          break;
        }
        default: ed.RemoveTable(sheet, i); break;
      }
      break;
    }
    case 29: {  // conditional formats and validation (s3-data), as the panels write them
      if (sheet >= sheets) break;
      const Sheet& s = book.GetSheet(static_cast<size_t>(sheet));
      const Range r = range();
      const std::string sq = data::JoinAreas({r});
      std::string xml, ext;
      if (rng.Chance(60)) {
        std::vector<data::CfRule> rules = data::ReadCfRules(s.Text());
        if (!rules.empty() && rng.Chance(30)) {
          data::ClearCfRules(rules, r);
        } else {
          const uint32_t dxf = ed.AddDxf(data::CfPreset(static_cast<int>(rng.Below(6))));
          const std::string id = "{00000000-0000-0000-0000-00000000000" + std::to_string(rng.Below(10)) + "}";
          data::CfRule n;
          switch (rng.Below(9)) {
            case 0: n = data::CellIsRule(sq, static_cast<data::CfOp>(rng.Below(8)), "5", "10", dxf); break;
            case 1: n = data::ExpressionRule(sq, kFormulas[rng.Below(kFormulaCount)], dxf); break;
            case 2: n = data::TextRule(sq, static_cast<data::CfText>(rng.Below(4)), "t1", dxf); break;
            case 3: n = data::TopRule(sq, 1 + rng.Below(10), rng.Chance(50), rng.Chance(50), dxf); break;
            case 4: n = data::DuplicateRule(sq, rng.Chance(50), dxf); break;
            case 5: n = data::DateRule(sq, static_cast<int>(rng.Below(10)), dxf); break;
            case 6: n = data::DataBarRule(sq, static_cast<int>(rng.Below(12)), id); break;
            case 7: n = data::IconSetRule(sq, static_cast<int>(rng.Below(20)), id); break;
            default: n = data::ColorScaleRule(sq, static_cast<int>(rng.Below(12))); break;
          }
          rules.insert(rules.begin(), std::move(n));
        }
        data::WriteCfRules(rules, s.Text(), xml, ext);
        ed.SetSheetRules(sheet, Editor::RuleKind::ConditionalFormats, xml, ext, u"New Rule");
      } else {
        std::vector<data::Validation> rules = data::ReadValidations(s.Text());
        data::ClearValidations(rules, r);
        if (rng.Chance(70)) {
          data::Validation v;
          v.type = static_cast<data::Validation::Type>(rng.Below(8));
          v.op = static_cast<data::CfOp>(rng.Below(8));
          v.f1 = v.type == data::Validation::Type::List ? (rng.Chance(50) ? "\"a,b,c\"" : "Sheet2!$A$1:$A$3")
                 : v.type == data::Validation::Type::Custom ? kFormulas[rng.Below(kFormulaCount)]
                                                            : "1";
          v.f2 = "9";
          v.alert = static_cast<data::Validation::Alert>(rng.Below(3));
          v.prompt = rng.Chance(50) ? "Enter <a> & b" : "";
          v.sqref = sq;
          rules.push_back(std::move(v));
        }
        data::WriteValidations(rules, s.Text(), xml, ext);
        ed.SetSheetRules(sheet, Editor::RuleKind::Validations, xml, ext, u"Data Validation");
      }
      break;
    }
    case 30: {  // pictures and shapes (s3-data): inserted, formatted
      edit::drawing::Place p;
      p.from = {col(), row(), 0, 0};
      p.to = {p.from.col + 1 + rng.Below(4), p.from.row + rng.Below(6), static_cast<int64_t>(rng.Below(50000)), 0};
      p.cx = 914400;
      p.cy = rng.Chance(30) ? 0 : 685800;
      switch (rng.Below(3)) {
        case 0: ed.InsertPicture(sheet, std::string(reinterpret_cast<const char*>(kPng), sizeof kPng), p, rng.Chance(50) ? "alt & <x>" : ""); break;
        case 1: ed.InsertShape(sheet, static_cast<data::ShapeKind>(rng.Below(6)), p, rng.Chance(50) ? "t & <x>" : ""); break;
        default: {
          const std::string drawing = ed.SheetDrawing(sheet, false);
          if (drawing.empty()) break;
          const std::vector<edit::drawing::Object> objects = edit::drawing::Objects(ed.PartBytes(drawing));
          if (objects.empty()) break;
          const uint32_t id = objects[rng.Below(static_cast<uint32_t>(objects.size()))].id;
          const std::string anchor = ed.AnchorXml(drawing, id);
          data::ObjectFormat f = data::ReadObjectFormat(anchor);
          f.fill = static_cast<data::ObjectFormat::Paint>(rng.Below(3));
          f.fillRgb = rng.Below(0xFFFFFF);
          f.line = static_cast<data::ObjectFormat::Paint>(rng.Below(3));
          f.lineRgb = rng.Below(0xFFFFFF);
          f.lineWidth = rng.Below(8);
          f.text = rng.Chance(50) ? "x\ny & <z>" : "";
          f.altText = "a \"b\"";
          const std::string xml = data::FormatObject(anchor, f, 1u + rng.Below(15));
          if (!xml.empty()) ed.SetObjectXml(sheet, id, xml, u"Format Object");
        }
      }
      break;
    }
    case 26: {  // a chart (s3-charts)
      Editor::NewChart c;
      c.kind = static_cast<edit::ChartKind>(rng.Below(7));
      c.data = range();
      c.at = CellAddr{row(), col()};
      ed.InsertChart(sheet, c);
      break;
    }
    case 27: {  // the sheet's last chart, picture or shape: moved, deleted, copied, edited
      const std::vector<edit::drawing::Object> objects = edit::drawing::Objects(ed.PartBytes(ed.SheetDrawing(sheet, false)));
      if (objects.empty()) break;
      const uint32_t id = objects.back().id;
      edit::drawing::Place p;
      p.from = {col(), row(), static_cast<int64_t>(rng.Below(50000)), 0};
      p.to = {p.from.col + rng.Below(8), p.from.row + rng.Below(15), 0, static_cast<int64_t>(rng.Below(50000))};
      switch (rng.Below(4)) {
        case 0: ed.MoveObject(sheet, id, p); break;
        case 1: ed.DeleteObjects(sheet, {id}); break;
        case 2: ed.PasteObject(sheet, ed.CopyObject(sheet, id), p); break;
        default: {
          edit::ChartEdit e;
          if (rng.Chance(50)) e.kind = static_cast<edit::ChartKind>(rng.Below(7));
          if (rng.Chance(50)) e.title = edit::ChartText{rng.Chance(80), rng.Chance(50) ? "T & <t>" : ""};
          if (rng.Chance(50)) e.xTitle = edit::ChartText{rng.Chance(80), "x"};
          if (rng.Chance(50)) e.legend = " brtl"[rng.Below(5)];
          if (rng.Chance(50)) e.labels = rng.Chance(50);
          if (rng.Chance(30)) e.colors = {{rng.Below(3), rng.Chance(50) ? -1 : static_cast<int32_t>(rng.Below(4)), rng.Below(0xFFFFFF)}};
          ed.EditChart(sheet, id, e);
        }
      }
      break;
    }
    case 31: {  // links (Insert > Link, Ctrl+K; s3-review): a place, a web or mail address, or removed
      if (rng.Chance(25)) {
        ed.SetHyperlink(sheet, range(), std::nullopt);
        break;
      }
      Hyperlink link;
      switch (rng.Below(4)) {
        case 0: link.location = rng.Chance(50) ? "Sheet2!A1" : "N" + std::to_string(rng.Below(4)); break;
        case 1: link.target = "https://example.com/a?b=1&c=<d>#e"; break;
        case 2: link.target = "mailto:someone@example.com?subject=a%20b"; break;
        default: link.target = rng.Chance(50) ? "..\\files\\a b.xlsx" : "file:///C:/x/\"y\".txt"; break;
      }
      if (rng.Chance(50)) link.display = "Go & <see>";
      if (rng.Chance(30)) link.tooltip = "tip \"t\"";
      ed.SetHyperlink(sheet, range(), link);
      break;
    }
    case 32: {  // notes and threaded comments (New Note, New Comment; s3-review): added, replied to, resolved, deleted
      const CellAddr at{row(), col()};
      if (rng.Chance(50)) {
        if (rng.Chance(25)) {
          ed.SetNote(sheet, at, std::nullopt);
          break;
        }
        Editor::Note n;
        n.author = rng.Chance(50) ? "Roy" : "";
        n.text = rng.Chance(50) ? "Note & <b>\nline 2" : std::string(rng.Below(300), 'n');
        n.shown = rng.Chance(30);
        if (rng.Chance(40)) {
          n.width = rng.Below(400);
          n.height = rng.Below(300);
          for (uint32_t& a : n.anchor) a = rng.Below(rng.Chance(10) ? 20000 : 30);
        }
        ed.SetNote(sheet, at, n);
        break;
      }
      std::optional<Editor::Thread> t = sheet < sheets ? ed.ThreadAt(sheet, at) : std::nullopt;
      if (t && rng.Chance(25)) {
        ed.SetThread(sheet, at, std::nullopt);
        break;
      }
      if (!t || rng.Chance(30)) t = Editor::Thread{};
      if (rng.Chance(30)) t->resolved = !t->resolved;
      if (t->comments.empty() || rng.Chance(70)) t->comments.push_back({rng.Chance(50) ? "A & B" : "C", "Reply <" + std::to_string(rng.Below(9)) + ">", "", ""});
      ed.SetThread(sheet, at, t);
      break;
    }
    case 33: {  // protection (Protect Sheet, Protect Workbook, Format Cells > Protection; s3-review)
      edit::ProtectionPassword pw;
      if (rng.Chance(30)) pw.legacy = "CC1A";
      else if (rng.Chance(20)) pw = edit::ProtectionPassword{"SHA-512", "aGFzaA==", "c2FsdA==", 100000, ""};  // not made here: hashing is slow
      switch (rng.Below(4)) {
        case 0: {
          if (rng.Chance(30)) {
            ed.SetSheetProtection(sheet, std::nullopt);
            break;
          }
          edit::SheetProtection p;
          p.objects = rng.Chance(50);
          p.formatCells = rng.Chance(50);
          p.insertRows = rng.Chance(50);
          p.deleteColumns = rng.Chance(50);
          p.sort = rng.Chance(50);
          p.autoFilter = rng.Chance(50);
          p.selectLockedCells = rng.Chance(20);
          p.password = pw;
          ed.SetSheetProtection(sheet, p);
          break;
        }
        case 1: {
          if (rng.Chance(40)) {
            ed.SetWorkbookProtection(std::nullopt);
            break;
          }
          edit::WorkbookProtection p;
          p.lockWindows = rng.Chance(30);
          p.password = pw;
          ed.SetWorkbookProtection(p);
          break;
        }
        default: {
          const bool locked = rng.Chance(30), hidden = rng.Chance(30);
          ed.MapStyles(sheet, range(), [&](uint32_t s) {
            Xf x = book.GetStyles().CellXf(s);
            x.protection.locked = locked;
            x.protection.hidden = hidden;
            x.applyProtection = true;
            return ed.AddXf(x);
          });
        }
      }
      break;
    }
    case 34: {  // page setup (Page Setup, print preview; s3-print)
      if (sheet >= sheets) break;
      const Sheet& s = book.GetSheet(static_cast<size_t>(sheet));
      PageSetup p = s.Page();
      HeaderFooter hf = s.HeadersFooters();
      static const char* const kCodes[] = {"", "&L&\"Arial,Bold\"&12Page &P of &N", "&C&A & &F", "&R&D &T", "&L<x> & \"y\"",
                                           "&C&G", "&&&", "&L&K00FF00green&R&B&I&U&S&Xa&Yb"};
      switch (rng.Below(4)) {
        case 0:
          p.orientation = static_cast<Orientation>(rng.Below(3));
          p.paperSize = rng.Chance(80) ? 1 + rng.Below(10) : rng.Below(200);
          p.scale = rng.Chance(80) ? 10 + rng.Below(391) : rng.Below(1000);
          p.fitToWidth = rng.Below(4);
          p.fitToHeight = rng.Below(4);
          p.firstPageNumber = static_cast<int>(rng.Below(20)) - 5;
          p.useFirstPageNumber = rng.Chance(50);
          p.pageOrder = static_cast<PageOrder>(rng.Below(2));
          p.blackAndWhite = rng.Chance(30);
          p.draft = rng.Chance(20);
          p.cellComments = static_cast<CellComments>(rng.Below(3));
          p.errors = static_cast<PrintErrors>(rng.Below(4));
          break;
        case 1:
          p.margins.left = rng.Below(30) / 10.0;
          p.margins.right = rng.Below(30) / 10.0;
          p.margins.top = rng.Below(30) / 10.0;
          p.margins.bottom = rng.Below(30) / 10.0;
          p.margins.header = rng.Below(20) / 10.0;
          p.margins.footer = rng.Below(20) / 10.0;
          p.horizontalCentered = rng.Chance(50);
          p.verticalCentered = rng.Chance(50);
          p.headings = rng.Chance(50);
          p.gridLines = rng.Chance(50);
          break;
        default:
          hf.differentOddEven = rng.Chance(30);
          hf.differentFirst = rng.Chance(30);
          hf.scaleWithDoc = rng.Chance(70);
          hf.alignWithMargins = rng.Chance(70);
          hf.oddHeader = kCodes[rng.Below(static_cast<uint32_t>(std::size(kCodes)))];
          hf.oddFooter = kCodes[rng.Below(static_cast<uint32_t>(std::size(kCodes)))];
          if (hf.differentOddEven) hf.evenHeader = kCodes[rng.Below(static_cast<uint32_t>(std::size(kCodes)))];
          if (hf.differentFirst) hf.firstFooter = kCodes[rng.Below(static_cast<uint32_t>(std::size(kCodes)))];
      }
      ed.SetPageSetup(sheet, p, hf, rng.Chance(30));
      break;
    }
    case 35: {  // page breaks, print area and titles (s3-print)
      if (rng.Chance(60)) {
        std::vector<PageBreak> rows, cols;
        for (uint32_t n = rng.Below(4); n > 0; --n) rows.push_back(PageBreak{1 + row() % (kMaxRows - 1), 0, kMaxCols - 1, true, false});
        for (uint32_t n = rng.Below(3); n > 0; --n) cols.push_back(PageBreak{1 + col() % (kMaxCols - 1), 0, kMaxRows - 1, true, false});
        ed.SetPageBreaks(sheet, rows, cols);
        break;
      }
      static const char* const kAreas[] = {"Sheet1!$A$1:$C$9", "'Sheet 1'!$B$2:$D$4,'Sheet 1'!$F$1:$F$3", "Sheet1!$1:$2",
                                           "Sheet1!$A:$B,Sheet1!$1:$1", "#REF!"};
      const bool area = rng.Chance(50);
      if (rng.Chance(25)) ed.DeleteName(area ? "_xlnm.Print_Area" : "_xlnm.Print_Titles", sheet);
      else ed.SetName(DefinedName{area ? "_xlnm.Print_Area" : "_xlnm.Print_Titles", sheet, kAreas[rng.Below(static_cast<uint32_t>(std::size(kAreas)))], false});
      break;
    }
    case 24: {  // cut and paste
      const Range from = range();
      ed.MoveCells(sheet, from, CellAddr{row(), col()});
      break;
    }
    case 25: {
      if (rng.Chance(30)) {
        ed.SetFilter(sheet, std::nullopt);
        break;
      }
      SheetFilter f;
      f.ref = range();
      if (rng.Chance(60)) {
        FilterColumn c;
        c.colId = rng.Below(f.ref.lastCol - f.ref.firstCol + 1);
        c.kind = static_cast<FilterColumn::Kind>(rng.Below(5));
        c.values = {"t" + std::to_string(rng.Below(9))};
        c.blank = rng.Chance(50);
        c.conditions = {{rng.Chance(50) ? "greaterThan" : "equal", std::to_string(rng.Below(100))}};
        c.both = rng.Chance(50);
        c.count = 1 + rng.Below(20);
        c.percent = rng.Chance(30);
        f.columns.push_back(std::move(c));
      }
      ed.SetFilter(sheet, f);
      break;
    }
    case 0:
    case 1: ed.SetCell(sheet, row(), col(), input()); break;
    case 2: {
      std::vector<std::pair<CellAddr, CellInput>> cells;
      for (uint32_t n = rng.Below(6); n > 0; --n) cells.push_back({CellAddr{row(), col()}, input()});
      ed.SetCells(sheet, cells);
      break;
    }
    case 3: ed.ClearContents(sheet, range()); break;
    case 4: rng.Chance(50) ? ed.ClearAll(sheet, range()) : ed.ClearFormats(sheet, range()); break;
    case 5: {
      Font f = styles.Fonts().empty() ? Font{} : styles.Fonts()[0];
      f.bold = rng.Chance(50);
      f.size = 8 + rng.Below(10);
      Xf x;
      x.font = ed.AddFont(f);
      x.numFmt = rng.Chance(50) ? ed.AddNumFmt(rng.Chance(50) ? "0.00%" : "[Red]#,##0;(#,##0)") : rng.Below(50);
      x.applyFont = x.applyNumFmt = true;
      ed.SetStyle(sheet, range(), ed.AddXf(x));
      break;
    }
    case 6: {
      Fill fill;
      fill.pattern = PatternType::Solid;
      fill.fg = Color{Color::Kind::Rgb, 0xFF000000u | rng.Below(0xFFFFFF), 0};
      Border border;
      border.top.style = BorderStyle::Thin;
      const uint32_t fi = ed.AddFill(fill), bi = ed.AddBorder(border);
      ed.MapStyles(sheet, range(), [&](uint32_t s) {
        Xf x = book.GetStyles().CellXf(s);
        x.fill = fi;
        x.border = bi;
        x.applyFill = x.applyBorder = true;
        return ed.AddXf(x);
      });
      break;
    }
    case 7: {
      const uint32_t first = row(), last = first + rng.Below(3);
      switch (rng.Below(3)) {
        case 0: ed.SetRowHeight(sheet, first, last, rng.Below(60)); break;
        case 1: ed.HideRows(sheet, first, last, rng.Chance(50)); break;
        default: ed.SetRowOutline(sheet, first, last, static_cast<uint8_t>(rng.Below(9)), rng.Chance(30)); break;
      }
      break;
    }
    case 8: {
      const uint32_t first = col(), last = first + rng.Below(3);
      switch (rng.Below(3)) {
        case 0: ed.SetColumnWidth(sheet, first, last, rng.Below(40)); break;
        case 1: ed.HideColumns(sheet, first, last, rng.Chance(50)); break;
        default: ed.SetColumnOutline(sheet, first, last, static_cast<uint8_t>(rng.Below(9)), rng.Chance(30)); break;
      }
      break;
    }
    case 9: rng.Chance(50) ? ed.InsertRows(sheet, row(), 1 + rng.Below(3)) : ed.DeleteRows(sheet, row(), 1 + rng.Below(3)); break;
    case 10:
      rng.Chance(50) ? ed.InsertColumns(sheet, col(), 1 + rng.Below(3)) : ed.DeleteColumns(sheet, col(), 1 + rng.Below(3));
      break;
    case 11: {
      const auto dir = rng.Chance(50) ? Editor::Direction::DownOrUp : Editor::Direction::RightOrLeft;
      rng.Chance(50) ? ed.InsertCells(sheet, range(), dir) : ed.DeleteCells(sheet, range(), dir);
      break;
    }
    case 12: {
      const Range r = range();
      const bool rows = rng.Chance(50);
      std::vector<uint32_t> order(rows ? r.lastRow - r.firstRow + 1 : r.lastCol - r.firstCol + 1);
      for (uint32_t i = 0; i < order.size(); ++i) order[i] = i;
      for (size_t i = order.size(); i > 1; --i) std::swap(order[i - 1], order[rng.Below(static_cast<uint32_t>(i))]);
      std::vector<Editor::SortKey> keys;
      if (rng.Chance(50)) keys.push_back({rows ? r.firstCol : r.firstRow, rng.Chance(50)});
      rows ? ed.SortRows(sheet, r, order, keys) : ed.SortColumns(sheet, r, order, keys);
      break;
    }
    case 13: rng.Chance(60) ? ed.Merge(sheet, range()) : ed.Unmerge(sheet, range()); break;
    case 14:
      switch (rng.Below(4)) {
        case 0: ed.AddSheet(static_cast<int>(rng.Below(static_cast<uint32_t>(sheets) + 1)) - 1); break;
        case 1: ed.RenameSheet(sheet, rng.Chance(50) ? "Data & More" : "S" + std::to_string(rng.Below(9))); break;
        case 2: ed.MoveSheet(sheet, static_cast<int>(rng.Below(static_cast<uint32_t>(sheets)))); break;
        default: ed.DeleteSheet(sheet); break;
      }
      break;
    case 15:
      switch (rng.Below(3)) {
        case 0: ed.SetTabColor(sheet, Color{Color::Kind::Theme, rng.Below(10), -0.25}); break;
        case 1: ed.SetSheetState(sheet, static_cast<SheetState>(rng.Below(3))); break;
        default: ed.FreezePanes(sheet, rng.Below(4), rng.Below(4)); break;
      }
      break;
    case 16:
      if (rng.Chance(70)) ed.SetName(DefinedName{"N" + std::to_string(rng.Below(4)), rng.Chance(30) ? sheet : -1,
                                                 kFormulas[rng.Below(kFormulaCount)], false});
      else ed.DeleteName("N" + std::to_string(rng.Below(4)), rng.Chance(30) ? sheet : -1);
      break;
    case 17:
      switch (rng.Below(4)) {
        case 0: ed.SetActiveSheet(sheet); break;
        case 1: ed.SetSelection(sheet, CellAddr{row(), col()}, {range()}); break;
        case 2: ed.SetTopLeft(sheet, CellAddr{row(), col()}); break;
        default: ed.SetZoom(sheet, rng.Below(500)); break;
      }
      break;
    case 18:
    case 19: ed.Undo(1 + rng.Below(3)); break;
    case 20: ed.Redo(1 + rng.Below(2)); break;
    case 21:
      if (groups < 2) {
        ed.BeginGroup(u"Group");
        ++groups;
      }
      break;
    case 22:
      if (groups > 0) {
        rng.Chance(30) ? ed.CancelGroup() : ed.EndGroup();
        --groups;
      }
      break;
    default: {
      CellInput in = CellInput::Formula("A1:A2*2");
      in.array = true;
      const uint32_t r = row(), c = col();
      in.arrayRange = Range{r, c, std::min(r + 1, kMaxRows - 1), c};
      ed.SetCell(sheet, r, c, in);
      break;
    }
  }
  Check(book.SheetCount() > 0 || !hadSheets);  // the last sheet is never deleted
}

// The parts of a saved file, by name: their bytes as text.
std::vector<std::pair<std::string, std::string>> PartsOf(const std::vector<uint8_t>& zip, const Limits& limits) {
  Status st;
  auto pkg = Package::Open(Snapshot::FromBytes(std::vector<uint8_t>(zip)), limits, st);
  Check(pkg != nullptr);
  std::vector<std::pair<std::string, std::string>> out;
  std::vector<uint8_t> bytes;
  for (const auto& part : pkg->Parts()) {
    Check(pkg->ReadPart(part.name, bytes).Ok());
    out.push_back({part.name, std::string(bytes.begin(), bytes.end())});
  }
  return out;
}

// Saved as the app saves (BeginSave, PrepareSave, WriteSave, CommitSave); empty when refused.
std::vector<uint8_t> Save(Workbook& book, Editor& ed) {
  ed.BeginSave();
  xlsx::SavePlan plan;
  MemorySink sink;
  if (!xlsx::PrepareSave(book, ed, plan, book.Format()).Ok() || !xlsx::WriteSave(book, plan, sink).Ok()) {
    ed.EndSave();
    return {};
  }
  xlsx::CommitSave(book, ed, plan);
  return std::move(sink.bytes);
}

void EditAndSave(const std::vector<uint8_t>& zip, const Limits& limits, uint64_t seed) {
  auto read = [&] {
    Status st;
    auto pkg = Package::Open(Snapshot::FromBytes(std::vector<uint8_t>(zip)), limits, st);
    return pkg ? xlsx::Read(std::move(pkg), st, {.loadAll = true}) : nullptr;
  };
  std::unique_ptr<Workbook> book = read(), again = read(), fresh = read();
  if (!book || !again || !fresh) return;
  const std::string asRead = CellsOf(*fresh);
  Editor ed(*book);
  edit::Journal journal;
  ed.SetJournal(&journal);
  Rng rng{seed};
  int groups = 0;
  const uint32_t steps = 8 + rng.Below(24);
  const bool hadSheets = book->SheetCount() > 0;
  for (uint32_t i = 0; i < steps; ++i) EditOnce(ed, *book, rng, groups, hadSheets);
  for (; groups > 0; --groups) ed.EndGroup();

  // The journal makes every edit again, alike.
  Editor replayed(*again);
  size_t applied = 0;
  edit::Result r = replayed.Replay(journal.All(), &applied);
  if (!r) std::fprintf(stderr, "replay stopped after %zu records\n", applied);
  Check(r.ok);
  const std::string edited = CellsOf(*book), remade = CellsOf(*again);
  if (edited != remade) {
    size_t at = 0;
    while (at < edited.size() && at < remade.size() && edited[at] == remade[at]) ++at;
    const size_t from = edited.rfind('\n', at) == std::string::npos ? 0 : edited.rfind('\n', at) + 1;
    std::fprintf(stderr, "the journal remade other cells:\n  edited: %s\n  remade: %s\n", edited.substr(from, 120).c_str(),
                 remade.substr(from, 120).c_str());
  }
  Check(edited == remade);

  const size_t history = ed.UndoNames().size();
  const std::vector<uint8_t> saved = Save(*book, ed), savedAgain = Save(*again, replayed);
  Check(saved.empty() == savedAgain.empty());
  if (!saved.empty()) {
    Check(PartsOf(saved, limits) == PartsOf(savedAgain, limits));
    Status st;
    auto pkg = Package::Open(Snapshot::FromBytes(std::vector<uint8_t>(saved)), limits, st);
    Check(pkg != nullptr);
    Status before;
    auto original = Package::Open(Snapshot::FromBytes(std::vector<uint8_t>(zip)), limits, before);
    if (original && CheckPackage(*original).Ok()) {
      CheckReport report = CheckPackage(*pkg);
      for (const std::string& e : report.errors) std::fprintf(stderr, "saved file: %s\n", e.c_str());
      Check(report.Ok());
    }
    const bool reads = xlsx::Read(std::move(pkg), st, {.loadAll = true}) != nullptr;
    if (!reads) {
      std::fprintf(stderr, "the save doesn't read: %s\n", st.message.c_str());
      if (std::getenv("FUZZ_XLSX_DUMP")) {
        if (FILE* f = std::fopen(std::getenv("FUZZ_XLSX_DUMP"), "wb")) {
          std::fwrite(saved.data(), 1, saved.size(), f);
          std::fclose(f);
        }
      }
    }
    Check(reads);
  }

  // Undoing everything gives the cells back (unless a save that removed sheets ended the history).
  if (ed.UndoNames().size() != history) return;
  while (ed.CanUndo()) Check(ed.Undo().ok);
  const std::string undone = CellsOf(*book);
  if (undone != asRead) std::fprintf(stderr, "undone:\n%s\nas read:\n%s\n", undone.c_str(), asRead.c_str());
  Check(undone == asRead);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const Limits limits = FuzzLimits();
  std::string_view input(reinterpret_cast<const char*>(data), size);
  std::vector<uint8_t> zip = input.starts_with("PK\x03\x04") ? std::vector<uint8_t>(data, data + size) : ZipFromText(input);

  const std::vector<uint8_t> input0 = zip;
  Status st;
  auto pkg = Package::Open(Snapshot::FromBytes(std::move(zip)), limits, st);
  if (!pkg) return 0;
  std::unique_ptr<Workbook> book = xlsx::Read(std::move(pkg), st, {.loadAll = true});
  if (!book) return 0;
  ReadEverything(*book);
  DrawObjects(*book);
  ReadRulesAndObjects(*book);

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

  EditAndSave(input0, limits, HashOf(data, size));
  return 0;
}
