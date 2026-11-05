// fwtool pdf-check (milestone D4): Export PDF measured against the screen. Each Word document is
// laid out as the app lays it out and its pages drawn twice, once by the screen's renderer and
// once into a PDF through draw::PdfDocument. The PDF is then checked:
//  - its structure, strictly: the header, every cross-reference entry pointing at its object, each
//    stream's length, the trailer;
//  - read back with PDFium: the page count and sizes, each page's text (the same characters as the
//    layout's lines), and its lines, which must break where the layout breaks them (the fidelity
//    measurement, with the PDF as the reference);
//  - its pages as PDFium renders them, against the screen's, in small blocks.
// A PDF the app exported (the script ops exportpdf: and pagepngs:) is checked the same way with
// --pngs DIR, against the pages the app drew, and its links, bookmarks and title are listed.
#include <windows.h>
#include <d2d1_1.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <fpdf_doc.h>
#include <fpdf_text.h>
#include <fpdfview.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>

#include "Common.h"
#include "Fidelity.h"
#include "LayoutHook.h"
#include "Reference.h"
#include "core/Strings.h"
#include "doc/layout/Layout.h"
#include "doc/model/Document.h"
#include "draw/D2DPainter.h"
#include "draw/Drawing.h"
#include "draw/Pdf.h"
#include "pkg/XmlDom.h"

using Microsoft::WRL::ComPtr;
namespace layout = fw::doc::layout;

namespace fwtool {
namespace {

// A page as gray pixels.
struct Gray {
  UINT width = 0, height = 0;
  std::vector<uint8_t> px;
};

uint8_t Luma(uint8_t b, uint8_t g, uint8_t r) { return static_cast<uint8_t>((r * 299 + g * 587 + b * 114) / 1000); }

void FromBgra(const uint8_t* data, UINT width, UINT height, size_t stride, Gray& out) {
  out.width = width;
  out.height = height;
  out.px.resize(static_cast<size_t>(width) * height);
  for (UINT y = 0; y < height; ++y) {
    const uint8_t* row = data + y * stride;
    for (UINT x = 0; x < width; ++x) out.px[static_cast<size_t>(y) * width + x] = Luma(row[x * 4], row[x * 4 + 1], row[x * 4 + 2]);
  }
}

struct Graphics {
  ComPtr<ID2D1Factory1> d2d;
  ComPtr<IWICImagingFactory> wic;
  bool Init() {
    D2D1_FACTORY_OPTIONS opts{};
    return SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &opts,
                                       reinterpret_cast<void**>(d2d.GetAddressOf()))) &&
           SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)));
  }
};

// A page drawn by the screen's renderer (Direct2D, in software), on white.
bool RenderScreen(Graphics& g, UINT width, UINT height, float scale, const std::function<void(draw::Painter&)>& paint,
                  Gray& out) {
  ComPtr<IWICBitmap> bitmap;
  ComPtr<ID2D1RenderTarget> target;
  ComPtr<ID2D1DeviceContext> dc;
  const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
      D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f,
      96.f);
  if (FAILED(g.wic->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap)) ||
      FAILED(g.d2d->CreateWicBitmapRenderTarget(bitmap.Get(), props, &target)) || FAILED(target.As(&dc)))
    return false;
  dc->BeginDraw();
  dc->Clear(D2D1::ColorF(1, 1, 1, 1));
  {
    draw::D2DPainter painter(dc.Get());
    painter.Transform(draw::Matrix::Scale(scale, scale));
    paint(painter);
  }
  if (FAILED(dc->EndDraw())) return false;
  std::vector<uint8_t> bgra(static_cast<size_t>(width) * height * 4);
  if (FAILED(bitmap->CopyPixels(nullptr, width * 4, static_cast<UINT>(bgra.size()), bgra.data()))) return false;
  FromBgra(bgra.data(), width, height, width * 4, out);
  return true;
}

