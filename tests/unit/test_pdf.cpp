// Export PDF (milestone D4): the font subsetter (draw::sfnt) and the PDF writer (draw::PdfDocument).
// fwtool pdf-check measures exported documents against the screen; these check the pieces.
#include "TestHarness.h"

#include <windows.h>

#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "draw/Images.h"
#include "draw/Pdf.h"
#include "draw/Sfnt.h"
#include "miniz.h"

using Microsoft::WRL::ComPtr;
using namespace draw;

namespace {

std::vector<uint8_t> ArialFile() {
  wchar_t dir[MAX_PATH];
  GetWindowsDirectoryW(dir, MAX_PATH);
  std::ifstream in(std::wstring(dir) + L"\\Fonts\\arial.ttf", std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

ComPtr<IDWriteFontFace> ArialFace() {
  ComPtr<IDWriteFactory> dw;
  ComPtr<IDWriteFontCollection> fonts;
  ComPtr<IDWriteFontFamily> family;
  ComPtr<IDWriteFont> font;
  ComPtr<IDWriteFontFace> face;
  UINT32 index = 0;
  BOOL exists = FALSE;
  if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dw.GetAddressOf()))) ||
      FAILED(dw->GetSystemFontCollection(&fonts)) || FAILED(fonts->FindFamilyName(L"Arial", &index, &exists)) || !exists ||
      FAILED(fonts->GetFontFamily(index, &family)) ||
      FAILED(family->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, &font)) ||
      FAILED(font->CreateFontFace(&face)))
    return nullptr;
  return face;
}

uint16_t U16(const std::vector<uint8_t>& b, size_t at) { return at + 2 <= b.size() ? static_cast<uint16_t>(b[at] << 8 | b[at + 1]) : 0; }
uint32_t U32(const std::vector<uint8_t>& b, size_t at) {
  return at + 4 <= b.size() ? uint32_t{b[at]} << 24 | uint32_t{b[at + 1]} << 16 | uint32_t{b[at + 2]} << 8 | b[at + 3] : 0;
}

// A glyph's bytes in a font's glyf table, through loca.
std::vector<uint8_t> GlyphData(const sfnt::Tables& t, uint16_t glyph) {
  std::vector<uint8_t> head, loca, glyf;
  if (!t(sfnt::Tag("head"), head) || !t(sfnt::Tag("loca"), loca) || !t(sfnt::Tag("glyf"), glyf)) return {};
  const bool longLoca = U16(head, 50) == 1;
  const uint32_t from = longLoca ? U32(loca, glyph * 4u) : U16(loca, glyph * 2u) * 2u;
  const uint32_t to = longLoca ? U32(loca, glyph * 4u + 4) : U16(loca, glyph * 2u + 2) * 2u;
  if (from >= to || to > glyf.size()) return {};
  return std::vector<uint8_t>(glyf.begin() + from, glyf.begin() + to);
}

uint32_t FileSum(const std::vector<uint8_t>& b) {
  uint32_t sum = 0;
  for (size_t i = 0; i < b.size(); i += 4) sum += U32(b, i);
  return sum;
}

// Every stream of a PDF, inflated where it's compressed, one after another: what the pages draw,
// the ToUnicode maps.
std::string Streams(const std::string& pdf) {
  std::string out;
  for (size_t at = pdf.find(">>\nstream\n"); at != std::string::npos; at = pdf.find(">>\nstream\n", at + 1)) {
    const size_t dict = pdf.rfind("<<", at);
    const size_t length = pdf.rfind("/Length ", at);
    if (dict == std::string::npos || length == std::string::npos || length < dict) continue;
    const size_t n = std::stoul(pdf.substr(length + 8, 12));
    const std::string data = pdf.substr(at + 10, n);
    if (pdf.substr(dict, at - dict).find("/FlateDecode") == std::string::npos) {
      out += data;
      continue;
    }
    std::vector<unsigned char> buf(std::max<size_t>(1024, data.size() * 8));
    for (;;) {
      mz_ulong size = static_cast<mz_ulong>(buf.size());
      const int r = mz_uncompress(buf.data(), &size, reinterpret_cast<const unsigned char*>(data.data()), static_cast<mz_ulong>(data.size()));
      if (r == MZ_BUF_ERROR && buf.size() < (64u << 20)) {
        buf.resize(buf.size() * 4);
        continue;
      }
      if (r == MZ_OK) out.append(reinterpret_cast<const char*>(buf.data()), size);
      break;
    }
    out += "\n";
  }
  return out;
}

