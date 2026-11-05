// Layout (Layout.h): the public face of the pagination engine (Engine.h), and what is done with
// pages once they exist: painting, the layout dump, hit-testing and carets.
#include "doc/layout/Layout.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <vector>

#include <dwrite.h>
#include <wrl/client.h>

#include "core/OpenTimings.h"
#include "core/Strings.h"
#include "doc/layout/Engine.h"
#include "draw/Painter.h"

namespace fw::doc::layout {

struct Layout::Impl {
  Impl(const Document& doc, text::Fonts& fonts, const Options& options, OpenTimings* timings)
      : engine(doc, fonts, options, timings) {}
  Engine engine;
};

Layout::Layout(const Document& doc, text::Fonts& fonts, Options options, OpenTimings* timings)
    : impl_(std::make_unique<Impl>(doc, fonts, options, timings)) {}
Layout::~Layout() = default;

void Layout::LayOutPages(size_t count) {
  Engine& e = impl_->engine;
  while (!e.Done() && e.Pages().size() < count) e.NextPage();
}

bool Layout::Continue(double milliseconds) {
  Engine& e = impl_->engine;
  auto start = std::chrono::steady_clock::now();
  do {
    if (!e.NextPage()) break;
  } while (!e.Done() && std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() <
                            milliseconds);
  return !e.Done();
}

bool Layout::Done() const { return impl_->engine.Done(); }
size_t Layout::PageCount() const { return impl_->engine.Pages().size(); }
const Page& Layout::GetPage(size_t index) const { return impl_->engine.Pages()[index]; }
uint32_t Layout::Revision() const { return impl_->engine.Revision(); }
size_t Layout::MemoryBytes() const { return impl_->engine.MemoryBytes(); }

// ---- Selection and find

namespace {

// The model offset of a layout offset on a line, through the paragraph's own text mapping.
uint32_t ToModel(Engine& e, const LineBox& lb, uint32_t layoutOffset) {
  ParaText pt;
  e.BuildText(lb.para, PageFields(), pt);
  return pt.ModelOffset(layoutOffset);
}

}  // namespace

// A turned line's frame on the page (LineBox::turn).
static draw::Matrix TurnOf(const LineBox& lb) {
  return draw::Matrix::Rotate(90.0f * static_cast<float>(lb.turn)) * draw::Matrix::Translate(lb.turnX, lb.turnY);
}

// A page point in a line's own frame: itself unless the line is turned.
static draw::Point InFrame(const LineBox& lb, float x, float y) {
  switch (lb.turn) {
    case 1: return {y - lb.turnY, lb.turnX - x};
    case 2: return {lb.turnX - x, lb.turnY - y};
    case 3: return {lb.turnY - y, x - lb.turnX};
    default: return {x, y};
  }
}

// A rectangle in a line's frame on the page (turned by quarter turns, it stays a rectangle).
static Rect OnPage(const LineBox& lb, const Rect& r) {
  if (!lb.turn) return r;
  const draw::Matrix m = TurnOf(lb);
  const draw::Point a = m.Apply({r.left, r.top}), b = m.Apply({r.right, r.bottom});
  return {std::min(a.x, b.x), std::min(a.y, b.y), std::max(a.x, b.x), std::max(a.y, b.y)};
}

std::optional<TextPos> Layout::HitTest(size_t page, float x, float y) const {
  Engine& e = impl_->engine;
  if (page >= e.Pages().size()) return std::nullopt;
  const Page& p = e.Pages()[page];
  const LineBox* best = nullptr;
  float bestDistance = 1e30f;
  float bestX = x;
  for (const LineBox& lb : p.lines) {
    if (lb.para == kNone) continue;
    const draw::Point at = InFrame(lb, x, y);
    float dy = at.y < lb.top ? lb.top - at.y : at.y >= lb.top + lb.height ? at.y - (lb.top + lb.height) : 0;
    float left = lb.x + lb.line.left, right = lb.x + lb.line.right;
    float dx = at.x < left ? left - at.x : at.x > right ? at.x - right : 0;
    float d = dy * 4 + dx;  // the line under the point first, then the nearest
    if (d < bestDistance) {
      bestDistance = d;
      best = &lb;
      bestX = at.x;
    }
  }
  if (!best) return std::nullopt;
  uint32_t offset = best->line.HitTest(bestX - best->x);
  return TextPos{best->para, ToModel(e, *best, offset)};
}

std::optional<Layout::Caret> Layout::CaretAt(TextPos pos) {
  Engine& e = impl_->engine;
  for (size_t i = 0;; ++i) {
    while (i >= e.Pages().size() && !e.Done()) e.NextPage();
    if (i >= e.Pages().size()) return std::nullopt;
    const Page& p = e.Pages()[i];
    for (size_t k = 0; k < p.lines.size(); ++k) {
      const LineBox& lb = p.lines[k];
      if (lb.para != pos.para) continue;
      ParaText pt;
      e.BuildText(lb.para, PageFields(), pt);
      uint32_t at = pt.LayoutOffset(pos.offset);
      bool lastOfPara = lb.line.ending == text::LineEnd::ParagraphEnd;
      if (at >= lb.line.start && (at < lb.line.end || (at == lb.line.end && lastOfPara))) {
        const float cx = lb.x + lb.line.CaretX(at);
        if (!lb.turn) return Caret{i, cx, lb.top, lb.top + lb.height};
        const Rect r = OnPage(lb, {cx, lb.top, cx, lb.top + lb.height});
        return Caret{i, r.left, r.top, r.top};
      }
    }
  }
}

std::vector<std::pair<size_t, Rect>> Layout::Ranges(TextPos from, TextPos to) {
  std::vector<std::pair<size_t, Rect>> out;
  if (to < from) std::swap(from, to);
  Engine& e = impl_->engine;
  bool started = false, finished = false;
  for (size_t i = 0; !finished; ++i) {
    while (i >= e.Pages().size() && !e.Done()) e.NextPage();
    if (i >= e.Pages().size()) break;
    const Page& p = e.Pages()[i];
    for (const LineBox& lb : p.lines) {
      if (lb.para == kNone || lb.para < from.para || lb.para > to.para) continue;
      ParaText pt;
      e.BuildText(lb.para, PageFields(), pt);
      uint32_t a = lb.para == from.para ? pt.LayoutOffset(from.offset) : 0;
      const uint32_t end = lb.para == to.para ? pt.LayoutOffset(to.offset) : UINT32_MAX;
      // The line that holds the end is the last: nothing after it is in the range, even when the
      // range ends exactly at the line's end.
      if (lb.para == to.para && end <= lb.line.end) finished = true;
      a = std::max(a, lb.line.start);
      const uint32_t b = std::min(end, lb.line.end);
      if (a >= b) continue;
      started = true;
      for (const auto& [l, r] : lb.line.Ranges(a, b)) out.push_back({i, OnPage(lb, {lb.x + l, lb.top, lb.x + r, lb.top + lb.height})});
    }
    if (started && e.Pages()[i].lines.empty()) break;
  }
  return out;
}

std::optional<size_t> Layout::PageOf(TextPos pos) {
  if (auto c = CaretAt(pos)) return c->page;
  return std::nullopt;
}

// ---- Painting

namespace {

draw::Color ColorOf(uint32_t rgb) {
  return {static_cast<float>((rgb >> 16) & 255) / 255.0f, static_cast<float>((rgb >> 8) & 255) / 255.0f,
          static_cast<float>(rgb & 255) / 255.0f, 1};
}

void Line(draw::Painter& painter, float x1, float y1, float x2, float y2, float width, uint32_t rgb,
          std::vector<float> dashes = {}) {
  draw::Path path;
  path.MoveTo({x1, y1});
  path.LineTo({x2, y2});
  draw::Pen pen;
  pen.brush = draw::Brush::Solid(ColorOf(rgb));
  pen.width = width;
  pen.dashes = std::move(dashes);
  painter.StrokePath(path, pen);
}

void StrokeEdge(draw::Painter& painter, const Edge& e) {
  const Border& b = e.border;
  float w = std::max(0.25f, Pt(b.width));
  uint32_t rgb = b.color.automatic ? 0 : b.color.rgb;
  std::vector<float> dashes;
  switch (b.style) {
    case BorderStyle::Dotted: dashes = {1, 1}; break;
    case BorderStyle::Dashed: case BorderStyle::DashSmallGap: dashes = {3, 2}; break;
    case BorderStyle::DotDash: dashes = {3, 1, 1, 1}; break;
    case BorderStyle::DotDotDash: dashes = {3, 1, 1, 1, 1, 1}; break;
    default: break;
  }
  if (b.style == BorderStyle::Double) {
    bool horizontal = std::abs(e.y2 - e.y1) < std::abs(e.x2 - e.x1);
    float ox = horizontal ? 0 : w, oy = horizontal ? w : 0;
    Line(painter, e.x1 - ox, e.y1 - oy, e.x2 - ox, e.y2 - oy, w, rgb);
    Line(painter, e.x1 + ox, e.y1 + oy, e.x2 + ox, e.y2 + oy, w, rgb);
    return;
  }
  Line(painter, e.x1, e.y1, e.x2, e.y2, w, rgb, std::move(dashes));
}

void Placeholder(draw::Painter& painter, const Rect& r) {
  draw::Pen pen;
  pen.brush = draw::Brush::Solid({0.75f, 0.75f, 0.75f, 1});
  pen.width = 0.75f;
  painter.StrokePath(draw::Path::FromRect({r.left, r.top, r.right, r.bottom}), pen);
}

}  // namespace

void Layout::Paint(draw::Painter& painter, size_t page, const DrawingPainter& drawing) const {
  const Engine& e = impl_->engine;
  if (page >= e.Pages().size()) return;
  const Page& p = e.Pages()[page];
  auto paintLine = [&](const LineBox& lb) {
    const bool clip = lb.clip.Width() > 0;
    if (clip || lb.turn) painter.Save();
    if (clip) painter.ClipRect({lb.clip.left, lb.clip.top, lb.clip.right, lb.clip.bottom});
    if (lb.turn) painter.Transform(TurnOf(lb));
    for (const text::GlyphPiece& g : lb.line.pieces) {
      const size_t at = lb.paint + g.run;
      if (at >= p.paints.size()) continue;
      const RunPaint& rp = p.paints[at];
      if (rp.highlight && !g.leader)
        painter.FillRect({lb.x + g.x, lb.top, lb.x + g.x + g.width, lb.top + lb.height},
                         draw::Brush::Solid(ColorOf(rp.highlightColor)));
    }
    for (const text::GlyphPiece& g : lb.line.pieces) {
      const size_t at = lb.paint + g.run;
      const RunPaint* rp = at < p.paints.size() ? &p.paints[at] : nullptr;
      uint32_t color = rp ? rp->color : 0;
      if (!g.glyphs.empty()) {
        const draw::Point origin{lb.x + g.OriginX(), lb.baseline - g.rise};
        if (g.scale != 1 && g.scale > 0) {
          // Scaled characters (w:w): the glyphs drawn through a horizontal scale about the origin.
          std::vector<float> advances(g.advances);
          for (float& a : advances) a /= g.scale;
          draw::GlyphRun run = g.Run();
          run.advances = advances.data();
          painter.Save();
          painter.Transform({g.scale, 0, 0, 1, origin.x * (1 - g.scale), 0});
          painter.DrawGlyphs(origin, run, draw::Brush::Solid(ColorOf(color)));
          painter.Restore();
        } else {
          painter.DrawGlyphs(origin, g.Run(), draw::Brush::Solid(ColorOf(color)));
        }
      }
      if (!rp || g.hyphen || g.leader) continue;
      float x1 = lb.x + g.x, x2 = x1 + g.width, base = lb.baseline - g.rise;
      if (rp->underline != Underline::None) {
        float t = std::max(0.5f, rp->underlineThickness), y = base + std::max(1.0f, rp->underlineOffset);
        bool dbl = rp->underline == Underline::Double || rp->underline == Underline::WavyDouble;
        float w = rp->underline == Underline::Thick ? t * 2 : t;
        std::vector<float> dashes;
        if (rp->underline == Underline::Dotted || rp->underline == Underline::DottedHeavy) dashes = {1, 1};
        else if (rp->underline == Underline::Dash || rp->underline == Underline::DashLong) dashes = {3, 2};
        Line(painter, x1, y, x2, y, w, rp->underlineColor, dashes);
        if (dbl) Line(painter, x1, y + t * 2, x2, y + t * 2, w, rp->underlineColor);
      }
      if (rp->strike || rp->doubleStrike) {
        float t = std::max(0.5f, rp->strikeThickness), y = base - std::max(1.0f, rp->strikeOffset);
        Line(painter, x1, y, x2, y, t, color);
        if (rp->doubleStrike) Line(painter, x1, y - t * 2, x2, y - t * 2, t, color);
      }
    }
    if (clip || lb.turn) painter.Restore();
  };
  // A text box's lines go on top of its own shape, which is filled.
  auto drawOne = [&](const DrawingBox& d) {
    if (drawing) drawing(painter, d);
    else Placeholder(painter, d.rect);
    for (const LineBox& lb : p.lines)
      if (lb.drawing == d.drawing) paintLine(lb);
  };
  for (const DrawingBox& d : p.drawings)
    if (d.behindText) drawOne(d);
  for (const Fill& f : p.fills) painter.FillRect({f.rect.left, f.rect.top, f.rect.right, f.rect.bottom}, draw::Brush::Solid(ColorOf(f.color)));
  for (const LineBox& lb : p.lines)
    if (lb.drawing == doc::kNone) paintLine(lb);
  for (const Edge& edge : p.edges) StrokeEdge(painter, edge);
  for (const DrawingBox& d : p.drawings)
    if (!d.behindText) drawOne(d);
}

// ---- Fonts

namespace {

// The East Asian scripts a font can be for, by Windows charset.
enum class Script { None, Japanese, Korean, SimplifiedChinese, TraditionalChinese };

// Well-known East Asian families, by their English and native names: the script, whether they're
// serif (Mincho, Song, Ming, Batang) and whether fixed pitch (MS Mincho; MS PMincho is proportional).
struct KnownFamily {
  const wchar_t* names[3];
  Script script;
  bool serif, fixed;
};
constexpr KnownFamily kKnownFamilies[] = {
    {{L"MS Mincho", L"\xFF2D\xFF33 \x660E\x671D", L"MS \x660E\x671D"}, Script::Japanese, true, true},
    {{L"MS PMincho", L"\xFF2D\xFF33 \xFF30\x660E\x671D", L"MS P\x660E\x671D"}, Script::Japanese, true, false},
    {{L"MS Gothic", L"\xFF2D\xFF33 \x30B4\x30B7\x30C3\x30AF", L"MS \x30B4\x30B7\x30C3\x30AF"}, Script::Japanese, false, true},
    {{L"MS PGothic", L"\xFF2D\xFF33 \xFF30\x30B4\x30B7\x30C3\x30AF", L"MS P\x30B4\x30B7\x30C3\x30AF"}, Script::Japanese, false, false},
    {{L"Yu Mincho", L"\x6E38\x660E\x671D", nullptr}, Script::Japanese, true, false},
    {{L"Yu Gothic", L"\x6E38\x30B4\x30B7\x30C3\x30AF", nullptr}, Script::Japanese, false, false},
    {{L"Meiryo", L"\x30E1\x30A4\x30EA\x30AA", nullptr}, Script::Japanese, false, false},
    {{L"SimSun", L"\x5B8B\x4F53", nullptr}, Script::SimplifiedChinese, true, false},
    {{L"NSimSun", L"\x65B0\x5B8B\x4F53", nullptr}, Script::SimplifiedChinese, true, true},
    {{L"SimHei", L"\x9ED1\x4F53", nullptr}, Script::SimplifiedChinese, false, false},
    {{L"Microsoft YaHei", L"\x5FAE\x8F6F\x96C5\x9ED1", nullptr}, Script::SimplifiedChinese, false, false},
    {{L"KaiTi", L"\x6977\x4F53", nullptr}, Script::SimplifiedChinese, true, false},
    {{L"FangSong", L"\x4EFF\x5B8B", nullptr}, Script::SimplifiedChinese, true, false},
    {{L"PMingLiU", L"\x65B0\x7D30\x660E\x9AD4", nullptr}, Script::TraditionalChinese, true, false},
    {{L"MingLiU", L"\x7D30\x660E\x9AD4", nullptr}, Script::TraditionalChinese, true, true},
    {{L"Microsoft JhengHei", L"\x5FAE\x8EDF\x6B63\x9ED1\x9AD4", nullptr}, Script::TraditionalChinese, false, false},
    {{L"DFKai-SB", L"\x6A19\x6977\x9AD4", nullptr}, Script::TraditionalChinese, true, false},
    {{L"Batang", L"\xBC14\xD0D5", nullptr}, Script::Korean, true, false},
    {{L"BatangChe", L"\xBC14\xD0D5\xCCB4", nullptr}, Script::Korean, true, true},
    {{L"Gungsuh", L"\xAD81\xC11C", nullptr}, Script::Korean, true, false},
    {{L"Gulim", L"\xAD74\xB9BC", nullptr}, Script::Korean, false, false},
    {{L"GulimChe", L"\xAD74\xB9BC\xCCB4", nullptr}, Script::Korean, false, true},
    {{L"Dotum", L"\xB3CB\xC6C0", nullptr}, Script::Korean, false, false},
    {{L"Malgun Gothic", L"\xB9D1\xC740 \xACE0\xB515", nullptr}, Script::Korean, false, false},
};

const KnownFamily* Known(std::wstring_view family) {
  const std::wstring key = ToLower(std::wstring(family));
  for (const KnownFamily& k : kKnownFamilies)
    for (const wchar_t* n : k.names)
      if (n && ToLower(std::wstring(n)) == key) return &k;
  return nullptr;
}

// The script of an East Asian family the font table describes, or None: its charset; else the
// code pages it covers (sig csb0 bits 17-21), when they name one script; else its name's letters.
Script ScriptOf(const FontInfo* f, std::wstring_view family) {
  if (f) {
    if (f->charset == "80") return Script::Japanese;
    if (f->charset == "81" || f->charset == "82") return Script::Korean;
    if (f->charset == "86") return Script::SimplifiedChinese;
    if (f->charset == "88") return Script::TraditionalChinese;
    const uint32_t csb0 = f->sig[4];
    const bool jp = csb0 & (1u << 17), sc = csb0 & (1u << 18), kr = csb0 & ((1u << 19) | (1u << 21)),
               tc = csb0 & (1u << 20);
    if (jp + sc + kr + tc == 1) return jp ? Script::Japanese : sc ? Script::SimplifiedChinese : kr ? Script::Korean : Script::TraditionalChinese;
  }
  bool han = false;
  for (wchar_t c : family) {
    if ((c >= 0xAC00 && c <= 0xD7AF) || (c >= 0x1100 && c <= 0x11FF) || (c >= 0x3130 && c <= 0x318F)) return Script::Korean;
    if ((c >= 0x3040 && c <= 0x30FF) || (c >= 0xFF66 && c <= 0xFF9F)) return Script::Japanese;
    if (c >= 0x4E00 && c <= 0x9FFF) han = true;
  }
  return han ? Script::SimplifiedChinese : Script::None;
}

// Whether a name says serif: Mincho, Song, Ming, Kai, Fang, Batang, Myeongjo, Gungsuh.
bool SerifName(std::wstring_view family) {
  const std::wstring n = ToLower(std::wstring(family));
  for (const wchar_t* k : {L"\x660E\x671D", L"mincho", L"\x5B8B", L"song", L"sun", L"\x660E\x9AD4", L"ming", L"\x6977",
                           L"kai", L"\x4EFF", L"fang", L"\xBC14\xD0D5", L"batang", L"\xBA85\xC870", L"myeongjo",
                           L"\xAD81\xC11C", L"gungsuh"})
    if (n.find(k) != std::wstring::npos) return true;
  return false;
}

int HexByte(std::string_view s, size_t at) {
  int v = 0;
  for (size_t i = at; i < at + 2 && i < s.size(); ++i) {
    const char c = s[i];
    const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
    if (d < 0) return -1;
    v = v * 16 + d;
  }
  return s.size() >= at + 2 ? v : -1;
}

// Whether the PC has a family, by any of its localized names.
bool SystemHas(const wchar_t* family) {
  static const Microsoft::WRL::ComPtr<IDWriteFontCollection> fonts = [] {
    Microsoft::WRL::ComPtr<IDWriteFactory> factory;
    Microsoft::WRL::ComPtr<IDWriteFontCollection> collection;
    if (SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                      reinterpret_cast<IUnknown**>(factory.GetAddressOf()))))
      factory->GetSystemFontCollection(&collection);
    return collection;
  }();
  UINT32 index = 0;
  BOOL exists = FALSE;
  return fonts && SUCCEEDED(fonts->FindFamilyName(family, &index, &exists)) && exists;
}