// A page as PDFium renders it, on white.
bool RenderPdfium(FPDF_PAGE page, UINT width, UINT height, Gray& out) {
  FPDF_BITMAP bitmap = FPDFBitmap_Create(static_cast<int>(width), static_cast<int>(height), 0);
  if (!bitmap) return false;
  FPDFBitmap_FillRect(bitmap, 0, 0, static_cast<int>(width), static_cast<int>(height), 0xFFFFFFFF);
  FPDF_RenderPageBitmap(bitmap, page, 0, 0, static_cast<int>(width), static_cast<int>(height), 0, 0);
  FromBgra(static_cast<const uint8_t*>(FPDFBitmap_GetBuffer(bitmap)), width, height,
           static_cast<size_t>(FPDFBitmap_GetStride(bitmap)), out);
  FPDFBitmap_Destroy(bitmap);
  return true;
}

bool ReadPng(Graphics& g, const std::filesystem::path& file, Gray& out) {
  ComPtr<IWICBitmapDecoder> decoder;
  ComPtr<IWICBitmapFrameDecode> frame;
  ComPtr<IWICFormatConverter> converter;
  if (FAILED(g.wic->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)) ||
      FAILED(decoder->GetFrame(0, &frame)) || FAILED(g.wic->CreateFormatConverter(&converter)) ||
      FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                   WICBitmapPaletteTypeCustom)))
    return false;
  UINT w = 0, h = 0;
  converter->GetSize(&w, &h);
  std::vector<uint8_t> bgra(static_cast<size_t>(w) * h * 4);
  if (FAILED(converter->CopyPixels(nullptr, w * 4, static_cast<UINT>(bgra.size()), bgra.data()))) return false;
  FromBgra(bgra.data(), w, h, w * 4, out);
  return true;
}

// The share of blocks (kBlock pixels square) whose average gray differs by more than kTolerance:
// glyph edges smoothed differently average out, a missing or misplaced glyph, line or picture
// doesn't.
constexpr UINT kBlock = 6;
constexpr int kTolerance = 24;
double DifferingBlocks(const Gray& a, const Gray& b) {
  const UINT w = std::min(a.width, b.width), h = std::min(a.height, b.height);
  if (!w || !h) return 1;
  size_t blocks = 0, differing = 0;
  for (UINT by = 0; by + kBlock <= h; by += kBlock)
    for (UINT bx = 0; bx + kBlock <= w; bx += kBlock) {
      int sa = 0, sb = 0;
      for (UINT y = by; y < by + kBlock; ++y)
        for (UINT x = bx; x < bx + kBlock; ++x) {
          sa += a.px[static_cast<size_t>(y) * a.width + x];
          sb += b.px[static_cast<size_t>(y) * b.width + x];
        }
      ++blocks;
      if (std::abs(sa - sb) > kTolerance * static_cast<int>(kBlock * kBlock)) ++differing;
    }
  return blocks ? static_cast<double>(differing) / static_cast<double>(blocks) : 0;
}

// ---- Structure

bool ParseInt(std::string_view s, size_t& at, long long& value) {
  while (at < s.size() && (s[at] == ' ' || s[at] == '\r' || s[at] == '\n')) ++at;
  const auto [end, ec] = std::from_chars(s.data() + at, s.data() + s.size(), value);
  if (ec != std::errc()) return false;
  at = static_cast<size_t>(end - s.data());
  return true;
}