// The strict reading of the cross-reference table: every entry where its object is.
bool XrefPointsAtObjects(const std::string& pdf) {
  const size_t sx = pdf.rfind("startxref");
  if (sx == std::string::npos) return false;
  const size_t xref = std::stoul(pdf.substr(sx + 10, 12));
  if (pdf.compare(xref, 4, "xref") != 0) return false;
  const size_t line = pdf.find('\n', xref + 5);
  const size_t count = std::stoul(pdf.substr(xref + 7, line - xref - 7));
  for (size_t i = 1; i < count; ++i) {
    const std::string entry = pdf.substr(line + 1 + i * 20, 20);
    if (entry[17] != 'n') continue;
    const size_t offset = std::stoul(entry.substr(0, 10));
    const std::string head = std::to_string(i) + " 0 obj";
    if (pdf.compare(offset, head.size(), head) != 0) return false;
  }
  return pdf.find("trailer", line) != std::string::npos;
}

// An 8 x 8 gray JPEG, as WIC writes one.
std::vector<uint8_t> GrayJpeg() {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  ComPtr<IWICImagingFactory> wic;
  ComPtr<IWICBitmap> bitmap;
  ComPtr<IStream> stream;
  ComPtr<IWICBitmapEncoder> encoder;
  ComPtr<IWICBitmapFrameEncode> frame;
  std::vector<uint8_t> px(8 * 8 * 3, 0x80);
  WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) ||
      FAILED(wic->CreateBitmapFromMemory(8, 8, format, 8 * 3, static_cast<UINT>(px.size()), px.data(), &bitmap)) ||
      FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) ||
      FAILED(wic->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder)) ||
      FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) || FAILED(encoder->CreateNewFrame(&frame, nullptr)) ||
      FAILED(frame->Initialize(nullptr)) || FAILED(frame->SetSize(8, 8)) || FAILED(frame->SetPixelFormat(&format)) ||
      FAILED(frame->WriteSource(bitmap.Get(), nullptr)) || FAILED(frame->Commit()) || FAILED(encoder->Commit()))
    return {};
  STATSTG stat{};
  stream->Stat(&stat, STATFLAG_NONAME);
  std::vector<uint8_t> bytes(static_cast<size_t>(stat.cbSize.QuadPart));
  const LARGE_INTEGER zero{};
  ULONG read = 0;
  stream->Seek(zero, STREAM_SEEK_SET, nullptr);
  stream->Read(bytes.data(), static_cast<ULONG>(bytes.size()), &read);
  bytes.resize(read);
  return bytes;
}

GlyphRun Run(IDWriteFontFace* face, const UINT16* glyphs, const float* advances, uint32_t n, const char16_t* text) {
  GlyphRun run;
  run.face = face;
  run.emSize = 12;
  run.count = n;
  run.indices = glyphs;
  run.advances = advances;
  run.text = text;
  run.textLength = static_cast<uint32_t>(std::char_traits<char16_t>::length(text));
  return run;
}

}  // namespace

TEST(Sfnt_ReadsArial) {
  const std::vector<uint8_t> file = ArialFile();
  CHECK(!file.empty());
  if (file.empty()) return;
  const std::optional<sfnt::Info> info = sfnt::ReadInfo(sfnt::FileTables(file));
  CHECK(info.has_value());
  if (!info) return;
  CHECK(!info->cff && !info->variable && info->embeddable);
  CHECK_EQ(info->unitsPerEm, 2048);
  CHECK(info->numGlyphs > 1000);
  CHECK_EQ(info->postScriptName, std::string("ArialMT"));
  CHECK(info->ascent > 0 && info->descent < 0);
}

TEST(Sfnt_SubsetKeepsGlyphIdsAndComposites) {
  const std::vector<uint8_t> file = ArialFile();
  if (file.empty()) return;
  const sfnt::Tables tables = sfnt::FileTables(file);
  const std::optional<sfnt::Info> info = sfnt::ReadInfo(tables);
  if (!info) return;
  // A composite glyph and its first part.
  uint16_t composite = 0, part = 0;
  for (uint16_t g = 1; g < info->numGlyphs && !composite; ++g) {
    const std::vector<uint8_t> data = GlyphData(tables, g);
    if (data.size() >= 14 && static_cast<int16_t>(U16(data, 0)) < 0) {
      composite = g;
      part = U16(data, 12);
    }
  }
  CHECK(composite != 0 && part != 0 && part != composite);
  const uint16_t plain = 36;  // 'A' in Arial
  const std::vector<uint8_t> subset = sfnt::Subset(tables, {plain, composite});
  CHECK(!subset.empty() && subset.size() < file.size() / 4);
  const sfnt::Tables kept = sfnt::FileTables(subset);
  const std::optional<sfnt::Info> again = sfnt::ReadInfo(kept);
  CHECK(again && again->numGlyphs == info->numGlyphs && again->unitsPerEm == info->unitsPerEm);
  // The same ids, the same outlines; the composite's part came along; the rest are empty.
  CHECK(GlyphData(kept, plain) == GlyphData(tables, plain));
  CHECK(!GlyphData(kept, part).empty());
  CHECK(GlyphData(kept, composite).size() >= GlyphData(tables, composite).size());
  CHECK(GlyphData(kept, 1000).empty() || plain == 1000 || part == 1000 || composite == 1000);
  CHECK_EQ(FileSum(subset), 0xB1B0AFBAu);
  CHECK(sfnt::Subset(kept, {plain, composite}) == subset);
  // A TrueType font has no CFF to embed whole.
  CHECK(sfnt::Whole(tables).empty());
}