// The East Asian family drawn for a missing one of the script: the closest in style that the PC
// has, ending with the ones every Windows 10 and 11 has (Yu Gothic, MS Gothic, SimSun, Microsoft
// YaHei, Microsoft JhengHei, Malgun Gothic). Empty if it has none of them.
std::wstring EastAsianSubstitute(Script script, bool serif, bool fixed) {
  std::vector<const wchar_t*> c;
  switch (script) {
    case Script::Japanese:
      if (serif) c = fixed ? std::vector<const wchar_t*>{L"MS Mincho", L"Yu Mincho", L"MS Gothic", L"Yu Gothic"}
                           : std::vector<const wchar_t*>{L"MS PMincho", L"Yu Mincho", L"MS PGothic", L"Yu Gothic"};
      else c = fixed ? std::vector<const wchar_t*>{L"MS Gothic", L"Yu Gothic"}
                     : std::vector<const wchar_t*>{L"MS PGothic", L"Yu Gothic", L"Meiryo", L"MS Gothic"};
      break;
    case Script::SimplifiedChinese:
      c = serif ? std::vector<const wchar_t*>{fixed ? L"NSimSun" : L"SimSun", L"SimSun", L"Microsoft YaHei"}
                : std::vector<const wchar_t*>{L"Microsoft YaHei", L"SimHei", L"SimSun"};
      break;
    case Script::TraditionalChinese:
      c = serif ? std::vector<const wchar_t*>{fixed ? L"MingLiU" : L"PMingLiU", L"MingLiU", L"Microsoft JhengHei"}
                : std::vector<const wchar_t*>{L"Microsoft JhengHei", L"PMingLiU"};
      break;
    case Script::Korean:
      c = serif ? std::vector<const wchar_t*>{fixed ? L"BatangChe" : L"Batang", L"Batang", L"Malgun Gothic"}
                : std::vector<const wchar_t*>{fixed ? L"GulimChe" : L"Malgun Gothic", L"Malgun Gothic", L"Gulim", L"Dotum"};
      break;
    case Script::None: break;
  }
  for (const wchar_t* name : c)
    if (SystemHas(name)) return name;
  return {};
}

}  // namespace