// The first problem with the file's structure, or empty: the strict reading a validator does of a
// classic cross-reference table, which a forgiving reader would repair without a word.
std::string CheckStructure(std::string_view pdf) {
  if (!pdf.starts_with("%PDF-1.")) return "no %PDF-1.x header";
  std::string_view end = pdf;
  while (!end.empty() && (end.back() == '\n' || end.back() == '\r' || end.back() == ' ')) end.remove_suffix(1);
  if (!end.ends_with("%%EOF")) return "doesn't end with %%EOF";
  const size_t sx = pdf.rfind("startxref");
  if (sx == std::string_view::npos) return "no startxref";
  size_t at = sx + 9;
  long long xref = 0;
  if (!ParseInt(pdf, at, xref) || xref <= 0 || static_cast<size_t>(xref) >= pdf.size())
    return "startxref isn't an offset in the file";
  if (pdf.substr(static_cast<size_t>(xref), 4) != "xref") return "startxref doesn't point at the cross-reference table";
  at = static_cast<size_t>(xref) + 4;
  long long first = -1, count = 0;
  if (!ParseInt(pdf, at, first) || !ParseInt(pdf, at, count) || first != 0 || count < 1)
    return "the cross-reference table doesn't start at object 0";
  while (at < pdf.size() && (pdf[at] == ' ' || pdf[at] == '\r' || pdf[at] == '\n')) ++at;
  if (at + static_cast<size_t>(count) * 20 > pdf.size()) return "the cross-reference table is cut short";
  std::vector<size_t> offsets(static_cast<size_t>(count), 0);
  for (long long i = 0; i < count; ++i) {
    const std::string_view e = pdf.substr(at + static_cast<size_t>(i) * 20, 20);
    const std::string_view eol = e.substr(18, 2);
    if (e[10] != ' ' || e[16] != ' ' || (e[17] != 'n' && e[17] != 'f') || (eol != " \n" && eol != "\r\n" && eol != " \r"))
      return "cross-reference entry " + std::to_string(i) + " isn't 20 bytes of the classic form";
    long long offset = 0, gen = 0;
    size_t p = 0;
    if (!ParseInt(e.substr(0, 10), p, offset) || (p = 0, !ParseInt(e.substr(11, 5), p, gen)))
      return "cross-reference entry " + std::to_string(i) + " has no numbers";
    if (i == 0 && (e[17] != 'f' || gen != 65535)) return "cross-reference entry 0 isn't the free list's head";
    if (e[17] == 'n') {
      const std::string head = std::to_string(i) + " " + std::to_string(gen) + " obj";
      if (static_cast<size_t>(offset) >= pdf.size() || pdf.substr(static_cast<size_t>(offset), head.size()) != head)
        return "cross-reference entry " + std::to_string(i) + " doesn't point at object " + std::to_string(i);
      offsets[static_cast<size_t>(i)] = static_cast<size_t>(offset);
    }
  }
  at += static_cast<size_t>(count) * 20;
  const size_t trailer = pdf.find("trailer", at);
  if (trailer != at && trailer != at + 1) return "no trailer right after the cross-reference table";
  const std::string_view dict = pdf.substr(trailer, sx - trailer);
  size_t sizeAt = dict.find("/Size");
  long long size = 0;
  if (sizeAt == std::string_view::npos || (sizeAt += 5, !ParseInt(dict, sizeAt, size)) || size != count)
    return "the trailer's /Size isn't the number of objects";
  for (const char* key : {"/Root ", "/Info ", "/ID"})
    if (dict.find(key) == std::string_view::npos) return std::string("the trailer has no ") + key;
  // Each object ends before the next begins, and each stream's /Length is its data's.
  auto direct = [&](std::string_view body, size_t from, long long& value) {
    size_t p = from;
    if (!ParseInt(body, p, value)) return false;
    size_t q = p;
    long long gen = 0;
    if (ParseInt(body, q, gen) && body.substr(q).starts_with(" R")) {  // an indirect length
      if (value <= 0 || value >= count || !offsets[static_cast<size_t>(value)]) return false;
      std::string_view obj = pdf.substr(offsets[static_cast<size_t>(value)]);
      size_t r = obj.find("obj") + 3;
      return ParseInt(obj, r, value);
    }
    return true;
  };
  for (size_t i = 1; i < offsets.size(); ++i) {
    if (!offsets[i]) continue;
    const size_t stop = pdf.find("endobj", offsets[i]);
    if (stop == std::string_view::npos || stop > static_cast<size_t>(xref)) return "object " + std::to_string(i) + " has no endobj";
    const std::string_view body = pdf.substr(offsets[i], stop - offsets[i]);
    const size_t stream = body.find(">>\nstream\n");
    if (stream == std::string_view::npos) continue;
    const size_t length = body.rfind("/Length ", stream);
    long long n = -1;
    if (length == std::string_view::npos || !direct(body, length + 8, n) || n < 0)
      return "stream " + std::to_string(i) + " has no /Length";
    const size_t data = stream + 10;
    if (data + static_cast<size_t>(n) > body.size() || !body.substr(data + static_cast<size_t>(n)).starts_with("\nendstream"))
      return "stream " + std::to_string(i) + "'s /Length isn't its data's";
  }
  return {};
}

