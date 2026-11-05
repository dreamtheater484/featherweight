#include "draw/Pdf.h"

#include <d2d1.h>
#include <dwrite_3.h>
#include <wrl/client.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <map>
#include <optional>
#include <set>
#include <string_view>
#include <unordered_map>

#include "draw/Images.h"
#include "draw/Sfnt.h"
#include "miniz.h"

namespace draw {

using Microsoft::WRL::ComPtr;

namespace {

// ---------------------------------------------------------------------------
// Writing

// A number as PDF writes it: at most 3 decimals, no trailing zeros.
void Num(std::string& out, double v) {
  if (!std::isfinite(v) || std::abs(v) < 0.0005) {
    out += '0';
    return;
  }
  char buf[48];
  auto r = std::to_chars(buf, buf + sizeof buf, v, std::chars_format::fixed, 3);
  char* end = r.ptr;
  while (end > buf && end[-1] == '0') --end;
  if (end > buf && end[-1] == '.') --end;
  if (end - buf == 2 && buf[0] == '-' && buf[1] == '0') {
    out += '0';
    return;
  }
  out.append(buf, end);
}
std::string N(double v) {
  std::string s;
  Num(s, v);
  return s;
}
std::string Ref(uint32_t object) { return std::to_string(object) + " 0 R"; }

void Hex4(std::string& out, uint32_t v) {
  static const char kHex[] = "0123456789ABCDEF";
  for (int shift = 12; shift >= 0; shift -= 4) out += kHex[(v >> shift) & 15];
}

// A text string: UTF-16BE with a byte order mark, in hex.
std::string TextString(std::u16string_view s) {
  std::string out = "<FEFF";
  for (char16_t c : s) Hex4(out, c);
  return out + ">";
}

// A literal string with PDF's escapes: for addresses, which are ASCII.
std::string Literal(std::string_view s) {
  std::string out = "(";
  for (char c : s) {
    if (c == '(' || c == ')' || c == '\\') out += '\\';
    if (static_cast<unsigned char>(c) < 32 || static_cast<unsigned char>(c) > 126) {
      char buf[8];
      snprintf(buf, sizeof buf, "\\%03o", static_cast<unsigned char>(c));
      out += buf;
    } else {
      out += c;
    }
  }
  return out + ")";
}

std::string Deflate(std::string_view data) {
  mz_ulong size = mz_compressBound(static_cast<mz_ulong>(data.size()));
  std::string out(size, '\0');
  if (mz_compress2(reinterpret_cast<unsigned char*>(out.data()), &size,
                   reinterpret_cast<const unsigned char*>(data.data()), static_cast<mz_ulong>(data.size()),
                   MZ_DEFAULT_COMPRESSION) != MZ_OK) {
    return {};
  }
  out.resize(size);
  return out;
}

// The file: objects written as they're made, with their offsets for the cross-reference table.
class Writer {
 public:
  Writer() { out_ = "%PDF-1.7\n%\xE2\xE3\xCF\xD3\n"; }
  uint32_t Reserve() {
    offsets_.push_back(0);
    return static_cast<uint32_t>(offsets_.size());
  }
  void Object(uint32_t object, std::string_view body) {
    Begin(object);
    out_ += body;
    out_ += "\nendobj\n";
  }
  // A stream: `dict` holds the entries besides Length and Filter.
  void Stream(uint32_t object, std::string_view dict, std::string_view data, bool compress = true,
              std::string_view filter = {}) {
    std::string packed;
    if (compress && filter.empty()) {
      packed = Deflate(data);
      if (packed.empty() && !data.empty()) compress = false;
    }
    std::string_view body = compress && filter.empty() ? std::string_view(packed) : data;
    Begin(object);
    out_ += "<<";
    out_ += dict;
    out_ += "/Length " + std::to_string(body.size());
    if (!filter.empty()) out_ += std::string("/Filter/") + std::string(filter);
    else if (compress) out_ += "/Filter/FlateDecode";
    out_ += ">>\nstream\n";
    out_ += body;
    out_ += "\nendstream\nendobj\n";
  }
  std::string Finish(uint32_t root, uint32_t info) {
    for (uint32_t i = 1; i <= offsets_.size(); ++i)
      if (offsets_[i - 1] == 0) Object(i, "null");  // reserved and never needed
    const size_t xref = out_.size();
    out_ += "xref\n0 " + std::to_string(offsets_.size() + 1) + "\n0000000000 65535 f \n";
    char line[32];
    for (size_t o : offsets_) {
      snprintf(line, sizeof line, "%010zu 00000 n \n", o);
      out_ += line;
    }
    // The file's ID: a hash of what it holds.
    uint64_t h1 = 14695981039346656037ull, h2 = 1099511628211ull;
    for (char c : out_) {
      h1 = (h1 ^ static_cast<uint8_t>(c)) * 1099511628211ull;
      h2 = (h2 + static_cast<uint8_t>(c)) * 6364136223846793005ull + 1442695040888963407ull;
    }
    char id[40];
    snprintf(id, sizeof id, "%016llX%016llX", static_cast<unsigned long long>(h1), static_cast<unsigned long long>(h2));
    out_ += "trailer\n<</Size " + std::to_string(offsets_.size() + 1) + "/Root " + Ref(root) +
            (info ? "/Info " + Ref(info) : std::string()) + "/ID[<" + id + "><" + id + ">]>>\nstartxref\n" +
            std::to_string(xref) + "\n%%EOF\n";
    return std::move(out_);
  }