std::wstring SubstituteFamily(const Document& doc, std::wstring_view family) {
  if (!text::Fonts::BuiltInSubstitute(family).empty()) return {};
  const std::string name = WideToUtf8(family);
  const FontInfo* f = nullptr;
  for (const FontInfo& info : doc.Fonts())
    if (info.name == name) {
      f = &info;
      break;
    }
  // An East Asian font gets an East Asian stand-in of its script, never a Latin one, whose letters
  // and spaces would be wrong for the text (a fixed-pitch Mincho as Courier New): by the family's
  // own name when it's a well-known one, else by its charset, code pages or name, and its style.
  if (const KnownFamily* k = Known(family)) return EastAsianSubstitute(k->script, k->serif, k->fixed);
  const int serifStyle = f && f->panose.size() >= 4 && f->panose.substr(0, 2) == "02" ? HexByte(f->panose, 2) : -1;
  if (const Script script = ScriptOf(f, family); script != Script::None) {
    const bool serif = SerifName(family) || (serifStyle >= 2 && serifStyle <= 10);
    return EastAsianSubstitute(script, serif, f && f->pitch == "fixed");
  }
  if (!f) return {};
  if (f->pitch == "fixed") return L"Courier New";
  // PANOSE: family kind 2 is Latin text; the second digit is the serif style (11-13 sans serif).
  if (serifStyle >= 11 && serifStyle <= 13) return L"Arial";
  if (serifStyle >= 2 && serifStyle <= 10) return L"Times New Roman";
  if (f->family == "swiss") return L"Arial";
  if (f->family == "roman") return L"Times New Roman";
  if (f->family == "modern") return L"Tahoma";
  return {};
}