// ---- Text

bool Ignored(wchar_t c) { return iswspace(c) || c == 0xAD || c == 0xFFFE || c == 0xFFFF || c == 0x200B || c == 0; }

// A page's characters, sorted, without white space: the PDF's text and the layout's lines have the
// same ones whichever order a reader finds them in.
std::wstring Characters(std::wstring text) {
  std::erase_if(text, Ignored);
  std::sort(text.begin(), text.end());
  return text;
}

std::wstring PdfiumText(FPDF_PAGE page) {
  std::wstring text;
  if (FPDF_TEXTPAGE t = FPDFText_LoadPage(page)) {
    for (int i = 0, n = FPDFText_CountChars(t); i < n; ++i) {
      if (FPDFText_IsGenerated(t, i) == 1) continue;
      // A hyphen ending a line comes back as U+0002, for readers that join the word again.
      const unsigned int c = FPDFText_IsHyphen(t, i) == 1 ? L'-' : FPDFText_GetUnicode(t, i);
      if (c >= 0x10000) {
        text += static_cast<wchar_t>(0xD800 + ((c - 0x10000) >> 10));
        text += static_cast<wchar_t>(0xDC00 + ((c - 0x10000) & 0x3FF));
      } else {
        text += static_cast<wchar_t>(c);
      }
    }
    FPDFText_ClosePage(t);
  }
  return text;
}

// The first characters that one side has and the other hasn't, for the report.
std::string Missing(const std::wstring& want, const std::wstring& have) {
  std::wstring a, b;
  std::set_difference(want.begin(), want.end(), have.begin(), have.end(), std::back_inserter(a));
  std::set_difference(have.begin(), have.end(), want.begin(), want.end(), std::back_inserter(b));
  auto show = [](std::wstring s) {
    s.erase(std::unique(s.begin(), s.end()), s.end());
    if (s.size() > 12) s.resize(12);
    std::string out;
    for (wchar_t c : s) {
      char buf[16];
      std::snprintf(buf, sizeof buf, c >= 0x20 && c < 0x7F ? "%c" : "U+%04X ", static_cast<unsigned>(c));
      out += buf;
    }
    return out;
  };
  std::string out;
  if (!a.empty()) out += std::to_string(a.size()) + " missing (" + show(a) + ")";
  if (!b.empty()) out += std::string(out.empty() ? "" : ", ") + std::to_string(b.size()) + " extra (" + show(b) + ")";
  return out;
}

// ---- Drawings, as DocView draws them

