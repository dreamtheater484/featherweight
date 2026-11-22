// Text style inheritance (plan §8.3): a shape's paragraphs and runs resolved from the
// presentation's default text style, the master's text styles, the placeholder chain's list
// styles, the shape's style reference and list style, and the paragraph's and run's own properties.
#include <windows.h>

#include <algorithm>
#include <array>
#include <string>

#include "core/Strings.h"
#include "slide/Internal.h"

namespace fw::slide {

using namespace detail;

namespace {

constexpr int kLevels = 9;
constexpr size_t kMaxParagraphs = 100000;
constexpr size_t kMaxRuns = 100000;

struct Level {
  ParagraphProps para;
  RunProps run;
};
using Levels = std::array<Level, kLevels>;

struct Ctx {
  const Presentation& deck;
  const SlideBase& owner;  // the part whose relationships the text's links and pictures use
  const SlideBase& shownOn;
  const draw::Theme& theme;
  draw::ColorContext colors;
};

std::wstring Typeface(std::string_view face, const draw::Theme& theme) {
  if (face.size() == 6 && face[0] == '+' && face[3] == '-') {
    const draw::ThemeFont& f = face.substr(1, 2) == "mj" ? theme.MajorFont() : theme.MinorFont();
    std::string_view which = face.substr(4, 2);
    const std::string& name = which == "ea" ? f.eastAsian : which == "cs" ? f.complexScript : f.latin;
    return fw::Utf8ToWide(name);
  }
  return fw::Utf8ToWide(face);
}

Underline UnderlineOf(std::string_view u) {
  static constexpr std::pair<std::string_view, Underline> kTable[] = {
      {"none", Underline::None},           {"sng", Underline::Single},
      {"dbl", Underline::Double},          {"heavy", Underline::Heavy},
      {"dotted", Underline::Dotted},       {"dottedHeavy", Underline::DottedHeavy},
      {"dash", Underline::Dash},           {"dashHeavy", Underline::DashHeavy},
      {"dashLong", Underline::DashLong},   {"dashLongHeavy", Underline::DashLongHeavy},
      {"dotDash", Underline::DotDash},     {"dotDashHeavy", Underline::DotDashHeavy},
      {"dotDotDash", Underline::DotDotDash}, {"dotDotDashHeavy", Underline::DotDotDashHeavy},
      {"wavy", Underline::Wavy},           {"wavyHeavy", Underline::WavyHeavy},
      {"wavyDbl", Underline::WavyDouble},  {"words", Underline::Words},
  };
  for (const auto& [name, value] : kTable) {
    if (name == u) return value;
  }
  return Underline::None;
}

std::optional<draw::Color> FirstColor(pugi::xml_node fill, const Ctx& c) {
  std::string_view k = LocalNameOf(fill);
  if (k == "solidFill") return draw::ReadColor(fill, c.colors);
  if (k == "gradFill") {
    pugi::xml_node gs = A(A(fill, "gsLst"), "gs");
    return draw::ReadColor(gs, c.colors);
  }
  if (k == "pattFill") return draw::ReadColor(A(fill, "fgClr"), c.colors);
  return std::nullopt;
}

void ApplyRPr(pugi::xml_node rPr, RunProps& r, const Ctx& c) {
  if (!rPr) return;
  if (pugi::xml_attribute a = rPr.attribute("sz")) r.size = std::clamp(static_cast<float>(Number(a.value(), 1800) / 100), 1.f, 4000.f);
  if (rPr.attribute("b")) r.bold = Bool(rPr, "b", false);
  if (rPr.attribute("i")) r.italic = Bool(rPr, "i", false);
  if (pugi::xml_attribute a = rPr.attribute("u")) r.underline = UnderlineOf(a.value());
  if (pugi::xml_attribute a = rPr.attribute("strike")) {
    std::string_view v = a.value();
    r.strike = v == "sngStrike" ? Strike::Single : v == "dblStrike" ? Strike::Double : Strike::None;
  }
  if (pugi::xml_attribute a = rPr.attribute("cap")) {
    std::string_view v = a.value();
    r.caps = v == "small" ? Caps::Small : v == "all" ? Caps::All : Caps::None;
  }
  if (pugi::xml_attribute a = rPr.attribute("baseline")) r.baseline = std::clamp(static_cast<float>(Number(a.value(), 0) / 100000), -10.f, 10.f);
  if (pugi::xml_attribute a = rPr.attribute("spc")) r.spacing = std::clamp(static_cast<float>(Number(a.value(), 0) / 100), -4000.f, 4000.f);
  if (pugi::xml_attribute a = rPr.attribute("kern")) r.kern = std::clamp(static_cast<float>(Number(a.value(), 0) / 100), 0.f, 4000.f);
  if (pugi::xml_attribute a = rPr.attribute("lang")) r.lang = a.value();
  ForEachChild(rPr, [&](pugi::xml_node n) {
    std::string_view k = LocalNameOf(n);
    if (k == "ln") {
      r.outline = n;
    } else if (k == "noFill") {
      r.noFill = true;
      r.fill = {};
    } else if (k == "solidFill" || k == "gradFill" || k == "pattFill" || k == "blipFill") {
      r.noFill = false;
      r.fill = k == "solidFill" ? pugi::xml_node() : n;
      if (std::optional<draw::Color> col = FirstColor(n, c)) r.color = *col;
    } else if (k == "effectLst" || k == "effectDag") {
      r.effects = n;
    } else if (k == "highlight") {
      r.highlight = draw::ReadColor(n, c.colors);
    } else if (k == "uFillTx") {
      r.underlineColor.reset();
    } else if (k == "uFill") {
      r.underlineColor = FirstColor(A(n, "solidFill") ? A(n, "solidFill") : n.first_child(), c);
    } else if (k == "latin") {
      r.latin = Typeface(n.attribute("typeface").value(), c.theme);
    } else if (k == "ea") {
      r.eastAsian = Typeface(n.attribute("typeface").value(), c.theme);
    } else if (k == "cs") {
      r.complexScript = Typeface(n.attribute("typeface").value(), c.theme);
    } else if (k == "sym") {
      r.symbol = Typeface(n.attribute("typeface").value(), c.theme);
    } else if (k == "hlinkClick") {
      r.click = ReadHyperlink(n, c.owner, c.deck);
    } else if (k == "hlinkMouseOver") {
      r.hover = ReadHyperlink(n, c.owner, c.deck);
    }
    return true;
  });
}

Spacing ReadSpacing(pugi::xml_node n, Spacing def) {
  if (pugi::xml_node pct = A(n, "spcPct")) {
    return {true, std::clamp(static_cast<float>(Number(pct, "val", 100000) / 100000), 0.f, 132.f)};
  }
  if (pugi::xml_node pts = A(n, "spcPts")) {
    return {false, std::clamp(static_cast<float>(Number(pts, "val", 0) / 100), 0.f, 1584.f)};
  }
  return def;
}

TextAlign AlignOf(std::string_view a, TextAlign def) {
  if (a == "l") return TextAlign::Left;
  if (a == "ctr") return TextAlign::Center;
  if (a == "r") return TextAlign::Right;
  if (a == "just") return TextAlign::Justify;
  if (a == "justLow") return TextAlign::JustifyLow;
  if (a == "dist") return TextAlign::Distributed;
  if (a == "thaiDist") return TextAlign::ThaiDistributed;
  return def;
}

// a:pPr, a list style's level (lvlNpPr) or defPPr. `run` gets its defRPr, when given.
void ApplyPPr(pugi::xml_node pPr, ParagraphProps& p, RunProps* run, const Ctx& c, const SlideBase& styleOwner) {
  if (!pPr) return;
  constexpr float kMaxIndent = 4032;
  if (pPr.attribute("marL")) p.marginLeft = std::clamp(Emu(pPr, "marL"), 0.f, kMaxIndent);
  if (pPr.attribute("marR")) p.marginRight = std::clamp(Emu(pPr, "marR"), 0.f, kMaxIndent);
  if (pPr.attribute("indent")) p.indent = std::clamp(Emu(pPr, "indent"), -kMaxIndent, kMaxIndent);
  if (pugi::xml_attribute a = pPr.attribute("algn")) p.align = AlignOf(a.value(), p.align);
  if (pugi::xml_attribute a = pPr.attribute("fontAlgn")) {
    std::string_view v = a.value();
    p.fontAlign = v == "t"      ? FontAlign::Top
                  : v == "ctr"  ? FontAlign::Center
                  : v == "base" ? FontAlign::Baseline
                  : v == "b"    ? FontAlign::Bottom
                                : FontAlign::Auto;
  }
  if (pPr.attribute("defTabSz")) p.defaultTab = std::clamp(Emu(pPr, "defTabSz", 914400), 0.f, kMaxIndent);
  if (pPr.attribute("rtl")) p.rtl = Bool(pPr, "rtl", false);
  if (pPr.attribute("eaLnBrk")) p.eastAsianLineBreak = Bool(pPr, "eaLnBrk", true);
  if (pPr.attribute("latinLnBrk")) p.latinLineBreak = Bool(pPr, "latinLnBrk", false);
  if (pPr.attribute("hangingPunct")) p.hangingPunctuation = Bool(pPr, "hangingPunct", false);
  Bullet& b = p.bullet;
  ForEachChild(pPr, [&](pugi::xml_node n) {
    std::string_view k = LocalNameOf(n);
    if (k == "lnSpc") {
      p.lineSpacing = ReadSpacing(n, p.lineSpacing);
    } else if (k == "spcBef") {
      p.spaceBefore = ReadSpacing(n, p.spaceBefore);
    } else if (k == "spcAft") {
      p.spaceAfter = ReadSpacing(n, p.spaceAfter);
    } else if (k == "buClrTx") {
      b.color.reset();
    } else if (k == "buClr") {
      b.color = draw::ReadColor(n, c.colors);
    } else if (k == "buSzTx") {
      b.sizePercent = 1;
      b.sizePoints = 0;
    } else if (k == "buSzPct") {
      b.sizePercent = std::clamp(static_cast<float>(Number(n, "val", 100000) / 100000), 0.25f, 4.f);
      b.sizePoints = 0;
    } else if (k == "buSzPts") {
      b.sizePoints = std::clamp(static_cast<float>(Number(n, "val", 1800) / 100), 1.f, 4000.f);
    } else if (k == "buFontTx") {
      b.font.clear();
    } else if (k == "buFont") {
      b.font = Typeface(n.attribute("typeface").value(), c.theme);
    } else if (k == "buNone") {
      b.kind = Bullet::Kind::None;
    } else if (k == "buAutoNum") {
      b.kind = Bullet::Kind::AutoNumber;
      b.scheme = n.attribute("type").value();
      b.startAt = std::clamp(static_cast<int>(Number(n, "startAt", 1)), 1, 32767);
    } else if (k == "buChar") {
      b.kind = Bullet::Kind::Char;
      b.character = fw::Utf8ToWide(n.attribute("char").value());
      if (b.character.size() > 8) b.character.resize(8);
    } else if (k == "buBlip") {
      b.kind = Bullet::Kind::Picture;
      b.pictureRel = std::string(RelId(A(n, "blip"), "embed"));
      b.picturePart = styleOwner.part;
    } else if (k == "tabLst") {
      p.tabs.clear();
      ForEachChild(n, [&](pugi::xml_node t) {
        if (LocalNameOf(t) != "tab") return true;
        TabStop stop;
        stop.position = std::clamp(Emu(t, "pos"), -kMaxIndent, kMaxIndent);
        std::string_view al = t.attribute("algn").value();
        stop.kind = al == "ctr" ? TabStop::Kind::Center
                    : al == "r" ? TabStop::Kind::Right
                    : al == "dec" ? TabStop::Kind::Decimal
                                  : TabStop::Kind::Left;
        p.tabs.push_back(stop);
        return p.tabs.size() < 64;
      });
    } else if (k == "defRPr" && run) {
      ApplyRPr(n, *run, c);
    }
    return true;
  });
}

void ApplyListStyle(pugi::xml_node lst, Levels& lv, const Ctx& c, const SlideBase& styleOwner) {
  if (!lst) return;
  if (pugi::xml_node def = A(lst, "defPPr")) {
    for (Level& l : lv) ApplyPPr(def, l.para, &l.run, c, styleOwner);
  }
  char name[] = "lvl1pPr";
  for (int i = 0; i < kLevels; ++i) {
    name[3] = static_cast<char>('1' + i);
    if (pugi::xml_node n = A(lst, name)) ApplyPPr(n, lv[i].para, &lv[i].run, c, styleOwner);
  }
}

// p:style's a:fontRef: the theme's major or minor fonts, and a color.
void ApplyFontRef(pugi::xml_node style, Levels& lv, const Ctx& c) {
  pugi::xml_node ref = A(style, "fontRef");
  if (!ref) return;
  std::string_view idx = ref.attribute("idx").value();
  const draw::ThemeFont* f = idx == "major" ? &c.theme.MajorFont() : idx == "minor" ? &c.theme.MinorFont() : nullptr;
  std::optional<draw::Color> color = draw::ReadColor(ref, c.colors);
  for (Level& l : lv) {
    if (f) {
      l.run.latin = fw::Utf8ToWide(f->latin);
      l.run.eastAsian = fw::Utf8ToWide(f->eastAsian);
      l.run.complexScript = fw::Utf8ToWide(f->complexScript);
    }
    if (color) {
      l.run.color = *color;
      l.run.noFill = false;
      l.run.fill = {};
    }
  }
}

Levels Defaults(const Ctx& c) {
  Levels lv;
  for (int i = 0; i < kLevels; ++i) {
    lv[i].para.level = i;
    lv[i].run.latin = fw::Utf8ToWide(c.theme.MinorFont().latin);
    lv[i].run.eastAsian = fw::Utf8ToWide(c.theme.MinorFont().eastAsian);
    lv[i].run.complexScript = fw::Utf8ToWide(c.theme.MinorFont().complexScript);
    if (std::optional<draw::Color> tx1 = draw::SchemeColor("tx1", c.colors)) lv[i].run.color = *tx1;
  }
  return lv;
}

BodyProps ReadBody(pugi::xml_node bodyPr) {
  BodyProps b;
  if (!bodyPr) return b;
  constexpr float kMax = 4032;
  b.left = std::clamp(Emu(bodyPr, "lIns", 91440), 0.f, kMax);
  b.top = std::clamp(Emu(bodyPr, "tIns", 45720), 0.f, kMax);
  b.right = std::clamp(Emu(bodyPr, "rIns", 91440), 0.f, kMax);
  b.bottom = std::clamp(Emu(bodyPr, "bIns", 45720), 0.f, kMax);
  std::string_view anchor = bodyPr.attribute("anchor").value();
  b.anchor = anchor == "ctr"    ? BodyProps::Anchor::Middle
             : anchor == "b"    ? BodyProps::Anchor::Bottom
             : anchor == "just" ? BodyProps::Anchor::Justified
             : anchor == "dist" ? BodyProps::Anchor::Distributed
                                : BodyProps::Anchor::Top;
  b.anchorCenter = Bool(bodyPr, "anchorCtr", false);
  b.wrap = std::string_view(bodyPr.attribute("wrap").value()) != "none";
  std::string_view vert = bodyPr.attribute("vert").value();
  b.vertical = vert == "vert"             ? BodyProps::Vertical::Vertical
               : vert == "vert270"        ? BodyProps::Vertical::Vertical270
               : vert == "wordArtVert"    ? BodyProps::Vertical::WordArtVertical
               : vert == "eaVert"         ? BodyProps::Vertical::EastAsian
               : vert == "mongolianVert"  ? BodyProps::Vertical::Mongolian
               : vert == "wordArtVertRtl" ? BodyProps::Vertical::WordArtRtl
                                          : BodyProps::Vertical::Horizontal;
  b.rotation = static_cast<float>(Number(bodyPr, "rot", 0) / 60000.0);
  b.upright = Bool(bodyPr, "upright", false);
  b.rtlColumns = Bool(bodyPr, "rtlCol", false);
  b.columns = std::clamp(static_cast<int>(Number(bodyPr, "numCol", 1)), 1, 16);
  b.columnSpacing = std::clamp(Emu(bodyPr, "spcCol"), 0.f, kMax);
  if (pugi::xml_node fit = A(bodyPr, "normAutofit")) {
    b.autofit = BodyProps::Autofit::Shrink;
    b.fontScale = std::clamp(static_cast<float>(Number(fit, "fontScale", 100000) / 100000), 0.01f, 1.f);
    b.lineSpacingReduction = std::clamp(static_cast<float>(Number(fit, "lnSpcReduction", 0) / 100000), 0.f, 1.f);
  } else if (A(bodyPr, "spAutoFit")) {
    b.autofit = BodyProps::Autofit::ResizeShape;
  }
  b.warp = A(bodyPr, "prstTxWarp").attribute("prst").value();
  if (b.warp == "textNoShape") b.warp.clear();
  return b;
}

// PowerPoint's date and time fields (datetime1-13) show the date and time now, in the run's
// language, as PowerPoint updates them when it shows or exports a slide. Empty for other types.
std::wstring DateField(std::string_view type, const std::string& lang) {
  struct Format {
    const wchar_t* date;  // a GetDateFormatEx picture; nullptr: the locale's short or long date
    bool longDate;
    const wchar_t* time;  // a GetTimeFormatEx picture, or nullptr
  };
  static constexpr Format kFormats[] = {
      {nullptr, false, nullptr},                   // datetime1: 11/20/2026
      {nullptr, true, nullptr},                    // datetime2: Friday, November 20, 2026
      {L"d MMMM yyyy", false, nullptr},            // datetime3
      {L"MMMM d, yyyy", false, nullptr},           // datetime4
      {L"d-MMM-yy", false, nullptr},               // datetime5
      {L"MMMM yy", false, nullptr},                // datetime6
      {L"MMM-yy", false, nullptr},                 // datetime7
      {nullptr, false, L"h:mm tt"},                // datetime8: date and time
      {nullptr, false, L"h:mm:ss tt"},             // datetime9
      {L"", false, L"H:mm"},                       // datetime10: time only
      {L"", false, L"H:mm:ss"},                    // datetime11
      {L"", false, L"h:mm tt"},                    // datetime12
      {L"", false, L"h:mm:ss tt"},                 // datetime13
  };
  std::string_view n = type.substr(8);
  int i = 0;
  for (char ch : n) {
    if (ch < '0' || ch > '9' || i > 13) return {};
    i = i * 10 + (ch - '0');
  }
  if (n.empty()) i = 1;
  if (i < 1 || i > 13) return {};
  const Format& f = kFormats[i - 1];
  std::wstring locale = fw::Utf8ToWide(lang);
  if (!locale.empty() && !IsValidLocaleName(locale.c_str())) locale.clear();
  const wchar_t* name = locale.empty() ? LOCALE_NAME_USER_DEFAULT : locale.c_str();
  SYSTEMTIME now;
  GetLocalTime(&now);
  wchar_t buf[128];
  std::wstring out;
  if (!f.date || *f.date) {
    DWORD flags = f.date ? 0 : f.longDate ? DATE_LONGDATE : DATE_SHORTDATE;
    if (GetDateFormatEx(name, flags, &now, f.date, buf, 128, nullptr) > 0) out = buf;
  }
  if (f.time && GetTimeFormatEx(name, 0, &now, f.time, buf, 128) > 0) {
    if (!out.empty()) out += L' ';
    out += buf;
  }
  return out;
}

// The paragraphs of a text body, over the resolved levels.
void ReadParagraphs(pugi::xml_node txBody, const Levels& lv, const Ctx& c, TextBody& out) {
  struct Counter {
    std::string scheme;
    int next = 1;
    bool active = false;
  };
  std::array<Counter, kLevels> counters;
  size_t runs = 0;
  const int slideNumber = c.shownOn.kind == PartKind::Slide ? static_cast<const Slide&>(c.shownOn).number : -1;
  ForEachChild(txBody, [&](pugi::xml_node p) {
    if (!Is(p, kA, "p")) return true;
    Paragraph para;
    para.node = p;
    pugi::xml_node pPr = A(p, "pPr");
    int level = std::clamp(static_cast<int>(Number(pPr, "lvl", 0)), 0, kLevels - 1);
    para.props = lv[level].para;
    ApplyPPr(pPr, para.props, nullptr, c, c.owner);
    para.props.level = level;
    const RunProps& base = lv[level].run;
    para.endProps = base;
    ForEachChild(p, [&](pugi::xml_node n) {
      std::string_view k = LocalNameOf(n);
      if (NamespaceOf(n) != kA) return true;
      if (k == "r" || k == "fld" || k == "br") {
        TextRun run;
        run.node = n;
        run.kind = k == "r" ? TextRun::Kind::Text : k == "fld" ? TextRun::Kind::Field : TextRun::Kind::Break;
        run.props = base;
        ApplyRPr(A(n, "rPr"), run.props, c);
        if (k != "br") run.text = fw::Utf8ToWide(A(n, "t").text().get());
        if (k == "fld") {
          run.fieldType = n.attribute("type").value();
          if (run.fieldType == "slidenum" && slideNumber >= 0 && !c.owner.part.empty()) run.text = std::to_wstring(slideNumber);
          if (run.fieldType.starts_with("datetime")) {
            std::wstring now = DateField(run.fieldType, run.props.lang);
            if (!now.empty()) run.text = std::move(now);
          }
        }
        para.runs.push_back(std::move(run));
        ++runs;
      } else if (k == "endParaRPr") {
        ApplyRPr(n, para.endProps, c);
      }
      return runs < kMaxRuns;
    });
    // Automatic numbering: a list goes on at its level until a paragraph at that level isn't
    // numbered; deeper levels start again under each shallower paragraph.
    for (int i = level + 1; i < kLevels; ++i) counters[i].active = false;
    Counter& ctr = counters[level];
    const Bullet& b = para.props.bullet;
    bool empty = para.runs.empty();
    if (b.kind == Bullet::Kind::AutoNumber && !empty) {
      if (!ctr.active || ctr.scheme != b.scheme) ctr = {b.scheme, b.startAt, true};
      para.number = ctr.next++;
    } else if (!empty) {
      ctr.active = false;
    }
    out.paragraphs.push_back(std::move(para));
    return out.paragraphs.size() < kMaxParagraphs && runs < kMaxRuns;
  });
}

const draw::Theme& ThemeOf(const SlideBase& shownOn) {
  return shownOn.theme ? *shownOn.theme : *draw::Theme::Default();
}

}  // namespace

std::wstring DateFieldText(std::string_view type, const std::string& lang) {
  return type.starts_with("datetime") ? DateField(type, lang) : std::wstring();
}

namespace detail {

std::unique_ptr<TextBody> ResolveText(const Presentation& deck, const Shape& shape, const SlideBase& owner,
                                      const SlideBase& shownOn, pugi::xml_node workingCopy) {
  auto out = std::make_unique<TextBody>();
  // A shape's own text body (a placeholder's textNode is in its merged copy); SmartArt's dsp:txBody
  // and the like come as a stand-in shape whose textNode is the body itself. Text being edited
  // (P2) is a working copy of it.
  pugi::xml_node txBody = workingCopy ? workingCopy : shape.drawNode == shape.node ? shape.textNode : P(shape.node, "txBody");
  out->node = txBody;
  out->body = ReadBody(A(shape.textNode, "bodyPr"));
  Ctx c{deck, owner, shownOn, ThemeOf(shownOn), {}};
  c.colors.theme = &c.theme;
  c.colors.colorMap = &shownOn.colorMap;
  Levels lv = Defaults(c);
  ApplyListStyle(deck.DefaultTextStyle(), lv, c, owner);
  // The master's text style for this kind of text.
  if (const Master* m = MasterOf(owner)) {
    pugi::xml_node style;
    if (m->kind == PartKind::NotesMaster) {
      if (shape.placeholder.type == PlaceholderType::Body) style = m->bodyStyle;
    } else {
      switch (shape.placeholder.type) {
        case PlaceholderType::Title:
        case PlaceholderType::CenteredTitle:
          style = m->titleStyle;
          break;
        case PlaceholderType::Body:
        case PlaceholderType::Subtitle:
        case PlaceholderType::Object:
        case PlaceholderType::Chart:
        case PlaceholderType::Table:
        case PlaceholderType::ClipArt:
        case PlaceholderType::Diagram:
        case PlaceholderType::Media:
        case PlaceholderType::Picture:
          style = m->bodyStyle;
          break;
        case PlaceholderType::None:
          // Text boxes and shapes start from the default text style alone (PowerPoint 2021,
          // probed 2026-11-20: otherStyle doesn't reach them).
          break;
        default:
          style = m->otherStyle;
          break;
      }
    }
    ApplyListStyle(style, lv, c, *m);
  }
  // The placeholders it inherits from, the master's first.
  std::vector<const Shape*> chain;
  for (const Shape* s = shape.inherits; s && chain.size() < 4; s = s->inherits) chain.push_back(s);
  for (size_t i = chain.size(); i-- > 0;) {
    const SlideBase& from = chain[i]->owner ? *chain[i]->owner : owner;
    ApplyListStyle(A(P(chain[i]->node, "txBody"), "lstStyle"), lv, c, from);
  }
  pugi::xml_node style;
  ForEachChild(shape.drawNode, [&](pugi::xml_node n) {
    if (LocalNameOf(n) == "style") style = n;  // p:style, dsp:style, wps:style
    return !style;
  });
  ApplyFontRef(style, lv, c);
  ApplyListStyle(A(txBody, "lstStyle"), lv, c, owner);
  ReadParagraphs(txBody, lv, c, *out);
  return out;
}

std::unique_ptr<TextBody> ResolvePlainText(const Presentation& deck, pugi::xml_node txBody, const SlideBase& owner,
                                           const SlideBase& shownOn, const std::vector<pugi::xml_node>& tcTxStyles) {
  auto out = std::make_unique<TextBody>();
  out->node = txBody;
  out->body = ReadBody(A(txBody, "bodyPr"));
  Ctx c{deck, owner, shownOn, ThemeOf(shownOn), {}};
  c.colors.theme = &c.theme;
  c.colors.colorMap = &shownOn.colorMap;
  Levels lv = Defaults(c);
  ApplyListStyle(deck.DefaultTextStyle(), lv, c, owner);
  // Table cells take the master's other style, but not its colors: their text is tx1 unless the
  // table style says otherwise (PowerPoint 2021, probed 2026-11-20).
  if (const Master* m = MasterOf(owner); m && m->kind == PartKind::Master) ApplyListStyle(m->otherStyle, lv, c, *m);
  if (std::optional<draw::Color> tx1 = draw::SchemeColor("tx1", c.colors)) {
    for (Level& l : lv) {
      l.run.color = *tx1;
      l.run.noFill = false;
      l.run.fill = {};
    }
  }
  for (pugi::xml_node tcTxStyle : tcTxStyles) {
    std::string_view b = tcTxStyle.attribute("b").value(), i = tcTxStyle.attribute("i").value();
    ApplyFontRef(tcTxStyle, lv, c);
    std::wstring latin, ea, cs;
    if (pugi::xml_node font = A(tcTxStyle, "font")) {
      latin = Typeface(A(font, "latin").attribute("typeface").value(), c.theme);
      ea = Typeface(A(font, "ea").attribute("typeface").value(), c.theme);
      cs = Typeface(A(font, "cs").attribute("typeface").value(), c.theme);
    }
    std::optional<draw::Color> color = draw::ReadColor(tcTxStyle, c.colors);
    for (Level& l : lv) {
      if (b == "on") l.run.bold = true;
      if (b == "off") l.run.bold = false;
      if (i == "on") l.run.italic = true;
      if (i == "off") l.run.italic = false;
      if (!latin.empty()) l.run.latin = latin;
      if (!ea.empty()) l.run.eastAsian = ea;
      if (!cs.empty()) l.run.complexScript = cs;
      if (color) {
        l.run.color = *color;
        l.run.noFill = false;
        l.run.fill = {};
      }
    }
  }
  ApplyListStyle(A(txBody, "lstStyle"), lv, c, owner);
  ReadParagraphs(txBody, lv, c, *out);
  return out;
}

}  // namespace detail

}  // namespace fw::slide