// ---- The layout dump

LayoutDump Layout::Dump(std::string source) const {
  const Engine& e = impl_->engine;
  LayoutDump dump;
  dump.source = std::move(source);
  dump.producer = "Featherweight";
  for (const Page& p : e.Pages()) {
    LayoutPage page;
    page.width = p.width;
    page.height = p.height;
    for (const LineBox& lb : p.lines) {
      bool any = false;
      for (char16_t c : lb.text)
        if (c != u' ' && c != u'\t') any = true;
      if (!any) continue;
      LayoutLine line;
      line.story = lb.story;
      line.x = lb.x + lb.line.contentLeft;
      line.baseline = lb.baseline;
      if (lb.turn) {  // where the turned line starts on the page
        const draw::Point at = TurnOf(lb).Apply({static_cast<float>(line.x), static_cast<float>(line.baseline)});
        line.x = at.x;
        line.baseline = at.y;
      }
      line.width = lb.line.contentRight - lb.line.contentLeft;
      float size = 0;
      for (const text::GlyphPiece& g : lb.line.pieces)
        if (!g.leader && !g.hyphen) size = std::max(size, g.emSize);
      line.size = size;
      line.text.assign(lb.text.begin(), lb.text.end());
      page.lines.push_back(std::move(line));
    }
    dump.pages.push_back(std::move(page));
  }
  return dump;
}

}  // namespace fw::doc::layout