class Drawings {
 public:
  explicit Drawings(const fw::doc::Document& doc) : doc_(doc), images_(std::make_shared<draw::ImageCache>()) {
    if (const fw::doc::Part* main = doc.FindPart(fw::doc::PartKind::Document)) {
      theme_ = draw::PackageResources::FindTheme(doc.Package(), main->name);
      colorMap_ = draw::PackageResources::FindColorMap(doc.Package(), main->name);
    }
    slots_.resize(doc.Drawings().size());
  }
  void Draw(draw::Painter& painter, const layout::DrawingBox& box) {
    if (box.drawing >= slots_.size()) return;
    Slot& slot = slots_[box.drawing];
    const fw::doc::Drawing& d = doc_.Drawings()[box.drawing];
    if (!slot.tried) {
      slot.tried = true;
      const char* wrapper = d.kind == fw::doc::DrawingKind::DrawingML ? "w:drawing"
                            : d.kind == fw::doc::DrawingKind::Vml     ? "w:pict"
                            : d.kind == fw::doc::DrawingKind::Object  ? "w:object"
                                                                      : nullptr;
      if (wrapper && d.part < doc_.Parts().size()) {
        const std::string xml = std::string("<") + wrapper +
                                " xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">" +
                                doc_.DrawingXml(d) + "</" + wrapper + ">";
        slot.xml = std::make_unique<fw::pkg::XmlDom>();
        if (slot.xml->Parse(xml).Ok()) {
          slot.resources = std::make_unique<draw::PackageResources>(doc_.Package(), doc_.Parts()[d.part].name, theme_,
                                                                    colorMap_, images_);
          slot.drawing = d.kind == fw::doc::DrawingKind::DrawingML
                             ? draw::Drawing::FromWordDrawing(slot.xml->Root(), *slot.resources)
                             : draw::Drawing::FromVml(slot.xml->Root(), *slot.resources);
        }
      }
    }
    const draw::Rect frame{box.rect.left, box.rect.top, box.rect.right, box.rect.bottom};
    if (slot.drawing) {
      if (!slot.drawing->IsHidden()) slot.drawing->Draw(painter, frame);
      return;
    }
    draw::Pen pen;
    pen.brush = draw::Brush::Solid({0.75f, 0.75f, 0.75f, 1});
    pen.width = 0.75f;
    painter.StrokePath(draw::Path::FromRect(frame), pen);
  }

 private:
  struct Slot {
    bool tried = false;
    std::unique_ptr<fw::pkg::XmlDom> xml;
    std::unique_ptr<draw::PackageResources> resources;
    std::unique_ptr<draw::Drawing> drawing;
  };
  const fw::doc::Document& doc_;
  std::shared_ptr<draw::ImageCache> images_;
  std::shared_ptr<const draw::Theme> theme_;
  draw::ColorMap colorMap_;
  std::vector<Slot> slots_;
};

// ---- Checking one PDF

// Pages whose blocks differ more than this fail. Text drawn by two renderers, each with its own
// smoothing and hinting, stays well under it; a missing word or picture doesn't.
constexpr double kMaxDifferingBlocks = 0.02;

struct Result {
  bool ok = true;
  std::string notes;
  void Fail(const std::string& why) {
    ok = false;
    notes += (notes.empty() ? "" : "; ") + why;
  }
  void Note(const std::string& what) { notes += (notes.empty() ? "" : "; ") + what; }
};

std::string Percent(double share) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.2f%%", share * 100);
  return buf;
}

// The structure and PDFium's reading of a PDF; `page(i, pdfPage)` checks each page further.
FPDF_DOCUMENT Open(const std::string& bytes, size_t pages, Result& r) {
  if (const std::string problem = CheckStructure(bytes); !problem.empty()) r.Fail("structure: " + problem);
  std::string error;
  if (!InitPdfium(error)) {
    r.Fail(error);
    return nullptr;
  }
  FPDF_DOCUMENT doc = FPDF_LoadMemDocument64(bytes.data(), bytes.size(), nullptr);
  if (!doc) {
    r.Fail("PDFium can't read it (error " + std::to_string(FPDF_GetLastError()) + ")");
    return nullptr;
  }
  if (static_cast<size_t>(FPDF_GetPageCount(doc)) != pages)
    r.Fail("PDFium reads " + std::to_string(FPDF_GetPageCount(doc)) + " pages, not " + std::to_string(pages));
  return doc;
}

void PixelReport(const std::vector<double>& shares, Result& r) {
  if (shares.empty()) return;
  const auto worst = std::max_element(shares.begin(), shares.end());
  double sum = 0;
  for (double s : shares) sum += s;
  r.Note("pixels " + Percent(sum / static_cast<double>(shares.size())) + " (page " +
         std::to_string(worst - shares.begin() + 1) + ": " + Percent(*worst) + ")");
  if (*worst > kMaxDifferingBlocks)
    r.Fail("page " + std::to_string(worst - shares.begin() + 1) + " differs from the screen in " + Percent(*worst) +
           " of its blocks");
}

