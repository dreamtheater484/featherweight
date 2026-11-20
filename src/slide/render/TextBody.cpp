#include "slide/render/TextBody.h"

#include <algorithm>
#include <cmath>
#include <cwctype>

#include "core/Strings.h"
#include "draw/Xml.h"
#include "text/Fonts.h"

namespace fw::slide::render {

using namespace draw::xml;

namespace {

constexpr size_t kMaxParagraphs = 5000;
constexpr size_t kMaxChars = 200000;
constexpr int kLevels = 9;

float Pct(pugi::xml_node n, const char* attr, double def) { return static_cast<float>(Number(n, attr, def) / 100000.0); }

// spcPct (thousandths of a percent of a line) or spcPts (hundredths of a point).
struct Spacing {
  bool percent = true;
  float value = 0;
};

Spacing ReadSpacing(pugi::xml_node n, Spacing def) {
  if (pugi::xml_node pct = A(n, "spcPct")) return {true, std::clamp(Pct(pct, "val", 100000), 0.f, 132.f)};
  if (pugi::xml_node pts = A(n, "spcPts")) return {false, std::clamp(static_cast<float>(Number(pts, "val", 0) / 100), 0.f, 1584.f)};
  return def;
}

struct RunProps {
  float size = 18;
  bool bold = false, italic = false;
  std::string underline = "none";
  std::string strike = "noStrike";
  text::Caps caps = text::Caps::None;
  float spacing = 0;   // points added after each character
  float baseline = 0;  // fraction of the size, up
  float kern = 0;      // smallest size kerned; 0: none
  std::string latin, ea, cs;
  std::optional<draw::Color> color;
  bool noFill = false;
  std::optional<draw::Color> highlight;
  std::string lang;
  pugi::xml_node link;  // a:hlinkClick
};

void ApplyRun(RunProps& r, pugi::xml_node rPr, const draw::ColorContext& colors) {
  if (!rPr) return;
  if (pugi::xml_attribute a = rPr.attribute("sz")) r.size = std::clamp(static_cast<float>(a.as_double(1800) / 100), 0.5f, 4000.f);
  if (rPr.attribute("b")) r.bold = Bool(rPr, "b", false);
  if (rPr.attribute("i")) r.italic = Bool(rPr, "i", false);
  if (pugi::xml_attribute a = rPr.attribute("u")) r.underline = a.value();
  if (pugi::xml_attribute a = rPr.attribute("strike")) r.strike = a.value();
  if (pugi::xml_attribute a = rPr.attribute("cap")) {
    std::string_view v = a.value();
    r.caps = v == "all" ? text::Caps::All : v == "small" ? text::Caps::Small : text::Caps::None;
  }
  if (pugi::xml_attribute a = rPr.attribute("spc")) r.spacing = std::clamp(static_cast<float>(a.as_double(0) / 100), -400.f, 4000.f);
  if (rPr.attribute("baseline")) r.baseline = std::clamp(Pct(rPr, "baseline", 0), -10.f, 10.f);
  if (pugi::xml_attribute a = rPr.attribute("kern")) r.kern = std::clamp(static_cast<float>(a.as_double(0) / 100), 0.f, 4000.f);
  if (pugi::xml_attribute a = rPr.attribute("lang")) r.lang = a.value();
  ForEachChild(rPr, [&](pugi::xml_node c) {
    if (NamespaceOf(c) != kA) return true;
    std::string_view k = LocalNameOf(c);
    if (k == "solidFill") {
      if (auto col = draw::ReadColor(c, colors)) {
        r.color = col;
        r.noFill = false;
      }
    } else if (k == "gradFill") {
      // Text in a gradient: its first stop's color (a gradient per glyph run isn't drawn yet).
      if (pugi::xml_node gs = A(A(c, "gsLst"), "gs"))
        if (auto col = draw::ReadColor(gs, colors)) {
          r.color = col;
          r.noFill = false;
        }
    } else if (k == "pattFill") {
      if (auto col = draw::ReadColor(A(c, "fgClr"), colors)) {
        r.color = col;
        r.noFill = false;
      }
    } else if (k == "noFill") {
      r.noFill = true;
    } else if (k == "highlight") {
      r.highlight = draw::ReadColor(c, colors);
    } else if (k == "latin") {
      r.latin = c.attribute("typeface").value();
    } else if (k == "ea") {
      r.ea = c.attribute("typeface").value();
    } else if (k == "cs") {
      r.cs = c.attribute("typeface").value();
    } else if (k == "hlinkClick") {
      r.link = c;
    }
    return true;
  });
}

struct ParaProps {
  float marL = 0, marR = 0, indent = 0;
  std::string algn = "l";
  float defTab = 72;
  bool rtl = false;
  Spacing line{true, 1}, before{true, 0}, after{true, 0};
  enum class Bullet : uint8_t { None, Char, Number, Picture };
  Bullet bullet = Bullet::None;
  std::wstring bulletChar;
  std::string scheme = "arabicPeriod";
  int startAt = 1;
  std::string bulletRel;
  bool bulletFontText = true;
  std::string bulletFont;
  int bulletSizeMode = 0;  // 0: the text's, 1: percent of it, 2: points
  float bulletSize = 1;
  bool bulletColorText = true;
  draw::Color bulletColor;
  std::vector<text::TabStop> tabs;
  RunProps run;
};

void ApplyPara(ParaProps& p, pugi::xml_node pPr, const draw::ColorContext& colors) {
  if (!pPr) return;
  if (pPr.attribute("marL")) p.marL = std::clamp(EmuToPt(Number(pPr, "marL", 0)), 0.f, 4000.f);
  if (pPr.attribute("marR")) p.marR = std::clamp(EmuToPt(Number(pPr, "marR", 0)), 0.f, 4000.f);
  if (pPr.attribute("indent")) p.indent = std::clamp(EmuToPt(Number(pPr, "indent", 0)), -4000.f, 4000.f);
  if (pugi::xml_attribute a = pPr.attribute("algn")) p.algn = a.value();
  if (pPr.attribute("defTabSz")) p.defTab = std::clamp(EmuToPt(Number(pPr, "defTabSz", 914400)), 1.f, 4000.f);
  if (pPr.attribute("rtl")) p.rtl = Bool(pPr, "rtl", false);
  ForEachChild(pPr, [&](pugi::xml_node c) {
    if (NamespaceOf(c) != kA) return true;
    std::string_view k = LocalNameOf(c);
    if (k == "lnSpc") p.line = ReadSpacing(c, p.line);
    else if (k == "spcBef") p.before = ReadSpacing(c, p.before);
    else if (k == "spcAft") p.after = ReadSpacing(c, p.after);
    else if (k == "buClrTx") p.bulletColorText = true;
    else if (k == "buClr") {
      if (auto col = draw::ReadColor(c, colors)) {
        p.bulletColorText = false;
        p.bulletColor = *col;
      }
    } else if (k == "buSzTx") p.bulletSizeMode = 0;
    else if (k == "buSzPct") {
      p.bulletSizeMode = 1;
      p.bulletSize = std::clamp(Pct(c, "val", 100000), 0.25f, 4.f);
    } else if (k == "buSzPts") {
      p.bulletSizeMode = 2;
      p.bulletSize = std::clamp(static_cast<float>(Number(c, "val", 1800) / 100), 1.f, 4000.f);
    } else if (k == "buFontTx") p.bulletFontText = true;
    else if (k == "buFont") {
      p.bulletFontText = false;
      p.bulletFont = c.attribute("typeface").value();
    } else if (k == "buNone") p.bullet = ParaProps::Bullet::None;
    else if (k == "buChar") {
      p.bullet = ParaProps::Bullet::Char;
      p.bulletChar = Utf8ToWide(c.attribute("char").value()).substr(0, 4);
    } else if (k == "buAutoNum") {
      p.bullet = ParaProps::Bullet::Number;
      p.scheme = c.attribute("type").value();
      p.startAt = std::clamp(static_cast<int>(Number(c, "startAt", 1)), 1, 32767);
    } else if (k == "buBlip") {
      p.bullet = ParaProps::Bullet::Picture;
      p.bulletRel = std::string(RelId(A(c, "blip"), "embed"));
    } else if (k == "tabLst") {
      p.tabs.clear();
      ForEachChild(c, [&](pugi::xml_node tab) {
        text::TabStop t;
        t.position = std::clamp(EmuToPt(Number(tab, "pos", 0)), 0.f, 4000.f);
        std::string_view a = tab.attribute("algn").value();
        t.align = a == "ctr" ? text::TabAlign::Center : a == "r" ? text::TabAlign::End : a == "dec" ? text::TabAlign::Decimal
                                                                                                    : text::TabAlign::Start;
        p.tabs.push_back(t);
        return p.tabs.size() < 64;
      });
      std::sort(p.tabs.begin(), p.tabs.end(), [](const text::TabStop& a, const text::TabStop& b) { return a.position < b.position; });
    } else if (k == "defRPr") {
      ApplyRun(p.run, c, colors);
    }
    return true;
  });
}

// One level of a list-style container: a:defPPr first, then a:lvlNpPr.
void ApplyLevel(ParaProps& p, pugi::xml_node list, int level, const draw::ColorContext& colors) {
  if (!list) return;
  ApplyPara(p, A(list, "defPPr"), colors);
  char name[16] = "lvl1pPr";
  name[3] = static_cast<char>('1' + level);
  ApplyPara(p, A(list, name), colors);
}

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

std::wstring Alpha(int n, bool upper) {
  // a..z, then aa..zz, as PowerPoint counts.
  n = std::max(n, 1);
  const wchar_t c = static_cast<wchar_t>((upper ? L'A' : L'a') + (n - 1) % 26);
  return std::wstring(static_cast<size_t>((n - 1) / 26 + 1), c);
}

// ST_TextAutonumberScheme: the common ones; others count in arabic numerals with a period.
std::wstring AutoNumber(std::string_view scheme, int n) {
  std::wstring body;
  if (scheme.starts_with("roman")) body = Roman(n, scheme.starts_with("romanUc"));
  else if (scheme.starts_with("alpha")) body = Alpha(n, scheme.starts_with("alphaUc"));
  else if (scheme.starts_with("circleNum") && n >= 1 && n <= 20) return std::wstring(1, static_cast<wchar_t>(0x2460 + n - 1));
  else body = std::to_wstring(n);
  if (scheme.ends_with("ParenBoth")) return L"(" + body + L")";
  if (scheme.ends_with("ParenR")) return body + L")";
  if (scheme.ends_with("Period")) return body + L".";
  if (scheme.ends_with("Minus")) return L"- " + body + L" -";
  return body;
}

struct RunDraw {
  std::optional<draw::Color> color;  // none: not drawn (noFill)
  std::optional<draw::Color> highlight;
  uint8_t underline = 0;  // 0 none, 1 single, 2 double, 3 heavy
  uint8_t strike = 0;     // 0 none, 1 single, 2 double
  float ulPos = 0, ulThick = 0, stPos = 0, stThick = 0;
  int link = -1;
};

}  // namespace

std::wstring ThemeFace(std::string_view typeface, const draw::Theme* theme) {
  if (typeface.starts_with("+mn-") || typeface.starts_with("+mj-")) {
    if (!theme) return {};
    const draw::ThemeFont& f = typeface.starts_with("+mj") ? theme->MajorFont() : theme->MinorFont();
    return Utf8ToWide(typeface.ends_with("-ea") ? f.eastAsian : typeface.ends_with("-cs") ? f.complexScript : f.latin);
  }
  return Utf8ToWide(typeface);
}

struct TextBlock::Impl {
  struct Para {
    std::wstring text;
    std::deque<text::TextStyle> styles;
    text::TextStyle mark;
    std::vector<uint32_t> runStart;  // per run: its first offset in text
    std::vector<RunDraw> runs;
    std::vector<text::Line> lines;
    std::vector<draw::Point> origins;           // per line: x of column coordinates' 0, baseline y
    std::vector<std::pair<float, float>> rows;  // per line: top and bottom
    std::shared_ptr<const draw::Image> picture;  // a picture bullet, at its U+FFFC
    bool empty = true;
    bool bullet = false;  // the first run is the bullet and its tab
  };
  struct LinkRun {
    size_t para;
    uint32_t start, end;
    std::string relId, action;
  };
  std::vector<Para> paras;
  std::vector<LinkRun> links;
  draw::Matrix toBox;  // text coordinates → the box's
  float height = 0;
  bool wantMirror = true;
  draw::Rect rect;
};

TextBlock::TextBlock() : impl_(std::make_unique<Impl>()) {}
TextBlock::~TextBlock() = default;
TextBlock::TextBlock(TextBlock&&) noexcept = default;
TextBlock& TextBlock::operator=(TextBlock&&) noexcept = default;

void TextBlock::Build(text::Fonts& fonts, const draw::TextBox& box, const TextContext& ctx) {
  Impl& m = *impl_;
  m = Impl{};
  m.rect = box.rect;
  pugi::xml_node body = box.content;
  if (!body) return;
  const draw::ColorContext& colors = ctx.colors;
  const pugi::xml_node own = A(body, "lstStyle");
  const float fontScale = box.autofit == draw::TextBox::Autofit::ShrinkText ? box.fontScale : 1.f;
  const float lineReduction = box.autofit == draw::TextBox::Autofit::ShrinkText ? box.lineSpacingReduction : 0.f;
  const bool vertical = box.flow != draw::TextBox::Flow::Horizontal;
  const float boxW = std::max(0.f, vertical ? box.rect.Height() : box.rect.Width());
  const float boxH = std::max(0.f, vertical ? box.rect.Width() : box.rect.Height());
  const int columns = std::max(1, box.columns);
  const float colGap = columns > 1 ? box.columnSpacing : 0.f;
  const float colW = std::max(1.f, (boxW - colGap * (columns - 1)) / columns);
  const float layoutW = box.wrap ? colW : 100000.f;

  // The levels' properties, resolved once.
  ParaProps levels[kLevels];
  for (int l = 0; l < kLevels; ++l) {
    ParaProps& p = levels[l];
    for (pugi::xml_node list : ctx.lists) ApplyLevel(p, list, l, colors);
    if (ctx.fontColor) {
      p.run.color = ctx.fontColor;
      p.run.noFill = false;
    }
    if (!ctx.fontFace.empty()) p.run.latin = ctx.fontFace;
    if (ctx.color) {
      p.run.color = ctx.color;
      p.run.noFill = false;
    }
    if (ctx.bold) p.run.bold = *ctx.bold;
    if (ctx.italic) p.run.italic = *ctx.italic;
    if (!ctx.face.empty()) p.run.latin = ctx.face;
    ApplyLevel(p, own, l, colors);
  }

  const draw::Theme* theme = colors.theme;
  auto styleOf = [&](const RunProps& r) {
    text::TextStyle s;
    s.family = ThemeFace(r.latin.empty() ? "+mn-lt" : r.latin, theme);
    if (s.family.empty()) s.family = L"Calibri";
    s.eastAsiaFamily = ThemeFace(r.ea.empty() ? "+mn-ea" : r.ea, theme);
    s.complexFamily = ThemeFace(r.cs.empty() ? "+mn-cs" : r.cs, theme);
    float size = r.size * fontScale;
    if (r.baseline != 0) {
      s.baselineShift = r.baseline * size;
      size *= 2.f / 3;
    }
    s.size = std::max(0.5f, size);
    s.bold = s.complexBold = r.bold;
    s.italic = s.complexItalic = r.italic;
    s.caps = r.caps;
    s.letterSpacing = r.spacing;
    s.kerningMinSize = r.kern;
    if (!r.lang.empty()) s.language = Utf8ToWide(r.lang);
    return s;
  };
  auto runDraw = [&](const RunProps& r, const text::TextStyle& s) {
    RunDraw d;
    if (!r.noFill) d.color = r.color.value_or(draw::Color{0, 0, 0, 1});
    d.highlight = r.highlight;
    std::string_view u = r.underline;
    d.underline = u == "none" || u.empty() ? 0 : u.starts_with("dbl") ? 2 : u.starts_with("heavy") ? 3 : 1;
    d.strike = r.strike == "sngStrike" ? 1 : r.strike == "dblStrike" ? 2 : 0;
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

  // Automatic numbering: a counter per level, reset by a shallower paragraph.
  int counters[kLevels] = {};
  std::string counterScheme[kLevels];
  size_t chars = 0;
  float y = 0;
  int column = 0;
  float columnTop = 0;
  bool first = true;
  float maxBottom = 0;
  ForEachChild(body, [&](pugi::xml_node pn) {
    if (LocalNameOf(pn) != "p" || NamespaceOf(pn) != kA) return true;
    pugi::xml_node pPr = A(pn, "pPr");
    const int level = std::clamp(static_cast<int>(Number(pPr, "lvl", 0)), 0, kLevels - 1);
    ParaProps pp = levels[level];
    ApplyPara(pp, pPr, colors);
    Impl::Para& para = m.paras.emplace_back();
    // Runs.
    struct Piece {
      std::wstring text;
      RunProps props;
      pugi::xml_node link;
    };
    std::vector<Piece> pieces;
    RunProps markProps = pp.run;
    ForEachChild(pn, [&](pugi::xml_node r) {
      if (NamespaceOf(r) != kA) return true;
      const std::string_view k = LocalNameOf(r);
      if (k == "r" || k == "fld" || k == "br") {
        Piece piece;
        piece.props = pp.run;
        ApplyRun(piece.props, A(r, "rPr"), colors);
        if (k == "br") piece.text = L"\u000B";
        else if (k == "fld" && std::string_view(r.attribute("type").value()) == "slidenum") piece.text = std::to_wstring(ctx.slideNumber);
        else piece.text = Utf8ToWide(A(r, "t").text().get());
        // Line feeds inside a run break lines as a:br does; other control characters go.
        for (wchar_t& c : piece.text)
          if (c == L'\n' || c == L'\r') c = L'\u000B';
          else if (c < 0x20 && c != L'\t' && c != L'\u000B') c = L' ';
        piece.link = piece.props.link;
        if (!piece.text.empty() && chars < kMaxChars) {
          chars += piece.text.size();
          pieces.push_back(std::move(piece));
        }
      } else if (k == "endParaRPr") {
        ApplyRun(markProps, r, colors);
      }
      return true;
    });
    para.empty = pieces.empty();
    para.mark = styleOf(markProps);
    // Hyperlinks: the theme's hyperlink color and an underline, unless the run says to keep its own
    // color (a14's hlinkClr "tx").
    for (Piece& pc : pieces) {
      if (!pc.link || (RelId(pc.link, "id").empty() && std::string_view(pc.link.attribute("action").value()).empty())) continue;
      bool textColor = false;
      ForEachChild(A(pc.link, "extLst"), [&](pugi::xml_node ext) {
        ForEachChild(ext, [&](pugi::xml_node c) {
          if (LocalNameOf(c) == "hlinkClr" && std::string_view(c.attribute("val").value()) == "tx") textColor = true;
          return true;
        });
        return true;
      });
      if (!textColor) {
        if (auto c = draw::SchemeColor("hlink", colors)) pc.props.color = c;
        if (pc.props.underline == "none") pc.props.underline = "sng";
      }
    }
    // The bullet: a character, a number or a picture, then a tab to the text.
    std::wstring bulletText;
    RunProps bulletProps = pieces.empty() ? markProps : pieces.front().props;
    if (pp.bullet == ParaProps::Bullet::Number) {
      if (counterScheme[level] != pp.scheme || counters[level] == 0) counters[level] = pp.startAt - 1;
      counterScheme[level] = pp.scheme;
    }
    if (!para.empty) {
      for (int l = level + 1; l < kLevels; ++l) counters[l] = 0;
      if (pp.bullet != ParaProps::Bullet::Number) counters[level] = 0;
    }
    if (!para.empty && pp.bullet != ParaProps::Bullet::None) {
      if (!pp.bulletColorText) {
        bulletProps.color = pp.bulletColor;
        bulletProps.noFill = false;
      }
      if (pp.bulletSizeMode == 1) bulletProps.size *= pp.bulletSize;
      else if (pp.bulletSizeMode == 2) bulletProps.size = pp.bulletSize;
      bulletProps.underline = "none";
      bulletProps.strike = "noStrike";
      bulletProps.baseline = 0;
      bulletProps.spacing = 0;
      if (pp.bullet == ParaProps::Bullet::Char) {
        if (!pp.bulletFontText) bulletProps.latin = bulletProps.ea = bulletProps.cs = pp.bulletFont;
        bulletText = pp.bulletChar;
        const std::wstring face = ThemeFace(bulletProps.latin.empty() ? "+mn-lt" : bulletProps.latin, theme);
        if (IsSymbolFont(face))
          for (wchar_t& c : bulletText)
            if (c >= 0x20 && c < 0x100) c = static_cast<wchar_t>(0xF000 + c);
      } else if (pp.bullet == ParaProps::Bullet::Number) {
        bulletText = AutoNumber(pp.scheme, ++counters[level]);
        if (!pp.bulletFontText) bulletProps.latin = pp.bulletFont;
      } else if (pp.bullet == ParaProps::Bullet::Picture && ctx.resources && !pp.bulletRel.empty()) {
        para.picture = ctx.resources->GetImage(pp.bulletRel);
        if (para.picture) bulletText = L"￼";
      }
      if (!bulletText.empty()) bulletText += L'\t';
    }
    // The paragraph for fwtext.
    std::vector<text::TextRun> runs;
    auto add = [&](const std::wstring& t, const RunProps& props, pugi::xml_node link) {
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
        m.links.push_back({m.paras.size() - 1, para.runStart.back(), static_cast<uint32_t>(para.text.size()),
                           std::string(RelId(link, "id")), link.attribute("action").value()});
      }
      runs.push_back({static_cast<uint32_t>(t.size()), &st});
    };
    if (!bulletText.empty()) {
      add(bulletText, bulletProps, {});
      para.bullet = true;
    }
    for (const Piece& pc : pieces) add(pc.text, pc.props, pc.link);

    text::Paragraph tp;
    tp.text = para.text;
    tp.runs = runs;
    tp.markStyle = &para.mark;
    text::ParagraphFormat& f = tp.format;
    std::string_view algn = pp.algn;
    f.rightToLeft = pp.rtl;
    using text::Alignment;
    Alignment a = algn == "ctr"                         ? Alignment::Center
                  : algn == "r"                         ? Alignment::End
                  : algn == "just" || algn == "justLow" ? Alignment::Justify
                  : algn == "dist" || algn == "thaiDist" ? Alignment::Distribute
                                                         : Alignment::Start;
    if (pp.rtl && (a == Alignment::Start || a == Alignment::End)) a = a == Alignment::Start ? Alignment::End : Alignment::Start;
    f.alignment = box.wrap ? a : (pp.rtl ? Alignment::End : Alignment::Start);
    f.startIndent = pp.marL;
    f.endIndent = pp.marR;
    f.firstLineIndent = pp.indent;
    f.tabs = pp.tabs;
    f.defaultTabStop = pp.defTab;
    f.squeezeSpaces = false;
    f.hangPunctuation = false;
    para.lines = text::Layout(fonts, tp, layoutW);

    // Vertical placement. Percent line spacing scales each line's height (its font's ascent,
    // descent and gap); the lines' share of the stored autofit's reduction is taken off it.
    float largest = para.mark.size;
    if (!para.lines.empty()) {
      largest = 0;
      for (const text::GlyphPiece& g : para.lines.front().pieces) largest = std::max(largest, g.emSize);
      if (largest <= 0) largest = para.mark.size;
    }
    auto spacing = [&](const Spacing& s) { return s.percent ? s.value * largest * 1.2f : s.value * fontScale; };
    if (!first) y += spacing(pp.before);
    const float lineFactor = pp.line.percent ? std::max(0.f, pp.line.value - lineReduction) : 0.f;
    for (const text::Line& line : para.lines) {
      const float natural = line.ascent + line.descent + line.lineGap;
      const float pitch = pp.line.percent ? natural * lineFactor : pp.line.value * fontScale;
      // The baseline: the line's share above it, scaled like the line.
      const float above = natural > 0 ? pitch * (line.ascent + line.lineGap) / natural : 0;
      if (columns > 1 && column + 1 < columns && y + pitch > boxH + 0.01f && y > columnTop) {
        ++column;
        y = 0;
      }
      const float colX = column * (colW + colGap);
      float shift = 0;
      if (!box.wrap) {
        const float w = line.contentRight - line.contentLeft;
        const float left = pp.marL, right = boxW - pp.marR;
        if (a == Alignment::Center) shift = (left + right) / 2 - (line.contentLeft + w / 2);
        else if (a == Alignment::End) shift = right - line.contentRight;
      }
      para.origins.push_back({colX + shift, y + above});
      para.rows.emplace_back(y, y + pitch);
      y += pitch;
      maxBottom = std::max(maxBottom, y);
    }
    y += spacing(pp.after);
    first = false;
    return m.paras.size() < kMaxParagraphs;
  });
  // The block's height: the last line's bottom (the last paragraph's space after doesn't count).
  m.height = maxBottom;

  // Anchoring and the box: text coordinates (0, 0 at the top left of the text area, x along the
  // lines) to the box's.
  float dy = 0;
  switch (box.anchor) {
    case draw::TextBox::Anchor::Middle:
    case draw::TextBox::Anchor::Justified:
    case draw::TextBox::Anchor::Distributed:
      dy = (boxH - m.height) / 2;
      break;
    case draw::TextBox::Anchor::Bottom:
      dy = boxH - m.height;
      break;
    default:
      break;
  }
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
  draw::Matrix toArea = draw::Matrix::Translate(dx, dy);
  if (box.flow == draw::TextBox::Flow::Vertical270) toArea = toArea * draw::Matrix{0, -1, 1, 0, r.left, r.bottom};
  else if (vertical) toArea = toArea * draw::Matrix{0, 1, -1, 0, r.right, r.top};
  else toArea = toArea * draw::Matrix::Translate(r.left, r.top);
  if (box.rotation != 0) {
    const float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    toArea = toArea * draw::Matrix::Translate(-cx, -cy) * draw::Matrix::Rotate(box.rotation) * draw::Matrix::Translate(cx, cy);
  }
  m.toBox = toArea;
}

void TextBlock::Draw(draw::Painter& p, DrawFilter* filter, uint32_t shape) const {
  const Impl& m = *impl_;
  if (m.paras.empty()) return;
  p.Save();
  // PowerPoint doesn't mirror text when its shape is flipped: a horizontal flip leaves it as it
  // is, a vertical one turns it upside down.
  const draw::Matrix t = p.CurrentTransform();
  if (t.m11 * t.m22 - t.m12 * t.m21 < 0) {
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
      const auto runOf = [&](uint32_t offset) {
        size_t k = std::upper_bound(para.runStart.begin(), para.runStart.end(), offset) - para.runStart.begin();
        return k == 0 ? size_t{0} : k - 1;
      };
      // Highlights behind the text.
      for (size_t ri = 0; ri < para.runs.size(); ++ri) {
        if (!para.runs[ri].highlight) continue;
        const uint32_t s = para.runStart[ri], e = ri + 1 < para.runStart.size() ? para.runStart[ri + 1] : uint32_t(para.text.size());
        for (const auto& [x0, x1] : line.Ranges(s, e))
          p.FillRect({o.x + x0, para.rows[li].first, o.x + x1, para.rows[li].second}, draw::Brush::Solid(*para.runs[ri].highlight));
      }
      for (const text::GlyphPiece& g : line.pieces) {
        if (g.glyphs.empty() || g.run >= para.runs.size()) continue;
        const RunDraw& rd = para.runs[g.run];
        if (!rd.color) continue;
        const draw::Brush brush = draw::Brush::Solid(*rd.color);
        draw::GlyphRun run = g.Run();
        if (wantText && g.textEnd > g.textStart && g.textEnd <= para.text.size()) {
          run.text = reinterpret_cast<const char16_t*>(para.text.data() + g.textStart);
          run.textLength = g.textEnd - g.textStart;
        }
        const float baseline = o.y - g.rise;
        p.DrawGlyphs({o.x + g.OriginX(), baseline}, run, brush);
        if (rd.underline && !g.leader) {
          const float top = o.y + rd.ulPos - rd.ulThick / 2;
          p.FillRect({o.x + g.x, top, o.x + g.x + g.width, top + rd.ulThick}, brush);
          if (rd.underline == 2)
            p.FillRect({o.x + g.x, top + 2 * rd.ulThick, o.x + g.x + g.width, top + 3 * rd.ulThick}, brush);
        }
        if (rd.strike) {
          const float top = baseline - rd.stPos - rd.stThick / 2;
          p.FillRect({o.x + g.x, top, o.x + g.x + g.width, top + rd.stThick}, brush);
          if (rd.strike == 2) p.FillRect({o.x + g.x, top - 2 * rd.stThick, o.x + g.x + g.width, top - rd.stThick}, brush);
        }
      }
      if (para.picture)
        for (const text::InlineObject& obj : line.objects) {
          if (runOf(obj.offset) != 0) continue;
          if (std::shared_ptr<const draw::Bitmap> px = para.picture->Pixels(64, 64)) {
            const float h = obj.ascent + obj.descent;
            p.DrawBitmap(*px, {o.x + obj.x, o.y - obj.ascent, o.x + obj.x + obj.width, o.y - obj.ascent + h});
          }
        }
    }
  }
  p.Restore();
}

