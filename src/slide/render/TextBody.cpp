#include "slide/render/TextBody.h"

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <deque>
#include <limits>

#include "core/Strings.h"
#include "doc/layout/Math.h"
#include "draw/Shapes.h"
#include "draw/Xml.h"
#include "text/Fonts.h"
#include "text/LineLayout.h"

namespace fw::slide::render {

using namespace draw::xml;

namespace {

constexpr size_t kMaxChars = 200000;

bool IsSymbolFont(std::wstring_view f) {
  std::wstring l(f);
  for (wchar_t& c : l) c = static_cast<wchar_t>(std::towlower(c));
  return l == L"wingdings" || l == L"wingdings 2" || l == L"wingdings 3" || l == L"webdings" || l == L"symbol" ||
         l == L"marlett" || l == L"monotype sorts" || l == L"zapfdingbats" || l == L"mt extra";
}

std::wstring Roman(int n, bool upper) {
  static constexpr std::pair<int, const wchar_t*> kTable[] = {{1000, L"m"}, {900, L"cm"}, {500, L"d"}, {400, L"cd"},
                                                              {100, L"c"},  {90, L"xc"},  {50, L"l"},  {40, L"xl"},
                                                              {10, L"x"},   {9, L"ix"},   {5, L"v"},   {4, L"iv"},
                                                              {1, L"i"}};
  std::wstring out;
  n = std::clamp(n, 1, 3999);
  for (const auto& [v, s] : kTable)
    while (n >= v) {
      out += s;
      n -= v;
    }
  if (upper)
    for (wchar_t& c : out) c = static_cast<wchar_t>(std::towupper(c));
  return out;
}

// ST_TextAutonumberScheme: the common ones; the rest count in arabic numerals with a period.
std::wstring AutoNumber(std::string_view scheme, int n) {
  std::wstring body;
  if (scheme.starts_with("roman")) {
    body = Roman(n, scheme.starts_with("romanUc"));
  } else if (scheme.starts_with("alpha")) {
    n = std::max(n, 1);
    body.assign(static_cast<size_t>((n - 1) / 26 + 1), static_cast<wchar_t>((scheme.starts_with("alphaUc") ? L'A' : L'a') + (n - 1) % 26));
  } else if (scheme.starts_with("circleNum") && n >= 1 && n <= 20) {
    return std::wstring(1, static_cast<wchar_t>(0x2460 + n - 1));
  } else {
    body = std::to_wstring(n);
  }
  if (scheme.ends_with("ParenBoth")) return L"(" + body + L")";
  if (scheme.ends_with("ParenR")) return body + L")";
  if (scheme.ends_with("Period")) return body + L".";
  if (scheme.ends_with("Minus")) return L"- " + body + L" -";
  return body;
}

struct RunDraw {
  std::optional<draw::Color> color;  // none: not drawn (noFill)
  std::optional<draw::Color> outline;  // the glyphs' outline (a:ln), `outlineWidth` points wide
  float outlineWidth = 0;
  std::optional<draw::Color> highlight, underlineColor;
  uint8_t underline = 0;  // 0 none, 1 single, 2 double, 3 heavy
  uint8_t strike = 0;     // 0 none, 1 single, 2 double
  float ulPos = 0, ulThick = 0, stPos = 0, stThick = 0;
  int link = -1;
  // a:reflection: the text mirrored (scaled by reflScale, 0: none) under its baseline, `reflDist` points
  // apart, its alpha from reflStart at the mirror line to reflEnd at reflEndPos of its height.
  float reflScale = 0, reflDist = 0, reflStart = 1, reflEnd = 0, reflEndPos = 1;
};

// Whether a hyperlink keeps its text's color (a14's hlinkClr val="tx").
bool LinkKeepsTextColor(pugi::xml_node run) {
  bool keep = false;
  ForEachChild(A(A(A(run, "rPr"), "hlinkClick"), "extLst"), [&](pugi::xml_node ext) {
    ForEachChild(ext, [&](pugi::xml_node c) {
      if (LocalNameOf(c) == "hlinkClr" && std::string_view(c.attribute("val").value()) == "tx") keep = true;
      return !keep;
    });
    return !keep;
  });
  return keep;
}

}  // namespace

struct TextBlock::Impl {
  struct Para {
    std::wstring text;
    std::deque<text::TextStyle> styles;
    text::TextStyle mark;
    std::vector<uint32_t> runStart;  // per run: its first offset in text
    std::vector<RunDraw> runs;
    std::vector<text::Line> lines;
    std::vector<draw::Point> origins;           // per line: where column coordinates' 0 is, and the baseline
    std::vector<std::pair<float, float>> rows;  // per line: top and bottom
    std::shared_ptr<const draw::Image> picture;  // a picture bullet, at its U+FFFC
    // Per run: Office math (a14:m) laid out, drawn at the run's U+FFFC.
    std::vector<std::shared_ptr<const doc::layout::math::Equation>> math;
    bool empty = true;
    bool bullet = false;  // the first run is the bullet and its tab
    uint32_t first = 0;   // where the paragraph's own text starts in `text` (after the bullet)
  };
  struct LinkRun {
    size_t para;
    uint32_t start, end;
    fw::slide::Hyperlink link;
  };
  std::vector<Para> paras;
  std::vector<LinkRun> links;
  draw::Matrix toBox;  // text coordinates → the box's
  float height = 0;
  draw::Rect rect;
};

TextBlock::TextBlock() : impl_(std::make_unique<Impl>()) {}
TextBlock::~TextBlock() = default;
TextBlock::TextBlock(TextBlock&&) noexcept = default;
TextBlock& TextBlock::operator=(TextBlock&&) noexcept = default;

namespace {

// Whether the face drawn for `family` has a legacy 'kern' table, the only kerning PowerPoint uses.
bool KernTable(text::Fonts& fonts, const std::wstring& family, bool bold, bool italic) {
  const text::Face* face = fonts.FaceFor(family, bold, italic);
  if (!face) return false;
  const void* data = nullptr;
  UINT32 size = 0;
  void* context = nullptr;
  BOOL exists = FALSE;
  if (FAILED(face->Get()->TryGetFontTable(DWRITE_MAKE_OPENTYPE_TAG('k', 'e', 'r', 'n'), &data, &size, &context, &exists)) || !exists)
    return false;
  face->Get()->ReleaseFontTable(context);
  return size > 4;
}

}  // namespace

void TextBlock::Build(text::Fonts& fonts, const draw::TextBox& box, const fw::slide::TextBody& body, const TextExtras& extras) {
  using fw::slide::Bullet;
  using fw::slide::RunProps;
  using fw::slide::TextAlign;
  Impl& m = *impl_;
  m = Impl{};
  m.rect = box.rect;
  const float fontScale = box.autofit == draw::TextBox::Autofit::ShrinkText ? box.fontScale : 1.f;
  const float lineReduction = box.autofit == draw::TextBox::Autofit::ShrinkText ? box.lineSpacingReduction : 0.f;
  const bool vertical = box.flow != draw::TextBox::Flow::Horizontal;
  const float boxW = std::max(0.f, vertical ? box.rect.Height() : box.rect.Width());
  const float boxH = std::max(0.f, vertical ? box.rect.Width() : box.rect.Height());
  const int columns = std::max(1, box.columns);
  const float colGap = columns > 1 ? box.columnSpacing : 0.f;
  const float colW = std::max(1.f, (boxW - colGap * (columns - 1)) / columns);
  const float layoutW = box.wrap ? colW : 100000.f;

  auto styleOf = [&](const RunProps& r) {
    text::TextStyle s;
    s.family = r.latin.empty() ? std::wstring(L"Calibri") : r.latin;
    s.eastAsiaFamily = r.eastAsian;
    s.complexFamily = r.complexScript;
    // PowerPoint rounds a shrunk size to whole points (32 pt at 55 % is drawn at 18).
    float size = fontScale < 1 ? std::max(1.f, std::round(r.size * fontScale)) : r.size;
    if (r.baseline != 0) {
      s.baselineShift = r.baseline * size;
      s.shiftKeepsLine = true;
      size *= 2.f / 3;
    }
    s.size = std::max(0.5f, size);
    // Advances as GDI's in PowerPoint's master units, 576 an inch, each a whole number of them (its
    // PDFs: physik's Arial, letters-home's Georgia, rlf-webinar-ii's Garamond wrap as they do there).
    s.gdiPixelsPerPoint = 576.f / 72;
    s.directWriteMetrics = true;  // a font's typo metrics when it asks (embedded Public Sans, govau decks)
    s.bold = s.complexBold = r.bold;
    s.italic = s.complexItalic = r.italic;
    s.caps = r.caps == fw::slide::Caps::All ? text::Caps::All : r.caps == fw::slide::Caps::Small ? text::Caps::Small : text::Caps::None;
    s.letterSpacing = r.spacing;
    // PowerPoint kerns only by a font's 'kern' table, not by its OpenType (GPOS) pairs: Public Sans
    // (GPOS only) isn't kerned in its PDFs; Franklin Gothic (no GPOS pairs) is, and Segoe UI by the
    // table's pairs with the space too (unisaarland). DirectWrite's shaping skips the table, so its
    // pairs are applied as they are (TextStyle::kernTable).
    if (r.kern > 0 && KernTable(fonts, s.family, s.bold, s.italic)) {
      s.kerningMinSize = r.kern;
      s.kernTable = true;
    }
    s.simulatedBoldKeepsAdvances = true;  // PowerPoint's (cbs deck: Calibri Light bold, 6 % narrower than DirectWrite's)
    if (!r.lang.empty()) s.language = Utf8ToWide(r.lang);
    return s;
  };
  auto runDraw = [&](const RunProps& r, const text::TextStyle& s) {
    RunDraw d;
    if (!r.noFill) d.color = r.color;
    if (r.outline && !A(r.outline, "noFill")) {
      d.outline = draw::ReadColor(A(r.outline, "solidFill"), extras.colors);
      d.outlineWidth = std::max(0.25f, EmuToPt(Number(r.outline, "w", 9525)) * (fontScale < 1 ? fontScale : 1.f));
    }
    d.highlight = r.highlight;
    d.underlineColor = r.underlineColor;
    if (pugi::xml_node refl = A(r.effects, "reflection")) {
      d.reflScale = static_cast<float>(Number(refl, "sy", 100000) / 100000);
      d.reflDist = static_cast<float>(EmuToPt(Number(refl, "dist", 0)));
      d.reflStart = std::clamp(static_cast<float>(Number(refl, "stA", 100000) / 100000), 0.f, 1.f);
      d.reflEnd = std::clamp(static_cast<float>(Number(refl, "endA", 0) / 100000), 0.f, 1.f);
      d.reflEndPos = std::clamp(static_cast<float>(Number(refl, "endPos", 100000) / 100000), 0.01f, 1.f);
      if (d.reflScale > 0) d.reflScale = -d.reflScale;  // a reflection is mirrored
    }
    using U = fw::slide::Underline;
    d.underline = r.underline == U::None ? 0 : r.underline == U::Double || r.underline == U::WavyDouble ? 2
                  : r.underline == U::Heavy || r.underline == U::DottedHeavy || r.underline == U::DashHeavy ||
                          r.underline == U::DashLongHeavy || r.underline == U::DotDashHeavy || r.underline == U::DotDotDashHeavy ||
                          r.underline == U::WavyHeavy
                      ? 3
                      : 1;
    d.strike = r.strike == fw::slide::Strike::Single ? 1 : r.strike == fw::slide::Strike::Double ? 2 : 0;
    if (d.underline || d.strike) {
      if (const text::Face* f = fonts.FaceFor(s.family, s.bold, s.italic)) {
        const text::FontMetrics fm = f->Metrics(s.size);
        d.ulPos = fm.underlinePosition;
        d.ulThick = std::max(fm.underlineThickness, 0.25f);
        d.stPos = fm.strikethroughPosition;
        d.stThick = std::max(fm.strikethroughThickness, 0.25f);
      } else {
        d.ulPos = s.size / 10;
        d.ulThick = d.stThick = s.size / 18;
        d.stPos = s.size / 3;
      }
      if (d.underline == 3) d.ulThick *= 2;
    }
    return d;
  };

  size_t chars = 0;
  float y = 0, maxBottom = 0;
  int column = 0;
  bool first = true;
  // bodyPr spcFirstLastPara: the first paragraph's space before counts too, as PowerPoint draws
  // it (15-213 virtual memory's Google Slides bodies, 4.8 pt lower), and the last's space after.
  const bool firstLastSpacing = draw::xml::Bool(box.properties, "spcFirstLastPara", false);
  float lastAfter = 0;
  for (const fw::slide::Paragraph& src : body.paragraphs) {
    const fw::slide::ParagraphProps& pp = src.props;
    Impl::Para& para = m.paras.emplace_back();
    para.empty = src.runs.empty();
    para.mark = styleOf(src.endProps);
    std::vector<text::TextRun> runs;
    auto add = [&](const std::wstring& t, const RunProps& props, const fw::slide::Hyperlink* link) {
      para.runStart.push_back(static_cast<uint32_t>(para.text.size()));
      para.text += t;
      text::TextStyle& st = para.styles.emplace_back(styleOf(props));
      if (!t.empty() && t.front() == L'￼') {
        st.objectWidth = st.size;
        st.objectAscent = st.size * 0.8f;
        st.objectDescent = st.size * 0.2f;
      }
      para.runs.push_back(runDraw(props, st));
      if (link) {
        para.runs.back().link = static_cast<int>(m.links.size());
        m.links.push_back({m.paras.size() - 1, para.runStart.back(), static_cast<uint32_t>(para.text.size()), *link});
      }
      runs.push_back({static_cast<uint32_t>(t.size()), &st});
    };
    // The bullet: a character, a number or a picture, then a tab to the text. Its font, size and
    // color follow the first run unless the paragraph says otherwise.
    const bool bulletShown = !para.empty || static_cast<int>(m.paras.size() - 1) == extras.bulletEmptyParagraph;
    if (bulletShown && pp.bullet.kind != Bullet::Kind::None) {
      RunProps bp = para.empty ? src.endProps : src.runs.front().props;
      const Bullet& b = pp.bullet;
      if (b.color) {
        bp.color = *b.color;
        bp.noFill = false;
      }
      bp.size = b.sizePoints > 0 ? b.sizePoints : bp.size * b.sizePercent;
      bp.underline = fw::slide::Underline::None;
      bp.strike = fw::slide::Strike::None;
      bp.baseline = bp.spacing = 0;
      bp.highlight.reset();
      std::wstring t;
      if (b.kind == Bullet::Kind::Char) {
        // A bullet character is set upright and regular, whatever the first run's bold or italic
        // (PowerPoint's PDFs: ArialMT bullets before Arial-BoldMT text, cybersprinters and globale;
        // Wingdings 2 before italic, cs-2110 graphs); numbers keep them (ilo's 1. in Arial-BoldMT).
        bp.bold = bp.italic = false;
        // A bullet font the PC doesn't have gives way to the text's, as for numbers: 15-213's
        // Noto Sans Symbols squares drawn in the text's Calibri (or, where it lacks the character,
        // the usual fallback), not in Segoe UI.
        if (!b.font.empty() && (IsSymbolFont(b.font) || fonts.Installed(b.font)))
          bp.latin = bp.eastAsian = bp.complexScript = b.font;
        // Only the first character ("••" draws one, PowerPoint's PDF of a SmartArt list).
        t = b.character.substr(0, b.character.size() > 1 && b.character[0] >= 0xD800 && b.character[0] < 0xDC00 ? 2 : 1);
        if (IsSymbolFont(bp.latin))
          for (wchar_t& c : t)
            if (c >= 0x20 && c < 0x100) c = static_cast<wchar_t>(0xF000 + c);
      } else if (b.kind == Bullet::Kind::AutoNumber) {
        // A number font the PC doesn't have gives way to the text's (PowerPoint's PDF: buFont +mj-lt,
        // a theme font not installed, numbers in the run's Calibri).
        // Nor does a theme font named as such (+mj-lt), installed or not: edu-session-1's numbers in
        // the text's Tahoma, not the theme's Calibri Light.
        if (!b.font.empty() && !b.themeFont && fonts.Installed(b.font)) bp.latin = b.font;
        t = AutoNumber(b.scheme, std::max(src.number, 1));
      } else if (b.kind == Bullet::Kind::Picture && extras.picture) {
        para.picture = extras.picture(b.picturePart, b.pictureRel);
        if (para.picture) t = L"￼";
      }
      if (!t.empty()) {
        // No hanging indent (the bullet at or past where the text starts): the text follows the
        // bullet straight on, no tab between (PowerPoint's PDFs: marL 0, indent 0 draws "•text").
        // Nor when the bullet is as wide as the hanging indent or wider: the text starts where the
        // bullet ends, not at the next default tab stop (PowerPoint's PDFs: overheid workshop's 28 pt
        // "•" in 13.5 points, datamillnorth's Wingdings arrow in 18).
        bool tab = pp.indent < 0;
        if (tab && b.kind != Bullet::Kind::Picture) {
          const text::TextStyle bs = styleOf(bp);
          if (const text::Face* face = fonts.FaceFor(bs.family, bs.bold, bs.italic)) {
            float width = 0;
            for (wchar_t c : t) width += c >= 0xD800 && c < 0xE000 ? 0 : face->Advance(c, bs.size);
            tab = width < -pp.indent - 0.01f;
          }
        }
        add(tab ? t + L'\t' : t, bp, nullptr);
        para.bullet = true;
        para.first = static_cast<uint32_t>(para.text.size());
      }
    }
    std::optional<text::Alignment> mathAlign;  // a display equation's m:jc
    // An equation alone in its paragraph (blank runs aside) is set in display style, as PowerPoint
    // does; one among text, inline.
    size_t mathRuns = 0, textRuns = 0;
    for (const fw::slide::TextRun& r : src.runs) {
      if (r.kind == fw::slide::TextRun::Kind::Math) ++mathRuns;
      else if (r.kind == fw::slide::TextRun::Kind::Break || r.text.find_first_not_of(L" 	 ") != std::wstring::npos) ++textRuns;
    }
    const bool mathAlone = mathRuns == 1 && textRuns == 0;
    for (const fw::slide::TextRun& r : src.runs) {
      if (r.kind == fw::slide::TextRun::Kind::Math) {
        // Office math: laid out by the equation typesetter (Cambria Math's MATH table), at the run's
        // size, in its color; an inline object in the line.
        if (chars >= kMaxChars) continue;
        std::string xml;
        struct Sink : pugi::xml_writer {
          std::string& out;
          explicit Sink(std::string& o) : out(o) {}
          void write(const void* data, size_t size) override { out.append(static_cast<const char*>(data), size); }
        } sink(xml);
        const bool mathPara = LocalNameOf(r.node) == "oMathPara", display = mathPara || mathAlone;
        // The typesetter takes a display equation's m:oMath elements, an inline one's content.
        if (display && !mathPara) r.node.print(sink, "", pugi::format_raw);
        else
          for (pugi::xml_node c = r.node.first_child(); c; c = c.next_sibling()) c.print(sink, "", pugi::format_raw);
        const float size = styleOf(r.props).size;
        std::shared_ptr<const doc::layout::math::Equation> laid = doc::layout::math::LayOut(fonts, xml, size, display);
        auto eq = std::make_shared<doc::layout::math::Equation>(*laid);
        if (!r.props.noFill) {
          const auto c255 = [](float v) { return static_cast<uint32_t>(std::clamp(v, 0.f, 1.f) * 255 + 0.5f); };
          const uint32_t rgb = (c255(r.props.color.r) << 16) | (c255(r.props.color.g) << 8) | c255(r.props.color.b);
          for (auto& g : eq->glyphs)
            if (g.color == 0) g.color = rgb;
          for (auto& rule : eq->rules)
            if (rule.color == 0) rule.color = rgb;
        }
        add(L"\uFFFC", r.props, nullptr);
        text::TextStyle& st = para.styles.back();
        st.objectWidth = eq->width;
        st.objectAscent = eq->ascent;
        st.objectDescent = eq->descent;
        para.math.resize(para.runs.size());
        para.math.back() = std::move(eq);
        if (mathPara) {
          using J = doc::layout::math::Equation::Justify;
          mathAlign = laid->justify == J::Left ? text::Alignment::Start : laid->justify == J::Right ? text::Alignment::End : text::Alignment::Center;
        }
        chars += 1;
        continue;
      }
      std::wstring t = r.kind == fw::slide::TextRun::Kind::Break ? std::wstring(L"\u000B") : r.text;
      // Line feeds in a run break lines as a:br does; other control characters show as spaces.
      for (wchar_t& c : t)
        if (c == L'\n' || c == L'\r') c = L'\u000B';
        else if (c < 0x20 && c != L'\t' && c != L'\u000B') c = L' ';
      if (t.empty() || chars >= kMaxChars) continue;
      chars += t.size();
      RunProps props = r.props;
      const fw::slide::Hyperlink* link = r.props.click ? &r.props.click : nullptr;
      if (link && !LinkKeepsTextColor(r.node))
        if (std::optional<draw::Color> c = draw::SchemeColor("hlink", extras.colors)) {
          props.color = *c;
          props.noFill = false;
        }
      // Underlined even when it keeps its text's color (govau workshop 3's references).
      if (link && props.underline == fw::slide::Underline::None)
        props.underline = fw::slide::Underline::Single;
      // Symbol characters (U+F020 to U+F0FF) in the run's a:sym font, the rest in its own
      // (letters-home's Wingdings ticks, U+F0FC, among "(SP)" in Calibri).
      const auto symbolic = [](wchar_t c) { return c >= 0xF020 && c <= 0xF0FF; };
      if (!props.symbol.empty() && std::any_of(t.begin(), t.end(), symbolic)) {
        RunProps sym = props;
        sym.latin = sym.eastAsian = sym.complexScript = props.symbol;
        for (size_t i = 0; i < t.size();) {
          const bool s = symbolic(t[i]);
          size_t j = i;
          while (j < t.size() && symbolic(t[j]) == s) ++j;
          add(t.substr(i, j - i), s ? sym : props, link);
          i = j;
        }
        continue;
      }
      add(t, props, link);
    }

    text::Paragraph tp;
    tp.text = para.text;
    tp.runs = runs;
    tp.markStyle = &para.mark;
    text::ParagraphFormat& f = tp.format;
    using text::Alignment;
    Alignment a = pp.align == TextAlign::Center                                      ? Alignment::Center
                  : pp.align == TextAlign::Right                                     ? Alignment::End
                  : pp.align == TextAlign::Justify || pp.align == TextAlign::JustifyLow ? Alignment::Justify
                  : pp.align == TextAlign::Distributed || pp.align == TextAlign::ThaiDistributed ? Alignment::Distribute
                                                                                     : Alignment::Start;
    // DrawingML's alignment is visual (left, right); fwtext's logical.
    if (pp.rtl && (a == Alignment::Start || a == Alignment::End)) a = a == Alignment::Start ? Alignment::End : Alignment::Start;
    if (mathAlign) a = *mathAlign;
    f.rightToLeft = pp.rtl;
    f.alignment = box.wrap ? a : (pp.rtl ? Alignment::End : Alignment::Start);
    // Spaces before a break or the paragraph's end don't count when centering (anyone-out-there's titles).
    f.alignTrailingSpaces = false;
    f.startIndent = pp.marginLeft;
    f.endIndent = pp.marginRight;
    // The first line never starts before the text area (marL 0 with a hanging indent: PowerPoint's PDFs).
    f.firstLineIndent = std::max(pp.indent, -pp.marginLeft);
    // Its text then starts at -indent, the first line as if shifted to start at the edge, while the
    // lines after it stay at marL (rlf-webinar-i 5: 36 and 33 points; business bodies 10's marL 0).
    if (pp.marginLeft + pp.indent < 0) f.hangingStop = -pp.indent;
    for (const fw::slide::TabStop& t : pp.tabs) {
      text::TabStop ts;
      ts.position = t.position;
      ts.align = t.kind == fw::slide::TabStop::Kind::Center    ? text::TabAlign::Center
                 : t.kind == fw::slide::TabStop::Kind::Right   ? text::TabAlign::End
                 : t.kind == fw::slide::TabStop::Kind::Decimal ? text::TabAlign::Decimal
                                                               : text::TabAlign::Start;
      f.tabs.push_back(ts);
    }
    f.defaultTabStop = std::max(1.f, pp.defaultTab);
    f.squeezeSpaces = false;
    f.hangPunctuation = pp.hangingPunctuation;
    para.lines = text::Layout(fonts, tp, layoutW);
    // A centred or right-aligned paragraph's bullet goes with its text, as far before the first
    // line's text as the hanging indent says, not at the line's start (usgov-materials 7: "•" 27
    // points before a centred "Magnifying glass").
    if (para.bullet && pp.indent < 0 && !pp.rtl && (f.alignment == Alignment::Center || f.alignment == Alignment::End) &&
        !para.lines.empty()) {
      text::Line& first = para.lines.front();
      float textX = std::numeric_limits<float>::infinity(), bulletX = textX;
      for (const text::GlyphPiece& g : first.pieces) {
        float& at = g.run == 0 ? bulletX : textX;
        at = std::min(at, g.x);
      }
      for (const text::InlineObject& o : first.objects) {
        float& at = o.run == 0 ? bulletX : textX;
        at = std::min(at, o.x);
      }
      if (std::isfinite(textX) && std::isfinite(bulletX)) {
        const float shift = textX + pp.indent - bulletX;
        for (text::GlyphPiece& g : first.pieces)
          if (g.run == 0) g.x += shift;
        for (text::InlineObject& o : first.objects)
          if (o.run == 0) o.x += shift;
      }
    }

    // Vertical placement. Percent line spacing scales each line's height (its fonts' ascent,
    // descent and gap), less the stored autofit's reduction; paragraph spacing in percent is of
    // the first line's largest font.
    // The bullet (run 0) doesn't count: a bullet at 150 % leaves its line as high (PowerPoint's PDFs).
    const auto counts = [&](uint32_t run) { return !para.bullet || run != 0; };
    // A line with nothing drawn on it (a line break alone) is as high as the break's own size; one
    // after the last break, the paragraph mark's.
    const auto emptySize = [&](const text::Line& line) {
      if (line.start < para.text.size())
        for (size_t i = para.runStart.size(); i-- > 0;)
          if (para.runStart[i] <= line.start) return counts(static_cast<uint32_t>(i)) ? para.styles[i].size : para.mark.size;
      return para.mark.size;
    };
    float largest = 0;
    if (!para.lines.empty())
      for (const text::GlyphPiece& g : para.lines.front().pieces)
        if (counts(g.run)) largest = std::max(largest, g.emSize);
    if (largest <= 0) largest = para.lines.empty() ? para.mark.size : emptySize(para.lines.front());
    // Spacing given in points is laid out in whole points (PowerPoint's PDFs: cs-2110's 5.5 pt before
    // as 6, 16.5 as 17 and usgov-materials' 13.68 as 14; font-scale's 6.41 as 6 and annual-deaths'
    // exact 26.2-pt lines as 26); in percent, as worked out (20 % of 33.6 stays 6.72, cmsc-201).
    const auto points = [](float v) { return std::round(v); };
    auto spacing = [&](const fw::slide::Spacing& s) { return s.percent ? s.value * largest * 1.2f : points(s.value); };
    if (!first || firstLastSpacing) y += spacing(pp.spaceBefore);
    const float lineFactor = pp.lineSpacing.percent ? std::max(0.f, pp.lineSpacing.value - lineReduction) : 0.f;
    for (const text::Line& line : para.lines) {
      // PowerPoint's single line is 1.2 times the line's largest font size, whatever the font
      // (its PDFs: 21.6 pt for Calibri 18, whose own ascent and descent make 21.97), with the
      // baseline at the font's ascent's share of it.
      float size = 0;
      for (const text::GlyphPiece& g : line.pieces)
        if (counts(g.run)) size = std::max(size, g.emSize);
      // An equation counts as its run's font size, and the line is at least as high as the equation
      // (PowerPoint's PDFs: cs-444's lines of subscripted math at 1.2 times their size).
      float mathExtent = 0;
      for (const text::InlineObject& o : line.objects)
        if (counts(o.run)) {
          if (o.run < para.math.size() && para.math[o.run]) {
            size = std::max(size, para.styles[o.run].size);
            mathExtent = std::max(mathExtent, o.ascent + o.descent);
          } else {
            size = std::max(size, o.ascent + o.descent);
          }
        }
      if (size <= 0) size = emptySize(line);
      const float natural = std::max(1.2f * size, mathExtent);
      // Exact spacing stays as stated when autofit shrinks the text (govuk annual-deaths 7: 28 pt).
      const float pitch = pp.lineSpacing.percent ? natural * lineFactor : points(pp.lineSpacing.value);
      const float share = line.ascent + line.descent > 0 ? line.ascent / (line.ascent + line.descent) : 0;
      // Spacing under single, stated or an autofit's reduction, comes off the line's top: the part
      // below the baseline stays as at single (cbs at 90 %: 28-pt text 0.72 pt and a 44-pt title
      // 1.16 pt higher than shared out; govuk census shrunk 20 %: 1.0 pt). Wider spacing puts the
      // baseline at the fonts' ascent share of the line, but no lower than three quarters of it
      // (Calibri's share; era-systems' Trebuchet at 110 % and govau's Public Sans at 150 % sit there,
      // stat-512's Courier New at its own .735).
      const float above = pitch < natural            ? std::max(0.f, pitch - natural * (1 - share))
                          : pitch > natural * 1.001f ? pitch * std::min(share, 0.75f)
                                                     : pitch * share;
      float shift = 0;
      if (!box.wrap) {
        // Lines that don't wrap are aligned in the box as wide as they are.
        const float left = pp.marginLeft, right = boxW - pp.marginRight;
        if (a == Alignment::Center) shift = (left + right) / 2 - (line.contentLeft + line.contentRight) / 2;
        else if (a == Alignment::End) shift = right - line.contentRight;
      }
      para.origins.push_back({shift, y + above});
      para.rows.emplace_back(y, y + pitch);
      // The block ends at its last line's own bottom, without wider spacing's share below it
      // (cpb's bottom-anchored title at 120 %: 2.5 pt lower than to the line's full pitch).
      maxBottom = std::max(maxBottom, std::min(y + pitch, y + above + natural * (1 - share)));
      y += pitch;
    }
    lastAfter = spacing(pp.spaceAfter);
    y += lastAfter;
    first = false;
  }
  // The block's height: down to the last line; the last paragraph's space after counts only under
  // spcFirstLastPara.
  m.height = maxBottom;
  if (firstLastSpacing) m.height += lastAfter;
  // Columns: lines go down a column as far as the box, then on to the next; text too long for
  // them all is shared out evenly, as PowerPoint does.
  if (columns > 1 && maxBottom > 0) {
    const float room = maxBottom > boxH * columns ? maxBottom / columns : boxH;
    float start = 0, height = 0;
    for (Impl::Para& p : m.paras)
      for (size_t i = 0; i < p.rows.size(); ++i) {
        // A line goes on to the next column once both its line and its text (its top and its fonts'
        // own ascent and descent) would reach below this one's bottom: ilo 24's 20-point Arial line
        // ends 1.5 points below and stays, its text ending above; text-autofit 3's Calibri ends just
        // above, its text 0.36 below, and stays; cbs 5's line at 80 % goes. A paragraph's last line
        // that starts above the bottom stays with the lines before it, however far it reaches below
        // (era-systems 41), and so does an empty paragraph, which would start the next column with a
        // blank line (ilo 18).
        const bool lastOfSeveral = i > 0 && i + 1 == p.rows.size();
        const float reach = std::min(p.rows[i].second, p.rows[i].first + p.lines[i].ascent + p.lines[i].descent);
        const bool below = lastOfSeveral || p.empty ? p.rows[i].first - start >= room - 0.01f : reach - start > room + 0.01f;
        if (column + 1 < columns && below && p.rows[i].first > start) {
          ++column;
          start = p.rows[i].first;
        }
        p.origins[i].x += column * (colW + colGap);
        p.origins[i].y -= start;
        p.rows[i].first -= start;
        p.rows[i].second -= start;
        height = std::max(height, p.rows[i].second);
      }
    m.height = height;
  }

  // Anchoring, then text coordinates (x along the lines from the text area's top left) to the box's.
  float dy = 0;
  if (box.anchor == draw::TextBox::Anchor::Bottom) dy = boxH - m.height;
  else if (box.anchor != draw::TextBox::Anchor::Top) dy = (boxH - m.height) / 2;
  float dx = 0;
  if (box.anchorCenter) {
    float lo = 1e9f, hi = -1e9f;
    for (const Impl::Para& p : m.paras)
      for (size_t i = 0; i < p.lines.size(); ++i) {
        lo = std::min(lo, p.origins[i].x + p.lines[i].contentLeft);
        hi = std::max(hi, p.origins[i].x + p.lines[i].contentRight);
      }
    if (lo <= hi) dx = (boxW - (hi - lo)) / 2 - lo;
  }
  const draw::Rect& r = box.rect;
  draw::Matrix t = draw::Matrix::Translate(dx, dy);
  if (box.flow == draw::TextBox::Flow::Vertical270) t = t * draw::Matrix{0, -1, 1, 0, r.left, r.bottom};
  else if (vertical) t = t * draw::Matrix{0, 1, -1, 0, r.right, r.top};
  else t = t * draw::Matrix::Translate(r.left, r.top);
  if (box.rotation != 0) {
    const float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    t = t * draw::Matrix::Translate(-cx, -cy) * draw::Matrix::Rotate(box.rotation) * draw::Matrix::Translate(cx, cy);
  }
  m.toBox = t;
}

void TextBlock::Draw(draw::Painter& p, DrawFilter* filter, uint32_t shape) const {
  const Impl& m = *impl_;
  if (m.paras.empty()) return;
  p.Save();
  // PowerPoint doesn't mirror text with its shape: a horizontal flip leaves it as it is, a
  // vertical one turns it upside down.
  const draw::Matrix ct = p.CurrentTransform();
  if (ct.m11 * ct.m22 - ct.m12 * ct.m21 < 0) {
    const float cx = (m.rect.left + m.rect.right) / 2;
    p.Transform({-1, 0, 0, 1, 2 * cx, 0});
  }
  p.Transform(m.toBox);
  const bool wantText = p.WantsText();
  for (size_t pi = 0; pi < m.paras.size(); ++pi) {
    const Impl::Para& para = m.paras[pi];
    if (filter && !filter->Paragraph(shape, static_cast<int>(pi))) continue;
    for (size_t li = 0; li < para.lines.size(); ++li) {
      const text::Line& line = para.lines[li];
      const draw::Point o = para.origins[li];
      for (size_t ri = 0; ri < para.runs.size(); ++ri) {
        if (!para.runs[ri].highlight) continue;
        const uint32_t s = para.runStart[ri];
        const uint32_t e = ri + 1 < para.runStart.size() ? para.runStart[ri + 1] : static_cast<uint32_t>(para.text.size());
        for (const auto& [x0, x1] : line.Ranges(s, e))
          p.FillRect({o.x + x0, para.rows[li].first, o.x + x1, para.rows[li].second}, draw::Brush::Solid(*para.runs[ri].highlight));
      }
      for (const text::GlyphPiece& g : line.pieces) {
        if (g.glyphs.empty() || g.run >= para.runs.size()) continue;
        const RunDraw& rd = para.runs[g.run];
        if (!rd.color && !rd.outline) continue;
        const draw::Brush brush = draw::Brush::Solid(rd.color ? *rd.color : *rd.outline);
        draw::GlyphRun run = g.Run();
        if (wantText && g.textEnd > g.textStart && g.textEnd <= para.text.size()) {
          run.text = reinterpret_cast<const char16_t*>(para.text.data() + g.textStart);
          run.textLength = g.textEnd - g.textStart;
        }
        const float baseline = o.y - g.rise;
        if (rd.color) p.DrawGlyphs({o.x + g.OriginX(), baseline}, run, brush);
        if (rd.outline) {
          // Outlined text: the glyphs' outlines stroked, over their fill.
          draw::Pen pen;
          pen.brush = draw::Brush::Solid(*rd.outline);
          pen.width = rd.outlineWidth;
          pen.join = draw::LineJoin::Round;
          p.Save();
          p.Transform(draw::Matrix::Translate(o.x + g.OriginX(), baseline));
          p.StrokePath(draw::GlyphRunOutline(run), pen);
          p.Restore();
        }
        if (rd.underline && !g.leader) {
          const draw::Brush ub = rd.underlineColor ? draw::Brush::Solid(*rd.underlineColor) : brush;
          const float top = o.y + rd.ulPos - rd.ulThick / 2;
          p.FillRect({o.x + g.x, top, o.x + g.x + g.width, top + rd.ulThick}, ub);
          if (rd.underline == 2) p.FillRect({o.x + g.x, top + 2 * rd.ulThick, o.x + g.x + g.width, top + 3 * rd.ulThick}, ub);
        }
        if (rd.strike) {
          const float top = baseline - rd.stPos - rd.stThick / 2;
          p.FillRect({o.x + g.x, top, o.x + g.x + g.width, top + rd.stThick}, brush);
          if (rd.strike == 2) p.FillRect({o.x + g.x, top - 2 * rd.stThick, o.x + g.x + g.width, top - rd.stThick}, brush);
        }
      }
      if (!para.math.empty())
        for (const text::InlineObject& obj : line.objects)
          if (obj.run < para.math.size() && para.math[obj.run])
            doc::layout::math::Paint(p, *para.math[obj.run], o.x + obj.x, o.y - obj.rise);
      if (para.picture && para.bullet)
        for (const text::InlineObject& obj : line.objects) {
          if (obj.run != 0) continue;
          if (std::shared_ptr<const draw::Bitmap> px = para.picture->Pixels(64, 64))
            p.DrawBitmap(*px, {o.x + obj.x, o.y - obj.ascent, o.x + obj.x + obj.width, o.y + obj.descent});
        }
    }
  }
  // Reflected text (a:reflection, rlf-webinar-iv's titles): each line's glyphs mirrored about its own
  // baseline (morocco 2's two-line title), fading out over endPos of the line's height (PowerPoint's
  // PDF bakes it as a picture, its reflection starting at the letters' feet; the blur is left out).
  struct Reflected {
    draw::Path outline;  // at `origin` in the text box's coordinates
    draw::Point origin;
    const RunDraw* rd;
    size_t line;  // in `inkTops` and `baselines`
  };
  std::vector<Reflected> reflected;
  std::vector<float> inkTops, baselines;
  for (size_t pi = 0; pi < m.paras.size(); ++pi) {
    const Impl::Para& para = m.paras[pi];
    if (filter && !filter->Paragraph(shape, static_cast<int>(pi))) continue;
    for (size_t li = 0; li < para.lines.size(); ++li) {
      bool started = false;  // this line's group begun
      for (const text::GlyphPiece& g : para.lines[li].pieces) {
        if (g.glyphs.empty() || g.run >= para.runs.size() || !para.runs[g.run].color || para.runs[g.run].reflScale == 0) continue;
        draw::Path outline = draw::GlyphRunOutline(g.Run());
        if (outline.Empty()) continue;
        const draw::Point at{para.origins[li].x + g.OriginX(), para.origins[li].y - g.rise};
        if (!started) {
          inkTops.push_back(1e9f);
          baselines.push_back(para.origins[li].y);
        }
        started = true;
        inkTops.back() = std::min(inkTops.back(), at.y + outline.Bounds().top);
        reflected.push_back({std::move(outline), at, &para.runs[g.run], inkTops.size() - 1});
      }
    }
  }
  for (const Reflected& r : reflected) {
    const RunDraw& rd = *r.rd;
    draw::Color from = *rd.color, to = *rd.color;
    from.a *= rd.reflStart;
    to.a *= rd.reflEnd;
    const float baseline = baselines[r.line], inkTop = inkTops[r.line];
    const float fadeTop = baseline - rd.reflEndPos * (baseline - inkTop);
    p.Save();
    p.Transform({1, 0, 0, rd.reflScale, 0, baseline + rd.reflDist - rd.reflScale * baseline});
    p.Transform(draw::Matrix::Translate(r.origin.x, r.origin.y));
    p.FillPath(r.outline, draw::Brush::Linear({0, baseline - r.origin.y}, {0, fadeTop - r.origin.y}, {{0, from}, {1, to}}));
    p.Restore();
  }
  p.Restore();
}

float TextBlock::ContentHeight() const { return impl_->height; }

namespace {
draw::Rect Transformed(const draw::Matrix& m, const draw::Rect& r) {
  const draw::Point pts[4] = {m.Apply({r.left, r.top}), m.Apply({r.right, r.top}), m.Apply({r.left, r.bottom}),
                              m.Apply({r.right, r.bottom})};
  draw::Rect out{pts[0].x, pts[0].y, pts[0].x, pts[0].y};
  for (const draw::Point& q : pts)
    out = {std::min(out.left, q.x), std::min(out.top, q.y), std::max(out.right, q.x), std::max(out.bottom, q.y)};
  return out;
}
}  // namespace

std::vector<draw::Rect> TextBlock::ParagraphBounds() const {
  std::vector<draw::Rect> out;
  for (const Impl::Para& p : impl_->paras) {
    draw::Rect r{};
    for (size_t i = 0; i < p.lines.size(); ++i) {
      const draw::Rect line{p.origins[i].x + p.lines[i].contentLeft, p.rows[i].first, p.origins[i].x + p.lines[i].contentRight,
                            p.rows[i].second};
      r = i == 0 ? line : draw::Rect{std::min(r.left, line.left), std::min(r.top, line.top), std::max(r.right, line.right),
                                     std::max(r.bottom, line.bottom)};
    }
    out.push_back(p.lines.empty() ? draw::Rect{} : Transformed(impl_->toBox, r));
  }
  return out;
}

std::vector<TextBlock::Link> TextBlock::Links() const {
  std::vector<Link> out;
  for (const Impl::LinkRun& l : impl_->links) {
    const Impl::Para& p = impl_->paras[l.para];
    for (size_t i = 0; i < p.lines.size(); ++i)
      for (const auto& [x0, x1] : p.lines[i].Ranges(l.start, l.end))
        out.push_back({Transformed(impl_->toBox, {p.origins[i].x + x0, p.rows[i].first, p.origins[i].x + x1, p.rows[i].second}), l.link});
  }
  return out;
}

// ---------------------------------------------------------------------------
// Editing (P2): carets, hit testing and selection over the laid-out lines.

draw::Matrix TextBlock::ToSlide(const draw::Matrix& boxToSlide) const {
  const Impl& m = *impl_;
  draw::Matrix t = m.toBox;
  if (boxToSlide.m11 * boxToSlide.m22 - boxToSlide.m12 * boxToSlide.m21 < 0) {
    const float cx = (m.rect.left + m.rect.right) / 2;
    t = t * draw::Matrix{-1, 0, 0, 1, 2 * cx, 0};
  }
  return t * boxToSlide;
}

size_t TextBlock::ParagraphCount() const { return impl_->paras.size(); }

uint32_t TextBlock::ParagraphLength(size_t para) const {
  if (para >= impl_->paras.size()) return 0;
  const Impl::Para& p = impl_->paras[para];
  return static_cast<uint32_t>(p.text.size()) - p.first;
}

namespace {

// The line of `lines` holding text offset `t`: at a boundary the next line, unless `lineEnd` and the
// line above wrapped there.
size_t LineOf(const std::vector<text::Line>& lines, uint32_t t, bool lineEnd) {
  if (lines.empty()) return 0;
  for (size_t i = 0; i < lines.size(); ++i) {
    const text::Line& l = lines[i];
    if (t < l.end) return i;
    if (t == l.end) {
      if (i + 1 == lines.size()) return i;
      if (lineEnd && l.ending == text::LineEnd::Wrap) return i;
    }
  }
  return lines.size() - 1;
}

}  // namespace

TextBlock::Caret TextBlock::CaretAt(size_t para, uint32_t offset, bool lineEnd) const {
  const Impl& m = *impl_;
  Caret c;
  if (m.paras.empty()) return c;
  para = std::min(para, m.paras.size() - 1);
  const Impl::Para& p = m.paras[para];
  const uint32_t t = std::min<uint32_t>(p.first + offset, static_cast<uint32_t>(p.text.size()));
  if (p.lines.empty()) return c;
  const size_t li = LineOf(p.lines, t, lineEnd);
  const text::Line& line = p.lines[li];
  c.x = p.origins[li].x + line.CaretX(std::clamp(t, line.start, line.end));
  c.top = p.rows[li].first;
  c.bottom = p.rows[li].second;
  return c;
}

namespace {

// The offset nearest x on a line, kept off the bullet and before a line break's character.
uint32_t OffsetOnLine(const text::Line& line, float x, uint32_t first, const std::wstring& text, bool& lineEnd) {
  uint32_t t = std::clamp(line.HitTest(x), line.start, line.end);
  lineEnd = false;
  if (t == line.end && line.end > line.start) {
    if (line.ending == text::LineEnd::LineBreak && line.end - 1 < text.size() && text[line.end - 1] == L'\u000B') t = line.end - 1;
    else if (line.ending == text::LineEnd::Wrap) lineEnd = true;
  }
  return std::max(t, first);
}

}  // namespace

TextBlock::Hit TextBlock::HitTest(draw::Point pt) const {
  const Impl& m = *impl_;
  Hit h;
  if (m.paras.empty()) return h;
  // The line whose row holds y; above the first, the first; below the last, the last; between
  // paragraphs (their spacing), the nearer.
  size_t bestPara = 0, bestLine = 0;
  float best = 1e30f;
  for (size_t pi = 0; pi < m.paras.size(); ++pi) {
    const Impl::Para& p = m.paras[pi];
    for (size_t li = 0; li < p.rows.size(); ++li) {
      // Columns: only lines of the column under x count, unless none does.
      const float left = p.origins[li].x + std::min(p.lines[li].left, p.lines[li].contentLeft);
      const float right = p.origins[li].x + std::max(p.lines[li].right, p.lines[li].contentRight);
      const float dx = pt.x < left ? left - pt.x : pt.x > right ? pt.x - right : 0;
      const float dy = pt.y < p.rows[li].first ? p.rows[li].first - pt.y : pt.y > p.rows[li].second ? pt.y - p.rows[li].second : 0;
      const float d = dy + dx * 4;  // across columns, x decides
      if (d < best) {
        best = d;
        bestPara = pi;
        bestLine = li;
      }
    }
  }
  const Impl::Para& p = m.paras[bestPara];
  h.para = bestPara;
  if (p.lines.empty()) return h;
  const uint32_t t = OffsetOnLine(p.lines[bestLine], pt.x - p.origins[bestLine].x, p.first, p.text, h.lineEnd);
  h.offset = t - p.first;
  return h;
}

std::pair<uint32_t, uint32_t> TextBlock::LineRange(size_t para, uint32_t offset, bool lineEnd) const {
  const Impl& m = *impl_;
  if (para >= m.paras.size()) return {0, 0};
  const Impl::Para& p = m.paras[para];
  if (p.lines.empty()) return {0, 0};
  const uint32_t t = std::min<uint32_t>(p.first + offset, static_cast<uint32_t>(p.text.size()));
  const text::Line& line = p.lines[LineOf(p.lines, t, lineEnd)];
  uint32_t end = line.end;
  if (line.ending == text::LineEnd::LineBreak && end > line.start && end - 1 < p.text.size() && p.text[end - 1] == L'\u000B') --end;
  const uint32_t start = std::max(line.start, p.first);
  return {start - p.first, std::max(end, start) - p.first};
}

bool TextBlock::Vertical(size_t para, uint32_t offset, bool lineEnd, int direction, float x, Hit& out) const {
  const Impl& m = *impl_;
  if (para >= m.paras.size() || m.paras[para].lines.empty()) return false;
  const Impl::Para& p = m.paras[para];
  const uint32_t t = std::min<uint32_t>(p.first + offset, static_cast<uint32_t>(p.text.size()));
  size_t pi = para, li = LineOf(p.lines, t, lineEnd);
  if (direction < 0) {
    if (li > 0) {
      --li;
    } else {
      do {
        if (pi == 0) return false;
        --pi;
      } while (m.paras[pi].lines.empty());
      li = m.paras[pi].lines.size() - 1;
    }
  } else {
    if (li + 1 < p.lines.size()) {
      ++li;
    } else {
      do {
        if (pi + 1 >= m.paras.size()) return false;
        ++pi;
      } while (m.paras[pi].lines.empty());
      li = 0;
    }
  }
  const Impl::Para& q = m.paras[pi];
  out.para = pi;
  out.offset = OffsetOnLine(q.lines[li], x - q.origins[li].x, q.first, q.text, out.lineEnd) - q.first;
  return true;
}

bool TextBlock::IsStop(size_t para, uint32_t offset) const {
  const Impl& m = *impl_;
  if (para >= m.paras.size()) return true;
  const Impl::Para& p = m.paras[para];
  const uint32_t t = p.first + offset;
  if (t >= p.text.size()) return true;
  for (const text::Line& l : p.lines)
    if (t >= l.start && t < l.end) return l.IsCaretStop(t);
  return true;
}

std::vector<draw::Rect> TextBlock::SelectionRects(size_t fromPara, uint32_t fromOffset, size_t toPara, uint32_t toOffset) const {
  const Impl& m = *impl_;
  std::vector<draw::Rect> out;
  for (size_t pi = fromPara; pi <= toPara && pi < m.paras.size(); ++pi) {
    const Impl::Para& p = m.paras[pi];
    const uint32_t len = static_cast<uint32_t>(p.text.size());
    const uint32_t s = pi == fromPara ? std::min(len, p.first + fromOffset) : p.first;
    const uint32_t e = pi == toPara ? std::min(len, p.first + toOffset) : len;
    for (size_t li = 0; li < p.lines.size(); ++li) {
      const text::Line& l = p.lines[li];
      const uint32_t a = std::max(s, l.start), b = std::min(e, l.end);
      const float top = p.rows[li].first, bottom = p.rows[li].second, ox = p.origins[li].x;
      if (a < b)
        for (const auto& [x0, x1] : l.Ranges(a, b)) out.push_back({ox + x0, top, ox + x1, bottom});
      // The paragraph's mark, when the selection goes on past it: a space's width.
      if (pi < toPara && li + 1 == p.lines.size()) {
        const float x = ox + l.CaretX(l.end);
        const float w = std::max(2.f, 0.25f * p.mark.size);
        out.push_back({x, top, x + w, bottom});
      }
    }
  }
  return out;
}

std::wstring TextBlock::PlainText() const {
  std::wstring out;
  for (const Impl::Para& p : impl_->paras) {
    if (!out.empty()) out += L'\n';
    const size_t from = p.bullet && p.runStart.size() > 1 ? p.runStart[1] : 0;
    for (size_t i = from; i < p.text.size(); ++i) out += p.text[i] == L'\u000B' ? L'\n' : p.text[i];
  }
  return out;
}

}  // namespace fw::slide::render