bool ReadFileBytes(const std::filesystem::path& path, std::string& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return !in.bad();
}

bool WriteFileBytes(const std::filesystem::path& path, std::string_view bytes) {
  std::ofstream out(path, std::ios::binary);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  return static_cast<bool>(out);
}

std::filesystem::path TempPdf() {
  wchar_t dir[MAX_PATH];
  GetTempPathW(MAX_PATH, dir);
  return std::filesystem::path(dir) / (L"fwtool-pdf-check-" + std::to_wstring(GetCurrentProcessId()) + L".pdf");
}

// A Word document: laid out, drawn both ways, and the PDF checked.
Result CheckDocument(Graphics& g, const Options& options, const std::filesystem::path& document) {
  Result r;
  const float scale = options.scale;
  std::string bytes;
  std::vector<Gray> screen;
  std::vector<std::pair<float, float>> sizes;
  fw::LayoutDump dump;
  std::string error;
  const bool laidOut = WithLayout(document, error, [&](const fw::doc::Document& doc, const layout::Layout& lay) {
    Drawings drawings(doc);
    auto drawing = [&](draw::Painter& p, const layout::DrawingBox& box) { drawings.Draw(p, box); };
    draw::PdfDocument pdf;
    for (size_t i = 0; i < lay.PageCount(); ++i) {
      const layout::Page& page = lay.GetPage(i);
      sizes.push_back({page.width, page.height});
      pdf.AddPage(page.width, page.height, [&](draw::Painter& p) { lay.Paint(p, i, drawing); });
      Gray gray;
      const UINT w = std::max(1u, static_cast<UINT>(std::ceil(page.width * scale)));
      const UINT h = std::max(1u, static_cast<UINT>(std::ceil(page.height * scale)));
      if (!RenderScreen(g, w, h, scale, [&](draw::Painter& p) { lay.Paint(p, i, drawing); }, gray))
        r.Fail("page " + std::to_string(i + 1) + " couldn't be drawn on the screen's renderer");
      screen.push_back(std::move(gray));
    }
    const std::vector<uint8_t> out = pdf.Finish();
    bytes.assign(out.begin(), out.end());
    dump = lay.Dump(fw::WideToUtf8(document.filename().wstring()));
  });
  if (!laidOut) {
    r.Fail(error);
    return r;
  }
  r.Note(std::to_string(sizes.size()) + " pages, " + std::to_string((bytes.size() + 1023) / 1024) + " KB");
  FPDF_DOCUMENT doc = Open(bytes, sizes.size(), r);
  if (!doc) return r;
  std::vector<double> shares;
  int textPages = 0;
  for (int i = 0; i < FPDF_GetPageCount(doc) && static_cast<size_t>(i) < sizes.size(); ++i) {
    FPDF_PAGE page = FPDF_LoadPage(doc, i);
    if (!page) {
      r.Fail("PDFium can't load page " + std::to_string(i + 1));
      continue;
    }
    if (std::abs(FPDF_GetPageWidthF(page) - sizes[i].first) > 0.01f || std::abs(FPDF_GetPageHeightF(page) - sizes[i].second) > 0.01f)
      r.Fail("page " + std::to_string(i + 1) + " isn't the layout's size");
    std::wstring lines;
    if (static_cast<size_t>(i) < dump.pages.size())
      for (const fw::LayoutLine& line : dump.pages[i].lines) lines += line.text;
    const std::wstring want = Characters(lines), have = Characters(PdfiumText(page));
    if (want != have) {
      if (!textPages++) r.Fail("page " + std::to_string(i + 1) + "'s text: " + Missing(want, have));
    }
    Gray pdfium;
    if (RenderPdfium(page, screen[i].width, screen[i].height, pdfium)) shares.push_back(DifferingBlocks(screen[i], pdfium));
    FPDF_ClosePage(page);
  }
  FPDF_CloseDocument(doc);
  if (textPages > 1) r.Note(std::to_string(textPages) + " pages' text differs");
  PixelReport(shares, r);
  // The lines, as the fidelity measurement finds them in a PDF.
  std::filesystem::path file = options.outDir.empty() ? TempPdf() : options.outDir / (document.stem().wstring() + L".pdf");
  if (!WriteFileBytes(file, bytes)) {
    r.Fail("can't write " + Display(file));
  } else if (std::optional<fw::LayoutDump> read = ReadPdfLines(file, error)) {
    // Without a structure tree a reader can't tell headers and notes from the main text, and
    // takes the lines in the order they're drawn: compare them all as one story, by position.
    // It also takes raised text (a note mark) for a line of its own: join pieces that touch.
    auto join = [](fw::LayoutDump& d) {
      for (fw::LayoutPage& page : d.pages) {
        auto& lines = page.lines;
        for (bool merged = true; merged;) {
          merged = false;
          for (size_t i = 0; i < lines.size() && !merged; ++i)
            for (size_t j = 0; j < lines.size() && !merged; ++j) {
              fw::LayoutLine &a = lines[i], &b = lines[j];
              if (i == j || std::abs(b.x - (a.x + a.width)) > 0.5 ||
                  std::abs(b.baseline - a.baseline) > 0.6 * std::max(a.size, b.size))
                continue;
              a.text += b.text;
              a.width = b.x + b.width - a.x;
              if (b.size > a.size) a.baseline = b.baseline, a.size = b.size;
              lines.erase(lines.begin() + static_cast<ptrdiff_t>(j));
              merged = true;
            }
        }
      }
    };
    join(*read);
    auto plain = [](fw::LayoutDump& d) {
      for (fw::LayoutPage& page : d.pages) {
        for (fw::LayoutLine& line : page.lines) line.story = fw::LayoutStory::Body;
        std::stable_sort(page.lines.begin(), page.lines.end(), [](const fw::LayoutLine& a, const fw::LayoutLine& b) {
          const long ya = std::lround(a.baseline * 2), yb = std::lround(b.baseline * 2);
          return ya != yb ? ya < yb : a.x < b.x;
        });
      }
    };
    plain(*read);
    plain(dump);
    const fidelity::Score s = fidelity::Compare(*read, dump);
    if (s.matched != s.lines) {
      std::string first;
      if (const fidelity::LineResult* d = s.FirstDifference()) {
        const fidelity::Line& l = s.reference.lines[d->line];
        if (static_cast<size_t>(l.page) < read->pages.size() && static_cast<size_t>(l.index) < read->pages[l.page].lines.size())
          first = ", first on page " + std::to_string(l.page + 1) + ": \"" +
                  fw::WideToUtf8(read->pages[l.page].lines[l.index].text.substr(0, 60)) + "\"";
      }
      r.Fail(std::to_string(s.lines - s.matched) + " of " + std::to_string(s.lines) + " lines read back differently" + first);
    }
  } else {
    r.Fail("lines: " + error);
  }
  if (options.outDir.empty()) DeleteFileW(file.c_str());
  return r;
}