float TextBlock::ContentHeight() const { return impl_->height; }

bool TextBlock::Empty() const {
  for (const Impl::Para& p : impl_->paras)
    if (!p.empty) return false;
  return true;
}

namespace {
draw::Rect Transformed(const draw::Matrix& m, const draw::Rect& r) {
  const draw::Point pts[4] = {m.Apply({r.left, r.top}), m.Apply({r.right, r.top}), m.Apply({r.left, r.bottom}),
                              m.Apply({r.right, r.bottom})};
  draw::Rect out{pts[0].x, pts[0].y, pts[0].x, pts[0].y};
  for (const draw::Point& q : pts) {
    out.left = std::min(out.left, q.x);
    out.top = std::min(out.top, q.y);
    out.right = std::max(out.right, q.x);
    out.bottom = std::max(out.bottom, q.y);
  }
  return out;
}
}  // namespace

std::vector<draw::Rect> TextBlock::ParagraphBounds() const {
  std::vector<draw::Rect> out;
  for (const Impl::Para& p : impl_->paras) {
    draw::Rect r{};
    bool any = false;
    for (size_t i = 0; i < p.lines.size(); ++i) {
      const draw::Rect line{p.origins[i].x + p.lines[i].contentLeft, p.rows[i].first, p.origins[i].x + p.lines[i].contentRight,
                            p.rows[i].second};
      if (!any) r = line;
      r = {std::min(r.left, line.left), std::min(r.top, line.top), std::max(r.right, line.right), std::max(r.bottom, line.bottom)};
      any = true;
    }
    out.push_back(any ? Transformed(impl_->toBox, r) : draw::Rect{});
  }
  return out;
}

std::vector<TextBlock::Link> TextBlock::Links() const {
  std::vector<Link> out;
  for (const Impl::LinkRun& l : impl_->links) {
    const Impl::Para& p = impl_->paras[l.para];
    for (size_t i = 0; i < p.lines.size(); ++i)
      for (const auto& [x0, x1] : p.lines[i].Ranges(l.start, l.end))
        out.push_back({Transformed(impl_->toBox, {p.origins[i].x + x0, p.rows[i].first, p.origins[i].x + x1, p.rows[i].second}),
                       l.relId, l.action});
  }
  return out;
}

std::wstring TextBlock::PlainText() const {
  std::wstring out;
  for (const Impl::Para& p : impl_->paras) {
    if (!out.empty()) out += L'\n';
    // Leave the bullet out: it is the first run when the text starts with one.
    const size_t from = p.bullet && p.runStart.size() > 1 ? p.runStart[1] : 0;
    for (size_t i = from; i < p.text.size(); ++i) out += p.text[i] == L'\u000B' ? L'\n' : p.text[i];
  }
  return out;
}

}  // namespace fw::slide::render
