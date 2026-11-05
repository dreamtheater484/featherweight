#include "doc/edit/Format.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <map>

#include "doc/model/Internal.h"

namespace fw::doc::edit {

using namespace names;

namespace {

constexpr uint32_t kRStyle = Name("rStyle"), kRFonts = Name("rFonts"), kB = Name("b"), kBCs = Name("bCs");
constexpr uint32_t kI = Name("i"), kICs = Name("iCs"), kStrike = Name("strike"), kDStrike = Name("dstrike");
constexpr uint32_t kCaps = Name("caps"), kSmallCaps = Name("smallCaps"), kColor = Name("color"), kSz = Name("sz");
constexpr uint32_t kSzCs = Name("szCs"), kHighlight = Name("highlight"), kU = Name("u"), kVertAlign = Name("vertAlign");
constexpr uint32_t kRPrChange = Name("rPrChange"), kPStyle = Name("pStyle"), kNumPr = Name("numPr"), kIlvl = Name("ilvl");
constexpr uint32_t kJc = Name("jc"), kInd = Name("ind"), kSpacing = Name("spacing"), kPBdr = Name("pBdr"), kShd = Name("shd");
constexpr uint32_t kPPrChange = Name("pPrChange");

bool Strict(const Document& doc) {
  const Part* p = doc.FindPart(PartKind::Document);
  return p && IsStrictUri(p->uri[static_cast<size_t>(Ns::W)]);
}

std::string Hex6(uint32_t rgb) {
  char buf[8];
  std::snprintf(buf, sizeof(buf), "%06X", rgb & 0xFFFFFF);
  return buf;
}

std::string Num(int64_t v) { return std::to_string(v); }

// The w:r elements with text in [a, b) of a paragraph, in order, each once.
std::vector<ElemId> RunsIn(const Document& doc, uint32_t para, uint32_t a, uint32_t b) {
  std::vector<ElemId> out;
  for (const TextRun& r : doc.Runs(para)) {
    if (!r.run || r.displayOnly || r.start >= b || r.end <= a) continue;
    ElemId id = r.run.Id();
    if (std::find(out.begin(), out.end(), id) == out.end()) out.push_back(id);
  }
  return out;
}

// The paragraph's [from, to) within the range.
std::pair<uint32_t, uint32_t> Span(const Document& doc, Range r, uint32_t para) {
  auto len = static_cast<uint32_t>(doc.Para(para).text.size());
  uint32_t a = para == r.from.para ? std::min(r.from.offset, len) : 0;
  uint32_t b = para == r.to.para ? std::min(r.to.offset, len) : len;
  return {a, std::max(a, b)};
}

// Sets an on/off property: removed when the styles give the same value, written otherwise.
void SetOnOff(TreeEdit& ed, ElemId pr, uint32_t name, bool value, bool inherited) {
  if (value == inherited) {
    ed.Remove(pr, name);
    return;
  }
  ElemId e = ed.Child(pr, name);
  if (value) ed.RemoveAttr(e, kVal);
  else ed.SetAttr(e, kVal, "0");
}

void SetVal(TreeEdit& ed, ElemId pr, uint32_t name, std::string_view value) { ed.SetAttr(ed.Child(pr, name), kVal, value); }

void SetColor(TreeEdit& ed, ElemId e, uint32_t val, Color c) {
  ed.SetAttr(e, val, c.automatic ? std::string("auto") : Hex6(c.rgb));
  for (uint32_t n : {Name("themeColor"), Name("themeTint"), Name("themeShade")}) ed.RemoveAttr(e, n);
}

}  // namespace

void WriteRunFormat(TreeEdit& ed, ElemId rPr, const RunFormat& f, const RunProps& inh) {
  const Document& doc = ed.Doc();
  if (f.font) {
    if (*f.font == inh.fonts.ascii && *f.font == inh.fonts.hAnsi) {
      if (ElemId rf = ed.Child(rPr, kRFonts, false); rf != kNoElem) {
        for (uint32_t n : {Name("ascii"), Name("hAnsi"), Name("cs"), Name("asciiTheme"), Name("hAnsiTheme"), Name("cstheme")})
          ed.RemoveAttr(rf, n);
        if (Node(&doc.GetTree(), rf).AttrCount() == 0) ed.Remove(rPr, kRFonts);
      }
    } else {
      ElemId rf = ed.Child(rPr, kRFonts);
      for (uint32_t n : {Name("ascii"), Name("hAnsi"), Name("cs")}) ed.SetAttr(rf, n, *f.font);
      for (uint32_t n : {Name("asciiTheme"), Name("hAnsiTheme"), Name("cstheme")}) ed.RemoveAttr(rf, n);
    }
  }
  if (f.themeFont && !f.font) {
    // Word's references: the Latin font for ASCII and the rest of Latin text, the complex script one.
    const bool major = *f.themeFont == ThemeFontSlot::Headings;
    const std::string_view latin = doc.ThemeFont(major ? "majorHAnsi" : "minorHAnsi");
    if (!latin.empty() && latin == inh.fonts.ascii && latin == inh.fonts.hAnsi) {
      // The styles give it already (through the theme, as Normal usually does): the run's own goes.
      if (ElemId rf = ed.Child(rPr, kRFonts, false); rf != kNoElem) {
        for (uint32_t n : {Name("ascii"), Name("hAnsi"), Name("cs"), Name("asciiTheme"), Name("hAnsiTheme"), Name("cstheme")})
          ed.RemoveAttr(rf, n);
        if (Node(&doc.GetTree(), rf).AttrCount() == 0) ed.Remove(rPr, kRFonts);
      }
    } else {
      const ElemId rf = ed.Child(rPr, kRFonts);
      ed.SetAttr(rf, Name("asciiTheme"), major ? "majorHAnsi" : "minorHAnsi");
      ed.SetAttr(rf, Name("hAnsiTheme"), major ? "majorHAnsi" : "minorHAnsi");
      ed.SetAttr(rf, Name("cstheme"), major ? "majorBidi" : "minorBidi");
      for (uint32_t n : {Name("ascii"), Name("hAnsi"), Name("cs")}) ed.RemoveAttr(rf, n);
    }
  }
  if (f.size) {
    if (*f.size == inh.size && *f.size == inh.sizeCs) {
      ed.Remove(rPr, kSz);
      ed.Remove(rPr, kSzCs);
    } else {
      SetVal(ed, rPr, kSz, Num(f.size->Value()));
      SetVal(ed, rPr, kSzCs, Num(f.size->Value()));
    }
  }
  if (f.bold) {
    SetOnOff(ed, rPr, kB, *f.bold, inh.bold);
    SetOnOff(ed, rPr, kBCs, *f.bold, inh.boldCs);
  }
  if (f.italic) {
    SetOnOff(ed, rPr, kI, *f.italic, inh.italic);
    SetOnOff(ed, rPr, kICs, *f.italic, inh.italicCs);
  }
  if (f.strike) {
    SetOnOff(ed, rPr, kStrike, *f.strike, inh.strike);
    if (*f.strike) SetOnOff(ed, rPr, kDStrike, false, inh.doubleStrike);
  }
  if (f.doubleStrike) {
    SetOnOff(ed, rPr, kDStrike, *f.doubleStrike, inh.doubleStrike);
    if (*f.doubleStrike) SetOnOff(ed, rPr, kStrike, false, inh.strike);
  }
  if (f.caps) SetOnOff(ed, rPr, kCaps, *f.caps, inh.caps);
  if (f.smallCaps) SetOnOff(ed, rPr, kSmallCaps, *f.smallCaps, inh.smallCaps);
  if (f.underline) {
    if (*f.underline == inh.underline) {
      ed.Remove(rPr, kU);
    } else {
      static constexpr std::pair<Underline, const char*> kNames[] = {
          {Underline::None, "none"}, {Underline::Single, "single"}, {Underline::Words, "words"},
          {Underline::Double, "double"}, {Underline::Thick, "thick"}, {Underline::Dotted, "dotted"},
          {Underline::Dash, "dash"}, {Underline::Wave, "wave"}};
      const char* name = "single";
      for (auto [u, n] : kNames)
        if (u == *f.underline) name = n;
      ElemId u = ed.Child(rPr, kU);
      ed.SetAttr(u, kVal, name);
      if (*f.underline == Underline::None)
        for (uint32_t n : {Name("color"), Name("themeColor"), Name("themeTint"), Name("themeShade")}) ed.RemoveAttr(u, n);
    }
  }
  if (f.color) {
    if (*f.color == inh.color) ed.Remove(rPr, kColor);
    else SetColor(ed, ed.Child(rPr, kColor), kVal, *f.color);
  } else if (f.themeColor) {
    // Word writes the color the theme gives now too, for readers without themes.
    const ThemeColorRef& t = *f.themeColor;
    if (const std::optional<uint32_t> rgb = doc.ThemeColor(t.name, t.tint, t.shade)) {
      const ElemId c = ed.Child(rPr, kColor);
      ed.SetAttr(c, kVal, Hex6(*rgb));
      ed.SetAttr(c, Name("themeColor"), t.name);
      auto byte = [](int v) {
        static constexpr char kHex[] = "0123456789ABCDEF";
        return std::string{kHex[(v >> 4) & 15], kHex[v & 15]};
      };
      if (t.tint >= 0 && t.tint <= 255) ed.SetAttr(c, Name("themeTint"), byte(t.tint));
      else ed.RemoveAttr(c, Name("themeTint"));
      if (t.shade >= 0 && t.shade <= 255 && t.tint < 0) ed.SetAttr(c, Name("themeShade"), byte(t.shade));
      else ed.RemoveAttr(c, Name("themeShade"));
    }
  }
  if (f.highlight) {
    bool none = f.highlight->empty() || *f.highlight == "none";
    if (none && !inh.highlight.present) ed.Remove(rPr, kHighlight);
    else SetVal(ed, rPr, kHighlight, none ? std::string("none") : *f.highlight);
  }
  if (f.vertAlign) {
    if (*f.vertAlign == inh.vertAlign) {
      ed.Remove(rPr, kVertAlign);
    } else {
      SetVal(ed, rPr, kVertAlign,
             *f.vertAlign == VertAlign::Superscript ? "superscript" : *f.vertAlign == VertAlign::Subscript ? "subscript" : "baseline");
    }
  }
}

namespace {

// One run's (or paragraph mark's) w:rPr after the format, against what its styles give.
void ApplyRunFormat(TreeEdit& ed, const Document& doc, uint32_t para, ElemId rPr, const RunFormat& f) {
  if (f.charStyle) {
    if (f.charStyle->empty()) ed.Remove(rPr, kRStyle);
    else SetVal(ed, rPr, kRStyle, *f.charStyle);
  }
  WriteRunFormat(ed, rPr, f, doc.StyleRun(para, Node(&doc.GetTree(), rPr).Child(kRStyle).AttrOr(kVal)));
}

// Calls fn(para, rPr) for every run in the range and every paragraph mark it covers.
template <class Fn>
void ForEachRunProps(TreeEdit& ed, Range range, Fn fn) {
  const Document& doc = ed.Doc();
  std::vector<uint32_t> paras = ParagraphsOf(doc, range);
  for (uint32_t p : paras) {
    auto [a, b] = Span(doc, range, p);
    if (a < b) {
      ed.SplitRunAt(p, a);
      ed.SplitRunAt(p, b);
      for (ElemId run : RunsIn(doc, p, a, b)) fn(p, ed.Child(run, kRPr));
    }
    bool emptyHere = range.Empty() && doc.Para(p).text.empty();
    if (p < range.to.para || emptyHere) fn(p, ed.Path(doc.Para(p).elem, {kPPr, kRPr}));
  }
}

// What a run or mark resolves to, for the state: the characters [a, b) of the paragraph.
template <class Fn>
void ForEachResolved(const Document& doc, Range range, Fn fn) {
  std::vector<uint32_t> paras = ParagraphsOf(doc, range);
  if (range.Empty() && !paras.empty()) {
    uint32_t p = paras.front();
    uint32_t off = range.from.offset;
    std::vector<TextRun> runs = doc.Runs(p);
    const TextRun* at = nullptr;
    for (const TextRun& r : runs) {
      if (r.fieldCode) continue;
      if (r.start < off && off <= r.end) at = &r;  // the character before the caret
      if (!at && r.start >= off) {
        at = &r;  // at a paragraph's start: the first character
        break;
      }
    }
    fn(at ? doc.ResolvedRun(p, at->run) : doc.ResolvedMark(p));
    return;
  }
  bool any = false;
  for (uint32_t p : paras) {
    auto [a, b] = Span(doc, range, p);
    for (const TextRun& r : doc.Runs(p)) {
      if (r.fieldCode || r.start >= b || r.end <= a) continue;
      fn(doc.ResolvedRun(p, r.run));
      any = true;
    }
  }
  if (!any && !paras.empty()) fn(doc.ResolvedMark(paras.front()));
}

void SetIndent(TreeEdit& ed, const Document& doc, ElemId pPr, const ParaProps& inh, const ParaFormat& f) {
  ElemId ind = ed.Child(pPr, kInd);
  Node n(&doc.GetTree(), ind);
  bool strict = Strict(doc);
  // A side is written as start/end when the document already uses those names.
  auto side = [&](uint32_t start, uint32_t left) { return strict || n.Has(start) ? start : left; };
  auto setSide = [&](std::optional<Twips> v, Twips inherited, uint32_t start, uint32_t left, uint32_t startChars,
                     uint32_t leftChars) {
    if (!v) return;
    for (uint32_t c : {startChars, leftChars}) ed.RemoveAttr(ind, c);
    uint32_t name = side(start, left);
    ed.RemoveAttr(ind, name == start ? left : start);
    if (*v == inherited) ed.RemoveAttr(ind, name);
    else ed.SetAttr(ind, name, Num(v->Value()));
  };
  setSide(f.indStart, inh.indStart, Name("start"), Name("left"), Name("startChars"), Name("leftChars"));
  setSide(f.indEnd, inh.indEnd, Name("end"), Name("right"), Name("endChars"), Name("rightChars"));
  if (f.firstLine) {
    for (uint32_t c : {Name("firstLine"), Name("hanging"), Name("firstLineChars"), Name("hangingChars")}) ed.RemoveAttr(ind, c);
    if (*f.firstLine != inh.firstLine) {
      if (f.firstLine->Value() >= 0) ed.SetAttr(ind, Name("firstLine"), Num(f.firstLine->Value()));
      else ed.SetAttr(ind, Name("hanging"), Num(-f.firstLine->Value()));
    }
  }
  if (Node(&doc.GetTree(), ind).AttrCount() == 0) ed.Remove(pPr, kInd);
}

void SetSpacing(TreeEdit& ed, const Document& doc, ElemId pPr, const ParaProps& inh, const ParaFormat& f) {
  ElemId sp = ed.Child(pPr, kSpacing);
  auto setEdge = [&](std::optional<Twips> v, Twips inherited, bool inheritedAuto, uint32_t name, uint32_t lines, uint32_t autoName) {
    if (!v) return;
    ed.RemoveAttr(sp, lines);
    ed.RemoveAttr(sp, autoName);
    if (*v == inherited && !inheritedAuto) ed.RemoveAttr(sp, name);
    else ed.SetAttr(sp, name, Num(v->Value()));
  };
  setEdge(f.spaceBefore, inh.spaceBefore, inh.spaceBeforeAuto, Name("before"), Name("beforeLines"), Name("beforeAutospacing"));
  setEdge(f.spaceAfter, inh.spaceAfter, inh.spaceAfterAuto, Name("after"), Name("afterLines"), Name("afterAutospacing"));
  if (f.line) {
    LineRule rule = f.lineRule.value_or(LineRule::Auto);
    if (*f.line == inh.line && rule == inh.lineRule) {
      ed.RemoveAttr(sp, Name("line"));
      ed.RemoveAttr(sp, Name("lineRule"));
    } else {
      ed.SetAttr(sp, Name("line"), Num(*f.line));
      ed.SetAttr(sp, Name("lineRule"), rule == LineRule::Exact ? "exact" : rule == LineRule::AtLeast ? "atLeast" : "auto");
    }
  }
  if (Node(&doc.GetTree(), sp).AttrCount() == 0) ed.Remove(pPr, kSpacing);
}

void SetBorders(TreeEdit& ed, ElemId pPr, BorderSides sides) {
  ed.Remove(pPr, kPBdr);
  if (sides == BorderSides::None) return;
  ElemId bdr = ed.Child(pPr, kPBdr);
  auto line = [&](uint32_t name) {
    ElemId e = ed.Child(bdr, name);
    ed.SetAttr(e, kVal, "single");
    ed.SetAttr(e, Name("sz"), "4");
    ed.SetAttr(e, Name("space"), "1");
    ed.SetAttr(e, Name("color"), "auto");
  };
  switch (sides) {
    case BorderSides::Top: line(Name("top")); break;
    case BorderSides::Bottom: line(Name("bottom")); break;
    case BorderSides::Left: line(Name("left")); break;
    case BorderSides::Right: line(Name("right")); break;
    case BorderSides::All: line(Name("between")); [[fallthrough]];
    case BorderSides::Outside:
      for (uint32_t n : {Name("top"), Name("left"), Name("bottom"), Name("right")}) line(n);
      break;
    case BorderSides::None: break;
  }
}

}  // namespace

bool RunFormat::Any() const {
  return font || themeFont || size || bold || italic || strike || doubleStrike || caps || smallCaps || underline || color ||
         themeColor || highlight || vertAlign || charStyle;
}

bool ParaFormat::Any() const {
  return jc || indStart || indEnd || firstLine || spaceBefore || spaceAfter || line || keepNext || keepLines ||
         pageBreakBefore || widowControl || contextualSpacing || borders || shading;
}

std::string_view JcValue(const Document& doc, Justification jc) {
  bool strict = Strict(doc);
  switch (jc) {
    case Justification::Start: return strict ? "start" : "left";
    case Justification::End: return strict ? "end" : "right";
    case Justification::Center: return "center";
    case Justification::Both: return "both";
    case Justification::Distribute: return "distribute";
    default: return "left";
  }
}

std::vector<uint32_t> ParagraphsOf(const Document& doc, Range r) {
  std::vector<uint32_t> out;
  if (r.from.para == kNone || r.from.para >= doc.Paragraphs().size()) return out;
  uint32_t story = doc.Para(r.from.para).story;
  uint32_t last = std::min<uint32_t>(r.to.para, static_cast<uint32_t>(doc.Paragraphs().size()) - 1);
  if (last > r.from.para && r.to.offset == 0) --last;
  for (uint32_t p = r.from.para; p <= last; ++p)
    if (doc.Para(p).story == story && !doc.ParaNode(p).DisplayOnly()) out.push_back(p);
  return out;
}

FormatState StateOf(const Document& doc, Range range) {
  FormatState s;
  bool first = true;
  bool bold = true, italic = true, strike = true, sup = true, sub = true, underline = true;
  bool fontMixed = false, sizeMixed = false, colorMixed = false, highlightMixed = false, charStyleMixed = false;
  ForEachResolved(doc, range, [&](const RunProps& p) {
    bold &= p.bold;
    italic &= p.italic;
    strike &= p.strike;
    sup &= p.vertAlign == VertAlign::Superscript;
    sub &= p.vertAlign == VertAlign::Subscript;
    underline &= p.underline != Underline::None;
    if (first) {
      s.font = std::string(p.fonts.ascii);
      s.size = p.size;
      s.color = p.color;
      s.highlight = p.highlight;
      s.charStyle = std::string(p.charStyle);
      s.lang = std::string(p.lang);
      first = false;
      return;
    }
    fontMixed |= s.font && *s.font != p.fonts.ascii;
    sizeMixed |= s.size && *s.size != p.size;
    colorMixed |= s.color && !(*s.color == p.color);
    highlightMixed |= s.highlight && !(*s.highlight == p.highlight);
    charStyleMixed |= s.charStyle && *s.charStyle != p.charStyle;
  });
  if (first) return s;
  s.bold = bold;
  s.italic = italic;
  s.strike = strike;
  s.superscript = sup;
  s.subscript = sub;
  s.underline = underline;
  if (fontMixed) s.font.reset();
  if (sizeMixed) s.size.reset();
  if (colorMixed) s.color.reset();
  if (highlightMixed) s.highlight.reset();
  if (charStyleMixed) s.charStyle.reset();

  bool firstPara = true, bullets = true, numbered = true;
  for (uint32_t p : ParagraphsOf(doc, range)) {
    std::optional<ListLabel> label = doc.Label(p);  // first: it resolves other paragraphs too
    const ParaProps& pp = doc.ResolvedPara(p);
    bullets &= label && label->bullet;
    numbered &= label && !label->bullet;
    if (firstPara) {
      s.jc = pp.jc;
      s.paraStyle = std::string(pp.style);
      s.lineRule = pp.lineRule;
      s.line = pp.line;
      if (label) s.level = label->level;
      firstPara = false;
      continue;
    }
    if (s.jc && *s.jc != pp.jc) s.jc.reset();
    if (s.paraStyle && *s.paraStyle != pp.style) s.paraStyle.reset();
    if (s.line && (*s.line != pp.line || s.lineRule != pp.lineRule)) {
      s.line.reset();
      s.lineRule.reset();
    }
    if (s.level && (!label || label->level != *s.level)) s.level.reset();
  }
  s.bullets = !firstPara && bullets;
  s.numbered = !firstPara && numbered;
  return s;
}

void FormatRuns(TreeEdit& ed, Range range, const RunFormat& format) {
  if (!format.Any()) return;
  ed.Begin(u"Format");
  ed.Local();
  ForEachRunProps(ed, range, [&](uint32_t para, ElemId rPr) { ApplyRunFormat(ed, ed.Doc(), para, rPr, format); });
  ed.Commit();
}

void FormatParagraphs(TreeEdit& ed, Range range, const ParaFormat& f) {
  if (!f.Any()) return;
  const Document& doc = ed.Doc();
  ed.Begin(u"Paragraph");
  ed.Local();
  for (uint32_t p : ParagraphsOf(doc, range)) {
    WriteParaFormat(ed, ed.Child(doc.Para(p).elem, kPPr), f, doc.StylePara(p));
  }
  ed.Commit();
}

void WriteParaFormat(TreeEdit& ed, ElemId pPr, const ParaFormat& f, const ParaProps& inh) {
  const Document& doc = ed.Doc();
  {
    if (f.jc) {
      if (*f.jc == inh.jc) ed.Remove(pPr, kJc);
      else SetVal(ed, pPr, kJc, JcValue(doc, *f.jc));
    }
    if (f.indStart || f.indEnd || f.firstLine) SetIndent(ed, doc, pPr, inh, f);
    if (f.spaceBefore || f.spaceAfter || f.line) SetSpacing(ed, doc, pPr, inh, f);
    if (f.keepNext) SetOnOff(ed, pPr, Name("keepNext"), *f.keepNext, inh.keepNext);
    if (f.keepLines) SetOnOff(ed, pPr, Name("keepLines"), *f.keepLines, inh.keepLines);
    if (f.pageBreakBefore) SetOnOff(ed, pPr, Name("pageBreakBefore"), *f.pageBreakBefore, inh.pageBreakBefore);
    if (f.widowControl) SetOnOff(ed, pPr, Name("widowControl"), *f.widowControl, inh.widowControl);
    if (f.contextualSpacing) SetOnOff(ed, pPr, Name("contextualSpacing"), *f.contextualSpacing, inh.contextualSpacing);
    if (f.borders) SetBorders(ed, pPr, *f.borders);
    if (f.shading) {
      if (f.shading->automatic) {
        ed.Remove(pPr, kShd);
      } else {
        ElemId shd = ed.Child(pPr, kShd);
        ed.SetAttr(shd, kVal, "clear");
        ed.SetAttr(shd, Name("color"), "auto");
        SetColor(ed, shd, Name("fill"), *f.shading);
        for (uint32_t n : {Name("themeFill"), Name("themeFillTint"), Name("themeFillShade")}) ed.RemoveAttr(shd, n);
      }
    }
  }
}

void ToggleRuns(TreeEdit& ed, Range range, Toggle what) {
  FormatState s = StateOf(ed.Doc(), range);
  RunFormat f;
  switch (what) {
    case Toggle::Bold: f.bold = !s.bold; break;
    case Toggle::Italic: f.italic = !s.italic; break;
    case Toggle::Underline: f.underline = s.underline ? Underline::None : Underline::Single; break;
    case Toggle::Strike: f.strike = !s.strike; break;
    case Toggle::Superscript: f.vertAlign = s.superscript ? VertAlign::Baseline : VertAlign::Superscript; break;
    case Toggle::Subscript: f.vertAlign = s.subscript ? VertAlign::Baseline : VertAlign::Subscript; break;
  }
  static constexpr const char16_t* kLabels[] = {u"Bold", u"Italic", u"Underline", u"Strikethrough", u"Superscript", u"Subscript"};
  ed.Begin(kLabels[static_cast<int>(what)]);
  ed.Local();
  FormatRuns(ed, range, f);
  ed.Commit();
}

HalfPoints NextFontSize(HalfPoints size, bool grow) {
  static constexpr int kSizes[] = {8, 9, 10, 11, 12, 14, 16, 18, 20, 22, 24, 26, 28, 36, 48, 72};  // points
  int half = size.Value();
  if (grow) {
    if (half < 16) return HalfPoints(half + 2 - half % 2);  // by a point below 8
    for (int pt : kSizes)
      if (pt * 2 > half) return HalfPoints(pt * 2);
    return HalfPoints(std::min(1638 * 2, (half / 20 + 1) * 20));  // by 10 points above 72
  }
  if (half <= 16) return HalfPoints(std::max(2, half % 2 ? half - 1 : half - 2));
  if (half > 144) return HalfPoints(std::max(144, half % 20 ? half / 20 * 20 : half - 20));
  for (auto it = std::rbegin(kSizes); it != std::rend(kSizes); ++it)
    if (*it * 2 < half) return HalfPoints(*it * 2);
  return HalfPoints(16);
}

void GrowFont(TreeEdit& ed, Range range, bool grow, bool onePoint) {
  ed.Begin(grow ? u"Grow font" : u"Shrink font");
  ed.Local();
  const Document& doc = ed.Doc();
  ForEachRunProps(ed, range, [&](uint32_t para, ElemId rPr) {
    // The run's size as it stands: its own w:sz, or what its styles give.
    Node n(&doc.GetTree(), rPr);
    HalfPoints size = doc.StyleRun(para, n.Child(kRStyle).AttrOr(kVal)).size;
    if (auto sz = n.Child(kSz).Int(kVal)) size = HalfPoints(static_cast<int32_t>(*sz));
    RunFormat f;
    f.size = onePoint ? HalfPoints(std::clamp(size.Value() + (grow ? 2 : -2), 2, 3276)) : NextFontSize(size, grow);
    ApplyRunFormat(ed, doc, para, rPr, f);
  });
  ed.Commit();
}

void ClearRunFormatting(TreeEdit& ed, Range range) {
  ed.Begin(u"Clear formatting");
  ed.Local();
  ForEachRunProps(ed, range, [&](uint32_t, ElemId rPr) { ed.Clear(rPr, {kRStyle, kRPrChange}); });
  ed.Commit();
}

void ClearParaFormatting(TreeEdit& ed, Range range) {
  const Document& doc = ed.Doc();
  ed.Begin(u"Reset paragraph");
  ed.Local();
  for (uint32_t p : ParagraphsOf(doc, range)) {
    if (ElemId pPr = ed.Child(doc.Para(p).elem, kPPr, false); pPr != kNoElem)
      ed.Clear(pPr, {kPStyle, kNumPr, kRPr, kSectPr, kPPrChange});
  }
  ed.Commit();
}

void ClearAllFormatting(TreeEdit& ed, Range range) {
  const Document& doc = ed.Doc();
  ed.Begin(u"Clear all formatting");
  ed.Local(true);  // its style and list go
  ForEachRunProps(ed, range, [&](uint32_t, ElemId rPr) { ed.Clear(rPr, {kRPrChange}); });
  for (uint32_t p : ParagraphsOf(doc, range)) {
    if (ElemId pPr = ed.Child(doc.Para(p).elem, kPPr, false); pPr != kNoElem) ed.Clear(pPr, {kRPr, kSectPr, kPPrChange});
  }
  ed.Commit();
}

void ShiftIndent(TreeEdit& ed, Range range, bool increase) {
  const Document& doc = ed.Doc();
  ed.Begin(increase ? u"Increase indent" : u"Decrease indent");
  ed.Local(true);  // a list item's level renumbers the items after it
  const int32_t step = std::max<int32_t>(1, doc.Settings().defaultTabStop.Value());
  for (uint32_t p : ParagraphsOf(doc, range)) {
    const ParaProps& pp = doc.ResolvedPara(p);
    if (!pp.numId.empty() && pp.numId != "0") {
      int32_t level = std::clamp(pp.ilvl + (increase ? 1 : -1), 0, 8);
      ElemId ilvl = ed.Path(doc.Para(p).elem, {kPPr, kNumPr, kIlvl});
      ed.SetAttr(ilvl, kVal, Num(level));
      continue;
    }
    int32_t at = pp.indStart.Value();
    int32_t to = increase ? (at / step + 1) * step : std::max(0, (at % step ? at / step : at / step - 1) * step);
    ParaFormat f;
    f.indStart = Twips(to);
    FormatParagraphs(ed, Range{{p, 0}, {p, 0}}, f);
  }
  ed.Commit();
}

void ChangeTabStops(TreeEdit& ed, Range range, const std::vector<Twips>& remove, const std::vector<TabStop>& add) {
  if (remove.empty() && add.empty()) return;
  static constexpr uint32_t kTabs = Name("tabs");
  const Document& doc = ed.Doc();
  const bool strict = Strict(doc);
  auto at = [](const std::vector<TabStop>& v, Twips pos) {
    return std::find_if(v.begin(), v.end(), [&](const TabStop& t) { return t.pos == pos; });
  };
  auto byPos = [](const TabStop& a, const TabStop& b) { return a.pos < b.pos; };
  ed.Begin(u"Tabs");
  ed.Local();
  for (uint32_t p : ParagraphsOf(doc, range)) {
    std::vector<TabStop> had = doc.ResolvedPara(p).tabs, want = had;
    want.erase(std::remove_if(want.begin(), want.end(),
                              [&](const TabStop& t) {
                                return std::find(remove.begin(), remove.end(), t.pos) != remove.end() ||
                                       at(add, t.pos) != add.end();
                              }),
               want.end());
    want.insert(want.end(), add.begin(), add.end());
    // A paragraph whose stops stay as they were isn't written again.
    std::stable_sort(had.begin(), had.end(), byPos);
    std::stable_sort(want.begin(), want.end(), byPos);
    if (want == had) continue;
    // What the paragraph writes: the style's stops it doesn't have cleared, its own as they are.
    const std::vector<TabStop> inherited = doc.StylePara(p).tabs;
    std::vector<TabStop> own;
    for (const TabStop& t : inherited)
      if (at(want, t.pos) == want.end()) own.push_back({t.pos, TabAlign::Clear, TabLeader::None});
    for (const TabStop& t : want)
      if (auto i = at(inherited, t.pos); i == inherited.end() || !(*i == t)) own.push_back(t);
    std::stable_sort(own.begin(), own.end(), byPos);
    const ElemId para = doc.Para(p).elem;
    if (own.empty()) {
      if (doc.NodeOf(para).Path({kPPr, kTabs})) ed.Remove(ed.Child(para, kPPr), kTabs);
      continue;
    }
    const ElemId tabs = ed.Path(para, {kPPr, kTabs});
    ed.Clear(tabs);
    for (const TabStop& t : own) {
      static constexpr const char* kLeaders[] = {"none", "dot", "hyphen", "underscore", "heavy", "middleDot"};
      const char* val = t.align == TabAlign::Center    ? "center"
                        : t.align == TabAlign::End     ? (strict ? "end" : "right")
                        : t.align == TabAlign::Decimal ? "decimal"
                        : t.align == TabAlign::Bar     ? "bar"
                        : t.align == TabAlign::Num     ? "num"
                        : t.align == TabAlign::Clear   ? "clear"
                        : strict                       ? "start"
                                                       : "left";
      std::string xml = std::string("<w:tab w:val=\"") + val + "\"";
      if (t.leader != TabLeader::None) xml += std::string(" w:leader=\"") + kLeaders[static_cast<int>(t.leader)] + "\"";
      xml += " w:pos=\"" + Num(t.pos.Value()) + "\"/>";
      ed.AddXml(tabs, xml);
    }
  }
  ed.Commit();
}

}  // namespace fw::doc::edit