// A PDF the app exported, against the pages it drew (pagepngs:).
Result CheckExported(Graphics& g, const Options& options, const std::filesystem::path& file) {
  Result r;
  std::string bytes;
  if (!ReadFileBytes(file, bytes)) {
    r.Fail("can't read it");
    return r;
  }
  std::vector<std::filesystem::path> pngs;
  for (size_t i = 1;; ++i) {
    std::filesystem::path png = options.pngs / (L"page-" + std::to_wstring(i) + L".png");
    if (!std::filesystem::exists(png)) break;
    pngs.push_back(png);
  }
  FPDF_DOCUMENT doc = Open(bytes, pngs.empty() ? 0 : pngs.size(), r);
  if (!doc) return r;
  const int count = FPDF_GetPageCount(doc);
  r.Note(std::to_string(count) + " pages, " + std::to_string((bytes.size() + 1023) / 1024) + " KB");
  int uris = 0, internal = 0;
  size_t chars = 0;
  std::vector<double> shares;
  for (int i = 0; i < count; ++i) {
    FPDF_PAGE page = FPDF_LoadPage(doc, i);
    if (!page) {
      r.Fail("PDFium can't load page " + std::to_string(i + 1));
      continue;
    }
    chars += Characters(PdfiumText(page)).size();
    int at = 0;
    FPDF_LINK link = nullptr;
    while (FPDFLink_Enumerate(page, &at, &link)) {
      if (FPDF_DEST dest = FPDFLink_GetDest(doc, link)) {
        if (FPDFDest_GetDestPageIndex(doc, dest) < 0) r.Fail("a link on page " + std::to_string(i + 1) + " goes nowhere");
        ++internal;
      } else if (FPDF_ACTION action = FPDFLink_GetAction(link); action && FPDFAction_GetType(action) == PDFACTION_URI) {
        ++uris;
      } else {
        r.Fail("a link on page " + std::to_string(i + 1) + " has no target");
      }
    }
    if (static_cast<size_t>(i) < pngs.size()) {
      Gray screen, pdfium;
      if (!ReadPng(g, pngs[i], screen)) r.Fail("can't read " + Display(pngs[i]));
      else if (RenderPdfium(page, screen.width, screen.height, pdfium)) shares.push_back(DifferingBlocks(screen, pdfium));
    }
    FPDF_ClosePage(page);
  }
  int bookmarks = 0;
  std::function<void(FPDF_BOOKMARK)> walk = [&](FPDF_BOOKMARK parent) {
    for (FPDF_BOOKMARK b = FPDFBookmark_GetFirstChild(doc, parent); b; b = FPDFBookmark_GetNextSibling(doc, b)) {
      ++bookmarks;
      FPDF_DEST dest = FPDFBookmark_GetDest(doc, b);
      if (!dest || FPDFDest_GetDestPageIndex(doc, dest) < 0) r.Fail("bookmark " + std::to_string(bookmarks) + " goes nowhere");
      walk(b);
    }
  };
  walk(nullptr);
  auto meta = [&](const char* key) {
    std::wstring value(256, L'\0');
    unsigned long n = FPDF_GetMetaText(doc, key, value.data(), static_cast<unsigned long>(value.size() * 2));
    value.resize(n >= 2 ? n / 2 - 1 : 0);
    return fw::WideToUtf8(value);
  };
  r.Note(std::to_string(chars) + " characters, " + std::to_string(uris) + " web links, " + std::to_string(internal) +
         " links inside, " + std::to_string(bookmarks) + " bookmarks, title \"" + meta("Title") + "\", author \"" +
         meta("Author") + "\"");
  FPDF_CloseDocument(doc);
  PixelReport(shares, r);
  return r;
}

}  // namespace

