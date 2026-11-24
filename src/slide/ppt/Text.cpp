// Text in a .ppt ([MS-PPT] 2.9): the characters and their paragraph and character runs, read from the
// records and written as DrawingML paragraphs with every property resolved (the document's
// defaults, the master's style for the text type and level, the ruler, then the runs), so a slide
// shows as PowerPoint 97-2003 drew it whatever the .pptx's own inheritance would do.
#include <algorithm>
#include <cstdio>
#include <string>

#include "slide/ppt/Internal.h"

namespace fw::slide::ppt::detail {
namespace {

std::string Num(long long v) { return std::to_string(v); }
long long Emu(double units) { return static_cast<long long>(units * kEmuPerUnit + (units < 0 ? -0.5 : 0.5)); }

void AppendUtf8(std::string& out, uint32_t c) {
  if (c < 0x80) {
    out += static_cast<char>(c);
  } else if (c < 0x800) {
    out += static_cast<char>(0xC0 | c >> 6), out += static_cast<char>(0x80 | (c & 0x3F));
  } else if (c < 0x10000) {
    out += static_cast<char>(0xE0 | c >> 12), out += static_cast<char>(0x80 | (c >> 6 & 0x3F)), out += static_cast<char>(0x80 | (c & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | c >> 18), out += static_cast<char>(0x80 | (c >> 12 & 0x3F));
    out += static_cast<char>(0x80 | (c >> 6 & 0x3F)), out += static_cast<char>(0x80 | (c & 0x3F));
  }
}

// The base type whose master style a text type adds to: centered title on title, the body kinds on body.
int BaseType(int type) {
  switch (type) {
    case kTitle: case kCenterTitle: return kTitle;
    case kBody: case kCenterBody: case kHalfBody: case kQuarterBody: return kBody;
    case kNotes: return kNotes;
    default: return kOther;
  }
}

// The paragraph and character properties a text type has at `level` before its runs.
void Layered(const Context& cx, int type, int level, Pf& pf, Cf& cf, bool placeholder = true) {
  pf = cx.basePf;
  cf = cx.baseCf;
  const auto layer = [&](const std::optional<MasterStyle>& style) {
    if (!style) return;
    for (int l = 0; l <= level && l < 5; ++l)
      if (const auto& lv = style->levels[l]) pf.Over(lv->first), cf.Over(lv->second);
  };
  const int base = BaseType(type);
  if (base == kOther && !placeholder && cx.docOther) layer(cx.docOther);
  else layer(cx.styles[base]);
  if (type != base && type >= 0 && type <= 8) layer(cx.styles[type]);
}

uint32_t ColorOf(uint32_t c, const Context& cx, uint32_t fallback) {
  const uint8_t index = static_cast<uint8_t>(c >> 24);
  if (index == 0xFE) return (c & 0xFF) << 16 | (c & 0xFF00) | (c >> 16 & 0xFF);
  if (index < 8) return cx.scheme.rgb[index];
  return fallback;
}

std::string FontName(const Context& cx, uint16_t ref) {
  if (!cx.fonts || ref >= cx.fonts->size()) return {};
  return (*cx.fonts)[ref];
}

std::string Spacing(const char* tag, int16_t v) {
  // Percent of a line when positive, master units (1/8 point) when negative.
  if (v >= 0) return std::string("<a:") + tag + "><a:spcPct val=\"" + Num(v * 1000LL) + "\"/></a:" + tag + ">";
  return std::string("<a:") + tag + "><a:spcPts val=\"" + Num(-v * 100LL / 8) + "\"/></a:" + tag + ">";
}

// a:pPr's attributes and children (without the element itself): `attrs` and `inner`.
// TextAutoNumberSchemeEnum in order, as DrawingML names the schemes.
constexpr const char* kAutoNumber[] = {
    "alphaLcPeriod", "alphaUcPeriod", "arabicParenR", "arabicPeriod", "romanLcParenBoth", "romanLcParenR", "romanLcPeriod",
    "romanUcPeriod", "alphaLcParenBoth", "alphaLcParenR", "alphaUcParenBoth", "alphaUcParenR", "arabicParenBoth",
    "arabicPlain", "romanUcParenBoth", "romanUcParenR", "ea1ChsPlain", "ea1ChsPeriod", "circleNumDbPlain",
    "circleNumWdWhitePlain", "circleNumWdBlackPlain", "ea1ChtPlain", "ea1ChtPeriod", "arabic1Minus", "arabic2Minus",
    "hebrew2Minus", "ea1JpnKorPlain", "ea1JpnKorPeriod", "arabicDbPlain", "arabicDbPeriod", "thaiAlphaPeriod",
    "thaiAlphaParenR", "thaiAlphaParenBoth", "thaiNumPeriod", "thaiNumParenR", "thaiNumParenBoth", "hindiAlphaPeriod",
    "hindiNumPeriod", "ea1JpnChsDbPeriod", "hindiNumParenR", "hindiAlpha1Period"};

void ParaProps(const Pf& pf, const Cf& cf, const Context& cx, int level, std::string& attrs, std::string& inner,
               const Pf9* pf9 = nullptr) {
  if (level > 0) attrs += " lvl=\"" + Num(level) + "\"";
  if (pf.Has(kLeftMargin)) attrs += " marL=\"" + Num(Emu(pf.leftMargin)) + "\"";
  if (pf.Has(kLeftMargin) || pf.Has(kIndent)) attrs += " indent=\"" + Num(Emu(pf.indent - (pf.Has(kLeftMargin) ? pf.leftMargin : 0))) + "\"";
  if (pf.Has(kAlign)) {
    static const char* const kAlgn[] = {"l", "ctr", "r", "just", "dist", "thaiDist", "justLow"};
    if (pf.align < 7) attrs += std::string(" algn=\"") + kAlgn[pf.align] + "\"";
  }
  if (pf.Has(kDefaultTab) && pf.defaultTab > 0) attrs += " defTabSz=\"" + Num(Emu(pf.defaultTab)) + "\"";
  if (pf.Has(kTextDirection) && pf.textDirection == 1) attrs += " rtl=\"1\"";  // right to left
  if (pf.Has(kLineSpacing)) inner += Spacing("lnSpc", pf.lineSpacing);
  if (pf.Has(kSpaceBefore)) inner += Spacing("spcBef", pf.spaceBefore);
  if (pf.Has(kSpaceAfter)) inner += Spacing("spcAft", pf.spaceAfter);
  if (pf.bulletFlags & 1) {
    if (pf.bulletFlags & 4) inner += "<a:buClr>" + ColorXml(ColorOf(pf.bulletColor, cx, ColorOf(cf.color, cx, cx.scheme.rgb[1]))) + "</a:buClr>";
    // The bullet's size: a share of the text's (25 to 400 %), or below zero its own, in hundredths of a point.
    if ((pf.bulletFlags & 8) && pf.bulletSize > 0) inner += "<a:buSzPct val=\"" + Num(std::clamp<int>(pf.bulletSize, 25, 400) * 1000LL) + "\"/>";
    else if ((pf.bulletFlags & 8) && pf.bulletSize < 0)  // points
      inner += "<a:buSzPts val=\"" + Num(std::clamp<int>(-pf.bulletSize, 1, 4000) * 100LL) + "\"/>";
    if (pf9 && pf9->autoNumber == 1) {  // a number, in the text's font
      const int scheme = pf9->scheme >= 0 && pf9->scheme < static_cast<int>(std::size(kAutoNumber)) ? pf9->scheme : 3;
      inner += std::string("<a:buAutoNum type=\"") + kAutoNumber[scheme] + "\"";
      if (pf9->start > 1 && pf9->start <= 32767) inner += " startAt=\"" + Num(pf9->start) + "\"";
      inner += "/>";
    } else {
      std::string font = pf.bulletFlags & 2 ? FontName(cx, pf.bulletFont) : std::string();
      if (!font.empty()) inner += "<a:buFont typeface=\"" + Esc(font) + "\"/>";
      uint32_t ch = pf.Has(kBulletChar) && pf.bulletChar ? pf.bulletChar : 0x2022;
      if (ch >= 0xF000 && ch <= 0xF0FF) ch -= 0xF000;  // a symbol font's character, as DrawingML names it
      if (ch < 0x20 || (ch >= 0xD800 && ch < 0xE000) || ch >= 0xFFFE) ch = 0x2022;
      std::string c;
      AppendUtf8(c, ch);
      inner += "<a:buChar char=\"" + Esc(c) + "\"/>";
    }
  } else {
    inner += "<a:buNone/>";
  }
  if (!pf.tabs.empty()) {
    inner += "<a:tabLst>";
    static const char* const kTab[] = {"l", "ctr", "r", "dec"};
    for (const auto& [pos, type] : pf.tabs)
      inner += "<a:tab pos=\"" + Num(Emu(pos)) + "\" algn=\"" + kTab[type & 3] + "\"/>";
    inner += "</a:tabLst>";
  }
}

// a:rPr (or a:defRPr / a:endParaRPr: `tag`) for character properties.
std::string RunProps(const char* tag, const Cf& cf, const Context& cx, const std::string& link = {}) {
  std::string s = std::string("<a:") + tag + " lang=\"en-US\"";
  if (cf.Has(kSize) && cf.size > 0) s += " sz=\"" + Num(std::clamp<int>(cf.size, 1, 4000) * 100LL) + "\"";
  s += cf.style & 1 ? " b=\"1\"" : " b=\"0\"";
  s += cf.style & 2 ? " i=\"1\"" : " i=\"0\"";
  if (cf.style & 4) s += " u=\"sng\"";
  if (cf.Has(kPosition) && cf.position) s += " baseline=\"" + Num(std::clamp<int>(cf.position, -100, 100) * 1000LL) + "\"";
  s += " dirty=\"0\">";
  s += "<a:solidFill>" + ColorXml(ColorOf(cf.color, cx, cx.scheme.rgb[1])) + "</a:solidFill>";
  if (cf.style & 0x10)
    s += "<a:effectLst><a:outerShdw dist=\"25400\" dir=\"2700000\" algn=\"tl\" rotWithShape=\"0\">" + ColorXml(cx.scheme.rgb[2]) +
         "</a:outerShdw></a:effectLst>";
  const std::string latin = FontName(cx, cf.font);
  if (!latin.empty()) s += "<a:latin typeface=\"" + Esc(latin) + "\"/>";
  const std::string ea = cf.Has(kEaTypeface) ? FontName(cx, cf.eaFont) : std::string();
  if (!ea.empty()) s += "<a:ea typeface=\"" + Esc(ea) + "\"/>";
  if (!latin.empty()) s += "<a:cs typeface=\"" + Esc(latin) + "\"/>";
  const std::string sym = cf.Has(kSymbolTypeface) ? FontName(cx, cf.symbolFont) : std::string();
  if (!sym.empty()) s += "<a:sym typeface=\"" + Esc(sym) + "\"/>";
  // A link is a relationship id, or "\x01" and a ppaction (a jump inside the show).
  if (!link.empty() && link[0] == '\x01') s += "<a:hlinkClick r:id=\"\" action=\"" + Esc(std::string_view(link).substr(1)) + "\"/>";
  else if (!link.empty()) s += "<a:hlinkClick r:id=\"" + link + "\"/>";
  s += std::string("</a:") + tag + ">";
  return s;
}

}  // namespace

void Pf::Over(const Pf& o) {
  for (int i = 0; i < 4; ++i)
    if (o.Has(i)) bulletFlags = static_cast<uint16_t>((bulletFlags & ~(1u << i)) | (o.bulletFlags & (1u << i)));
  if (o.Has(kBulletChar)) bulletChar = o.bulletChar;
  if (o.Has(kBulletFont)) bulletFont = o.bulletFont;
  if (o.Has(kBulletSize)) bulletSize = o.bulletSize;
  if (o.Has(kBulletColor)) bulletColor = o.bulletColor;
  if (o.Has(kAlign)) align = o.align;
  if (o.Has(kLineSpacing)) lineSpacing = o.lineSpacing;
  if (o.Has(kSpaceBefore)) spaceBefore = o.spaceBefore;
  if (o.Has(kSpaceAfter)) spaceAfter = o.spaceAfter;
  if (o.Has(kLeftMargin)) leftMargin = o.leftMargin;
  if (o.Has(kIndent)) indent = o.indent;
  if (o.Has(kDefaultTab)) defaultTab = o.defaultTab;
  if (o.Has(kTabStops)) tabs = o.tabs;
  if (o.Has(kFontAlign)) fontAlign = o.fontAlign;
  if (o.mask & (7u << kCharWrap)) wrapFlags = o.wrapFlags;
  if (o.Has(kTextDirection)) textDirection = o.textDirection;
  mask |= o.mask;
}

void Cf::Over(const Cf& o) {
  const uint16_t bits = static_cast<uint16_t>(o.mask & 0x3FFF);
  style = static_cast<uint16_t>((style & ~bits) | (o.style & bits));
  if (o.Has(kTypeface)) font = o.font;
  if (o.Has(kEaTypeface)) eaFont = o.eaFont;
  if (o.Has(kAnsiTypeface)) ansiFont = o.ansiFont;
  if (o.Has(kSymbolTypeface)) symbolFont = o.symbolFont;
  if (o.Has(kSize)) size = o.size;
  if (o.Has(kColor)) color = o.color;
  if (o.Has(kPosition)) position = o.position;
  mask |= o.mask;
}

bool ReadPf(Bytes& b, Pf& pf) {
  pf = {};
  pf.mask = b.U32();
  if (pf.mask & 0xF) pf.bulletFlags = b.U16();
  if (pf.Has(kBulletChar)) pf.bulletChar = b.U16();
  if (pf.Has(kBulletFont)) pf.bulletFont = b.U16();
  if (pf.Has(kBulletSize)) pf.bulletSize = b.I16();
  if (pf.Has(kBulletColor)) pf.bulletColor = b.U32();
  if (pf.Has(kAlign)) pf.align = b.U16();
  if (pf.Has(kLineSpacing)) pf.lineSpacing = b.I16();
  if (pf.Has(kSpaceBefore)) pf.spaceBefore = b.I16();
  if (pf.Has(kSpaceAfter)) pf.spaceAfter = b.I16();
  if (pf.Has(kLeftMargin)) pf.leftMargin = b.I16();
  if (pf.Has(kIndent)) pf.indent = b.I16();
  if (pf.Has(kDefaultTab)) pf.defaultTab = b.I16();
  if (pf.Has(kTabStops)) {
    const uint16_t n = b.U16();
    for (uint16_t i = 0; i < n && b.Ok(); ++i) {
      const int16_t pos = b.I16();
      const uint16_t type = b.U16();
      if (b.Ok() && pf.tabs.size() < 64) pf.tabs.emplace_back(pos, type);
    }
  }
  if (pf.Has(kFontAlign)) pf.fontAlign = b.U16();
  if (pf.mask & (7u << kCharWrap)) pf.wrapFlags = b.U16();
  if (pf.Has(kTextDirection)) pf.textDirection = b.U16();
  if (!b.Ok()) pf = {};
  return b.Ok();
}

bool ReadCf(Bytes& b, Cf& cf) {
  cf = {};
  cf.mask = b.U32();
  if (cf.mask & 0x3EB7) cf.style = b.U16();  // bold .. emboss, pp9rt
  if (cf.Has(kTypeface)) cf.font = b.U16();
  if (cf.Has(kEaTypeface)) cf.eaFont = b.U16();
  if (cf.Has(kAnsiTypeface)) cf.ansiFont = b.U16();
  if (cf.Has(kSymbolTypeface)) cf.symbolFont = b.U16();
  if (cf.Has(kSize)) cf.size = b.I16();
  if (cf.Has(kColor)) cf.color = b.U32();
  if (cf.Has(kPosition)) cf.position = b.I16();
  if (!b.Ok()) cf = {};
  return b.Ok();
}

bool ReadMasterStyle(const Rec& r, MasterStyle& out) {
  out = {};
  Bytes b(r.body);
  const uint16_t n = b.U16();
  for (uint16_t i = 0; i < n && i < 10 && b.Ok(); ++i) {
    const uint16_t level = r.inst >= 5 ? b.U16() : i;
    Pf pf;
    Cf cf;
    if (!ReadPf(b, pf) || !ReadCf(b, cf)) break;
    if (level < 5) out.levels[level] = std::make_pair(pf, cf);
  }
  return true;
}

std::span<const uint8_t> Pp9Tag(std::span<const uint8_t> progTags) {
  Records rs(progTags);
  for (Rec r; rs.Next(r);) {
    if (r.type != 0x138A) continue;  // ProgBinaryTag: its name, then its data
    Records tag(r.body);
    Rec name, data;
    if (!tag.Next(name) || name.type != 0x0FBA || !tag.Next(data) || data.type != 0x138B) continue;
    static constexpr char16_t kName[] = u"___PPT9";
    if (name.body.size() != 14) continue;
    bool same = true;
    for (size_t i = 0; i < 7 && same; ++i) same = (name.body[2 * i] | name.body[2 * i + 1] << 8) == kName[i];
    if (same) return data.body;
  }
  return {};
}

void ReadStyle9(std::span<const uint8_t> atom, std::vector<Pf9>& out) {
  out.clear();
  Bytes b(atom);  // PF9, CF9 and SI exceptions, in turn
  while (b.Left() >= 12 && out.size() < 16) {
    Pf9 p;
    const uint32_t pf = b.U32();
    if (pf & 0x00800000) b.I16();                     // bulletBlipRef
    if (pf & 0x02000000) p.autoNumber = b.U16() != 0;  // fBulletHasAutoNumber
    if (pf & 0x01000000) p.scheme = b.U16(), p.start = b.I16();
    if (b.U32() & 0x00100000) b.U32();  // CF9: its masks, and with pp10ext the pp10 run id
    const uint32_t si = b.U32();
    if (si & 0x001) b.U16();  // spellInfo
    if (si & 0x002) b.U16();  // lid
    if (si & 0x004) b.U16();  // altLid
    if (si & 0x040) b.U16();  // bidi
    if (si & 0x020) b.U32();  // pp10runid, grammarError
    if (si & 0x200) {         // smartTags
      const uint32_t n = b.U32();
      if (n > 4096) break;
      b.Skip(4 * n);
    }
    if (!b.Ok()) break;
    out.push_back(p);
  }
}

void ReadText(std::span<const uint8_t> records, Text& out, const std::function<std::string(uint32_t)>& link) {
  Records rs(records);
  std::string pendingLink;
  for (Rec r; rs.Next(r);) {
    Bytes b(r.body);
    switch (r.type) {
      case 0x0F9F: out.type = static_cast<int>(b.U32()); break;  // TextHeaderAtom
      case 0x0FA0:                                                 // TextCharsAtom
        out.chars.clear();
        for (size_t i = 0; i + 1 < r.body.size() && out.chars.size() < (1u << 20); i += 2)
          out.chars += static_cast<char16_t>(r.body[i] | r.body[i + 1] << 8);
        break;
      case 0x0FA8:  // TextBytesAtom: the low bytes of UTF-16
        out.chars.assign(r.body.begin(), r.body.begin() + std::min<size_t>(r.body.size(), 1u << 20));
        break;
      case 0x0FA1: {  // StyleTextPropAtom: paragraph runs, then character runs, over the text and one more
        out.pfRuns.clear();
        out.cfRuns.clear();
        const uint64_t total = out.chars.size() + 1;
        uint64_t covered = 0;
        while (covered < total && b.Ok() && out.pfRuns.size() < 100000) {
          PfRun run;
          run.count = b.U32();
          run.level = b.U16();
          if (!b.Ok() || run.count == 0 || !ReadPf(b, run.pf)) break;
          covered += run.count;
          out.pfRuns.push_back(std::move(run));
        }
        covered = 0;
        while (covered < total && b.Ok() && out.cfRuns.size() < 100000) {
          CfRun run;
          run.count = b.U32();
          if (!b.Ok() || run.count == 0 || !ReadCf(b, run.cf)) break;
          covered += run.count;
          out.cfRuns.push_back(std::move(run));
        }
        break;
      }
      case 0x0FA6: {  // TextRulerAtom
        Ruler ru;
        const uint32_t m = b.U32();
        if (m & 2) b.I16();  // cLevels
        if (m & 1) ru.defaultTab = b.I16();
        if (m & 4) {
          const uint16_t n = b.U16();
          for (uint16_t i = 0; i < n && b.Ok(); ++i) {
            const int16_t pos = b.I16();
            const uint16_t type = b.U16();
            if (ru.tabs.size() < 64) ru.tabs.emplace_back(pos, type);
          }
        }
        for (int l = 0; l < 5; ++l) {
          if (m & (8u << l)) ru.leftMargin[l] = b.I16();
          if (m & (0x100u << l)) ru.indent[l] = b.I16();
        }
        if (b.Ok()) out.ruler = ru;
        break;
      }
      case 0x0FD8: out.fields.push_back({b.U32(), Field::kSlideNumber, 0}); break;
      case 0x0FF7: {
        const uint32_t pos = b.U32();
        out.fields.push_back({pos, Field::kDateTime, b.U8()});
        break;
      }
      case 0x0FF8: out.fields.push_back({b.U32(), Field::kGenericDate, 0}); break;
      case 0x0FF9: out.fields.push_back({b.U32(), Field::kHeader, 0}); break;
      case 0x0FFA: out.fields.push_back({b.U32(), Field::kFooter, 0}); break;
      case 0x1015: out.fields.push_back({b.U32(), Field::kRtfDate, 0}); break;
      case 0x0FF2:  // InteractiveInfo: the link for the TextInteractiveInfoAtom after it
        pendingLink.clear();
        if (auto atom = Find(r.body, 0x0FF3)) {
          Bytes a(atom->body);
          a.U32();  // sound
          const uint32_t hyperlink = a.U32();
          const uint8_t action = a.U8();
          a.U8();
          const uint8_t jump = a.U8();
          if (action == 4 && link) pendingLink = link(hyperlink);
          else if (action == 3) {
            static const char* const kJump[] = {"", "nextslide", "previousslide", "firstslide", "lastslide", "lastslideviewed", "endshow"};
            if (jump >= 1 && jump <= 6) pendingLink = std::string("ppaction://hlinkshowjump?jump=") + kJump[jump];
          }
        }
        break;
      case 0x0FDF: {  // TextInteractiveInfoAtom: a range of the text
        const uint32_t begin = b.U32(), end = b.U32();
        if (b.Ok() && !pendingLink.empty() && begin < end && out.links.size() < 64) out.links.push_back({begin, end, pendingLink});
        break;
      }
      default: break;
    }
    if (out.fields.size() > 64) out.fields.resize(64);
    if (!out.fields.empty() && !b.Ok()) out.fields.pop_back();
  }
}

std::string ParagraphsXml(const Text& text, const Context& cx, const std::function<std::string(const std::string&)>& linkRel) {
  const std::u16string& s = text.chars;
  std::string out;
  // The run covering character `at`, by a running cursor (runs are in order).
  size_t pfIndex = 0, cfIndex = 0;
  uint64_t pfEnd = text.pfRuns.empty() ? UINT64_MAX : text.pfRuns[0].count;
  uint64_t cfEnd = text.cfRuns.empty() ? UINT64_MAX : text.cfRuns[0].count;
  const auto pfAt = [&](size_t at) -> const PfRun* {
    while (pfIndex + 1 < text.pfRuns.size() && at >= pfEnd) pfEnd += text.pfRuns[++pfIndex].count;
    return text.pfRuns.empty() ? nullptr : &text.pfRuns[pfIndex];
  };
  const auto cfAt = [&](size_t at) -> const Cf* {
    while (cfIndex + 1 < text.cfRuns.size() && at >= cfEnd) cfEnd += text.cfRuns[++cfIndex].count;
    return text.cfRuns.empty() ? nullptr : &text.cfRuns[cfIndex].cf;
  };
  const auto fieldAt = [&](size_t at) -> const Field* {
    for (const Field& f : text.fields)
      if (f.pos == at) return &f;
    return nullptr;
  };
  const auto linkAt = [&](size_t at) -> const TextLink* {
    for (const TextLink& l : text.links)
      if (at >= l.begin && at < l.end) return &l;
    return nullptr;
  };

  size_t start = 0;
  int paragraphs = 0;
  while (start <= s.size() && paragraphs < 100000) {
    size_t end = s.find(u'\r', start);
    if (end == std::u16string::npos) end = s.size();
    ++paragraphs;
    const PfRun* pr = pfAt(start);
    const int level = pr ? std::min<int>(pr->level, 4) : 0;
    Pf pf;
    Cf base;
    Layered(cx, text.type, level, pf, base, text.placeholder);
    const Pf styled = pf;
    if (text.ruler) {
      if (text.ruler->leftMargin[level]) pf.leftMargin = *text.ruler->leftMargin[level], pf.mask |= 1u << kLeftMargin;
      if (text.ruler->indent[level]) pf.indent = *text.ruler->indent[level], pf.mask |= 1u << kIndent;
      if (text.ruler->defaultTab) pf.defaultTab = *text.ruler->defaultTab, pf.mask |= 1u << kDefaultTab;
      if (!text.ruler->tabs.empty()) pf.tabs = text.ruler->tabs, pf.mask |= 1u << kTabStops;
    }
    if (pr) pf.Over(pr->pf);
    // A ruler's text offset at or left of the bullet doesn't take a bulleted paragraph's hanging indent
    // away: PowerPoint keeps the style's for it (the ruler records an unbulleted paragraph's margin).
    if ((pf.bulletFlags & 1) && text.ruler && text.ruler->leftMargin[level] && !(pr && pr->pf.Has(kLeftMargin)) &&
        pf.leftMargin <= pf.indent && styled.Has(kLeftMargin) && styled.leftMargin > pf.indent)
      pf.leftMargin = styled.leftMargin;
    // PowerPoint 2007's own margin and indent for the paragraph, when its copy of the text matches.
    if (const size_t i = static_cast<size_t>(paragraphs) - 1; i < text.indents.size() && text.indents[i]) {
      const auto unit = [](double emu) { return static_cast<int16_t>(std::clamp<long long>(std::llround(emu / kEmuPerUnit), -32768, 32767)); };
      pf.leftMargin = unit(text.indents[i]->first), pf.indent = unit(text.indents[i]->first + text.indents[i]->second);
      pf.mask |= 1u << kLeftMargin | 1u << kIndent;
    }
    const auto cfFor = [&](size_t at) {
      Cf cf = base;
      if (const Cf* c = cfAt(at)) cf.Over(*c);
      return cf;
    };
    // PowerPoint 2000's paragraph additions: the entry the first character's own CF run names.
    const Pf9* pf9 = nullptr;
    const Cf* own = cfAt(start);
    if (const size_t rt = own && (own->mask & 0x3C00) ? (own->style >> 10) & 0xF : 0; rt < text.pf9.size()) pf9 = &text.pf9[rt];
    std::string attrs, inner;
    ParaProps(pf, cfFor(start), cx, level, attrs, inner, pf9);
    out += "<a:p><a:pPr" + attrs + ">" + inner + "</a:pPr>";
    // Runs: split where the character properties, a link or a field change, and at line breaks.
    size_t i = start;
    while (i < end) {
      if (s[i] == u'\v') {
        out += "<a:br>" + RunProps("rPr", cfFor(i), cx) + "</a:br>";
        ++i;
        continue;
      }
      if (const Field* f = fieldAt(i)) {
        const char* type = f->kind == Field::kSlideNumber ? "slidenum" : f->kind == Field::kFooter || f->kind == Field::kHeader ? nullptr : "datetime";
        if (type) {
          char id[40];
          std::snprintf(id, sizeof id, "{B6F15528-21DE-4FAA-801E-634DDDAF%04X}", static_cast<unsigned>(i & 0xFFFF));
          out += std::string("<a:fld id=\"") + id + "\" type=\"" + type + "\">" + RunProps("rPr", cfFor(i), cx) + "<a:t>" +
                 (f->kind == Field::kSlideNumber ? std::string("\xE2\x80\xB9#\xE2\x80\xBA") : Esc(s.substr(i, 1))) + "</a:t></a:fld>";
          ++i;
          continue;
        }
      }
      // The run's properties before looking ahead: the run cursor only moves forward.
      const Cf runCf = cfFor(i);
      const Cf* run = cfAt(i);
      const TextLink* lk = linkAt(i);
      size_t j = i + 1;
      while (j < end && s[j] != u'\v' && cfAt(j) == run && linkAt(j) == lk && !fieldAt(j)) ++j;
      const std::string rel = lk && linkRel ? linkRel(lk->target) : std::string();
      out += "<a:r>" + RunProps("rPr", runCf, cx, rel) + "<a:t>" + Esc(s.substr(i, j - i)) + "</a:t></a:r>";
      i = j;
    }
    out += RunProps("endParaRPr", cfFor(end), cx) + "</a:p>";
    if (end >= s.size()) break;
    start = end + 1;
  }
  return out;
}

std::string LevelStylesXml(const Context& cx, int type) {
  std::string out;
  for (int level = 0; level < 5; ++level) {
    Pf pf;
    Cf cf;
    Layered(cx, type, level, pf, cf);
    std::string attrs, inner;
    ParaProps(pf, cf, cx, 0, attrs, inner);
    const std::string tag = "a:lvl" + Num(level + 1) + "pPr";
    std::string def = RunProps("defRPr", cf, cx);
    const int style = type == kTitle ? 0 : type == kBody ? 1 : type == kOther ? 2 : -1;
    if (style >= 0 && cx.kern[style][level] > 0) def.insert(9, " kern=\"" + Num(cx.kern[style][level]) + "\"");  // after "<a:defRPr"
    out += "<" + tag + attrs + ">" + inner + def + "</" + tag + ">";
  }
  return out;
}

std::string ColorXml(uint32_t rgb, int alpha) {
  char hex[8];
  std::snprintf(hex, sizeof hex, "%06X", rgb & 0xFFFFFF);
  if (alpha >= 100000) return std::string("<a:srgbClr val=\"") + hex + "\"/>";
  return std::string("<a:srgbClr val=\"") + hex + "\"><a:alpha val=\"" + Num(std::max(0, alpha)) + "\"/></a:srgbClr>";
}

std::string Esc(std::u16string_view s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    uint32_t c = s[i];
    if (c >= 0xD800 && c < 0xDC00 && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000) {
      c = 0x10000 + ((c - 0xD800) << 10) + (s[i + 1] - 0xDC00);
      ++i;
    } else if ((c >= 0xD800 && c < 0xE000) || c == 0xFFFE || c == 0xFFFF || (c < 0x20 && c != '\t')) {
      continue;  // not allowed in XML
    }
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      default: AppendUtf8(out, c);
    }
  }
  return out;
}

std::string Esc(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (const char ch : s) {
    const unsigned char c = static_cast<unsigned char>(ch);
    if (c < 0x20 && c != '\t') continue;
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      default: out += ch;
    }
  }
  return out;
}

Scheme ReadScheme(std::span<const uint8_t> body) {
  Scheme s;
  for (size_t i = 0; i < 8 && 4 * i + 3 <= body.size(); ++i)
    s.rgb[i] = uint32_t{body[4 * i]} << 16 | uint32_t{body[4 * i + 1]} << 8 | body[4 * i + 2];
  return s;
}

}  // namespace fw::slide::ppt::detail