 private:
  void Begin(uint32_t object) {
    if (object == 0 || object > offsets_.size()) return;
    offsets_[object - 1] = out_.size();
    out_ += std::to_string(object) + " 0 obj\n";
  }
  std::string out_;
  std::vector<size_t> offsets_;
};

// ---------------------------------------------------------------------------
// Fonts

// Glyph outlines from DirectWrite, collected into a Path.
class OutlineSink : public ID2D1SimplifiedGeometrySink {
 public:
  explicit OutlineSink(Path& path) : path_(path) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID2D1SimplifiedGeometrySink)) {
      *out = static_cast<ID2D1SimplifiedGeometrySink*>(this);
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
  ULONG STDMETHODCALLTYPE Release() override { return 1; }
  void STDMETHODCALLTYPE SetFillMode(D2D1_FILL_MODE mode) override {
    path_.fillRule = mode == D2D1_FILL_MODE_ALTERNATE ? Path::FillRule::EvenOdd : Path::FillRule::Winding;
  }
  void STDMETHODCALLTYPE SetSegmentFlags(D2D1_PATH_SEGMENT) override {}
  void STDMETHODCALLTYPE BeginFigure(D2D1_POINT_2F p, D2D1_FIGURE_BEGIN) override { path_.MoveTo({p.x, p.y}); }
  void STDMETHODCALLTYPE AddLines(const D2D1_POINT_2F* points, UINT32 count) override {
    for (UINT32 i = 0; i < count; ++i) path_.LineTo({points[i].x, points[i].y});
  }
  void STDMETHODCALLTYPE AddBeziers(const D2D1_BEZIER_SEGMENT* b, UINT32 count) override {
    for (UINT32 i = 0; i < count; ++i)
      path_.CubicTo({b[i].point1.x, b[i].point1.y}, {b[i].point2.x, b[i].point2.y}, {b[i].point3.x, b[i].point3.y});
  }
  void STDMETHODCALLTYPE EndFigure(D2D1_FIGURE_END end) override {
    if (end == D2D1_FIGURE_END_CLOSED) path_.Close();
  }
  HRESULT STDMETHODCALLTYPE Close() override { return S_OK; }

 private:
  Path& path_;
};

sfnt::Tables FaceTables(IDWriteFontFace* face) {
  return [face](uint32_t tag, std::vector<uint8_t>& data) {
    const void* p = nullptr;
    UINT32 size = 0;
    void* context = nullptr;
    BOOL exists = FALSE;
    // DirectWrite's tags are little-endian.
    const uint32_t dw = (tag >> 24) | ((tag >> 8) & 0xFF00) | ((tag << 8) & 0xFF0000) | (tag << 24);
    if (FAILED(face->TryGetFontTable(dw, &p, &size, &context, &exists)) || !exists) return false;
    const auto* b = static_cast<const uint8_t*>(p);
    data.assign(b, b + size);
    face->ReleaseFontTable(context);
    return true;
  };
}

struct PdfFont {
  ComPtr<IDWriteFontFace> face;
  uint32_t object = 0;  // the Type0 font
  std::string name;     // the resource name
  sfnt::Info info;
  bool outlines = false;  // drawn as outlines, its text invisible on top
  bool bold = false, oblique = false;  // DirectWrite's simulations
  float unitsPerEm = 1000;
  std::map<uint16_t, int> widths;            // used glyphs: advance in 1/1000 em
  std::map<uint16_t, std::u16string> text;   // what each glyph shows, for ToUnicode
};

// ---------------------------------------------------------------------------
// Matrices: Painter's a * b applies a first, as PDF's concatenation does.

Matrix Inverse(const Matrix& m) { return m.Inverse(); }

std::string MatrixOps(const Matrix& m) {
  std::string s;
  for (float v : {m.m11, m.m12, m.m21, m.m22, m.dx, m.dy}) {
    Num(s, v);
    s += ' ';
  }
  return s;
}
std::string MatrixArray(const Matrix& m) {
  std::string s = MatrixOps(m);
  s.pop_back();
  return "[" + s + "]";
}

// The bounds of `r` through `m`.
Rect Through(const Matrix& m, const Rect& r) {
  Point p[4] = {m.Apply({r.left, r.top}), m.Apply({r.right, r.top}), m.Apply({r.left, r.bottom}),
                m.Apply({r.right, r.bottom})};
  Rect b{p[0].x, p[0].y, p[0].x, p[0].y};
  for (const Point& q : p) {
    b.left = std::min(b.left, q.x);
    b.top = std::min(b.top, q.y);
    b.right = std::max(b.right, q.x);
    b.bottom = std::max(b.bottom, q.y);
  }
  return b;
}

// An SVG arc as cubic Béziers (the endpoint parameterization of SVG's implementation notes).
void ArcToCubics(Point from, const Path::Segment& s, std::string& out) {
  const Point to = s.p[0];
  double rx = std::abs(s.rx), ry = std::abs(s.ry);
  if (rx < 1e-6 || ry < 1e-6 || (from.x == to.x && from.y == to.y)) {
    Num(out, to.x);
    out += ' ';
    Num(out, to.y);
    out += " l\n";
    return;
  }
  const double phi = s.rotation * 3.14159265358979323846 / 180, c = std::cos(phi), sn = std::sin(phi);
  const double dx = (from.x - to.x) / 2.0, dy = (from.y - to.y) / 2.0;
  const double x1 = c * dx + sn * dy, y1 = -sn * dx + c * dy;
  const double lambda = x1 * x1 / (rx * rx) + y1 * y1 / (ry * ry);
  if (lambda > 1) {
    rx *= std::sqrt(lambda);
    ry *= std::sqrt(lambda);
  }
  const double num = rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1;
  const double den = rx * rx * y1 * y1 + ry * ry * x1 * x1;
  double k = den > 0 ? std::sqrt(std::max(0.0, num / den)) : 0;
  if (s.largeArc == s.clockwise) k = -k;
  const double cx1 = k * rx * y1 / ry, cy1 = -k * ry * x1 / rx;
  const double cx = c * cx1 - sn * cy1 + (from.x + to.x) / 2.0, cy = sn * cx1 + c * cy1 + (from.y + to.y) / 2.0;
  auto angle = [](double ux, double uy, double vx, double vy) {
    return std::atan2(ux * vy - uy * vx, ux * vx + uy * vy);
  };
  const double t1 = angle(1, 0, (x1 - cx1) / rx, (y1 - cy1) / ry);
  double dt = angle((x1 - cx1) / rx, (y1 - cy1) / ry, (-x1 - cx1) / rx, (-y1 - cy1) / ry);
  const double kPi = 3.14159265358979323846;
  if (s.clockwise && dt < 0) dt += 2 * kPi;
  if (!s.clockwise && dt > 0) dt -= 2 * kPi;
  const int parts = std::max(1, static_cast<int>(std::ceil(std::abs(dt) / (kPi / 2) - 1e-9)));
  const double step = dt / parts, alpha = 4.0 / 3.0 * std::tan(step / 4);
  auto at = [&](double t, double& x, double& y, double& ddx, double& ddy) {
    const double ex = rx * std::cos(t), ey = ry * std::sin(t);
    x = cx + c * ex - sn * ey;
    y = cy + sn * ex + c * ey;
    const double fx = -rx * std::sin(t), fy = ry * std::cos(t);
    ddx = c * fx - sn * fy;
    ddy = sn * fx + c * fy;
  };
  double t = t1;
  for (int i = 0; i < parts; ++i) {
    double ax, ay, adx, ady, bx, by, bdx, bdy;
    at(t, ax, ay, adx, ady);
    at(t + step, bx, by, bdx, bdy);
    if (i == parts - 1) {
      bx = to.x;
      by = to.y;
    }
    for (double v : {ax + alpha * adx, ay + alpha * ady, bx - alpha * bdx, by - alpha * bdy, bx, by}) {
      Num(out, v);
      out += ' ';
    }
    out += "c\n";
    t += step;
  }
}

// A path's construction operators.
std::string PathOps(const Path& p) {
  std::string out;
  Point last{}, start{};
  bool open = false;
  auto pt = [&](Point q) {
    Num(out, q.x);
    out += ' ';
    Num(out, q.y);
    out += ' ';
  };
  for (const Path::Segment& s : p.Segments()) {
    if (s.kind == Path::Kind::Move) {
      pt(s.p[0]);
      out += "m\n";
      last = start = s.p[0];
      open = true;
      continue;
    }
    if (s.kind == Path::Kind::Close) {
      if (open) out += "h\n";
      last = start;
      open = false;
      continue;
    }
    if (!open) {
      pt(last);
      out += "m\n";
      start = last;
      open = true;
    }
    switch (s.kind) {
      case Path::Kind::Line:
        pt(s.p[0]);
        out += "l\n";
        last = s.p[0];
        break;
      case Path::Kind::Quad: {
        const Point q = s.p[0], e = s.p[1];
        pt({last.x + 2.f / 3 * (q.x - last.x), last.y + 2.f / 3 * (q.y - last.y)});
        pt({e.x + 2.f / 3 * (q.x - e.x), e.y + 2.f / 3 * (q.y - e.y)});
        pt(e);
        out += "c\n";
        last = e;
        break;
      }
      case Path::Kind::Cubic:
        pt(s.p[0]);
        pt(s.p[1]);
        pt(s.p[2]);
        out += "c\n";
        last = s.p[2];
        break;
      case Path::Kind::Arc:
        ArcToCubics(last, s, out);
        last = s.p[0];
        break;
      default:
        break;
    }
  }
  return out;
}

bool SameColor(const Color& a, const Color& b) { return a.r == b.r && a.g == b.g && a.b == b.b; }

}  // namespace

// ---------------------------------------------------------------------------
// The document

struct PdfDocument::Impl {
  Writer w;
  uint32_t pagesObject = 0;
  struct PageRecord {
    uint32_t object = 0, contents = 0, resources = 0;
    float width = 0, height = 0;
    std::vector<std::string> annots;  // annotation dictionaries, written with the page
  };
  std::vector<PageRecord> pages;
  struct OutlineItem {
    int level = 0;
    std::u16string title;
    size_t page = 0;
    float y = 0;
  };
  std::vector<OutlineItem> outline;
  std::u16string title, author;

  // Shared resources, named across the whole file.
  std::vector<std::unique_ptr<PdfFont>> fonts;
  std::unordered_map<IDWriteFontFace*, PdfFont*> fontByFace;
  std::map<std::pair<uint64_t, bool>, std::pair<std::string, uint32_t>> images;  // (bitmap id, mask only) -> name, object
  std::map<std::pair<int, int>, std::pair<std::string, uint32_t>> alphas;         // (fill, stroke) in 1/1000
  uint32_t next = 1;  // for resource names

  std::string Name(char kind) { return std::string(1, kind) + std::to_string(next++); }

  PdfFont* FontFor(IDWriteFontFace* face);
  // An ExtGState for these alphas (1/1000).
  std::pair<std::string, uint32_t> Alpha(int fill, int stroke) {
    auto& slot = alphas[{fill, stroke}];
    if (slot.second == 0) {
      slot = {Name('G'), w.Reserve()};
      w.Object(slot.second, "<</Type/ExtGState/ca " + N(fill / 1000.0) + "/CA " + N(stroke / 1000.0) + ">>");
    }
    return slot;
  }
  // The image for a bitmap: its name and object.
  std::pair<std::string, uint32_t> ImageFor(const Bitmap& bitmap, bool smooth);
  void WriteFonts();
  std::vector<uint8_t> Finish();
};