TEST(Sfnt_RefusesWhatIsntAFont) {
  std::vector<uint8_t> junk(4096, 0x5A);
  CHECK(!sfnt::ReadInfo(sfnt::FileTables(junk)).has_value());
  CHECK(sfnt::Subset(sfnt::FileTables(junk), {0, 1}).empty());
  const std::vector<uint8_t> file = ArialFile();
  if (file.size() < 2000) return;
  // Cut short: the tables past the end are gone, so it isn't a font any more.
  const std::vector<uint8_t> cut(file.begin(), file.begin() + 2000);
  CHECK(!sfnt::ReadInfo(sfnt::FileTables(cut)).has_value());
  CHECK(sfnt::Subset(sfnt::FileTables(cut), {0, 36}).empty());
}

TEST(Pdf_TextLinksOutlineAndInfo) {
  ComPtr<IDWriteFontFace> face = ArialFace();
  CHECK(face != nullptr);
  if (!face) return;
  const UINT32 points[] = {'H', 'i'};
  UINT16 glyphs[2];
  face->GetGlyphIndices(points, 2, glyphs);
  const float advances[] = {8.67f, 2.67f};
  PdfDocument pdf;
  pdf.AddPage(200, 100, [&](Painter& p) {
    CHECK(p.WantsText());
    p.FillRect({10, 10, 50, 30}, Brush::Solid({1, 0, 0, 1}));
    p.DrawGlyphs({20, 60}, Run(face.Get(), glyphs, advances, 2, u"Hi"), Brush::Solid({0, 0, 0, 1}));
  });
  pdf.AddPage(200, 100, [](Painter&) {});
  pdf.AddLink(0, {20, 50, 40, 62}, "https://example.com/");
  pdf.AddLink(0, {60, 50, 80, 62}, 1, 20);
  pdf.AddOutline(0, u"Heading", 1, 20);
  pdf.SetInfo(u"A title", u"An author");
  CHECK_EQ(pdf.PageCount(), 2u);
  const std::vector<uint8_t> bytes = pdf.Finish();
  const std::string s(bytes.begin(), bytes.end());
  CHECK(s.starts_with("%PDF-1.7"));
  CHECK(s.find("%%EOF", s.size() - 8) != std::string::npos);
  CHECK(XrefPointsAtObjects(s));
  // The font: a subset, with its text.
  CHECK(s.find("+ArialMT") != std::string::npos);
  CHECK(s.find("/FontFile2 ") != std::string::npos);
  CHECK(s.find("/ToUnicode ") != std::string::npos);
  const std::string streams = Streams(s);
  CHECK(streams.find("beginbfchar") != std::string::npos);
  CHECK(streams.find("<0048>") != std::string::npos && streams.find("<0069>") != std::string::npos);  // H, i
  CHECK(streams.find(" 12 Tf") != std::string::npos);  // the size where readers look for it
  CHECK(streams.find("]TJ") != std::string::npos);
  CHECK(streams.find("1 0 0 rg") != std::string::npos);  // the red rectangle
  // Links, the outline and the title.
  CHECK(s.find("/S/URI/URI(https://example.com/)") != std::string::npos);
  CHECK(s.find("/Subtype/Link") != std::string::npos && s.find("/XYZ 0 80 null") != std::string::npos);
  CHECK(s.find("/Type/Outlines") != std::string::npos && s.find("/PageMode/UseOutlines") != std::string::npos);
  CHECK(s.find("/Title<FEFF0041") != std::string::npos);  // "A title", as UTF-16
}

TEST(Pdf_GroupsAreTransparencyGroups) {
  PdfDocument pdf;
  pdf.AddPage(100, 100, [](Painter& p) {
    p.BeginGroup(0.5f);
    p.FillRect({0, 0, 50, 50}, Brush::Solid({0, 0, 1, 1}));
    p.FillRect({25, 25, 75, 75}, Brush::Solid({0, 0, 1, 1}));
    p.Restore();
  });
  const std::vector<uint8_t> bytes = pdf.Finish();
  const std::string s(bytes.begin(), bytes.end());
  CHECK(XrefPointsAtObjects(s));
  CHECK(s.find("/Group<</S/Transparency>>") != std::string::npos);
  CHECK(s.find("/ca 0.5/CA 0.5") != std::string::npos);
  CHECK(Streams(s).find(" Do") != std::string::npos);
}