int RunPdfCheck(const Options& options) {
  Graphics g;
  if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) || !g.Init()) {
    std::fprintf(stderr, "Direct2D and WIC aren't available\n");
    return 1;
  }
  if (!options.outDir.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(options.outDir, ec);
  }
  int checked = 0, failed = 0;
  std::vector<std::filesystem::path> files;
  for (const auto& input : options.inputs)
    if (fw::ToLower(input.extension().wstring()) == L".pdf") files.push_back(input);
  for (const auto& f : CollectFiles(options.inputs))
    if (IsWordDocument(fw::WideToUtf8(f.filename().wstring()))) files.push_back(f);
  for (const auto& file : files) {
    const bool exported = fw::ToLower(file.extension().wstring()) == L".pdf";
    if (exported && options.pngs.empty()) {
      std::fprintf(stderr, "%s: a PDF needs --pngs DIR, the pages the app drew\n", Display(file).c_str());
      return 2;
    }
    const Result r = exported ? CheckExported(g, options, file) : CheckDocument(g, options, file);
    ++checked;
    if (!r.ok) ++failed;
    std::printf("%s %s: %s\n", r.ok ? "ok    " : "FAILED", Display(file).c_str(), r.notes.c_str());
    std::fflush(stdout);
  }
  std::printf("%d checked, %d failed\n", checked, failed);
  return failed || !checked ? 1 : 0;
}

}  // namespace fwtool