PdfFont* PdfDocument::Impl::FontFor(IDWriteFontFace* face) {
  if (!face) return nullptr;
  if (auto it = fontByFace.find(face); it != fontByFace.end()) return it->second;
  // The same font through another face object: the same file, index and simulations.
  auto key = [](IDWriteFontFace* f) {
    std::string k;
    UINT32 n = 1;
    IDWriteFontFile* file = nullptr;
    if (SUCCEEDED(f->GetFiles(&n, &file)) && file) {
      const void* ref = nullptr;
      UINT32 size = 0;
      if (SUCCEEDED(file->GetReferenceKey(&ref, &size))) k.assign(static_cast<const char*>(ref), size);
      file->Release();
    }
    return k + "#" + std::to_string(f->GetIndex()) + "#" + std::to_string(f->GetSimulations());
  };
  const std::string k = key(face);
  for (auto& f : fonts)
    if (key(f->face.Get()) == k) {
      fontByFace[face] = f.get();
      return f.get();
    }
  auto font = std::make_unique<PdfFont>();
  font->face = face;
  DWRITE_FONT_METRICS m{};
  face->GetMetrics(&m);
  font->unitsPerEm = m.designUnitsPerEm ? static_cast<float>(m.designUnitsPerEm) : 1000.f;
  const DWRITE_FONT_SIMULATIONS sim = face->GetSimulations();
  font->bold = sim & DWRITE_FONT_SIMULATIONS_BOLD;
  font->oblique = sim & DWRITE_FONT_SIMULATIONS_OBLIQUE;
  if (auto info = sfnt::ReadInfo(FaceTables(face))) {
    font->info = *info;
    ComPtr<IDWriteFontFace5> face5;
    const bool varied = SUCCEEDED(face->QueryInterface(IID_PPV_ARGS(&face5))) && face5->HasVariations();
    font->outlines = !info->embeddable || (info->cff && info->cidKeyed) || (info->variable && varied);
    // A program that can't be made: outlines too (decided now, before any text is drawn with it).
    if (!font->outlines) {
      const sfnt::Tables tables = FaceTables(face);
      font->outlines = (info->cff ? sfnt::Whole(tables) : sfnt::Subset(tables, {0})).empty();
    }
  } else {
    font->outlines = true;  // its tables don't hold together: drawn as outlines only
    font->info.unitsPerEm = static_cast<uint16_t>(font->unitsPerEm);
  }
  font->name = Name('F');
  font->object = w.Reserve();
  PdfFont* raw = font.get();
  fonts.push_back(std::move(font));
  fontByFace[face] = raw;
  return raw;
}

std::pair<std::string, uint32_t> PdfDocument::Impl::ImageFor(const Bitmap& bitmap, bool smooth) {
  auto& slot = images[{bitmap.Id(), smooth}];
  if (slot.second) return slot;
  slot = {Name('I'), w.Reserve()};
  const std::string interpolate = smooth ? "/Interpolate true" : "";
  // A JPEG as it is: baseline or progressive, 8 bits, gray or color (CMYK files are decoded).
  if (std::shared_ptr<const Image> source = bitmap.Jpeg()) {
    std::vector<uint8_t> file;
    if (source->Encoded(file) && file.size() > 4 && file[0] == 0xFF && file[1] == 0xD8) {
      int width = 0, height = 0, components = 0;
      for (size_t p = 2; p + 4 <= file.size();) {
        if (file[p] != 0xFF) break;
        const uint8_t marker = file[p + 1];
        if (marker == 0xD8 || (marker >= 0xD0 && marker <= 0xD7) || marker == 0x01) {
          p += 2;
          continue;
        }
        const size_t length = size_t{file[p + 2]} << 8 | file[p + 3];
        if (marker == 0xC0 || marker == 0xC1 || marker == 0xC2) {
          if (p + 10 <= file.size() && file[p + 4] == 8) {
            height = file[p + 5] << 8 | file[p + 6];
            width = file[p + 7] << 8 | file[p + 8];
            components = file[p + 9];
          }
          break;
        }
        if (marker == 0xDA || length < 2) break;
        p += 2 + length;
      }
      if (width > 0 && height > 0 && (components == 1 || components == 3)) {
        w.Stream(slot.second,
                 "/Type/XObject/Subtype/Image/Width " + std::to_string(width) + "/Height " + std::to_string(height) +
                     "/ColorSpace/" + (components == 1 ? "DeviceGray" : "DeviceRGB") + "/BitsPerComponent 8" +
                     interpolate,
                 std::string_view(reinterpret_cast<const char*>(file.data()), file.size()), false, "DCTDecode");
        return slot;
      }
    }
  }
  // Pixels: straight RGB, and a soft mask when any is transparent.
  const int width = std::max(1, bitmap.Width()), height = std::max(1, bitmap.Height());
  std::string rgb, alpha;
  rgb.reserve(static_cast<size_t>(width) * height * 3);
  alpha.reserve(static_cast<size_t>(width) * height);
  bool transparent = false;
  const uint8_t* px = bitmap.Pixels();
  const size_t count = static_cast<size_t>(bitmap.Width()) * bitmap.Height();
  for (size_t i = 0; i < count; ++i) {
    const uint8_t b = px[i * 4], g = px[i * 4 + 1], r = px[i * 4 + 2], a = px[i * 4 + 3];
    auto straight = [a](uint8_t c) { return static_cast<char>(a ? std::min(255, (c * 255 + a / 2) / a) : 0); };
    rgb += straight(r);
    rgb += straight(g);
    rgb += straight(b);
    alpha += static_cast<char>(a);
    transparent |= a != 255;
  }
  if (count == 0) {
    rgb.assign(3, '\0');
    alpha.assign(1, '\0');
    transparent = true;
  }
  std::string dict = "/Type/XObject/Subtype/Image/Width " + std::to_string(width) + "/Height " +
                     std::to_string(height) + "/BitsPerComponent 8" + interpolate;
  if (transparent) {
    const uint32_t mask = w.Reserve();
    w.Stream(mask, dict + "/ColorSpace/DeviceGray", alpha);
    dict += "/SMask " + Ref(mask);
  }
  w.Stream(slot.second, dict + "/ColorSpace/DeviceRGB", rgb);
  return slot;
}

// ---------------------------------------------------------------------------
// The painter

namespace {

// What a content stream uses, by resource name.
struct Resources {
  std::map<std::string, uint32_t> fonts, xobjects, states, patterns, shadings;
  std::string Dict() const {
    std::string s = "<<";
    auto add = [&s](const char* kind, const std::map<std::string, uint32_t>& m) {
      if (m.empty()) return;
      s += std::string("/") + kind + "<<";
      for (const auto& [name, object] : m) s += "/" + name + " " + Ref(object);
      s += ">>";
    };
    add("Font", fonts);
    add("XObject", xobjects);
    add("ExtGState", states);
    add("Pattern", patterns);
    add("Shading", shadings);
    return s + ">>";
  }
};

}  // namespace

class PdfPainter : public Painter {
 public:
  PdfPainter(PdfDocument::Impl& doc, float width, float height) : doc_(doc), width_(width), height_(height) {
    flip_ = {1, 0, 0, -1, 0, height};
    levels_.push_back({});
    levels_.back().ops = "1 0 0 -1 0 " + N(height) + " cm\n";
    states_.push_back({});
  }
  // The page's content and resources.
  const std::string& PageOps() const { return levels_.front().ops; }
  const Resources& PageResources() const { return levels_.front().res; }
  void End() {
    FlushSpaces();
    while (states_.size() > 1) Restore();
  }

  void Save() override {
    last_.face = nullptr;
    State s = states_.back();
    s.group = false;
    states_.push_back(s);
    Out() += "q\n";
  }
  void Restore() override {
    last_.face = nullptr;
    if (states_.size() <= 1) return;
    const State s = states_.back();
    states_.pop_back();
    if (s.group && levels_.size() > 1) {
      // The group as a transparency group form, blended at its opacity.
      Level group = std::move(levels_.back());
      levels_.pop_back();
      const Rect page = Through(Inverse(s.groupBase), {0, 0, width_, height_});
      const uint32_t form = doc_.w.Reserve();
      const std::string name = doc_.Name('X');
      doc_.w.Stream(form,
                    "/Type/XObject/Subtype/Form/BBox[" + N(page.left) + " " + N(page.top) + " " + N(page.right) + " " +
                        N(page.bottom) + "]/Group<</S/Transparency>>/Resources " + group.res.Dict(),
                    group.ops);
      Level& parent = levels_.back();
      parent.res.xobjects[name] = form;
      const auto gs = doc_.Alpha(s.groupOpacity, s.groupOpacity);
      parent.res.states[gs.first] = gs.second;
      parent.ops += "/" + gs.first + " gs /" + name + " Do\n";
    }
    Out() += "Q\n";
  }
  void Transform(const Matrix& m) override {
    last_.face = nullptr;
    states_.back().ctm = m * states_.back().ctm;
    Out() += MatrixOps(m) + "cm\n";
  }
  Matrix CurrentTransform() const override { return states_.back().ctm; }
  void ClipRect(const Rect& r) override {
    last_.face = nullptr;
    Out() += N(r.left) + " " + N(r.top) + " " + N(r.Width()) + " " + N(r.Height()) + " re W n\n";
  }
  void ClipPath(const Path& p) override {
    last_.face = nullptr;
    if (p.Empty()) {
      Out() += "0 0 0 0 re W n\n";
      return;
    }
    Out() += PathOps(p) + (p.fillRule == Path::FillRule::EvenOdd ? "W* n\n" : "W n\n");
  }
  void BeginGroup(float opacity) override {
    Save();
    State& s = states_.back();
    s.group = true;
    s.groupOpacity = std::clamp(static_cast<int>(std::lround(opacity * 1000)), 0, 1000);
    s.groupBase = s.ctm * flip_;
    levels_.push_back({});
    levels_.back().base = s.groupBase;
    // A transparency group starts opaque, with its parent's colors: unknown here, set at first use.
    s.fill = s.stroke = Color{-1, -1, -1, 1};
    s.fillAlpha = s.strokeAlpha = 1000;
  }