TEST(Pdf_JpegGoesInAsItIs) {
  const std::vector<uint8_t> jpeg = GrayJpeg();
  CHECK(jpeg.size() > 100);
  const std::shared_ptr<Image> image = Image::Open(jpeg);
  CHECK(image != nullptr);
  if (!image) return;
  const std::shared_ptr<const Bitmap> pixels = image->Pixels();
  CHECK(pixels != nullptr && pixels->Jpeg() != nullptr);
  if (!pixels) return;
  PdfDocument pdf;
  pdf.AddPage(100, 100, [&](Painter& p) { p.DrawBitmap(*pixels, {10, 10, 90, 90}); });
  const std::vector<uint8_t> bytes = pdf.Finish();
  const std::string s(bytes.begin(), bytes.end());
  CHECK(XrefPointsAtObjects(s));
  CHECK(s.find("/DCTDecode") != std::string::npos);
  CHECK(s.find(std::string(reinterpret_cast<const char*>(jpeg.data()), jpeg.size())) != std::string::npos);
}

TEST(Pdf_SpacesGoWithTheirWords) {
  // "a b" as three runs, the space alone, with something drawn after each word (an underline):
  // the space goes into a text object with a word, so readers find it.
  ComPtr<IDWriteFontFace> face = ArialFace();
  if (!face) return;
  const UINT32 points[] = {'a', ' ', 'b'};
  UINT16 glyphs[3];
  face->GetGlyphIndices(points, 3, glyphs);
  const float advances[] = {6.67f, 3.33f, 6.67f};
  PdfDocument pdf;
  pdf.AddPage(100, 50, [&](Painter& p) {
    const Brush black = Brush::Solid({0, 0, 0, 1});
    p.DrawGlyphs({10, 30}, Run(face.Get(), &glyphs[0], &advances[0], 1, u"a"), black);
    p.FillRect({10, 32, 16.67f, 33}, black);
    p.DrawGlyphs({16.67f, 30}, Run(face.Get(), &glyphs[1], &advances[1], 1, u" "), black);
    p.FillRect({16.67f, 32, 20, 33}, black);
    p.DrawGlyphs({20, 30}, Run(face.Get(), &glyphs[2], &advances[2], 1, u"b"), black);
  });
  const std::vector<uint8_t> bytes = pdf.Finish();
  const std::string streams = Streams(std::string(bytes.begin(), bytes.end()));
  char joined[16];
  std::snprintf(joined, sizeof joined, "[<%04X>", glyphs[1]);
  const size_t at = streams.find(joined);  // a TJ that starts with the space...
  CHECK(at != std::string::npos);
  char next[8];
  std::snprintf(next, sizeof next, "<%04X>", glyphs[2]);
  CHECK(at != std::string::npos && streams.find(next, at) < streams.find("]TJ", at));  // ...and goes on to the b
}

TEST(Pdf_LetterSpacingIsCharacterSpacing) {
  ComPtr<IDWriteFontFace> face = ArialFace();
  if (!face) return;
  const UINT32 points[] = {'w', 'i', 'd', 'e'};
  UINT16 glyphs[4];
  face->GetGlyphIndices(points, 4, glyphs);
  DWRITE_GLYPH_METRICS m[4];
  face->GetDesignGlyphMetrics(glyphs, 4, m, FALSE);
  float advances[4];
  for (int i = 0; i < 4; ++i) advances[i] = m[i].advanceWidth * 12.f / 2048 + 2;  // 2 pt apart
  PdfDocument pdf;
  pdf.AddPage(100, 50, [&](Painter& p) {
    p.DrawGlyphs({10, 30}, Run(face.Get(), glyphs, advances, 4, u"wide"), Brush::Solid({0, 0, 0, 1}));
  });
  const std::vector<uint8_t> bytes = pdf.Finish();
  const std::string streams = Streams(std::string(bytes.begin(), bytes.end()));
  // The 2 pt, give or take the widths' rounding, set before the glyphs and back to 0 after.
  const size_t tc = streams.find(" Tc\n");
  CHECK(tc != std::string::npos);
  if (tc == std::string::npos) return;
  const size_t start = streams.rfind('\n', tc) + 1;
  CHECK_NEAR(std::stod(streams.substr(start, tc - start)), 2.0, 0.02);
  CHECK(streams.find("\n0 Tc\n", tc) != std::string::npos);
}
