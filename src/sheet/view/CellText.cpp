#include "sheet/view/CellText.h"

#include <algorithm>
#include <cmath>
#include <cwctype>

#include "core/Strings.h"
#include "text/Fonts.h"

namespace fw::sheet::view {

namespace {

// Whether text reads right to left by its first strong character, as Excel's "context" reading
// order decides General alignment: Hebrew, Arabic and the like right, letters of other scripts left.
bool StartsRightToLeft(const std::wstring& s) {
  for (wchar_t c : s) {
    if ((c >= 0x0590 && c <= 0x08FF) || (c >= 0xFB1D && c <= 0xFDFF) || (c >= 0xFE70 && c <= 0xFEFF)) return true;
    if (iswalpha(c)) return false;
  }
  return false;
}

struct FitContext {
  FontMeasure* measure;
  const Font* font;
  const Font* base;
  int avail;  // pixels
};

// The width of a formatted value as Excel fits it to the cell: pads as spaces (an accounting
// format's "_(" and "_)" don't make 45.678,68 "####" in 70 pixels), fills at no width.
int ShownWidth(FontMeasure& m, const Formatted& f, const Font& font, const Font& base) {
  return m.Width(f.text, font, base);
}

bool Fits(void* context, const Formatted& f) {
  auto* c = static_cast<FitContext*>(context);
  if (f.generalLength && f.generalAt + f.generalLength <= f.text.size()) {
    // General, as Excel fits it: a digit's width a character, a leading minus as wide as it is,
    // and room for one digit however narrow the column (measured on 400 cells of 10 widths).
    const std::wstring_view t = f.text;
    const int digit = c->measure->Of(*c->font, *c->base).digit;
    int w = ShownWidth(*c->measure, f, *c->font, *c->base) - c->measure->Width(t.substr(f.generalAt, f.generalLength), *c->font, *c->base);
    for (uint32_t i = 0; i < f.generalLength; ++i)
      w += i == 0 && t[f.generalAt] == L'-' ? c->measure->Width(L"-", *c->font, *c->base) : digit;
    return w <= std::max(c->avail, digit);
  }
  return ShownWidth(*c->measure, f, *c->font, *c->base) <= c->avail;
}

// The pixels of a column a value can't use: two a side and the gridline for a 7-pixel digit,
// more for a bigger font (Arial 12's 9-pixel digit: 7, so "69.483.939" is "########" in 86).
int PadPx(FontMeasure& m, const Font& font, const Font& base) {
  return 2 * ((m.Of(font, base).digit + 3) / 4) + 1;
}

// As a value that doesn't fit shows: no text, the cell filled with '#'.
void Overflow(Formatted& f) {
  f.text.clear();
  f.pads.clear();
  f.overflow = true;
  f.fill = 0;
  f.fillChar = 0;
}

// Excel indents by three spaces of the cell's font a level.
int IndentPx(FontMeasure& m, const Xf& xf, const Font& font, const Font& base) {
  return xf.alignment.indent ? xf.alignment.indent * 3 * m.Of(font, base).space : 0;
}

text::TextStyle StyleOf(const Font& f, const Font& base, double size) {
  text::TextStyle s;
  s.family = FontMeasure::Family(f, base);
  s.size = static_cast<float>(size);
  s.bold = f.bold;
  s.italic = f.italic;
  if (f.vertAlign == VertAlign::Superscript) s.verticalAlign = text::VerticalAlign::Superscript;
  if (f.vertAlign == VertAlign::Subscript) s.verticalAlign = text::VerticalAlign::Subscript;
  return s;
}

}  // namespace

uint32_t FormatColor(const Styles& styles, int index) {
  const std::vector<uint32_t>& palette = styles.Palette();
  const size_t at = static_cast<size_t>(index) + 7;
  const uint32_t rgb = at < palette.size() ? palette[at] : PaletteColor(index);
  return 0xFF000000u | (rgb & 0xFFFFFF);
}

CellText::CellText(const Workbook& book, text::Fonts& fonts, FontMeasure& measure, const NumLocale& locale)
    : book_(book), fonts_(fonts), measure_(measure), locale_(locale) {}

const NumberFormat& CellText::FormatOf(uint32_t numFmtId) {
  auto& slot = formats_[numFmtId];
  if (!slot) {
    const std::string_view code = book_.GetStyles().FormatCode(numFmtId);
    slot = code.empty() ? std::make_unique<NumberFormat>(NumberFormat::Builtin(static_cast<int>(numFmtId), locale_))
                        : std::make_unique<NumberFormat>(Utf8ToWide(code));
  }
  return *slot;
}

std::wstring CellText::Text(const Sheet&, const CellInfo& cell) {
  const Styles& st = book_.GetStyles();
  const Xf& xf = st.CellXf(cell.style);
  Formatted f;
  FormatContext ctx;
  ctx.locale = &locale_;
  ctx.date1904 = book_.Date1904();
  switch (cell.value.type) {
    case ValueType::Empty: return {};
    case ValueType::Number: FormatOf(xf.numFmt).Format(cell.value.number, ctx, f); break;
    case ValueType::String: {
      const NumberFormat& nf = FormatOf(xf.numFmt);
      if (nf.IsGeneral()) return Utf8ToWide(cell.value.text);
      nf.Format(Utf8ToWide(cell.value.text), ctx, f);
      break;
    }
    case ValueType::Bool: return cell.value.boolean ? L"TRUE" : L"FALSE";
    case ValueType::Error: return Utf8ToWide(ErrorText(cell.value.error));
  }
  return f.overflow ? std::wstring() : f.text;
}

void CellText::Show(const Sheet&, const CellInfo& cell, int widthPx, Shown& out, const NumberFormat* format) {
  out = Shown{};
  const Styles& st = book_.GetStyles();
  const Xf& xf = st.CellXf(cell.style);
  const Font& base = st.DefaultFont();
  const Font& font = st.FontOf(xf);
  const int avail = std::max(0, widthPx - PadPx(measure_, font, base) - IndentPx(measure_, xf, font, base));
  FitContext fit{&measure_, &font, &base, avail};
  FormatContext ctx;
  ctx.locale = &locale_;
  ctx.date1904 = book_.Date1904();
  // Shrink to fit makes the font smaller instead: never "####".
  const bool fits = widthPx > 0 && !xf.alignment.shrinkToFit;
  auto tooWide = [&](const Formatted& v) { return fits && measure_.Width(v.text, font, base) > avail; };
  if (fits) {
    ctx.fits = &Fits;
    ctx.fitsContext = &fit;
  }
  Formatted f;
  switch (cell.value.type) {
    case ValueType::Empty: return;
    case ValueType::Number:
      (format ? *format : FormatOf(xf.numFmt)).Format(cell.value.number, ctx, f);
      out.number = true;
      break;
    case ValueType::String: {
      const NumberFormat& nf = format ? *format : FormatOf(xf.numFmt);
      if (nf.IsGeneral()) f.text = Utf8ToWide(cell.value.text);
      else nf.Format(Utf8ToWide(cell.value.text), ctx, f);
      break;
    }
    case ValueType::Bool:
      f.text = cell.value.boolean ? L"TRUE" : L"FALSE";
      out.center = true;
      break;
    case ValueType::Error:
      f.text = Utf8ToWide(ErrorText(cell.value.error));
      out.center = true;
      // An error too wide for the cell is "####", as a number is.
      if (tooWide(f)) Overflow(f);
      break;
  }
  out.overflow = f.overflow;
  out.fill = f.fill >= 0;
  if (f.color) out.color = FormatColor(st, f.color);
  out.text = f.text;
  out.hidden.assign(out.text.size(), 0);
  // Pads: the character whose width the space takes, not drawn.
  for (const Formatted::Pad& p : f.pads) {
    if (p.at < out.text.size()) {
      out.text[p.at] = p.as;
      out.hidden[p.at] = 1;
    }
  }
  // A fill repeats its character across what is left of the cell ("####" too).
  if (f.fill >= 0 && widthPx > 0) {
    const wchar_t ch = f.fillChar ? f.fillChar : L'#';
    const int one = std::max(1, measure_.Width(std::wstring_view(&ch, 1), font, base));
    const int used = f.overflow ? 0 : measure_.Width(out.text, font, base);
    // Narrower than one, the cell shows nothing.
    const int n = std::max(0, (avail - used) / one);
    const size_t at = std::min<size_t>(static_cast<size_t>(f.fill), out.text.size());
    out.text.insert(at, static_cast<size_t>(n), ch);
    out.hidden.insert(out.hidden.begin() + at, static_cast<size_t>(n), 0);
    out.fillAt = static_cast<uint32_t>(at);
    out.fillCount = static_cast<uint32_t>(n);
  }
}

const CellText::Layout& CellText::Get(const Sheet& sheet, const CellInfo& cell, int widthPx, const TextLook& look,
                                      int textPx) {
  if (textPx < 0) textPx = widthPx;
  if (cacheSheet_ != &sheet) {
    cache_.clear();
    cacheSheet_ = &sheet;
  }
  if (cache_.size() > 40000 && !held_) cache_.clear();  // not under a paint's layouts (a crash)
  const uint64_t key = (static_cast<uint64_t>(cell.row) << 14) | cell.col;
  auto& slot = cache_[key];
  if (!slot) slot = std::make_unique<Layout>();
  if (slot->widthPx != widthPx || slot->textPx != textPx || !(slot->look == look))
    Make(sheet, cell, widthPx, look, textPx, *slot);
  return *slot;
}

void CellText::Make(const Sheet& sheet, const CellInfo& cell, int widthPx, const TextLook& look, int textPx,
                    Layout& out) {
  out = Layout{};
  out.widthPx = widthPx;
  out.textPx = textPx;
  out.look = look;
  Show(sheet, cell, textPx, out.shown, look.numFmt);
  if (look.errors != PrintErrors::Displayed && cell.value.type == ValueType::Error) {
    // Printed as Excel's Page Setup says: blank, "--" or "#N/A" (centered, as errors are).
    out.shown.text = look.errors == PrintErrors::Dash ? L"--" : look.errors == PrintErrors::NA ? L"#N/A" : L"";
    out.shown.hidden.assign(out.shown.text.size(), 0);
    out.shown.overflow = false;
  }
  const Styles& st = book_.GetStyles();
  const Xf& xf = st.CellXf(cell.style);
  const Alignment& al = xf.alignment;
  const Font& base = st.DefaultFont();
  Font font = st.FontOf(xf);
  if (look.bold >= 0) font.bold = look.bold != 0;
  if (look.italic >= 0) font.italic = look.italic != 0;
  if (look.strike >= 0) font.strike = look.strike != 0;
  if (look.underline >= 0) font.underline = static_cast<Underline>(look.underline);
  // A fill counted again with the advances the text is drawn with (the screen's pixels are
  // narrower), and spaced out by what a whole character leaves: what comes before it at the cell's
  // left, what comes after at its right, as Excel shows "$" and the amount apart.
  float fillSpacing = 0;
  if (out.shown.fill && widthPx > 0 && out.shown.fillCount > 0) {
    Shown& s = out.shown;
    const text::Face* face = fonts_.FaceFor(FontMeasure::Family(font, base), font.bold, font.italic);
    const float size = static_cast<float>(FontMeasure::Size(font, base));
    const wchar_t ch = s.text[s.fillAt];
    const float one = face ? face->Advance(ch, size) : 0;
    if (one > 0) {
      float used = 0;
      for (size_t i = 0; i < s.text.size(); ++i)
        if (i < s.fillAt || i >= s.fillAt + s.fillCount) used += face->Advance(s.text[i], size);
      const float avail = std::max(0, widthPx - 5 - IndentPx(measure_, xf, font, base)) * 0.75f;
      uint32_t n = static_cast<uint32_t>(std::max(0.f, std::floor((avail - used) / one)));
      if (s.overflow) n = std::max(n, 1u);
      s.text.replace(s.fillAt, s.fillCount, n, ch);
      s.hidden.erase(s.hidden.begin() + s.fillAt, s.hidden.begin() + s.fillAt + s.fillCount);
      s.hidden.insert(s.hidden.begin() + s.fillAt, n, 0);
      s.fillCount = n;
      if (n > 0 && !s.overflow) fillSpacing = std::max(0.f, avail - used - n * one) / n;
    }
  }
  const Shown& shown = out.shown;

  out.align = al.horizontal;
  if (out.align == HAlign::General) {
    const bool rtlText = al.readingOrder == 2 || (al.readingOrder == 0 && StartsRightToLeft(shown.text));
    out.align = shown.number ? HAlign::Right : shown.center ? HAlign::Center : rtlText ? HAlign::Right : HAlign::Left;
  }
  out.stacked = al.rotation == 255;
  out.wrapped = !shown.number && !out.stacked &&
                (al.wrap || al.horizontal == HAlign::Justify || al.horizontal == HAlign::Distributed);
  const FontMeasure::Info& info = measure_.Of(font, base);
  out.ascent = info.ascent * 0.75f;
  out.descent = info.descent * 0.75f;
  out.pitch = info.rowHeight * 0.75f;
  out.size = static_cast<float>(FontMeasure::Size(font, base));
  if (shown.text.empty()) return;

  // The text and its runs: the rich text of a shared string, or the shown text in the cell's
  // font with its pads apart.
  std::wstring textW;
  std::vector<text::TextStyle> styles;
  std::vector<uint32_t> lengths;
  std::vector<Run> runs;
  auto runFor = [&](const Font& f, bool hidden) {
    Run r;
    r.color = st.Resolve(f.color, 0xFF000000);
    // A table's font color, under the number format's ([Red]); a conditional format's, over it.
    if (look.fontColor && !look.colorWins) r.color = look.fontColor;
    if (shown.color && !hidden) r.color = shown.color;
    if (look.fontColor && look.colorWins) r.color = look.fontColor;
    r.hidden = hidden;
    r.underline = f.underline != Underline::None;
    r.doubleUnderline = f.underline == Underline::Double || f.underline == Underline::DoubleAccounting;
    r.accounting = f.underline == Underline::SingleAccounting || f.underline == Underline::DoubleAccounting;
    r.strike = f.strike;
    if (r.underline || r.strike) {
      if (const text::Face* face = fonts_.FaceFor(FontMeasure::Family(f, base), f.bold, f.italic)) {
        const text::FontMetrics m = face->Metrics(static_cast<float>(FontMeasure::Size(f, base)));
        r.underlinePos = m.underlinePosition;
        r.underlineThick = std::max(m.underlineThickness, 0.5f);
        r.strikePos = m.strikethroughPosition;
        r.strikeThick = std::max(m.strikethroughThickness, 0.5f);
      }
    }
    return r;
  };
  const bool rich = cell.sharedString != kNone && cell.value.type == ValueType::String && !out.stacked &&
                    FormatOf(xf.numFmt).IsGeneral() && !book_.Strings().Runs(cell.sharedString).empty();
  if (rich) {
    const std::string_view utf8 = cell.value.text;
    const auto rr = book_.Strings().Runs(cell.sharedString);
    const std::vector<Font>& fonts = book_.Strings().Fonts();
    auto add = [&](size_t from, size_t to, uint32_t fontIndex) {
      if (to <= from) return;
      Font f = font;
      if (fontIndex != kNone && fontIndex < fonts.size()) {
        const Font& r = fonts[fontIndex];
        f = r;
        if (f.name.empty()) f.name = font.name;
        if (f.size <= 0) f.size = font.size;
        if (!f.color.IsSet()) f.color = font.color;
      }
      const std::wstring part = Utf8ToWide(utf8.substr(from, to - from));
      textW += part;
      lengths.push_back(static_cast<uint32_t>(part.size()));
      styles.push_back(StyleOf(f, base, FontMeasure::Size(f, base)));
      runs.push_back(runFor(f, false));
    };
    if (rr.front().start > 0) add(0, std::min<size_t>(rr.front().start, utf8.size()), kNone);
    for (size_t i = 0; i < rr.size(); ++i) {
      const size_t from = std::min<size_t>(rr[i].start, utf8.size());
      const size_t to = i + 1 < rr.size() ? std::min<size_t>(rr[i + 1].start, utf8.size()) : utf8.size();
      add(from, to, rr[i].font);
    }
  }
  if (textW.empty()) {
    styles.clear();
    lengths.clear();
    runs.clear();
    textW = shown.text;
    const text::TextStyle s = StyleOf(font, base, out.size);
    size_t i = 0;
    while (i < textW.size()) {
      const uint8_t hidden = shown.hidden[i];
      size_t j = i;
      while (j < textW.size() && shown.hidden[j] == hidden) ++j;
      styles.push_back(s);
      lengths.push_back(static_cast<uint32_t>(j - i));
      runs.push_back(runFor(font, hidden != 0));
      i = j;
    }
    if (fillSpacing > 0) {
      // The fill's characters in a run of their own, spaced out.
      std::vector<text::TextStyle> fs;
      std::vector<uint32_t> len;
      std::vector<Run> rn;
      const uint32_t f0 = shown.fillAt, f1 = shown.fillAt + shown.fillCount;
      uint32_t pos = 0;
      for (size_t k = 0; k < lengths.size(); ++k) {
        const uint32_t a = pos, b = pos + lengths[k];
        auto add = [&](uint32_t from, uint32_t to, bool spaced) {
          if (to <= from) return;
          fs.push_back(styles[k]);
          if (spaced) fs.back().letterSpacing = fillSpacing;
          len.push_back(to - from);
          rn.push_back(runs[k]);
        };
        add(a, std::min(b, f0), false);
        add(std::max(a, f0), std::min(b, f1), true);
        add(std::max(a, f1), b, false);
        pos = b;
      }
      styles = std::move(fs);
      lengths = std::move(len);
      runs = std::move(rn);
    }
  }
  // Line breaks show only in wrapped cells; a carriage return never does.
  for (wchar_t& c : textW) {
    if (c == L'\r' || (!out.wrapped && c == L'\n')) c = 0x200B;
  }
  if (out.stacked) {
    // One character a line, top to bottom.
    std::wstring s;
    std::vector<uint32_t> lens;
    for (size_t r = 0, at = 0; r < lengths.size(); at += lengths[r], ++r) {
      uint32_t n = 0;
      for (uint32_t k = 0; k < lengths[r]; ++k) {
        if (!s.empty()) {
          s += L'\n';
          ++n;
        }
        s += textW[at + k];
        ++n;
      }
      lens.push_back(n);
    }
    textW = std::move(s);
    lengths = std::move(lens);
  }

  out.text = textW;
  if (runs.size() == 1 && !out.wrapped && !out.stacked && LayOutSimple(textW, styles.front(), out)) {
    out.runs = std::move(runs);
    return;
  }

  text::Paragraph para;
  para.text = textW;
  for (size_t i = 0; i < lengths.size(); ++i) para.runs.push_back({lengths[i], &styles[i]});
  para.markStyle = &styles.front();
  float width = 1e6f;
  if (out.wrapped) {
    const int avail = std::max(1, widthPx - 5 - IndentPx(measure_, xf, font, base));
    width = avail * 0.75f;
    switch (out.align) {
      case HAlign::Center: para.format.alignment = text::Alignment::Center; break;
      case HAlign::Right: para.format.alignment = text::Alignment::End; break;
      case HAlign::Justify: para.format.alignment = text::Alignment::Justify; break;
      case HAlign::Distributed: para.format.alignment = text::Alignment::Distribute; break;
      default: break;
    }
  } else if (out.stacked) {
    para.format.alignment = text::Alignment::Center;
    width = 0;
    for (const text::TextStyle& s : styles) width = std::max(width, s.size * 2);
  }
  out.lines = text::Layout(fonts_, para, width);
  out.runs = std::move(runs);
  for (const text::Line& l : out.lines) out.width = std::max(out.width, l.contentRight - l.contentLeft);
}

bool CellText::LayOutSimple(std::wstring_view s, const text::TextStyle& style, Layout& out) {
  if (style.verticalAlign != text::VerticalAlign::Baseline || style.letterSpacing != 0 || style.scale != 1) return false;
  // Trailing spaces aren't content: leave them to the full layout.
  if (s.empty() || s.back() == L' ') return false;
  for (wchar_t c : s)
    if (c < 0x20 || (c > 0x7E && c < 0xA0) || c == 0xAD || c > 0xFF) return false;  // 0xAD: a soft hyphen
  Glyphs* g = nullptr;
  for (const auto& t : glyphs_)
    if (t->bold == style.bold && t->italic == style.italic && t->family == style.family) g = t.get();
  if (!g) {
    auto t = std::make_unique<Glyphs>();
    t->family = style.family;
    t->bold = style.bold;
    t->italic = style.italic;
    t->face = fonts_.FaceFor(style.family, style.bold, style.italic);
    if (t->face) {
      UINT32 cps[224];
      for (uint32_t i = 0; i < 224; ++i) cps[i] = 0x20 + i;
      DWRITE_GLYPH_METRICS gm[224]{};
      if (SUCCEEDED(t->face->Get()->GetGlyphIndices(cps, 224, t->id)) &&
          SUCCEEDED(t->face->Get()->GetDesignGlyphMetrics(t->id, 224, gm, FALSE))) {
        DWRITE_FONT_METRICS fm{};
        t->face->Get()->GetMetrics(&fm);
        for (uint32_t i = 0; i < 224; ++i)
          t->advance[i] = t->id[i] ? static_cast<float>(gm[i].advanceWidth) / fm.designUnitsPerEm : 0;
      } else {
        t->face = nullptr;
      }
    }
    g = t.get();
    glyphs_.push_back(std::move(t));
  }
  if (!g->face) return false;
  text::GlyphPiece piece;
  piece.glyphs.resize(s.size());
  piece.advances.resize(s.size());
  float x = 0;
  for (size_t i = 0; i < s.size(); ++i) {
    const uint32_t k = s[i] - 0x20u;
    if (!g->id[k]) return false;
    piece.glyphs[i] = g->id[k];
    piece.advances[i] = g->advance[k] * style.size;
    x += piece.advances[i];
  }
  piece.face = g->face->Get();
  piece.emSize = style.size;
  piece.width = x;
  piece.textEnd = static_cast<uint32_t>(s.size());
  const text::FontMetrics m = g->face->Metrics(style.size);
  text::Line line;
  line.end = piece.textEnd;
  line.ending = text::LineEnd::ParagraphEnd;
  line.right = 1e6f;
  line.contentRight = x;
  line.ascent = m.ascent;
  line.descent = m.descent;
  line.lineGap = m.lineGap;
  line.pieces.push_back(std::move(piece));
  out.lines.push_back(std::move(line));
  out.width = x;
  return true;
}

}  // namespace fw::sheet::view