  void FillPath(const Path& p, const Brush& b) override {
    if (p.Empty()) return;
    if (b.kind == Brush::Kind::Bitmap && b.extendX == Brush::Extend::Clamp && b.extendY == Brush::Extend::Clamp &&
        b.bitmap) {
      // A picture fill: the picture clipped to the shape (what lies past its edges stays empty).
      Save();
      ClipPath(p);
      Transform(b.bitmapTransform);
      DrawImage(*b.bitmap, {0, 0, static_cast<float>(b.bitmap->Width()), static_cast<float>(b.bitmap->Height())}, b.smooth);
      Restore();
      return;
    }
    Paint(b, false, [&] { Out() += PathOps(p) + (p.fillRule == Path::FillRule::EvenOdd ? "f*\n" : "f\n"); },
          p.Bounds());
  }
  void StrokePath(const Path& p, const Pen& pen) override {
    if (p.Empty() || pen.width <= 0) return;
    std::string style = N(pen.width) + " w " + std::to_string(pen.cap == LineCap::Round ? 1 : pen.cap == LineCap::Square ? 2 : 0) +
                        " J " + std::to_string(pen.join == LineJoin::Round ? 1 : pen.join == LineJoin::Bevel ? 2 : 0) +
                        " j " + N(std::max(1.f, pen.miterLimit)) + " M ";
    if (!pen.dashes.empty()) {
      style += "[";
      bool any = false;
      for (float d : pen.dashes) {
        style += N(std::max(0.f, d) * pen.width) + " ";
        any |= d > 0;
      }
      style += "] " + N(pen.dashOffset * pen.width) + " d\n";
      if (!any) return;  // all gaps
    } else {
      style += "[] 0 d\n";
    }
    Rect bounds = p.Bounds();
    bounds = {bounds.left - pen.width, bounds.top - pen.width, bounds.right + pen.width, bounds.bottom + pen.width};
    Paint(pen.brush, true, [&] { Out() += style + PathOps(p) + "S\n"; }, bounds);
  }
  void FillRect(const Rect& r, const Brush& b) override {
    if (b.kind == Brush::Kind::Bitmap) {
      Painter::FillRect(r, b);
      return;
    }
    Paint(b, false, [&] { Out() += N(r.left) + " " + N(r.top) + " " + N(r.Width()) + " " + N(r.Height()) + " re f\n"; }, r);
  }
  bool WantsText() const override { return true; }
  void DrawGlyphs(Point origin, const GlyphRun& run, const Brush& b) override;
  void DrawBitmap(const Bitmap& bitmap, const Rect& dest, const Rect* src, float opacity) override;
  void DrawVectorImage(const VectorImage& image, const Rect& dest, float opacity) override {
    // At 300 dpi (plan §4.6, decision 9).
    const Matrix m = CurrentTransform();
    const float f = 300.f / 72;
    const int w = std::clamp(static_cast<int>(std::ceil(std::abs(dest.Width()) * m.ScaleX() * f)), 1, 4096);
    const int h = std::clamp(static_cast<int>(std::ceil(std::abs(dest.Height()) * m.ScaleY() * f)), 1, 4096);
    if (std::shared_ptr<const Bitmap> pixels = RasterizeVectorImage(image, w, h)) DrawBitmap(*pixels, dest, nullptr, opacity);
  }

 private:
  struct State {
    Matrix ctm;  // the engine's transform: points, y down
    Color fill{0, 0, 0, 1}, stroke{0, 0, 0, 1};  // the current colors (alpha unused)
    int fillAlpha = 1000, strokeAlpha = 1000;
    bool group = false;  // Restore ends a group
    int groupOpacity = 1000;
    Matrix groupBase;
  };
  struct Level {
    std::string ops;
    Resources res;
    Matrix base;  // the PDF transform this stream's space starts from (identity for the page)
  };

  std::string& Out() { return levels_.back().ops; }
  Resources& Res() { return levels_.back().res; }

  // The pattern matrix for something in the current coordinates: from them to the stream's space.
  Matrix PatternMatrix(const Matrix& local = {}) const {
    return local * states_.back().ctm * flip_ * Inverse(levels_.back().base);
  }

  void SetAlpha(int fill, int stroke) {
    State& s = states_.back();
    if (s.fillAlpha == fill && s.strokeAlpha == stroke) return;
    const auto gs = doc_.Alpha(fill, stroke);
    Res().states[gs.first] = gs.second;
    Out() += "/" + gs.first + " gs\n";
    s.fillAlpha = fill;
    s.strokeAlpha = stroke;
  }

  // A PDF function for gradient stops: their colors, or (gray) their alphas.
  std::string StopFunction(const std::vector<GradientStop>& stopsIn, bool alpha) {
    std::vector<GradientStop> stops = stopsIn;
    if (stops.empty()) stops.push_back({0, {0, 0, 0, 0}});
    if (stops.front().offset > 0) stops.insert(stops.begin(), {0, stops.front().color});
    if (stops.back().offset < 1) stops.push_back({1, stops.back().color});
    auto color = [alpha](const Color& c) {
      if (alpha) return "[" + N(std::clamp(c.a, 0.f, 1.f)) + "]";
      return "[" + N(std::clamp(c.r, 0.f, 1.f)) + " " + N(std::clamp(c.g, 0.f, 1.f)) + " " + N(std::clamp(c.b, 0.f, 1.f)) + "]";
    };
    auto segment = [&](const Color& a, const Color& b) {
      return "<</FunctionType 2/Domain[0 1]/C0" + color(a) + "/C1" + color(b) + "/N 1>>";
    };
    if (stops.size() == 2) return segment(stops[0].color, stops[1].color);
    std::string fns, bounds, encode;
    float last = 0;
    for (size_t i = 0; i + 1 < stops.size(); ++i) {
      fns += segment(stops[i].color, stops[i + 1].color);
      encode += "0 1 ";
      if (i + 2 < stops.size()) {
        last = std::max(std::clamp(stops[i + 1].offset, 0.f, 1.f), last + 1e-4f);
        bounds += N(std::min(last, 1.f)) + " ";
      }
    }
    return "<</FunctionType 3/Domain[0 1]/Functions[" + fns + "]/Bounds[" + bounds + "]/Encode[" + encode + "]>>";
  }

  // A shading dictionary for a gradient brush, in the current coordinates.
  std::string Shading(const Brush& b, bool alpha) {
    const std::string space = alpha ? "/DeviceGray" : "/DeviceRGB";
    if (b.kind == Brush::Kind::Linear)
      return "<</ShadingType 2/ColorSpace" + space + "/Coords[" + N(b.start.x) + " " + N(b.start.y) + " " + N(b.end.x) +
             " " + N(b.end.y) + "]/Function" + StopFunction(b.stops, alpha) + "/Extend[true true]>>";
    // Radial: an ellipse is a circle drawn through a vertical scale (see RadialMatrix).
    const float rx = std::max(b.rx, 1e-3f), k = b.ry / rx;
    const float fx = b.center.x + b.focusOffset.x, fy = b.center.y + (k != 0 ? b.focusOffset.y / k : 0);
    return "<</ShadingType 3/ColorSpace" + space + "/Coords[" + N(fx) + " " + N(fy) + " 0 " + N(b.center.x) + " " +
           N(b.center.y) + " " + N(rx) + "]/Function" + StopFunction(b.stops, alpha) + "/Extend[true true]>>";
  }
  static Matrix RadialMatrix(const Brush& b) {
    if (b.kind != Brush::Kind::Radial) return {};
    const float k = b.ry / std::max(b.rx, 1e-3f);
    return Matrix::Translate(0, -b.center.y) * Matrix::Scale(1, k) * Matrix::Translate(0, b.center.y);
  }

