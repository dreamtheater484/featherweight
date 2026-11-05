#include "doc/edit/Styles.h"

#include <algorithm>
#include <map>

#include "doc/model/Internal.h"

namespace fw::doc::edit {

using namespace names;

namespace {

constexpr uint32_t kPStyle = Name("pStyle"), kNumPr = Name("numPr"), kSectPr = Name("sectPr");
constexpr uint32_t kRStyle = Name("rStyle"), kRPrChange = Name("rPrChange"), kPPrChange = Name("pPrChange");
constexpr uint32_t kSemiHidden = Name("semiHidden"), kUnhide = Name("unhideWhenUsed"), kQFormat = Name("qFormat");
constexpr uint32_t kHidden = Name("hidden"), kUiPriority = Name("uiPriority");

// Word's built-in styles (the Office 2013 and later definitions). Names are as Word writes them;
// basedOn, next and link name other built-ins. "{MAJOR}" stands for the heading font.
struct Builtin {
  std::string_view name, id;
  StyleType type;
  int priority;      // w:uiPriority; -1 for none
  bool gallery;      // recommended in the gallery before the document has it
  std::string_view basedOn, next, link;
  std::string_view pPr, rPr;
};

const Builtin kBuiltins[] = {
    {"Normal", "Normal", StyleType::Paragraph, -1, true, "", "", "", "", ""},
    {"Default Paragraph Font", "DefaultParagraphFont", StyleType::Character, 1, false, "", "", "", "", ""},
    {"No Spacing", "NoSpacing", StyleType::Paragraph, 1, true, "", "", "", "<w:spacing w:after=\"0\" w:line=\"240\" w:lineRule=\"auto\"/>", ""},
    {"heading 1", "Heading1", StyleType::Paragraph, 9, true, "Normal", "Normal", "Heading 1 Char",
     "<w:keepNext/><w:keepLines/><w:spacing w:before=\"240\" w:after=\"0\"/><w:outlineLvl w:val=\"0\"/>",
     "{MAJOR}<w:color w:val=\"2F5496\" w:themeColor=\"accent1\" w:themeShade=\"BF\"/><w:sz w:val=\"32\"/><w:szCs w:val=\"32\"/>"},
    {"heading 2", "Heading2", StyleType::Paragraph, 9, true, "Normal", "Normal", "Heading 2 Char",
     "<w:keepNext/><w:keepLines/><w:spacing w:before=\"40\" w:after=\"0\"/><w:outlineLvl w:val=\"1\"/>",
     "{MAJOR}<w:color w:val=\"2F5496\" w:themeColor=\"accent1\" w:themeShade=\"BF\"/><w:sz w:val=\"26\"/><w:szCs w:val=\"26\"/>"},
    {"heading 3", "Heading3", StyleType::Paragraph, 9, false, "Normal", "Normal", "Heading 3 Char",
     "<w:keepNext/><w:keepLines/><w:spacing w:before=\"40\" w:after=\"0\"/><w:outlineLvl w:val=\"2\"/>",
     "{MAJOR}<w:color w:val=\"1F3763\" w:themeColor=\"accent1\" w:themeShade=\"7F\"/><w:sz w:val=\"24\"/><w:szCs w:val=\"24\"/>"},
    {"heading 4", "Heading4", StyleType::Paragraph, 9, false, "Normal", "Normal", "Heading 4 Char",
     "<w:keepNext/><w:keepLines/><w:spacing w:before=\"40\" w:after=\"0\"/><w:outlineLvl w:val=\"3\"/>",
     "{MAJOR}<w:i/><w:iCs/><w:color w:val=\"2F5496\" w:themeColor=\"accent1\" w:themeShade=\"BF\"/>"},
    {"heading 5", "Heading5", StyleType::Paragraph, 9, false, "Normal", "Normal", "Heading 5 Char",
     "<w:keepNext/><w:keepLines/><w:spacing w:before=\"40\" w:after=\"0\"/><w:outlineLvl w:val=\"4\"/>",
     "{MAJOR}<w:color w:val=\"2F5496\" w:themeColor=\"accent1\" w:themeShade=\"BF\"/>"},
    {"heading 6", "Heading6", StyleType::Paragraph, 9, false, "Normal", "Normal", "Heading 6 Char",
     "<w:keepNext/><w:keepLines/><w:spacing w:before=\"40\" w:after=\"0\"/><w:outlineLvl w:val=\"5\"/>",
     "{MAJOR}<w:color w:val=\"1F3763\" w:themeColor=\"accent1\" w:themeShade=\"7F\"/>"},
    {"heading 7", "Heading7", StyleType::Paragraph, 9, false, "Normal", "Normal", "Heading 7 Char",
     "<w:keepNext/><w:keepLines/><w:spacing w:before=\"40\" w:after=\"0\"/><w:outlineLvl w:val=\"6\"/>",
     "{MAJOR}<w:i/><w:iCs/><w:color w:val=\"1F3763\" w:themeColor=\"accent1\" w:themeShade=\"7F\"/>"},
    {"heading 8", "Heading8", StyleType::Paragraph, 9, false, "Normal", "Normal", "Heading 8 Char",
     "<w:keepNext/><w:keepLines/><w:spacing w:before=\"40\" w:after=\"0\"/><w:outlineLvl w:val=\"7\"/>",
     "{MAJOR}<w:color w:val=\"272727\" w:themeColor=\"text1\" w:themeTint=\"D8\"/><w:sz w:val=\"21\"/><w:szCs w:val=\"21\"/>"},
    {"heading 9", "Heading9", StyleType::Paragraph, 9, false, "Normal", "Normal", "Heading 9 Char",
     "<w:keepNext/><w:keepLines/><w:spacing w:before=\"40\" w:after=\"0\"/><w:outlineLvl w:val=\"8\"/>",
     "{MAJOR}<w:i/><w:iCs/><w:color w:val=\"272727\" w:themeColor=\"text1\" w:themeTint=\"D8\"/><w:sz w:val=\"21\"/><w:szCs w:val=\"21\"/>"},
    {"Title", "Title", StyleType::Paragraph, 10, true, "Normal", "Normal", "Title Char",
     "<w:spacing w:after=\"0\" w:line=\"240\" w:lineRule=\"auto\"/><w:contextualSpacing/>",
     "{MAJOR}<w:spacing w:val=\"-10\"/><w:kern w:val=\"28\"/><w:sz w:val=\"56\"/><w:szCs w:val=\"56\"/>"},
    {"Subtitle", "Subtitle", StyleType::Paragraph, 11, true, "Normal", "Normal", "Subtitle Char", "<w:spacing w:after=\"160\"/>",
     "<w:color w:val=\"5A5A5A\" w:themeColor=\"text1\" w:themeTint=\"A5\"/><w:spacing w:val=\"15\"/>"},
    {"Subtle Emphasis", "SubtleEmphasis", StyleType::Character, 19, true, "Default Paragraph Font", "", "", "",
     "<w:i/><w:iCs/><w:color w:val=\"404040\" w:themeColor=\"text1\" w:themeTint=\"BF\"/>"},
    {"Emphasis", "Emphasis", StyleType::Character, 20, true, "Default Paragraph Font", "", "", "", "<w:i/><w:iCs/>"},
    {"Intense Emphasis", "IntenseEmphasis", StyleType::Character, 21, true, "Default Paragraph Font", "", "", "",
     "<w:i/><w:iCs/><w:color w:val=\"4472C4\" w:themeColor=\"accent1\"/>"},
    {"Strong", "Strong", StyleType::Character, 22, true, "Default Paragraph Font", "", "", "", "<w:b/><w:bCs/>"},
    {"Quote", "Quote", StyleType::Paragraph, 29, true, "Normal", "Normal", "Quote Char",
     "<w:spacing w:before=\"200\" w:after=\"160\"/><w:ind w:left=\"864\" w:right=\"864\"/><w:jc w:val=\"center\"/>",
     "<w:i/><w:iCs/><w:color w:val=\"404040\" w:themeColor=\"text1\" w:themeTint=\"BF\"/>"},
    {"Intense Quote", "IntenseQuote", StyleType::Paragraph, 30, true, "Normal", "Normal", "Intense Quote Char",
     "<w:pBdr><w:top w:val=\"single\" w:sz=\"4\" w:space=\"10\" w:color=\"4472C4\" w:themeColor=\"accent1\"/>"
     "<w:bottom w:val=\"single\" w:sz=\"4\" w:space=\"10\" w:color=\"4472C4\" w:themeColor=\"accent1\"/></w:pBdr>"
     "<w:spacing w:before=\"360\" w:after=\"360\"/><w:ind w:left=\"864\" w:right=\"864\"/><w:jc w:val=\"center\"/>",
     "<w:i/><w:iCs/><w:color w:val=\"4472C4\" w:themeColor=\"accent1\"/>"},
    {"List Paragraph", "ListParagraph", StyleType::Paragraph, 34, true, "Normal", "", "", "<w:ind w:left=\"720\"/><w:contextualSpacing/>", ""},
};

char Lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

bool SameName(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (Lower(a[i]) != Lower(b[i])) return false;
  return true;
}

// The id without spaces Word makes from a name: "Heading 1 Char" -> "Heading1Char".
std::string IdOf(std::string_view name) {
  std::string id;
  for (char c : name)
    if (c != ' ') id += c;
  return id;
}

const Style* FindByName(const Document& doc, std::string_view nameOrId) {
  for (const Style& s : doc.Styles())
    if (SameName(s.name, nameOrId)) return &s;
  for (const Style& s : doc.Styles())
    if (SameName(s.id, nameOrId)) return &s;
  return nullptr;
}

const Builtin* FindBuiltin(std::string_view nameOrId) {
  for (const Builtin& b : kBuiltins)
    if (SameName(b.name, nameOrId) || SameName(b.id, nameOrId)) return &b;
  return nullptr;
}

std::string UniqueId(const Document& doc, std::string_view base) {
  std::string id(base);
  for (int n = 1; doc.FindStyle(id); ++n) id = std::string(base) + std::to_string(n);
  return id;
}

void ReplaceAll(std::string& s, std::string_view from, std::string_view to) {
  for (size_t at = s.find(from); at != std::string::npos; at = s.find(from, at + to.size())) s.replace(at, from.size(), to);
}

bool Strict(const Document& doc) {
  const Part* p = doc.FindPart(PartKind::Document);
  return p && IsStrictUri(p->uri[static_cast<size_t>(Ns::W)]);
}

std::string XmlEscape(std::string_view s) {
  std::string out;
  for (char c : s) {
    if (c == '&') out += "&amp;";
    else if (c == '<') out += "&lt;";
    else if (c == '"') out += "&quot;";
    else out += c;
  }
  return out;
}

// The properties of a built-in, ready for the document: its heading font, start/end names in
// strict documents.
std::string Props(const Document& doc, std::string_view props) {
  std::string s(props);
  ReplaceAll(s, "{MAJOR}",
             doc.GetTheme().present
                 ? "<w:rFonts w:asciiTheme=\"majorHAnsi\" w:eastAsiaTheme=\"majorEastAsia\" w:hAnsiTheme=\"majorHAnsi\" w:cstheme=\"majorBidi\"/>"
                 : "<w:rFonts w:ascii=\"Calibri Light\" w:hAnsi=\"Calibri Light\"/>");
  if (Strict(doc)) {
    ReplaceAll(s, " w:left=", " w:start=");
    ReplaceAll(s, " w:right=", " w:end=");
  }
  return s;
}

std::string StyleXml(std::string_view type, std::string_view id, std::string_view name, bool isDefault, bool custom,
                     std::string_view basedOn, std::string_view next, std::string_view link, int priority, bool qFormat,
                     std::string_view extra, const std::string& pPr, const std::string& rPr) {
  std::string x = "<w:style w:type=\"" + std::string(type) + "\"";
  if (isDefault) x += " w:default=\"1\"";
  if (custom) x += " w:customStyle=\"1\"";
  x += " w:styleId=\"" + XmlEscape(id) + "\"><w:name w:val=\"" + XmlEscape(name) + "\"/>";
  if (!basedOn.empty()) x += "<w:basedOn w:val=\"" + XmlEscape(basedOn) + "\"/>";
  if (!next.empty()) x += "<w:next w:val=\"" + XmlEscape(next) + "\"/>";
  if (!link.empty()) x += "<w:link w:val=\"" + XmlEscape(link) + "\"/>";
  if (priority >= 0) x += "<w:uiPriority w:val=\"" + std::to_string(priority) + "\"/>";
  x += extra;
  if (qFormat) x += "<w:qFormat/>";
  if (!pPr.empty()) x += "<w:pPr>" + pPr + "</w:pPr>";
  if (!rPr.empty()) x += "<w:rPr>" + rPr + "</w:rPr>";
  return x + "</w:style>";
}

ElemId StylesRoot(const Document& doc) {
  const Part* p = doc.FindPart(PartKind::Styles);
  return p ? p->root : kNoElem;
}

// The id a built-in's basedOn or next names in this document: Normal is the default paragraph
// style, Default Paragraph Font the default character style (left out when there is none).
std::string Related(TreeEdit& ed, std::string_view name) {
  if (name.empty()) return {};
  const Document& doc = ed.Doc();
  if (name == "Normal") {
    if (const Style* d = doc.DefaultStyle(StyleType::Paragraph)) return std::string(d->id);
  } else if (name == "Default Paragraph Font") {
    const Style* d = doc.DefaultStyle(StyleType::Character);
    return d ? std::string(d->id) : std::string();
  }
  return EnsureStyle(ed, name);
}

std::string AddBuiltin(TreeEdit& ed, const Builtin& b) {
  const Document& doc = ed.Doc();
  ElemId root = StylesRoot(doc);
  if (root == kNoElem) return {};
  std::string basedOn = Related(ed, b.basedOn), next = Related(ed, b.next);
  std::string id = UniqueId(doc, b.id);
  bool para = b.type == StyleType::Paragraph;
  bool isDefault = (b.id == "Normal" && !doc.DefaultStyle(StyleType::Paragraph)) ||
                   (b.id == "DefaultParagraphFont" && !doc.DefaultStyle(StyleType::Character));
  std::string extra = b.id == "DefaultParagraphFont" ? "<w:semiHidden/><w:unhideWhenUsed/>"
                      : b.priority == 9 && b.id != "Heading1" ? "<w:unhideWhenUsed/>"
                                                              : "";
  std::string rPr = Props(doc, b.rPr);
  // A linked character style: Word's pair for headings, the title and quotes.
  std::string linkId;
  const Style* existingLink = b.link.empty() ? nullptr : FindByName(doc, b.link);
  if (existingLink) linkId = std::string(existingLink->id);
  else if (!b.link.empty()) linkId = UniqueId(doc, IdOf(b.link));
  if (linkId == id) linkId = UniqueId(doc, id + "Char");
  std::string xml = StyleXml(para ? "paragraph" : "character", id, b.name, isDefault, false, basedOn == id ? "" : basedOn,
                             next == id ? "" : next, linkId, b.priority, b.id != "DefaultParagraphFont", extra,
                             Props(doc, b.pPr), rPr);
  if (ed.AddXml(root, xml) == kNoElem) return {};
  ed.Reindex();
  if (!b.link.empty() && !existingLink) {
    const Style* dpf = doc.DefaultStyle(StyleType::Character);
    std::string charXml = StyleXml("character", linkId, b.link, false, true, dpf ? dpf->id : std::string_view(), "", id,
                                   b.priority, false, "", "", rPr);
    if (ed.AddXml(root, charXml) != kNoElem) ed.Reindex();
  }
  ed.TouchAll();
  return id;
}

// Paragraph properties a paragraph style replaces: what the formatting commands set.
constexpr uint32_t kParaFormatting[] = {Name("keepNext"),       Name("keepLines"), Name("pageBreakBefore"),
                                        Name("widowControl"),   Name("pBdr"),      Name("shd"),
                                        Name("spacing"),        Name("ind"),       Name("contextualSpacing"),
                                        Name("jc"),             Name("outlineLvl")};

// Word shows a style once it's used: semiHidden goes when the style says so.
void Unhide(TreeEdit& ed, const Style& s) {
  Node n = s.node;
  if (n.Child(kSemiHidden) && n.Child(kUnhide)) ed.Remove(n.Id(), kSemiHidden);
}

void ApplyParaStyle(TreeEdit& ed, uint32_t p, const Style& style) {
  const Document& doc = ed.Doc();
  ElemId pElem = doc.Para(p).elem;
  ElemId pPr = ed.Child(pElem, kPPr);
  for (uint32_t n : kParaFormatting) ed.Remove(pPr, n);
  if (style.isDefault) {
    ed.Remove(pPr, kPStyle);
    ed.Remove(pPr, kNumPr);
  } else {
    ed.SetAttr(ed.Child(pPr, kPStyle), kVal, style.id);
  }
  // Direct character formatting that covers more than half of the text goes.
  std::map<uint32_t, uint32_t> covered;
  uint32_t total = 0;
  std::vector<ElemId> runs;
  for (const TextRun& r : doc.Runs(p)) {
    if (!r.run || r.displayOnly || r.fieldCode) continue;
    uint32_t len = r.end - r.start;
    total += len;
    if (std::find(runs.begin(), runs.end(), r.run.Id()) == runs.end()) runs.push_back(r.run.Id());
    for (Node c : r.run.Child(kRPr).Kids())
      if (!c.Is(kRStyle) && !c.Is(kRPrChange)) covered[c.NameHash()] += len;
  }
  for (auto [name, chars] : covered) {
    if (chars * 2 <= total) continue;
    for (ElemId run : runs) ed.Remove(ed.Child(run, kRPr, false), name);
    ed.Remove(ed.Child(pPr, kRPr, false), name);
  }
  ed.TouchPara(p);
}

std::string TitleCase(std::string_view name) {
  std::string out(name);
  bool start = true;
  for (char& c : out) {
    if (start && c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    start = c == ' ';
  }
  return out;
}

}  // namespace

std::string DisplayName(std::string_view name) {
  for (char c : name)
    if (c >= 'A' && c <= 'Z') return std::string(name);  // as the author wrote it
  if (name.starts_with("toc ")) return "TOC " + std::string(name.substr(4));
  return TitleCase(name);
}

std::vector<StyleEntry> GalleryStyles(const Document& doc) {
  std::vector<std::pair<int64_t, StyleEntry>> items;
  for (const Style& s : doc.Styles()) {
    if (s.type != StyleType::Paragraph && s.type != StyleType::Character) continue;
    Node n = s.node;
    if (!n.Child(kQFormat).On() || n.Child(kSemiHidden).On() || n.Child(kHidden).On()) continue;
    int64_t prio = n.Child(kUiPriority).Int(kVal).value_or(99);
    if (s.isDefault && s.type == StyleType::Paragraph) prio = -1;
    items.push_back({prio, {std::string(s.id), DisplayName(s.name), s.type}});
  }
  for (const Builtin& b : kBuiltins) {
    if (!b.gallery || FindByName(doc, b.name)) continue;
    if (b.id == "Normal" && doc.DefaultStyle(StyleType::Paragraph)) continue;
    items.push_back({b.id == "Normal" ? -1 : b.priority, {std::string(), DisplayName(b.name), b.type}});
  }
  std::stable_sort(items.begin(), items.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  std::vector<StyleEntry> out;
  out.reserve(items.size());
  for (auto& [prio, e] : items) out.push_back(std::move(e));
  return out;
}

std::string EnsureStyle(TreeEdit& ed, std::string_view nameOrId) {
  const Document& doc = ed.Doc();
  if (nameOrId.empty()) return {};
  if (SameName(nameOrId, "Normal"))
    if (const Style* d = doc.DefaultStyle(StyleType::Paragraph)) return std::string(d->id);
  if (const Style* s = FindByName(doc, nameOrId)) return std::string(s->id);
  const Builtin* b = FindBuiltin(nameOrId);
  return b ? AddBuiltin(ed, *b) : std::string();
}

void ApplyStyle(TreeEdit& ed, Range range, std::string_view nameOrId) {
  const Document& doc = ed.Doc();
  if (ed.EnsurePart(PartKind::Styles) == kNoElem) return;
  ed.Begin(L"Style");
  std::string id = EnsureStyle(ed, nameOrId);
  const Style* s = id.empty() ? nullptr : doc.FindStyle(id);
  if (s && (s->type == StyleType::Paragraph || s->type == StyleType::Character)) {
    Unhide(ed, *s);
    std::vector<uint32_t> paras = ParagraphsOf(doc, range);
    auto len = paras.empty() ? 0u : static_cast<uint32_t>(doc.Para(paras.front()).text.size());
    bool partial = !range.Empty() && paras.size() == 1 && range.from.para == range.to.para &&
                   !(range.from.offset == 0 && range.to.offset >= len);
    const Style* linked = s->link == kNone ? nullptr : &doc.Styles()[s->link];
    if (s->type == StyleType::Character) {
      RunFormat f;
      f.charStyle = s->isDefault ? std::string() : id;
      FormatRuns(ed, range, f);
    } else if (partial && linked && linked->type == StyleType::Character) {
      RunFormat f;
      f.charStyle = std::string(linked->id);
      Unhide(ed, *linked);
      FormatRuns(ed, range, f);
    } else {
      Style copy = *s;  // ApplyParaStyle doesn't reindex, but keep it independent of the vector
      for (uint32_t p : paras) ApplyParaStyle(ed, p, copy);
    }
  }
  ed.Commit();
}

void ModifyStyle(TreeEdit& ed, std::string_view styleId, const RunFormat& run, const ParaFormat& para) {
  const Document& doc = ed.Doc();
  const Style* s = doc.FindStyle(styleId);
  if (!s || (s->type != StyleType::Paragraph && s->type != StyleType::Character)) return;
  ElemId elem = s->node.Id();
  bool isPara = s->type == StyleType::Paragraph;
  std::string basedOn = s->basedOn == kNone ? std::string() : std::string(doc.Styles()[s->basedOn].id);
  ed.Begin(L"Modify style");
  if (run.Any()) {
    RunFormat r = run;
    r.charStyle.reset();
    WriteRunFormat(ed, ed.Child(elem, kRPr), r, doc.StyleChainRun(basedOn));
  }
  if (para.Any() && isPara) WriteParaFormat(ed, ed.Child(elem, kPPr), para, doc.StyleChainPara(basedOn));
  ed.TouchAll();
  ed.Commit();
}

void UpdateStyleToMatch(TreeEdit& ed, Range range) {
  const Document& doc = ed.Doc();
  std::vector<uint32_t> paras = ParagraphsOf(doc, range);
  if (paras.empty()) return;
  uint32_t p = paras.front();
  const Style* s = doc.FindStyle(doc.ResolvedPara(p).style);
  if (!s || s->type != StyleType::Paragraph) return;
  Style style = *s;
  ElemId elem = style.node.Id();
  ed.Begin(L"Update style");
  // The paragraph's own formatting moves into the style.
  for (Node c : doc.ParaNode(p).Child(kPPr).Kids()) {
    uint32_t n = c.NameHash();
    if (n == kPStyle || n == kNumPr || n == kRPr || n == kSectPr || n == kPPrChange) continue;
    ed.CopyChild(c.Id(), ed.Child(elem, kPPr));
  }
  // So does its first character's.
  Node first;
  for (const TextRun& r : doc.Runs(p)) {
    if (!r.run || r.displayOnly || r.fieldCode) continue;
    if (!first || (r.start <= range.from.offset && range.from.offset < r.end)) first = r.run;
    if (r.start <= range.from.offset && range.from.offset < r.end) break;
  }
  if (first)
    for (Node c : first.Child(kRPr).Kids())
      if (!c.Is(kRStyle) && !c.Is(kRPrChange)) ed.CopyChild(c.Id(), ed.Child(elem, kRPr));
  // Then the paragraph takes the style again, without what moved.
  ApplyParaStyle(ed, p, style);
  ed.TouchAll();
  ed.Commit();
}

}  // namespace fw::doc::edit
