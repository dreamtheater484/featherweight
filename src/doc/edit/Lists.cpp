#include "doc/edit/Lists.h"

#include <algorithm>

#include "doc/edit/Styles.h"
#include "doc/model/Internal.h"

namespace fw::doc::edit {

using namespace names;

namespace {

constexpr uint32_t kPStyle = Name("pStyle"), kNumPr = Name("numPr"), kIlvl = Name("ilvl"), kNumId = Name("numId");
constexpr uint32_t kInd = Name("ind"), kNumFmt = Name("numFmt"), kLvlText = Name("lvlText"), kStart = Name("start");

// Word's private-use bullets: Symbol's dot, Wingdings' square and arrowhead (UTF-8).
constexpr const char* kSymbolDot = "\xEF\x82\xB7";
constexpr const char* kWingdingsSquare = "\xEF\x82\xA7";
constexpr const char* kWingdingsArrow = "\xEF\x83\x98";

struct LevelDef {
  std::string numFmt, text, font;
  const char* jc = "left";
  int left = 720, hanging = 360;
  int start = 1;
};

LevelDef DefaultLevel(ListKind kind, int k) {
  LevelDef d;
  switch (kind) {
    case ListKind::Bullet: {
      static const char* const kFonts[] = {"Symbol", "Courier New", "Wingdings"};
      static const char* const kTexts[] = {kSymbolDot, "o", kWingdingsSquare};
      d.numFmt = "bullet";
      d.text = kTexts[k % 3];
      d.font = kFonts[k % 3];
      d.left = 720 * (k + 1);
      break;
    }
    case ListKind::Number: {
      static const char* const kFormats[] = {"decimal", "lowerLetter", "lowerRoman"};
      d.numFmt = kFormats[k % 3];
      d.text = "%" + std::to_string(k + 1) + ".";
      d.left = 720 * (k + 1);
      if (k % 3 == 2) {
        d.jc = "right";
        d.hanging = 180;
      }
      break;
    }
    case ListKind::Multilevel: {
      static const int kLeft[] = {360, 792, 1224, 1728, 2232, 2736, 3240, 3744, 4320};
      static const int kHanging[] = {360, 432, 504, 648, 792, 936, 1080, 1224, 1440};
      d.numFmt = "decimal";
      for (int i = 0; i <= k; ++i) d.text += "%" + std::to_string(i + 1) + ".";
      d.left = kLeft[k];
      d.hanging = kHanging[k];
      break;
    }
  }
  return d;
}

std::string Escape(std::string_view s) {
  std::string out;
  for (char c : s) {
    if (c == '&') out += "&amp;";
    else if (c == '<') out += "&lt;";
    else if (c == '"') out += "&quot;";
    else out += c;
  }
  return out;
}

bool Strict(const Document& doc) {
  const Part* p = doc.FindPart(PartKind::Document);
  return p && IsStrictUri(p->uri[static_cast<size_t>(Ns::W)]);
}

std::string LevelXml(const Document& doc, int k, const LevelDef& d) {
  bool strict = Strict(doc);
  std::string jc = d.jc;
  if (strict) jc = jc == "left" ? "start" : jc == "right" ? "end" : jc;
  std::string x = "<w:lvl w:ilvl=\"" + std::to_string(k) + "\"><w:start w:val=\"" + std::to_string(d.start) +
                  "\"/><w:numFmt w:val=\"" + d.numFmt + "\"/><w:lvlText w:val=\"" + Escape(d.text) + "\"/><w:lvlJc w:val=\"" +
                  jc + "\"/><w:pPr><w:ind w:" + (strict ? "start" : "left") + "=\"" + std::to_string(d.left) +
                  "\" w:hanging=\"" + std::to_string(d.hanging) + "\"/></w:pPr>";
  if (!d.font.empty()) {
    std::string f = Escape(d.font);
    x += "<w:rPr><w:rFonts w:ascii=\"" + f + "\" w:hAnsi=\"" + f + "\"" + (d.font == "Courier New" ? " w:cs=\"" + f + "\"" : "") +
         " w:hint=\"default\"/></w:rPr>";
  }
  return x + "</w:lvl>";
}

std::string Hex8(uint32_t v) {
  static const char* kDigits = "0123456789ABCDEF";
  std::string s(8, '0');
  for (int i = 7; i >= 0; --i, v >>= 4) s[static_cast<size_t>(i)] = kDigits[v & 15];
  return s;
}

ElemId NumberingRoot(const Document& doc) {
  const Part* p = doc.FindPart(PartKind::Numbering);
  return p ? p->root : kNoElem;
}

// The next free w:abstractNumId and w:numId.
std::pair<int64_t, int64_t> NextIds(const Document& doc) {
  int64_t abs = 0, num = 1;
  if (ElemId root = NumberingRoot(doc); root != kNoElem) {
    for (Node n : doc.NodeOf(root).Kids()) {
      if (n.Is(Name("abstractNum"))) abs = std::max(abs, n.Int(Name("abstractNumId")).value_or(-1) + 1);
      else if (n.Is(Name("num"))) num = std::max(num, n.Int(Name("numId")).value_or(0) + 1);
    }
  }
  return {abs, num};
}

// Adds a list definition and a w:num for it; returns the numId (empty on failure).
std::string AddList(TreeEdit& ed, ListKind kind, const AutoList* first = nullptr) {
  const Document& doc = ed.Doc();
  ElemId root = NumberingRoot(doc);
  if (root == kNoElem) return {};
  auto [absId, numId] = NextIds(doc);
  // w:nsid tells lists apart when content is copied between documents: unique per definition.
  uint32_t nsid = 2166136261u;
  for (uint64_t v : {static_cast<uint64_t>(absId), static_cast<uint64_t>(doc.GetTree().elems.size()), static_cast<uint64_t>(doc.GetTree().attrs.size())})
    for (int i = 0; i < 8; ++i) nsid = (nsid ^ static_cast<uint8_t>(v >> (8 * i))) * 16777619u;
  std::string xml = "<w:abstractNum w:abstractNumId=\"" + std::to_string(absId) + "\"><w:nsid w:val=\"" + Hex8(nsid) +
                    "\"/><w:multiLevelType w:val=\"" + (kind == ListKind::Multilevel ? "multilevel" : "hybridMultilevel") +
                    "\"/>";
  for (int k = 0; k < 9; ++k) {
    LevelDef d = DefaultLevel(kind, k);
    if (k == 0 && first) {
      d.numFmt = first->numFmt;
      d.text = first->lvlText;
      d.font = first->font;
      d.start = first->start;
    }
    xml += LevelXml(doc, k, d);
  }
  xml += "</w:abstractNum>";
  if (ed.AddXml(root, xml) == kNoElem) return {};
  std::string id = std::to_string(numId);
  if (ed.AddXml(root, "<w:num w:numId=\"" + id + "\"><w:abstractNumId w:val=\"" + std::to_string(absId) + "\"/></w:num>") ==
      kNoElem)
    return {};
  ed.Reindex();
  return id;
}

bool InList(const ParaProps& pp) { return !pp.numId.empty() && pp.numId != "0"; }

bool StyleGivesList(const Document& doc, std::string_view styleId) {
  const Style* s = styleId.empty() ? nullptr : doc.FindStyle(styleId);
  for (int hops = 0; s && hops < 64; ++hops) {
    if (Node id = s->node.Path({kPPr, kNumPr, kNumId})) {
      std::string_view v = id.AttrOr(kVal);
      return !v.empty() && v != "0";
    }
    s = s->basedOn == kNone ? nullptr : &doc.Styles()[s->basedOn];
  }
  return false;
}

void SetNumPr(TreeEdit& ed, uint32_t para, std::string_view numId, int32_t level) {
  ElemId np = ed.Path(ed.Doc().Para(para).elem, {kPPr, kNumPr});
  ed.SetAttr(ed.Child(np, kIlvl), kVal, std::to_string(level));
  ed.SetAttr(ed.Child(np, kNumId), kVal, numId);
}

// Puts paragraphs into a list: their indents come from the list, and the default style becomes
// List Paragraph.
void Join(TreeEdit& ed, const std::vector<uint32_t>& paras, std::string_view numId, int32_t level, std::string_view listStyle) {
  const Document& doc = ed.Doc();
  for (uint32_t p : paras) {
    const ParaProps& pp = doc.ResolvedPara(p);
    int32_t lv = InList(pp) ? pp.ilvl : level;
    const Style* style = doc.FindStyle(pp.style);
    bool plain = !style || style->isDefault;
    ElemId pPr = ed.Child(doc.Para(p).elem, kPPr);
    ed.Remove(pPr, kInd);
    if (plain && !listStyle.empty()) ed.SetAttr(ed.Child(pPr, kPStyle), kVal, listStyle);
    SetNumPr(ed, p, numId, lv);
  }
}

// The paragraphs after `para` in its story, in order.
std::vector<uint32_t> Following(const Document& doc, uint32_t para) {
  std::vector<uint32_t> out;
  uint32_t story = doc.Para(para).story;
  for (auto q = static_cast<uint32_t>(para + 1); q < doc.Paragraphs().size(); ++q)
    if (doc.Para(q).story == story && !doc.ParaNode(q).DisplayOnly()) out.push_back(q);
  return out;
}

// The paragraph before `para` in its story; kNone at its start.
uint32_t Previous(const Document& doc, uint32_t para) {
  uint32_t story = doc.Para(para).story;
  for (uint32_t q = para; q-- > 0;)
    if (doc.Para(q).story == story && !doc.ParaNode(q).DisplayOnly()) return q;
  return kNone;
}

// Moves `para` and the rest of its list to another w:num.
void Repoint(TreeEdit& ed, uint32_t para, std::string_view numId) {
  const Document& doc = ed.Doc();
  std::string old(doc.ResolvedPara(para).numId);
  std::vector<std::pair<uint32_t, int32_t>> items{{para, doc.ResolvedPara(para).ilvl}};
  for (uint32_t q : Following(doc, para))
    if (doc.ResolvedPara(q).numId == old) items.emplace_back(q, doc.ResolvedPara(q).ilvl);
  for (auto [q, level] : items) SetNumPr(ed, q, numId, level);
}

}  // namespace

std::optional<ListKind> KindOfList(const Document& doc, std::string_view numId) {
  if (numId.empty() || numId == "0") return std::nullopt;
  Node l0 = doc.NumberingLevel(numId, 0);
  if (!l0) return std::nullopt;
  if (l0.Child(kNumFmt).AttrOr(kVal) == "bullet") return ListKind::Bullet;
  Node l1 = doc.NumberingLevel(numId, 1);
  if (l1 && l1.Child(kLvlText).AttrOr(kVal).find("%1") != std::string_view::npos) return ListKind::Multilevel;
  return ListKind::Number;
}

void ToggleList(TreeEdit& ed, Range range, ListKind kind) {
  const Document& doc = ed.Doc();
  std::vector<uint32_t> paras = ParagraphsOf(doc, range);
  if (paras.empty()) return;
  bool all = true;
  for (uint32_t p : paras) all &= KindOfList(doc, doc.ResolvedPara(p).numId) == kind;
  if (all) {
    RemoveList(ed, range);
    return;
  }
  if (ed.EnsurePart(PartKind::Numbering) == kNoElem || ed.EnsurePart(PartKind::Styles) == kNoElem) return;
  ed.Begin(kind == ListKind::Bullet ? u"Bullets" : kind == ListKind::Number ? u"Numbering" : u"Multilevel list");
  ed.Local(true);  // the numbers after it change too
  std::string numId;
  int32_t level = 0;
  // Next to a list of the same kind, the paragraphs join it.
  uint32_t prev = Previous(doc, paras.front());
  if (prev != kNone && KindOfList(doc, doc.ResolvedPara(prev).numId) == kind) {
    numId = std::string(doc.ResolvedPara(prev).numId);
    level = doc.ResolvedPara(prev).ilvl;
  } else {
    numId = AddList(ed, kind);
  }
  if (!numId.empty()) {
    std::string listStyle = EnsureStyle(ed, "List Paragraph");
    // Paragraphs in a list of another kind change kind, keeping their level.
    Join(ed, paras, numId, level, listStyle);
  }
  ed.Commit();
}

void RemoveList(TreeEdit& ed, Range range) {
  const Document& doc = ed.Doc();
  ed.Begin(u"Remove list");
  ed.Local(true);
  for (uint32_t p : ParagraphsOf(doc, range)) {
    const ParaProps& pp = doc.ResolvedPara(p);
    if (!InList(pp)) continue;
    std::string styleId(pp.style);
    const Style* style = doc.FindStyle(styleId);
    bool listParagraph = style && style->name == "List Paragraph";
    ElemId pPr = ed.Child(doc.Para(p).elem, kPPr);
    ed.Remove(pPr, kNumPr);
    if (listParagraph) {
      ed.Remove(pPr, kPStyle);
    } else if (StyleGivesList(doc, styleId)) {
      ed.SetAttr(ed.Path(pPr, {kNumPr, kNumId}), kVal, "0");
    }
  }
  ed.Commit();
}

void RestartNumbering(TreeEdit& ed, uint32_t para) {
  const Document& doc = ed.Doc();
  const ParaProps& pp = doc.ResolvedPara(para);
  ElemId root = NumberingRoot(doc);
  if (!InList(pp) || root == kNoElem) return;
  Node num;
  for (Node n : doc.NodeOf(root).Kids())
    if (n.Is(Name("num")) && n.AttrOr(Name("numId")) == pp.numId) num = n;
  if (!num) return;
  std::string absId(num.Child(Name("abstractNumId")).AttrOr(kVal));
  int32_t level = pp.ilvl;
  int64_t start = 0;
  if (Node l = doc.NumberingLevel(pp.numId, level)) start = l.Child(kStart).Int(kVal).value_or(0);
  ed.Begin(u"Restart numbering");
  ed.Local(true);
  std::string id = std::to_string(NextIds(doc).second);
  std::string xml = "<w:num w:numId=\"" + id + "\"><w:abstractNumId w:val=\"" + Escape(absId) + "\"/><w:lvlOverride w:ilvl=\"" +
                    std::to_string(level) + "\"><w:startOverride w:val=\"" + std::to_string(start) + "\"/></w:lvlOverride></w:num>";
  if (ed.AddXml(root, xml) != kNoElem) {
    ed.Reindex();
    Repoint(ed, para, id);
  }
  ed.Commit();
}

bool ContinueNumbering(TreeEdit& ed, uint32_t para) {
  const Document& doc = ed.Doc();
  if (!InList(doc.ResolvedPara(para))) return false;
  std::string own(doc.ResolvedPara(para).numId);  // a copy: resolving more paragraphs grows the cache
  std::optional<ListKind> kind = KindOfList(doc, own);
  std::string target;
  for (uint32_t q = Previous(doc, para); q != kNone && target.empty(); q = Previous(doc, q)) {
    const ParaProps& qp = doc.ResolvedPara(q);
    if (InList(qp) && qp.numId != own && KindOfList(doc, qp.numId) == kind) target = std::string(qp.numId);
  }
  if (target.empty()) return false;
  ed.Begin(u"Continue numbering");
  ed.Local(true);
  Repoint(ed, para, target);
  ed.Commit();
  return true;
}

namespace {

std::optional<int32_t> RomanValue(std::u16string_view s) {
  int32_t total = 0, prev = 0;
  for (size_t i = s.size(); i-- > 0;) {
    char16_t c = s[i] >= u'A' && s[i] <= u'Z' ? static_cast<char16_t>(s[i] - u'A' + u'a') : s[i];
    int32_t v = c == u'i' ? 1 : c == u'v' ? 5 : c == u'x' ? 10 : c == u'l' ? 50 : c == u'c' ? 100 : c == u'd' ? 500 : c == u'm' ? 1000 : 0;
    if (v == 0) return std::nullopt;
    total += v < prev ? -v : v;
    prev = std::max(prev, v);
  }
  return total > 0 ? std::optional<int32_t>(total) : std::nullopt;
}

bool AllCase(std::u16string_view s, bool upper) {
  for (char16_t c : s)
    if (upper ? (c < u'A' || c > u'Z') : (c < u'a' || c > u'z')) return false;
  return true;
}

}  // namespace

std::optional<AutoList> DetectAutoList(std::u16string_view typed) {
  if (typed.size() < 2 || (typed.back() != u' ' && typed.back() != u'\t')) return std::nullopt;
  std::u16string_view s = typed.substr(0, typed.size() - 1);
  AutoList a;
  a.length = static_cast<uint32_t>(typed.size());
  if (s == u"*" || s == u"-" || s == u">") {
    a.kind = ListKind::Bullet;
    a.numFmt = "bullet";
    a.lvlText = s == u"*" ? kSymbolDot : s == u"-" ? "-" : kWingdingsArrow;
    a.font = s == u"*" ? "Symbol" : s == u"-" ? "" : "Wingdings";
    return a;
  }
  bool paren = s.front() == u'(';
  char16_t delim = s.back();
  if ((delim != u'.' && delim != u')') || (paren && delim != u')')) return std::nullopt;
  std::u16string_view token = s.substr(paren ? 1 : 0, s.size() - (paren ? 2 : 1));
  if (token.empty() || token.size() > 4) return std::nullopt;
  a.kind = ListKind::Number;
  a.lvlText = paren ? "(%1)" : delim == u'.' ? "%1." : "%1)";
  if (std::all_of(token.begin(), token.end(), [](char16_t c) { return c >= u'0' && c <= u'9'; })) {
    a.numFmt = "decimal";
    a.start = 0;
    for (char16_t c : token) a.start = a.start * 10 + (c - u'0');
    return a;
  }
  bool upper = AllCase(token, true);
  if (!upper && !AllCase(token, false)) return std::nullopt;
  // "i" and "I" are Roman; other single letters are letters; longer tokens must be Roman numerals.
  if (token.size() == 1 && token != u"i" && token != u"I") {
    a.numFmt = upper ? "upperLetter" : "lowerLetter";
    a.start = (upper ? token[0] - u'A' : token[0] - u'a') + 1;
    return a;
  }
  std::optional<int32_t> roman = RomanValue(token);
  if (!roman) return std::nullopt;
  a.numFmt = upper ? "upperRoman" : "lowerRoman";
  a.start = *roman;
  return a;
}

void StartAutoList(TreeEdit& ed, uint32_t para, const AutoList& list) {
  if (ed.EnsurePart(PartKind::Numbering) == kNoElem || ed.EnsurePart(PartKind::Styles) == kNoElem) return;
  ed.Begin(u"AutoFormat");
  ed.Local(true);
  std::string numId = AddList(ed, list.kind == ListKind::Bullet ? ListKind::Bullet : ListKind::Number, &list);
  if (!numId.empty()) {
    Join(ed, {para}, numId, 0, EnsureStyle(ed, "List Paragraph"));
  }
  ed.Commit();
}

}  // namespace fw::doc::edit