  // Sets the fill (or stroke) paint, runs `draw`, and puts the state back if it had to change it.
  template <typename Draw>
  void Paint(const Brush& b, bool stroke, Draw draw, const Rect& bounds) {
    State& s = states_.back();
    if (b.kind == Brush::Kind::Solid || ((b.kind == Brush::Kind::Linear || b.kind == Brush::Kind::Radial) && b.stops.size() < 2)) {
      const Color c = b.kind == Brush::Kind::Solid ? b.color : (b.stops.empty() ? Color{0, 0, 0, 0} : b.stops[0].color);
      const int a = std::clamp(static_cast<int>(std::lround(c.a * 1000)), 0, 1000);
      if (a == 0) return;
      Color& current = stroke ? s.stroke : s.fill;
      if (!SameColor(current, c)) {
        Out() += N(std::clamp(c.r, 0.f, 1.f)) + " " + N(std::clamp(c.g, 0.f, 1.f)) + " " + N(std::clamp(c.b, 0.f, 1.f)) +
                 (stroke ? " RG\n" : " rg\n");
        current = c;
      }
      SetAlpha(stroke ? s.fillAlpha : a, stroke ? a : s.strokeAlpha);
      draw();
      return;
    }
    // Patterns: set inside q/Q, so the solid color state stays as it was.
    const State before = states_.back();
    Out() += "q\n";
    const std::string pattern = doc_.Name('P');
    const uint32_t object = doc_.w.Reserve();
    if (b.kind == Brush::Kind::Bitmap) {
      if (!b.bitmap) {
        Out() += "Q\n";
        states_.back() = before;
        return;
      }
      const auto image = doc_.ImageFor(*b.bitmap, b.smooth);
      const float w = static_cast<float>(b.bitmap->Width()), h = static_cast<float>(b.bitmap->Height());
      const bool mx = b.extendX == Brush::Extend::Mirror, my = b.extendY == Brush::Extend::Mirror;
      std::string cell = "q " + N(w) + " 0 0 " + N(-h) + " 0 " + N(h) + " cm /" + image.first + " Do Q\n";
      if (mx) cell += "q -" + N(w) + " 0 0 " + N(-h) + " " + N(2 * w) + " " + N(h) + " cm /" + image.first + " Do Q\n";
      if (my) cell += "q " + N(w) + " 0 0 " + N(h) + " 0 " + N(h) + " cm /" + image.first + " Do Q\n";
      if (mx && my) cell += "q -" + N(w) + " 0 0 " + N(h) + " " + N(2 * w) + " " + N(h) + " cm /" + image.first + " Do Q\n";
      const float xs = mx ? 2 * w : w, ys = my ? 2 * h : h;
      doc_.w.Stream(object,
                    "/Type/Pattern/PatternType 1/PaintType 1/TilingType 1/BBox[0 0 " + N(xs) + " " + N(ys) + "]/XStep " +
                        N(xs) + "/YStep " + N(ys) + "/Matrix" + MatrixArray(PatternMatrix(b.bitmapTransform)) +
                        "/Resources<</XObject<</" + image.first + " " + Ref(image.second) + ">>>>",
                    cell);
    } else {
      doc_.w.Object(object, "<</Type/Pattern/PatternType 2/Matrix" + MatrixArray(PatternMatrix(RadialMatrix(b))) +
                                "/Shading" + Shading(b, false) + ">>");
      // Alpha: one for all the stops, or a soft mask of them.
      bool varies = false;
      for (const GradientStop& st : b.stops) varies |= std::abs(st.color.a - b.stops[0].color.a) > 0.001f;
      if (varies) {
        const uint32_t mask = doc_.w.Reserve(), shading = doc_.w.Reserve();
        doc_.w.Object(shading, Shading(b, true));
        const Matrix radial = RadialMatrix(b);
        doc_.w.Stream(mask,
                      "/Type/XObject/Subtype/Form/BBox[" + N(bounds.left - 1) + " " + N(bounds.top - 1) + " " +
                          N(bounds.right + 1) + " " + N(bounds.bottom + 1) +
                          "]/Group<</S/Transparency/CS/DeviceGray>>/Resources<</Shading<</S " + Ref(shading) + ">>>>",
                      MatrixOps(radial) + "cm /S sh\n");
        const uint32_t gs = doc_.w.Reserve();
        const std::string gsName = doc_.Name('G');
        doc_.w.Object(gs, "<</Type/ExtGState/SMask<</Type/Mask/S/Luminosity/G " + Ref(mask) + ">>>>");
        Res().states[gsName] = gs;
        Out() += "/" + gsName + " gs\n";
      } else {
        const int a = std::clamp(static_cast<int>(std::lround(b.stops[0].color.a * 1000)), 0, 1000);
        if (a == 0) {
          Out() += "Q\n";
          states_.back() = before;
          return;
        }
        SetAlpha(stroke ? s.fillAlpha : a, stroke ? a : s.strokeAlpha);
      }
    }
    Res().patterns[pattern] = object;
    Out() += stroke ? "/Pattern CS /" + pattern + " SCN\n" : "/Pattern cs /" + pattern + " scn\n";
    draw();
    Out() += "Q\n";
    states_.back() = before;  // as Q puts it back
  }

  // The bitmap's pixels [0,w]x[0,h] (row 0 at the top) in the current coordinates.
  void DrawImage(const Bitmap& bitmap, const Rect& r, bool smooth) {
    const auto image = doc_.ImageFor(bitmap, smooth);
    Res().xobjects[image.first] = image.second;
    Out() += "q " + N(r.Width()) + " 0 0 " + N(-r.Height()) + " " + N(r.left) + " " + N(r.bottom) + " cm /" + image.first +
             " Do Q\n";
  }

  PdfDocument::Impl& doc_;
  float width_, height_;
  Matrix flip_;  // page points (y down) to PDF's default space
  std::vector<Level> levels_;
  std::vector<State> states_;
  // The text object written last, while nothing has been written after it. A run of only spaces
  // that goes on along its line is added to it: PDFium ignores a text object of only spaces, and
  // a word after it would run into the one before.
  struct LastText {
    IDWriteFontFace* face = nullptr;  // null: none
    PdfFont* font = nullptr;
    size_t end = 0;                   // Out()'s size after it
    float size = 0, y = 0, pen = 0;   // its size, baseline and where its pen ended
  } last_;
  bool AppendSpaces(Point origin, const GlyphRun& run, const Brush& b, PdfFont* font);
  // A run of only spaces that couldn't go into the text object before it (something was drawn
  // after that, such as an underline): held back to go at the front of the next run's. A space
  // shows nothing, so it can be the space of either one's font, in either one's color, and it is
  // kept in the coordinates it came in (`ctm`) while the next run's are set up (a scaled run's).
  struct Spaces {
    Matrix ctm;
    Point origin;
    Brush brush;
    IDWriteFontFace* face = nullptr;
    float emSize = 0;
    std::vector<uint16_t> indices;
    std::vector<float> advances;
    std::u16string text;
  };
  std::optional<Spaces> spaces_;
  void FlushSpaces();
  void DrawRun(Point origin, const GlyphRun& run, const Brush& b, PdfFont* font);
};

namespace {

// The glyphs for these spaces in another font, or empty if it has none.
std::vector<uint16_t> SpaceGlyphs(IDWriteFontFace* face, std::u16string_view spaces) {
  std::vector<uint16_t> glyphs;
  for (char16_t c : spaces) {
    UINT32 points[2] = {c, u' '};
    UINT16 found[2] = {};
    if (FAILED(face->GetGlyphIndices(points, 2, found)) || (!found[0] && !found[1])) return {};
    glyphs.push_back(found[0] ? found[0] : found[1]);
  }
  return glyphs;
}

// Whether a run is only spaces, as a reader needs them between words.
bool OnlySpaces(const GlyphRun& run, const Brush& b) {
  if (!run.text || run.textLength != run.count || (run.bidiLevel & 1) || run.sideways || !run.advances ||
      b.kind != Brush::Kind::Solid)
    return false;
  for (uint32_t i = 0; i < run.textLength; ++i) {
    const char16_t c = run.text[i];
    if (c != u' ' && c != 0xA0 && !(c >= 0x2000 && c <= 0x200A) && c != 0x202F && c != 0x205F && c != 0x3000) return false;
    if (run.offsets && (run.offsets[i].advanceOffset != 0 || run.offsets[i].ascenderOffset != 0)) return false;
  }
  return true;
}

}  // namespace

void PdfPainter::FlushSpaces() {
  if (!spaces_) return;
  Spaces held = std::move(*spaces_);
  spaces_.reset();
  if (PdfFont* font = doc_.FontFor(held.face)) {
    // In the coordinates they came in.
    Save();
    Transform(held.ctm * Inverse(states_.back().ctm));
    GlyphRun run;
    run.face = held.face;
    run.emSize = held.emSize;
    run.count = static_cast<uint32_t>(held.indices.size());
    run.indices = held.indices.data();
    run.advances = held.advances.data();
    run.text = held.text.data();
    run.textLength = static_cast<uint32_t>(held.text.size());
    DrawRun(held.origin, run, held.brush, font);
    Restore();
  }
}

bool PdfPainter::AppendSpaces(Point origin, const GlyphRun& run, const Brush&, PdfFont*) {
  static constexpr std::string_view kClose = "]TJ\nET\n";
  if (!last_.face || Out().size() != last_.end || std::abs(origin.y - last_.y) > 0.001f ||
      !std::string_view(Out()).ends_with(kClose))
    return false;
  const std::vector<uint16_t> glyphs = SpaceGlyphs(last_.face, {run.text, run.textLength});
  std::vector<DWRITE_GLYPH_METRICS> metrics(glyphs.size());
  if (glyphs.empty() ||
      FAILED(last_.face->GetDesignGlyphMetrics(glyphs.data(), static_cast<UINT32>(glyphs.size()), metrics.data(), FALSE)))
    return false;
  PdfFont* font = last_.font;
  std::string& o = Out();
  o.resize(o.size() - kClose.size());
  float pen = last_.pen, at = origin.x;
  const float size = last_.size;
  for (uint32_t i = 0; i < run.count; ++i) {
    const int width = static_cast<int>(std::lround(metrics[i].advanceWidth * 1000.0 / font->unitsPerEm));
    font->widths.emplace(glyphs[i], width);
    font->text.emplace(glyphs[i], std::u16string(1, run.text[i]));
    const float rounded = std::round((pen - at) * 1000 / size * 10) / 10;
    if (rounded != 0) o += N(rounded);
    pen -= rounded * size / 1000;
    o += "<";
    Hex4(o, glyphs[i]);
    o += ">";
    pen += width * size / 1000;
    at += run.advances[i];
  }
  o += kClose;
  last_.end = o.size();
  last_.pen = pen;
  return true;
}

void PdfPainter::DrawBitmap(const Bitmap& bitmap, const Rect& dest, const Rect* src, float opacity) {
  if (dest.Width() == 0 || dest.Height() == 0 || bitmap.Width() <= 0 || bitmap.Height() <= 0) return;
  const int a = std::clamp(static_cast<int>(std::lround(opacity * 1000)), 0, 1000);
  if (a == 0) return;
  Save();
  if (a < 1000) SetAlpha(a, states_.back().strokeAlpha);
  Rect at = dest;
  if (src && src->Width() > 0 && src->Height() > 0) {
    // The whole picture placed so `src` lands on `dest`, clipped to it.
    const float sx = dest.Width() / src->Width(), sy = dest.Height() / src->Height();
    at = {dest.left - src->left * sx, dest.top - src->top * sy, dest.left + (bitmap.Width() - src->left) * sx,
          dest.top + (bitmap.Height() - src->top) * sy};
    ClipRect(dest);
  }
  DrawImage(bitmap, at, true);
  Restore();
}

void PdfPainter::DrawGlyphs(Point origin, const GlyphRun& run, const Brush& b) {
  if (!run.face || run.count == 0 || !run.indices) return;
  PdfFont* font = doc_.FontFor(run.face);
  if (!font) return;
  // Spaces held back go at the front of this run, when it goes on along their line in their font.
  if (spaces_) {
    // Where the spaces are in this run's coordinates: both scale and move only.
    const Matrix& now = states_.back().ctm;
    auto plain = [](const Matrix& m) { return m.m12 == 0 && m.m21 == 0 && m.m11 > 0 && m.m22 > 0; };
    Point at{};
    if (plain(now) && plain(spaces_->ctm)) at = Inverse(now).Apply(spaces_->ctm.Apply(spaces_->origin));
    std::vector<uint16_t> glyphs;
    if (plain(now) && plain(spaces_->ctm) && !(run.bidiLevel & 1) && !run.sideways && run.advances && run.text &&
        std::abs(origin.y - at.y) < 0.01f && origin.x >= at.x && !(glyphs = SpaceGlyphs(run.face, spaces_->text)).empty()) {
      Spaces held = std::move(*spaces_);
      spaces_.reset();
      held.origin = at;
      for (float& a : held.advances) a *= held.ctm.m11 / now.m11;
      held.indices = std::move(glyphs);
      float along = 0;
      for (size_t i = 0; i + 1 < held.advances.size(); ++i) along += held.advances[i];
      held.advances.back() = origin.x - held.origin.x - along;  // the last space ends where the run starts
      held.indices.insert(held.indices.end(), run.indices, run.indices + run.count);
      held.advances.insert(held.advances.end(), run.advances, run.advances + run.count);
      held.text.append(run.text, run.textLength);
      std::vector<DWRITE_GLYPH_OFFSET> offsets;
      if (run.offsets) {
        offsets.resize(held.indices.size() - run.count);
        offsets.insert(offsets.end(), run.offsets, run.offsets + run.count);
      }
      GlyphRun joined = run;
      joined.count = static_cast<uint32_t>(held.indices.size());
      joined.indices = held.indices.data();
      joined.advances = held.advances.data();
      joined.offsets = run.offsets ? offsets.data() : nullptr;
      joined.text = held.text.data();
      joined.textLength = static_cast<uint32_t>(held.text.size());
      DrawRun(held.origin, joined, b, font);
      return;
    }
    FlushSpaces();
  }
  if (OnlySpaces(run, b)) {
    if (AppendSpaces(origin, run, b, font)) return;
    spaces_ = Spaces{states_.back().ctm, origin, b, run.face, run.emSize,
                     std::vector<uint16_t>(run.indices, run.indices + run.count),
                     std::vector<float>(run.advances, run.advances + run.count), std::u16string(run.text, run.textLength)};
    return;
  }
  DrawRun(origin, run, b, font);
}

void PdfPainter::DrawRun(Point origin, const GlyphRun& run, const Brush& b, PdfFont* font) {
  last_.face = nullptr;
  const bool rtl = run.bidiLevel & 1;
  if (font->outlines || run.sideways) {
    // The glyphs' outlines; the text goes invisibly on top (below), so it can still be found.
    Path outline;
    OutlineSink sink(outline);
    if (SUCCEEDED(run.face->GetGlyphRunOutline(run.emSize, run.indices, run.advances, run.offsets, run.count,
                                               run.sideways, rtl, &sink))) {
      Save();
      Transform(Matrix::Translate(origin.x, origin.y));
      FillPath(outline, b);
      Restore();
    }
    if (run.sideways) return;
  }

  // Each glyph's width in the font (1/1000 em), as the W array says, and what it shows.
  std::vector<DWRITE_GLYPH_METRICS> metrics(run.count);
  if (FAILED(run.face->GetDesignGlyphMetrics(run.indices, run.count, metrics.data(), FALSE))) return;
  std::vector<int> widths(run.count);
  std::vector<float> advances(run.count);
  for (uint32_t i = 0; i < run.count; ++i) {
    widths[i] = static_cast<int>(std::lround(metrics[i].advanceWidth * 1000.0 / font->unitsPerEm));
    font->widths.emplace(run.indices[i], widths[i]);
    advances[i] = run.advances ? run.advances[i] : metrics[i].advanceWidth * run.emSize / font->unitsPerEm;
  }
  // The text each glyph shows: walked with the font's cmap, so a glyph that is one character's own
  // is that character, and a ligature or a glyph the cmap doesn't give is the text up to the next
  // one that matches.
  bool actual = false;  // the glyphs alone don't say what the text is: ActualText says it
  if (run.text && run.textLength) {
    std::vector<UINT32> points;
    std::vector<uint32_t> starts;  // each code point's first UTF-16 unit
    for (uint32_t i = 0; i < run.textLength; ++i) {
      uint32_t cp = run.text[i];
      starts.push_back(i);
      if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < run.textLength && run.text[i + 1] >= 0xDC00 && run.text[i + 1] < 0xE000) {
        cp = 0x10000 + ((cp - 0xD800) << 10) + (run.text[i + 1] - 0xDC00);
        ++i;
      }
      points.push_back(cp);
    }
    std::vector<UINT16> cmap(points.size());
    if (points.empty() || FAILED(run.face->GetGlyphIndices(points.data(), static_cast<UINT32>(points.size()), cmap.data())))
      cmap.assign(points.size(), 0);
    std::u16string shown;
    const size_t n = points.size();
    size_t c = 0;
    for (uint32_t g = 0; g < run.count; ++g) {
      const uint16_t glyph = run.indices[g];
      size_t end = 0;  // the text this glyph shows: [c, end)
      if (c < n && cmap[c] == glyph) {
        end = c + 1;
      } else if (c < n && g + 1 == run.count) {
        end = n;  // the last glyph: the rest
      } else if (c < n) {
        // A ligature: up to the character the next glyph is, a few at most.
        for (size_t k = c + 1; k < n && k <= c + 8; ++k)
          if (cmap[k] == run.indices[g + 1]) {
            end = k;
            break;
          }
      }
      if (end > c) {
        const uint32_t from = starts[c], to = end < n ? starts[end] : run.textLength;
        font->text.emplace(glyph, std::u16string(run.text + from, run.text + to));
        c = end;
      } else if (c < n) {
        ++c;  // a form the cmap doesn't give (a mark, a contextual form): ActualText says it
      }
      if (auto it = font->text.find(glyph); it != font->text.end()) shown += it->second;
    }
    actual = shown != std::u16string_view(run.text, run.textLength);
  }

  // Where each glyph goes.
  std::vector<Point> at(run.count);
  float pen = 0;
  for (uint32_t i = 0; i < run.count; ++i) {
    const float adv = run.offsets ? run.offsets[i].advanceOffset : 0, asc = run.offsets ? run.offsets[i].ascenderOffset : 0;
    if (rtl) {
      pen += advances[i];
      at[i] = {origin.x - pen - adv, origin.y - asc};
    } else {
      at[i] = {origin.x + pen + adv, origin.y - asc};
      pen += advances[i];
    }
  }

  Res().fonts[font->name] = font->object;
  const float size = run.emSize;
  if (size <= 0) return;
  std::string text;
  if (actual) {
    text += "/Span<</ActualText" + TextString(std::u16string_view(run.text, run.textLength)) + ">>BDC\n";
  }
  // Letter spacing (w:spacing): the same gap after every glyph goes in Tc, as Word exports it.
  // As TJ adjustments, readers would take the gaps for spaces between words.
  float tc = 0;
  if (!rtl && run.count > 2) {
    std::vector<float> gaps;
    for (uint32_t i = 0; i + 1 < run.count; ++i)
      if (std::abs(at[i + 1].y - at[i].y) < 0.001f) gaps.push_back(at[i + 1].x - at[i].x - widths[i] * size / 1000);
    if (gaps.size() >= 2) {
      std::vector<float> sorted = gaps;
      std::nth_element(sorted.begin(), sorted.begin() + static_cast<ptrdiff_t>(sorted.size() / 2), sorted.end());
      const float median = sorted[sorted.size() / 2];
      const auto same = std::count_if(gaps.begin(), gaps.end(), [&](float g) { return std::abs(g - median) < 0.02f; });
      if (std::abs(median) > 0.05f && static_cast<size_t>(same) * 3 >= gaps.size() * 2) tc = std::round(median * 1000) / 1000;
    }
  }
  float penX = 0, penY = 0;  // where show() left the pen
  auto show = [&](bool bold) {
    std::string& o = Out();
    // The size goes in Tf, where readers look for it; the matrix only places and flips.
    o += "BT\n/" + font->name + " " + N(size) + " Tf\n";
    if (tc != 0) o += N(tc) + " Tc\n";
    // The render mode is text state, which lasts past ET: set every time.
    if (font->outlines) o += "3 Tr\n";
    else if (bold) o += "2 Tr " + N(size / 48) + " w\n";
    else o += "0 Tr\n";
    const char* skew = font->oblique ? " 0.33 " : " 0 ";
    std::string tj;
    float x = 0, y = 0;
    bool openTj = false;
    for (uint32_t i = 0; i < run.count; ++i) {
      if (!openTj || std::abs(at[i].y - y) > 0.001f) {
        if (openTj) o += tj + "]TJ\n";
        o += std::string("1 0") + skew + "-1 " + N(at[i].x) + " " + N(at[i].y) + " Tm\n";
        tj = "[";
        x = at[i].x;
        y = at[i].y;
        openTj = true;
      } else {
        // The gap to this glyph from where the last one left the pen, in 1/1000 of the size.
        const float adj = (x - at[i].x) * 1000 / size;
        const float rounded = std::round(adj * 10) / 10;
        if (rounded != 0) tj += N(rounded);
        x -= rounded * size / 1000;
      }
      tj += "<";
      Hex4(tj, run.indices[i]);
      tj += ">";
      x += widths[i] * size / 1000 + tc;
    }
    if (openTj) o += tj + "]TJ\n";
    o += "ET\n";
    if (tc != 0) o += "0 Tc\n";  // text state outlasts ET
    penX = x;
    penY = y;
  };
  // A text object a run of spaces may be added to: plain, left to right, nothing around it.
  auto remember = [&] {
    if (!actual && tc == 0 && !rtl) last_ = {run.face, font, Out().size(), size, penY, penX};
  };
  if (font->outlines) {
    Out() += text;
    show(false);
    if (actual) Out() += "EMC\n";
    remember();
    return;
  }
  if (font->bold && b.kind == Brush::Kind::Solid) {
    // Bold simulation: the glyphs filled and stroked in the same color.
    Save();
    Paint(b, true, [] {}, {});
    Paint(b, false, [&] { Out() += text; show(true); if (actual) Out() += "EMC\n"; }, {});
    Restore();
    return;
  }
  Paint(b, false, [&] { Out() += text; show(false); if (actual) Out() += "EMC\n"; }, {});
  remember();
}

// ---------------------------------------------------------------------------

void PdfDocument::Impl::WriteFonts() {
  for (const auto& f : fonts) {
    std::vector<uint16_t> glyphs;
    for (const auto& [g, width] : f->widths) glyphs.push_back(g);
    const float toThousand = 1000.f / std::max<uint16_t>(f->info.unitsPerEm, 16);
    // The font program: a TrueType subset, or a CFF font whole; none for outline-drawn fonts.
    std::string file;
    const sfnt::Tables tables = FaceTables(f->face.Get());
    uint32_t fontFile = 0;
    if (!f->outlines) {
      std::vector<uint8_t> program;
      if (f->info.cff) program = sfnt::Whole(tables);
      else if (f->info.subsettable) program = sfnt::Subset(tables, glyphs);
      else {
        std::vector<uint16_t> all(f->info.numGlyphs);
        for (uint16_t i = 0; i < all.size(); ++i) all[i] = i;
        program = sfnt::Subset(tables, all);
      }
      if (!program.empty()) {
        fontFile = w.Reserve();
        w.Stream(fontFile, f->info.cff ? "/Subtype/OpenType" : "/Length1 " + std::to_string(program.size()),
                 std::string_view(reinterpret_cast<const char*>(program.data()), program.size()));
      }
    }
    // A subset's name starts with a tag of six capitals made from its glyphs.
    std::string base = f->info.postScriptName.empty() ? "Font" + std::to_string(f->object) : f->info.postScriptName;
    if (fontFile && !f->info.cff) {
      uint32_t h = 2166136261u;
      for (uint16_t g : glyphs) h = (h ^ g) * 16777619u;
      std::string tag;
      for (int i = 0; i < 6; ++i, h /= 26) tag += static_cast<char>('A' + h % 26);
      base = tag + "+" + base;
    }
    if (f->bold) base += ",Bold";
    if (f->oblique) base += f->bold ? "Italic" : ",Italic";
    const uint32_t descriptor = w.Reserve(), cid = w.Reserve(), toUnicode = w.Reserve();
    int flags = 4;  // symbolic: glyphs by CID
    if (f->info.italicAngle != 0 || f->oblique) flags |= 64;
    w.Object(descriptor,
             "<</Type/FontDescriptor/FontName/" + base + "/Flags " + std::to_string(flags) + "/FontBBox[" +
                 N(f->info.xMin * toThousand) + " " + N(f->info.yMin * toThousand) + " " + N(f->info.xMax * toThousand) +
                 " " + N(f->info.yMax * toThousand) + "]/ItalicAngle " + N(f->info.italicAngle) + "/Ascent " +
                 N(f->info.ascent * toThousand) + "/Descent " + N(f->info.descent * toThousand) + "/CapHeight " +
                 N(f->info.capHeight * toThousand) + "/StemV " + std::to_string(f->info.weight >= 600 ? 120 : 80) +
                 (fontFile ? (f->info.cff ? "/FontFile3 " : "/FontFile2 ") + Ref(fontFile) : std::string()) + ">>");
    // Widths, in runs of consecutive glyphs.
    std::string widths;
    for (auto it = f->widths.begin(); it != f->widths.end();) {
      const uint16_t first = it->first;
      widths += std::to_string(first) + "[";
      uint16_t expect = first;
      while (it != f->widths.end() && it->first == expect) {
        widths += std::to_string(it->second) + " ";
        ++it;
        ++expect;
      }
      widths += "]";
    }
    const bool cff = f->info.cff && fontFile;
    w.Object(cid, std::string("<</Type/Font/Subtype/") + (cff ? "CIDFontType0" : "CIDFontType2") + "/BaseFont/" + base +
                      "/CIDSystemInfo<</Registry(Adobe)/Ordering(Identity)/Supplement 0>>/FontDescriptor " +
                      Ref(descriptor) + "/DW 1000/W[" + widths + "]" + (cff ? "" : "/CIDToGIDMap/Identity") + ">>");
    // ToUnicode: what each glyph shows.
    std::string cmap =
        "/CIDInit/ProcSet findresource begin\n12 dict begin\nbegincmap\n/CIDSystemInfo<</Registry(Adobe)/Ordering(UCS)/"
        "Supplement 0>>def\n/CMapName/Adobe-Identity-UCS def\n/CMapType 2 def\n1 begincodespacerange\n<0000><FFFF>\n"
        "endcodespacerange\n";
    std::vector<std::pair<uint16_t, const std::u16string*>> entries;
    for (const auto& [g, s] : f->text)
      if (!s.empty()) entries.push_back({g, &s});
    for (size_t i = 0; i < entries.size(); i += 100) {
      const size_t n = std::min<size_t>(100, entries.size() - i);
      cmap += std::to_string(n) + " beginbfchar\n";
      for (size_t k = i; k < i + n; ++k) {
        cmap += "<";
        Hex4(cmap, entries[k].first);
        cmap += "><";
        for (char16_t c : *entries[k].second) Hex4(cmap, c);
        cmap += ">\n";
      }
      cmap += "endbfchar\n";
    }
    cmap += "endcmap\nCMapName currentdict/CMap defineresource pop\nend\nend\n";
    w.Stream(toUnicode, "", cmap);
    w.Object(f->object, "<</Type/Font/Subtype/Type0/BaseFont/" + base + "/Encoding/Identity-H/DescendantFonts[" +
                            Ref(cid) + "]/ToUnicode " + Ref(toUnicode) + ">>");
  }
}

std::vector<uint8_t> PdfDocument::Impl::Finish() {
  WriteFonts();
  if (!pagesObject) pagesObject = w.Reserve();
  std::string kids;
  for (const PageRecord& p : pages) {
    std::string annots;
    for (const std::string& a : p.annots) {
      const uint32_t o = w.Reserve();
      w.Object(o, a);
      annots += Ref(o) + " ";
    }
    w.Object(p.object, "<</Type/Page/Parent " + Ref(pagesObject) + "/MediaBox[0 0 " + N(p.width) + " " + N(p.height) +
                           "]/Contents " + Ref(p.contents) + "/Resources " + Ref(p.resources) +
                           (annots.empty() ? "" : "/Annots[" + annots + "]") + ">>");
    kids += Ref(p.object) + " ";
  }
  w.Object(pagesObject, "<</Type/Pages/Kids[" + kids + "]/Count " + std::to_string(pages.size()) + ">>");

  // The outline: a tree by level.
  uint32_t outlines = 0;
  if (!outline.empty()) {
    outlines = w.Reserve();
    struct Node {
      uint32_t object = 0;
      size_t parent = 0;
      int level = -1;
      std::vector<size_t> kids;
      size_t item = 0;
    };
    std::vector<Node> nodes(1);  // 0: the root
    nodes[0].object = outlines;
    std::vector<size_t> stack{0};
    for (size_t i = 0; i < outline.size(); ++i) {
      const int level = std::max(0, outline[i].level);
      while (stack.size() > 1 && nodes[stack.back()].level >= level) stack.pop_back();
      Node n;
      n.object = w.Reserve();
      n.parent = stack.back();
      n.level = level;
      n.item = i;
      nodes.push_back(n);
      nodes[stack.back()].kids.push_back(nodes.size() - 1);
      stack.push_back(nodes.size() - 1);
    }
    std::function<int(size_t)> count = [&](size_t n) {
      int c = 0;
      for (size_t k : nodes[n].kids) c += 1 + count(k);
      return c;
    };
    for (size_t n = 1; n < nodes.size(); ++n) {
      const OutlineItem& item = outline[nodes[n].item];
      const Node& parent = nodes[nodes[n].parent];
      const auto pos = std::find(parent.kids.begin(), parent.kids.end(), n) - parent.kids.begin();
      std::string d = "<</Title" + TextString(item.title) + "/Parent " + Ref(parent.object);
      if (pos > 0) d += "/Prev " + Ref(nodes[parent.kids[pos - 1]].object);
      if (static_cast<size_t>(pos) + 1 < parent.kids.size()) d += "/Next " + Ref(nodes[parent.kids[pos + 1]].object);
      if (!nodes[n].kids.empty()) {
        d += "/First " + Ref(nodes[nodes[n].kids.front()].object) + "/Last " + Ref(nodes[nodes[n].kids.back()].object);
        d += "/Count " + std::to_string(count(n));  // open
      }
      if (item.page < pages.size())
        d += "/Dest[" + Ref(pages[item.page].object) + "/XYZ 0 " + N(pages[item.page].height - item.y) + " null]";
      w.Object(nodes[n].object, d + ">>");
    }
    w.Object(outlines, "<</Type/Outlines/First " + Ref(nodes[nodes[0].kids.front()].object) + "/Last " +
                           Ref(nodes[nodes[0].kids.back()].object) + "/Count " + std::to_string(count(0)) + ">>");
  }

  const uint32_t info = w.Reserve(), root = w.Reserve();
  std::time_t now = std::time(nullptr);
  std::tm utc{};
  gmtime_s(&utc, &now);
  char date[32];
  std::strftime(date, sizeof date, "D:%Y%m%d%H%M%SZ", &utc);
  std::string infoDict = "<</Producer(Featherweight)/CreationDate(" + std::string(date) + ")";
  if (!title.empty()) infoDict += "/Title" + TextString(title);
  if (!author.empty()) infoDict += "/Author" + TextString(author);
  w.Object(info, infoDict + ">>");
  w.Object(root, "<</Type/Catalog/Pages " + Ref(pagesObject) +
                     (outlines ? "/Outlines " + Ref(outlines) + "/PageMode/UseOutlines" : std::string()) +
                     (title.empty() ? "" : "/ViewerPreferences<</DisplayDocTitle true>>") + ">>");
  const std::string file = w.Finish(root, info);
  return std::vector<uint8_t>(file.begin(), file.end());
}

PdfDocument::PdfDocument() : impl_(std::make_unique<Impl>()) { impl_->pagesObject = impl_->w.Reserve(); }
PdfDocument::~PdfDocument() = default;

void PdfDocument::AddPage(float width, float height, const std::function<void(Painter&)>& paint) {
  Impl& d = *impl_;
  Impl::PageRecord page;
  page.object = d.w.Reserve();
  page.contents = d.w.Reserve();
  page.resources = d.w.Reserve();
  page.width = std::max(1.f, width);
  page.height = std::max(1.f, height);
  {
    PdfPainter painter(d, page.width, page.height);
    if (paint) paint(painter);
    painter.End();
    d.w.Stream(page.contents, "", painter.PageOps());
    d.w.Object(page.resources, painter.PageResources().Dict());
  }
  d.pages.push_back(std::move(page));
}

size_t PdfDocument::PageCount() const { return impl_->pages.size(); }

void PdfDocument::AddLink(size_t page, const Rect& rect, const std::string& uri) {
  Impl& d = *impl_;
  if (page >= d.pages.size() || uri.empty()) return;
  const float h = d.pages[page].height;
  d.pages[page].annots.push_back("<</Type/Annot/Subtype/Link/Rect[" + N(rect.left) + " " + N(h - rect.bottom) + " " +
                                 N(rect.right) + " " + N(h - rect.top) + "]/Border[0 0 0]/A<</S/URI/URI" + Literal(uri) +
                                 ">>>>");
}

void PdfDocument::AddLink(size_t page, const Rect& rect, size_t toPage, float toY) {
  Impl& d = *impl_;
  if (page >= d.pages.size() || toPage >= d.pages.size()) return;
  const float h = d.pages[page].height;
  d.pages[page].annots.push_back("<</Type/Annot/Subtype/Link/Rect[" + N(rect.left) + " " + N(h - rect.bottom) + " " +
                                 N(rect.right) + " " + N(h - rect.top) + "]/Border[0 0 0]/Dest[" +
                                 Ref(d.pages[toPage].object) + "/XYZ 0 " + N(d.pages[toPage].height - toY) + " null]>>");
}

void PdfDocument::AddOutline(int level, const std::u16string& title, size_t page, float y) {
  impl_->outline.push_back({level, title, page, y});
}

void PdfDocument::SetInfo(const std::u16string& title, const std::u16string& author) {
  impl_->title = title;
  impl_->author = author;
}

std::vector<uint8_t> PdfDocument::Finish() { return impl_->Finish(); }

}  // namespace draw
