// The .docx reader: parts into the model's element arena, everything else into the round-trip
// store (plan §4.3 rule 1), Markup Compatibility through pkg::mce, pairs through the reference
// registry (rule 2).
#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

#include "doc/docx/Docx.h"
#include "doc/model/Utf.h"
#include "pkg/Mce.h"
#include "pkg/XmlDom.h"
#include "pkg/XmlPull.h"

namespace fw::doc::docx {

using pkg::Status;

namespace {

constexpr std::string_view kWp = "http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing";
constexpr std::string_view kWp14 = "http://schemas.microsoft.com/office/word/2010/wordprocessingDrawing";
constexpr std::string_view kA = "http://schemas.openxmlformats.org/drawingml/2006/main";
constexpr std::string_view kA14 = "http://schemas.microsoft.com/office/drawing/2010/main";
constexpr std::string_view kPic = "http://schemas.openxmlformats.org/drawingml/2006/picture";
constexpr std::string_view kWps = "http://schemas.microsoft.com/office/word/2010/wordprocessingShape";
constexpr std::string_view kWpg = "http://schemas.microsoft.com/office/word/2010/wordprocessingGroup";
constexpr std::string_view kWpc = "http://schemas.microsoft.com/office/word/2010/wordprocessingCanvas";
constexpr std::string_view kV = "urn:schemas-microsoft-com:vml";
constexpr std::string_view kO = "urn:schemas-microsoft-com:office:office";
constexpr std::string_view kW10 = "urn:schemas-microsoft-com:office:word";
constexpr std::string_view kW15 = "http://schemas.microsoft.com/office/word/2012/wordml";

constexpr std::string_view kWordMainTypes[] = {
    "application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml",
    "application/vnd.ms-word.document.macroEnabled.main+xml",
    "application/vnd.openxmlformats-officedocument.wordprocessingml.template.main+xml",
    "application/vnd.ms-word.template.macroEnabledTemplate.main+xml",
};

// The last segment of a relationship type: "styles", "header", "officeDocument".
std::string_view RelKind(std::string_view type) {
  size_t slash = type.rfind('/');
  return slash == std::string_view::npos ? type : type.substr(slash + 1);
}

bool IsWhitespace(std::string_view s) {
  return std::all_of(s.begin(), s.end(), [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; });
}

std::string_view AsText(const std::vector<uint8_t>& b) {
  return std::string_view(reinterpret_cast<const char*>(b.data()), b.size());
}

// "position:absolute;margin-left:10pt;width:144pt" -> the value of one property.
std::string_view CssValue(std::string_view style, std::string_view prop) {
  size_t i = 0;
  while (i < style.size()) {
    size_t end = std::min(style.find(';', i), style.size());
    std::string_view decl = style.substr(i, end - i);
    i = end + 1;
    size_t colon = decl.find(':');
    if (colon == std::string_view::npos) continue;
    std::string_view name = decl.substr(0, colon), value = decl.substr(colon + 1);
    while (!name.empty() && name.front() == ' ') name.remove_prefix(1);
    while (!name.empty() && name.back() == ' ') name.remove_suffix(1);
    while (!value.empty() && value.front() == ' ') value.remove_prefix(1);
    while (!value.empty() && value.back() == ' ') value.remove_suffix(1);
    if (name == prop) return value;
  }
  return {};
}

// A CSS length as VML writes it ("144pt", "1.5in", "2cm", "96px", or a bare number of pixels... or
// of EMUs inside groups, which layout reads from the coordinate system instead).
std::optional<Emu> CssLength(std::string_view v) {
  if (v.empty()) return std::nullopt;
  if (v.ends_with("px")) {
    auto n = ParseMeasure(v.substr(0, v.size() - 2), 1);  // whole pixels
    if (!n) return std::nullopt;
    return Emu(*n * 9525);
  }
  auto pt100 = ParseMeasure(v, 100);  // hundredths of a point
  if (!pt100 || (v.back() >= '0' && v.back() <= '9')) return std::nullopt;
  return Emu(*pt100 * 127);
}

}  // namespace

const std::vector<std::string_view>& DefaultUnderstood() {
  static const std::vector<std::string_view> v = {kWp, kWp14, kA, kA14, kPic, kWps, kWpg, kWpc, kV, kO, kW10};
  return v;
}

class Reader {
 public:
  static std::unique_ptr<Document> Read(std::unique_ptr<pkg::Package> package, Status& status, const ReadOptions& options);

 private:
  Reader(Document& doc, const ReadOptions& options);
  Status Run();

  struct Binding {
    std::string_view prefix, uri;
  };
  struct Frame {
    pugi::xml_node xml;
    ElemId elem = kNoElem;
    size_t scope = 0;
    bool exit = false;
    bool capture = true;  // keep unknown content in the store (false for display-only and text-box trees)
  };
  struct Kid {
    pugi::xml_node xml;
    ElemId elem;
    bool capture;
  };

  Status ReadPart(const std::string& name, PartKind kind);
  void Walk(pugi::xml_node xml, ElemId elem, bool capture);
  void Enter(Frame& f);
  void Exit(const Frame& f);
  void ReadAttributes(pugi::xml_node xml, ElemId elem, bool capture);
  std::vector<Kid> ReadChildren(pugi::xml_node xml, ElemId elem, bool capture);
  // Creates the elements for the known children of `xml` (display-only ones from AlternateContent
  // branches too), in order, linked after `last`.
  ElemId KnownChild(pugi::xml_node child, ElemId parent, ElemId& last, bool displayOnly);
  void DisplayBranch(pugi::xml_node ac, ElemId parent, ElemId& last, std::vector<Kid>& kids);
  std::string_view Resolve(pugi::xml_node element, std::string_view prefix) const;
  Ns NsOf(pugi::xml_node element) const { return NsFromUri(Resolve(element, pkg::QNamePrefix(element.name()))); }
  void PushBindings(pugi::xml_node xml, bool rootOfPart);
  void ReadDrawing(pugi::xml_node obj, ElemId elem, bool displayOnly);
  void FindTextBoxes(pugi::xml_node from, uint32_t drawing);
  void ReadTextBoxes();
  void SetUpPrefixes(Part& part, pugi::xml_node rootXml);
  void ReadTheme(std::string_view name);
  void ReadCommentsExtended(std::string_view name);
  std::u16string& Text() { return doc_.paras_[curPara_].text; }
  Elem& E(ElemId id) { return doc_.tree_.elems[id]; }

  Document& doc_;
  pkg::mce::Understood understood_;
  std::unordered_set<std::string> pairSites_;  // "{ns}local" of elements that open or close a pair
  std::vector<Binding> scope_;
  pkg::NodeId nextNode_ = 1;
  uint32_t curPart_ = kNone, curStory_ = kNone, curPara_ = kNone;
  struct PendingTextBox {
    pugi::xml_node txbx;
    uint32_t drawing;
  };
  std::vector<PendingTextBox> textBoxes_;
  std::unordered_map<std::string, uint32_t> partsRead_;  // PartKey -> index in doc_.parts_
};

Reader::Reader(Document& doc, const ReadOptions& options) : doc_(doc) {
  for (Ns ns : {Ns::W, Ns::R, Ns::W14, Ns::M}) understood_.Add(NsUri(ns));
  understood_.Add("http://purl.oclc.org/ooxml/wordprocessingml/main");
  understood_.Add("http://purl.oclc.org/ooxml/officeDocument/relationships");
  understood_.Add("http://purl.oclc.org/ooxml/officeDocument/math");
  if (options.understood.empty()) {
    for (auto uri : DefaultUnderstood()) understood_.Add(uri);
  } else {
    for (const auto& uri : options.understood) understood_.Add(uri);
  }
  for (const auto& rule : doc_.registry_->Rules()) {
    if (rule.format != pkg::DocFormat::Word && rule.format != pkg::DocFormat::Any) continue;
    for (const auto& site : rule.sites)
      if ((site.role & (pkg::kOpens | pkg::kCloses)) && !site.elementLocal.empty())
        pairSites_.insert("{" + std::string(site.elementNs) + "}" + std::string(site.elementLocal));
  }
}

std::string_view Reader::Resolve(pugi::xml_node element, std::string_view prefix) const {
  if (prefix == "xml") return pkg::kXmlNamespace;
  for (auto a : element.attributes()) {
    std::string_view n = a.name();
    if (prefix.empty() ? n == "xmlns" : (n.size() == prefix.size() + 6 && n.starts_with("xmlns:") && n.substr(6) == prefix))
      return a.value();
  }
  for (size_t i = scope_.size(); i-- > 0;)
    if (scope_[i].prefix == prefix) return scope_[i].uri;
  return {};
}

void Reader::PushBindings(pugi::xml_node xml, bool rootOfPart) {
  for (auto a : xml.attributes()) {
    std::string_view n = a.name();
    if (n == "xmlns") {
      scope_.push_back({{}, a.value()});
    } else if (n.starts_with("xmlns:")) {
      std::string_view prefix = n.substr(6), uri = a.value();
      scope_.push_back({prefix, uri});
      // An inner element that rebinds a prefix the writer uses would make the writer's names
      // mean something else below it.
      if (!rootOfPart && curPart_ != kNone) {
        const Part& part = doc_.parts_[curPart_];
        for (size_t ns = 0; ns < kNsCount; ++ns)
          if (!part.prefix[ns].empty() && part.prefix[ns] == prefix && NsFromUri(uri) != static_cast<Ns>(ns))
            doc_.parts_[curPart_].writable = false;
      }
    }
  }
}

void Reader::ReadAttributes(pugi::xml_node xml, ElemId elem, bool capture) {
  const Schema& s = Schema::Get();
  TypeId type = s.Elem(E(elem).def).type;
  std::vector<pkg::XmlAttr> unknown;
  uint32_t first = static_cast<uint32_t>(doc_.tree_.attrs.size());
  uint16_t count = 0;
  for (auto a : xml.attributes()) {
    std::string_view name = a.name();
    bool decl = name == "xmlns" || name.starts_with("xmlns:");
    std::string_view prefix = pkg::QNamePrefix(name);
    std::string_view uri = decl ? pkg::kXmlnsNamespace : prefix.empty() ? std::string_view() : Resolve(xml, prefix);
    AttrDefId def = decl ? kNoDef : s.AttrOf(type, NameOf(NsFromUri(uri), pkg::QNameLocal(name)));
    if (def != kNoDef && count < 0xFFFF) {
      doc_.tree_.attrs.push_back(AttrVal{def, doc_.tree_.strings.Intern(a.value())});
      ++count;
    } else if (capture) {
      unknown.push_back({name.data(), std::string(uri), a.value()});
    }
  }
  E(elem).attrs = count ? first : 0;
  E(elem).attrCount = count;
  if (!unknown.empty()) doc_.store_.SetNodeAttributes(E(elem).node, doc_.store_.Intern(std::move(unknown)));
}

ElemId Reader::KnownChild(pugi::xml_node child, ElemId parent, ElemId& last, bool displayOnly) {
  const Schema& s = Schema::Get();
  TypeId ptype = s.Elem(E(parent).def).type;
  ElemDefId def = s.Child(ptype, NameOf(NsOf(child), pkg::QNameLocal(child.name())));
  if (def == kNoDef) return kNoElem;
  Role role = s.Type(s.Elem(def).type).role;
  bool inPara = curPara_ != kNone;
  bool textual = role == Role::Text || role == Role::Char || role == Role::Object || role == Role::Marker;
  if ((textual || role == Role::Run) && !inPara) return kNoElem;
  if ((role == Role::Paragraph || role == Role::Story) && inPara) return kNoElem;
  if (role == Role::Text) {
    for (auto c : child.children())
      if (c.type() != pugi::node_pcdata && c.type() != pugi::node_cdata) return kNoElem;  // keep it whole
  }
  ElemId id = doc_.tree_.Add(def, parent);
  if (displayOnly) E(id).flags |= kDisplayOnly;
  else E(id).node = nextNode_++;
  if (last == kNoElem) E(parent).first = id;
  else E(last).next = id;
  last = id;
  return id;
}

void Reader::DisplayBranch(pugi::xml_node ac, ElemId parent, ElemId& last, std::vector<Kid>& kids) {
  // The chosen branch's elements in document order, with nested blocks replaced by their own
  // chosen branch (an explicit stack, like everything else).
  std::vector<pugi::xml_node> work;
  auto pushChildren = [&](pugi::xml_node branch) {
    for (auto c = branch.last_child(); c; c = c.previous_sibling())
      if (c.type() == pugi::node_element) work.push_back(c);
  };
  pushChildren(pkg::mce::SelectBranch(ac, understood_));
  while (!work.empty()) {
    pugi::xml_node c = work.back();
    work.pop_back();
    if (pkg::IsElement(c, pkg::mce::kNamespace, "AlternateContent")) {
      pushChildren(pkg::mce::SelectBranch(c, understood_));
      continue;
    }
    ElemId id = KnownChild(c, parent, last, true);
    if (id != kNoElem) kids.push_back({c, id, false});
  }
}

std::vector<Reader::Kid> Reader::ReadChildren(pugi::xml_node xml, ElemId elem, bool capture) {
  std::vector<Kid> kids;
  ElemId last = kNoElem;
  if (!capture) {
    for (auto c : xml.children()) {
      if (c.type() != pugi::node_element) continue;
      if (pkg::IsElement(c, pkg::mce::kNamespace, "AlternateContent")) {
        DisplayBranch(c, elem, last, kids);
        continue;
      }
      ElemId id = KnownChild(c, elem, last, (E(elem).flags & kDisplayOnly) != 0);
      if (id != kNoElem) kids.push_back({c, id, false});
    }
    return kids;
  }
  Role role = Schema::Get().Type(Schema::Get().Elem(E(elem).def).type).role;
  bool opaque = role == Role::Object;
  doc_.store_.CaptureChildren(
      xml, E(elem).node,
      [&](pugi::xml_node c) -> pkg::NodeId {
        if (opaque) return pkg::kNoNode;  // an object's content is kept as it is
        if (pkg::IsElement(c, pkg::mce::kNamespace, "AlternateContent")) {
          // The whole block is kept and written back; the chosen branch is shown.
          DisplayBranch(c, elem, last, kids);
          return pkg::kNoNode;
        }
        ElemId id = KnownChild(c, elem, last, false);
        if (id == kNoElem) return pkg::kNoNode;
        kids.push_back({c, id, true});
        return E(id).node;
      },
      [&](pugi::xml_node c) { return pairSites_.contains(pkg::ExpandedName(c)); });
  // Whitespace that is an element's only content is content (canonical XML keeps it).
  bool onlySpace = xml.first_child() && role != Role::Text;
  for (auto c : xml.children())
    if (c.type() != pugi::node_pcdata || !IsWhitespace(c.value())) onlySpace = false;
  if (onlySpace)
    for (auto c : xml.children())
      doc_.store_.Add(E(elem).node, pkg::Fragment{pkg::Slot::Inside, {}, pkg::RoundTripStore::SerializeFragment(c)});
  return kids;
}

void Reader::Enter(Frame& f) {
  const Schema& s = Schema::Get();
  const ElemDef& def = s.Elem(E(f.elem).def);
  Role role = s.Type(def.type).role;
  bool inPara = curPara_ != kNone;
  switch (role) {
    case Role::Story: {
      Story story;
      story.part = curPart_;
      story.root = f.elem;
      std::string_view local = def.local;
      PartKind kind = curPart_ == kNone ? PartKind::Document : doc_.parts_[curPart_].kind;
      if (local == "body") story.kind = StoryKind::Main;
      else if (local == "footnote") story.kind = StoryKind::Footnote;
      else if (local == "endnote") story.kind = StoryKind::Endnote;
      else if (local == "comment") story.kind = StoryKind::Comment;
      else if (local == "txbxContent") story.kind = StoryKind::TextBox;
      else story.kind = kind == PartKind::Footer ? StoryKind::Footer : StoryKind::Header;
      if (story.kind == StoryKind::Footnote || story.kind == StoryKind::Endnote || story.kind == StoryKind::Comment)
        story.id = Node(&doc_.tree_, f.elem).AttrOr(Name("id"));
      curStory_ = static_cast<uint32_t>(doc_.stories_.size());
      if (story.kind == StoryKind::Main && doc_.main_ == kNone) doc_.main_ = curStory_;
      doc_.stories_.push_back(std::move(story));
      break;
    }
    case Role::Paragraph: {
      curPara_ = static_cast<uint32_t>(doc_.paras_.size());
      Paragraph p;
      p.elem = f.elem;
      p.story = curStory_;
      doc_.paras_.push_back(std::move(p));
      E(f.elem).start = curPara_;
      break;
    }
    case Role::Text: {
      E(f.elem).start = static_cast<uint32_t>(Text().size());
      for (auto c : f.xml.children()) AppendUtf16(Text(), c.value());
      E(f.elem).length = static_cast<uint32_t>(Text().size()) - E(f.elem).start;
      break;
    }
    case Role::Char: {
      Node n(&doc_.tree_, f.elem);
      char16_t ch = s.Type(def.type).ch;
      if (def.local == "br") {
        std::string_view type = n.AttrOr(Name("type"));
        ch = type == "page" ? u'\x0C' : type == "column" ? u'\x0E' : u'\x0B';
      } else if (def.local == "sym") {
        auto code = n.Hex(Name("char"));
        ch = code && *code > 0 && *code < 0xD800 ? static_cast<char16_t>(*code)
             : code && *code >= 0xE000 && *code <= 0xFFFF ? static_cast<char16_t>(*code)
                                                          : u'\xFFFD';
      } else if (def.local == "fldChar") {
        std::string_view type = n.AttrOr(Name("fldCharType"));
        ch = type == "begin" ? u'\x13' : type == "separate" ? u'\x14' : type == "end" ? u'\x15' : u'\xFFFD';
      }
      E(f.elem).start = static_cast<uint32_t>(Text().size());
      Text() += ch;
      E(f.elem).length = 1;
      break;
    }
    case Role::Object:
      E(f.elem).start = static_cast<uint32_t>(Text().size());
      Text() += u'\xFFFC';
      E(f.elem).length = 1;
      ReadDrawing(f.xml, f.elem, (E(f.elem).flags & kDisplayOnly) != 0);
      break;
    default:
      if (inPara) E(f.elem).start = static_cast<uint32_t>(Text().size());
      break;
  }
}

void Reader::Exit(const Frame& f) {
  const Schema& s = Schema::Get();
  Role role = s.Type(s.Elem(E(f.elem).def).type).role;
  if (role == Role::Paragraph) {
    E(f.elem).length = static_cast<uint32_t>(Text().size());
    curPara_ = kNone;
  } else if (role == Role::Story) {
    curStory_ = kNone;
  } else if (curPara_ != kNone && role != Role::Text && role != Role::Char && role != Role::Object) {
    E(f.elem).length = static_cast<uint32_t>(Text().size()) - E(f.elem).start;
  }
  scope_.resize(f.scope);
}

void Reader::Walk(pugi::xml_node xml, ElemId elem, bool capture) {
  std::vector<Frame> stack;
  stack.push_back({xml, elem, scope_.size(), false, capture});
  while (!stack.empty()) {
    Frame f = stack.back();
    stack.pop_back();
    if (f.exit) {
      Exit(f);
      continue;
    }
    f.scope = scope_.size();
    PushBindings(f.xml, f.elem == elem && curPart_ != kNone && doc_.parts_[curPart_].root == elem);
    ReadAttributes(f.xml, f.elem, f.capture);
    Enter(f);
    const Schema& s = Schema::Get();
    Role role = s.Type(s.Elem(E(f.elem).def).type).role;
    std::vector<Kid> kids;
    if (role != Role::Text) kids = ReadChildren(f.xml, f.elem, f.capture);
    stack.push_back({f.xml, f.elem, f.scope, true, f.capture});
    for (size_t i = kids.size(); i-- > 0;) stack.push_back({kids[i].xml, kids[i].elem, 0, false, kids[i].capture});
  }
}

// ---------- Drawings ----------

void Reader::ReadDrawing(pugi::xml_node obj, ElemId elem, bool displayOnly) {
  Drawing d;
  d.node = Node(&doc_.tree_, elem);
  d.pos = Pos{curPara_, E(elem).start};
  d.part = curPart_;
  d.displayOnly = displayOnly;
  std::string_view local = Schema::Get().Elem(E(elem).def).local;
  auto attrEmu = [](pugi::xml_node n, const char* name) { return Emu(ParseInt(n.attribute(name).value()).value_or(0)); };
  if (local == "drawing") {
    d.kind = DrawingKind::DrawingML;
    pugi::xml_node frame;
    for (auto c : obj.children())
      if (pkg::IsElement(c, kWp, "inline") || pkg::IsElement(c, kWp, "anchor")) frame = c;
    if (frame) {
      d.anchored = pkg::LocalNameOf(frame) == "anchor";
      d.wrap = d.anchored ? WrapType::None : WrapType::Inline;
      d.distT = attrEmu(frame, "distT");
      d.distB = attrEmu(frame, "distB");
      d.distL = attrEmu(frame, "distL");
      d.distR = attrEmu(frame, "distR");
      if (d.anchored) {
        d.simplePos = ParseOnOff(frame.attribute("simplePos").value()).value_or(false);
        d.behindDoc = ParseOnOff(frame.attribute("behindDoc").value()).value_or(false);
        d.locked = ParseOnOff(frame.attribute("locked").value()).value_or(false);
        d.layoutInCell = ParseOnOff(frame.attribute("layoutInCell").value()).value_or(true);
        d.allowOverlap = ParseOnOff(frame.attribute("allowOverlap").value()).value_or(true);
        d.hidden = ParseOnOff(frame.attribute("hidden").value()).value_or(false);
        d.relativeHeight = ParseInt(frame.attribute("relativeHeight").value()).value_or(0);
      }
      for (auto c : frame.children()) {
        if (c.type() != pugi::node_element || pkg::NamespaceOf(c) != kWp) continue;
        std::string_view n = pkg::LocalNameOf(c);
        if (n == "extent") {
          d.cx = attrEmu(c, "cx");
          d.cy = attrEmu(c, "cy");
        } else if (n == "effectExtent") {
          d.effectL = attrEmu(c, "l");
          d.effectT = attrEmu(c, "t");
          d.effectR = attrEmu(c, "r");
          d.effectB = attrEmu(c, "b");
        } else if (n == "simplePos") {
          d.simpleX = attrEmu(c, "x");
          d.simpleY = attrEmu(c, "y");
        } else if (n == "positionH" || n == "positionV") {
          bool h = n == "positionH";
          (h ? d.hRelativeFrom : d.vRelativeFrom) = c.attribute("relativeFrom").value();
          for (auto p : c.children()) {
            if (pkg::IsElement(p, kWp, "posOffset")) (h ? d.hOffset : d.vOffset) = Emu(ParseInt(p.text().get()).value_or(0));
            else if (pkg::IsElement(p, kWp, "align")) (h ? d.hAlign : d.vAlign) = p.text().get();
          }
        } else if (n == "wrapNone") {
          d.wrap = WrapType::None;
        } else if (n == "wrapSquare" || n == "wrapTight" || n == "wrapThrough") {
          d.wrap = n == "wrapSquare" ? WrapType::Square : n == "wrapTight" ? WrapType::Tight : WrapType::Through;
          d.wrapText = c.attribute("wrapText").value();
        } else if (n == "wrapTopAndBottom") {
          d.wrap = WrapType::TopAndBottom;
        } else if (n == "docPr") {
          d.id = static_cast<uint32_t>(ParseInt(c.attribute("id").value()).value_or(0));
          d.name = c.attribute("name").value();
          d.descr = c.attribute("descr").value();
          d.title = c.attribute("title").value();
          d.hidden = d.hidden || ParseOnOff(c.attribute("hidden").value()).value_or(false);
        }
      }
    }
  } else if (local == "pict" || local == "object") {
    d.kind = local == "pict" ? DrawingKind::Vml : DrawingKind::Object;
    pugi::xml_node shape;
    for (auto c : obj.children())
      if (c.type() == pugi::node_element && pkg::NamespaceOf(c) == kV && pkg::LocalNameOf(c) != "shapetype") {
        shape = c;
        break;
      }
    if (shape) {
      std::string_view style = shape.attribute("style").value();
      d.anchored = CssValue(style, "position") == "absolute";
      d.wrap = d.anchored ? WrapType::None : WrapType::Inline;
      d.cx = CssLength(CssValue(style, "width")).value_or(Emu(0));
      d.cy = CssLength(CssValue(style, "height")).value_or(Emu(0));
      d.hOffset = CssLength(CssValue(style, "margin-left"));
      d.vOffset = CssLength(CssValue(style, "margin-top"));
      d.hAlign = CssValue(style, "mso-position-horizontal");
      d.vAlign = CssValue(style, "mso-position-vertical");
      d.hRelativeFrom = CssValue(style, "mso-position-horizontal-relative");
      d.vRelativeFrom = CssValue(style, "mso-position-vertical-relative");
      d.relativeHeight = ParseInt(CssValue(style, "z-index")).value_or(0);
      d.behindDoc = d.relativeHeight < 0;
      d.hidden = CssValue(style, "visibility") == "hidden";
      d.name = shape.attribute("id").value();
      d.descr = shape.attribute("alt").value();
      for (auto c : shape.children()) {
        if (!pkg::IsElement(c, kW10, "wrap")) continue;
        std::string_view type = c.attribute("type").value();
        d.wrap = type == "square" ? WrapType::Square : type == "tight" ? WrapType::Tight
                 : type == "through" ? WrapType::Through : type == "topAndBottom" ? WrapType::TopAndBottom
                 : type == "none" ? WrapType::None : d.wrap;
        d.wrapText = c.attribute("side").value();
      }
    }
    if (local == "object") {
      Node n(&doc_.tree_, elem);
      if (auto w = n.Int(Name("dxaOrig")); w && d.cx.Value() == 0) d.cx = Emu(*w * 635);
      if (auto h = n.Int(Name("dyaOrig")); h && d.cy.Value() == 0) d.cy = Emu(*h * 635);
    }
  } else if (local == "contentPart") {
    d.kind = DrawingKind::ContentPart;
  } else {
    d.kind = DrawingKind::Equation;
  }
  if (displayOnly)  // nothing of it is in the store: the AlternateContent block is kept whole
    for (auto c : obj.children()) d.keptXml += pkg::RoundTripStore::SerializeFragment(c);
  auto index = static_cast<uint32_t>(doc_.drawings_.size());
  doc_.drawings_.push_back(std::move(d));
  if (local == "drawing" || local == "pict" || local == "object") FindTextBoxes(obj, index);
}

void Reader::FindTextBoxes(pugi::xml_node from, uint32_t drawing) {
  // Depth first, not into a text box's own content (its text boxes belong to its story) and not
  // into fallbacks (the chosen branch is what's shown).
  std::vector<pugi::xml_node> work;
  for (auto c = from.last_child(); c; c = c.previous_sibling()) work.push_back(c);
  while (!work.empty()) {
    pugi::xml_node n = work.back();
    work.pop_back();
    if (n.type() != pugi::node_element) continue;
    if (pkg::IsElement(n, pkg::mce::kNamespace, "Fallback")) continue;
    if (pkg::LocalNameOf(n) == "txbxContent" && NsFromUri(pkg::NamespaceOf(n)) == Ns::W) {
      textBoxes_.push_back({n, drawing});
      continue;
    }
    for (auto c = n.last_child(); c; c = c.previous_sibling()) work.push_back(c);
  }
}

void Reader::ReadTextBoxes() {
  // Text-box stories are read for display. The drawing that holds them is kept as XML and written
  // back as it was, so their elements stay out of the store (plan §6.3 "Editing": a text box edit
  // regenerates the drawing, from D4).
  const Schema& s = Schema::Get();
  ElemDefId def = s.Child(s.RootType(), Name("txbxContent"));
  for (size_t i = 0; i < textBoxes_.size(); ++i) {
    PendingTextBox tb = textBoxes_[i];
    // The namespaces in scope where the text box is: its ancestors' declarations, outermost first.
    std::vector<pugi::xml_node> chain;
    for (pugi::xml_node n = tb.txbx.parent(); n && n.type() == pugi::node_element; n = n.parent()) chain.push_back(n);
    scope_.clear();
    for (size_t k = chain.size(); k-- > 0;) PushBindings(chain[k], true);
    ElemId root = doc_.tree_.Add(def, kNoElem);
    E(root).flags |= kDetached;
    uint32_t savedPara = curPara_;
    curPara_ = kNone;
    Walk(tb.txbx, root, false);
    curPara_ = savedPara;
    // Walk made the story; link it to its drawing.
    for (size_t k = doc_.stories_.size(); k-- > 0;) {
      if (doc_.stories_[k].root != root) continue;
      doc_.stories_[k].drawing = tb.drawing;
      doc_.drawings_[tb.drawing].textBoxes.push_back(static_cast<uint32_t>(k));
      break;
    }
  }
  textBoxes_.clear();
  scope_.clear();
}

// ---------- Parts ----------

void Reader::SetUpPrefixes(Part& part, pugi::xml_node rootXml) {
  // The writer names each namespace with the prefix the root declares for it, and declares the
  // ones the root doesn't.
  bool strict = false;
  auto declared = [&](std::string_view prefix) {
    for (auto a : rootXml.attributes())
      if (std::string_view(a.name()) == (prefix.empty() ? std::string("xmlns") : "xmlns:" + std::string(prefix)))
        return true;
    return false;
  };
  for (auto a : rootXml.attributes()) {
    std::string_view q = a.name();
    if (!q.starts_with("xmlns:")) continue;
    Ns ns = NsFromUri(a.value());
    if (ns == Ns::W && IsStrictUri(a.value())) strict = true;
    if (ns == Ns::W || ns == Ns::R || ns == Ns::W14 || ns == Ns::M) {
      size_t i = static_cast<size_t>(ns);
      if (part.prefix[i].empty()) {
        part.prefix[i] = std::string(q.substr(6));
        part.uri[i] = a.value();
      }
    }
  }
  for (Ns ns : {Ns::W, Ns::R, Ns::W14, Ns::M}) {
    size_t i = static_cast<size_t>(ns);
    if (!part.prefix[i].empty()) continue;
    std::string uri(NsUri(ns));
    if (strict && ns == Ns::W) uri = "http://purl.oclc.org/ooxml/wordprocessingml/main";
    if (strict && ns == Ns::R) uri = "http://purl.oclc.org/ooxml/officeDocument/relationships";
    if (strict && ns == Ns::M) uri = "http://purl.oclc.org/ooxml/officeDocument/math";
    std::string prefix(NsPrefix(ns));
    while (declared(prefix)) prefix = "fw" + prefix;
    part.prefix[i] = prefix;
    part.uri[i] = uri;
    part.addDecls.emplace_back(prefix, uri);
  }
  part.prefix[static_cast<size_t>(Ns::Xml)] = "xml";
}

Status Reader::ReadPart(const std::string& name, PartKind kind) {
  std::string key = pkg::PartKey(name);
  if (partsRead_.contains(key)) return Status{};
  const pkg::Package& pkg = *doc_.package_;
  if (!pkg.FindPart(name)) return Status{};  // a dangling relationship: fwtool check reports it
  std::vector<uint8_t> bytes;
  Status st = pkg.ReadPart(name, bytes);
  if (!st.Ok()) return st;
  pkg::XmlDom dom;
  if (!(st = dom.Parse(AsText(bytes), pkg.GetLimits(), name.substr(1))).Ok()) return st;
  pugi::xml_node rootXml = dom.Root();
  scope_.clear();
  const Schema& s = Schema::Get();
  ElemDefId rootDef = s.Child(s.RootType(), NameOf(NsOf(rootXml), pkg::QNameLocal(rootXml.name())));
  if (rootDef == kNoDef || s.Elem(rootDef).local == "txbxContent") {
    if (kind == PartKind::Document)
      return Status::Fail(Status::Code::Unsupported, "This file isn't a Word document: its main part is something else.");
    return Status{};  // not WordprocessingML: the part stays as it is
  }

  Part part;
  part.name = name;
  part.kind = kind;
  part.docNode = nextNode_++;
  part.root = doc_.tree_.Add(rootDef, kNoElem);
  doc_.tree_.elems[part.root].node = nextNode_++;
  part.mustUnderstand = pkg::mce::CheckPart(rootXml, understood_).mustUnderstand;
  curPart_ = static_cast<uint32_t>(doc_.parts_.size());
  partsRead_[key] = curPart_;
  doc_.parts_.push_back(std::move(part));

  // What surrounds the root (the declaration, comments) belongs to the part's document node.
  ElemId root = doc_.parts_[curPart_].root;
  doc_.store_.CaptureChildren(dom.Doc(), doc_.parts_[curPart_].docNode, [&](pugi::xml_node c) -> pkg::NodeId {
    return c == rootXml ? doc_.tree_.elems[root].node : pkg::kNoNode;
  });
  // The prefixes the writer uses must be known before inner elements are checked against them.
  SetUpPrefixes(doc_.parts_[curPart_], rootXml);
  Walk(rootXml, root, true);
  ReadTextBoxes();
  curPart_ = kNone;
  return Status{};
}

void Reader::ReadTheme(std::string_view name) {
  std::vector<uint8_t> bytes;
  if (!doc_.package_->FindPart(name) || !doc_.package_->ReadPart(name, bytes).Ok()) return;
  pkg::XmlDom dom;
  if (!dom.Parse(AsText(bytes), doc_.package_->GetLimits()).Ok()) return;
  auto child = [](pugi::xml_node n, std::string_view local) {
    for (auto c : n.children())
      if (pkg::IsElement(c, kA, local)) return c;
    return pugi::xml_node();
  };
  pugi::xml_node elements = child(dom.Root(), "themeElements");
  if (!elements) return;
  Theme& t = doc_.theme_;
  t.present = true;
  static constexpr const char* kSlots[] = {"dk1", "lt1", "dk2", "lt2", "accent1", "accent2", "accent3",
                                           "accent4", "accent5", "accent6", "hlink", "folHlink"};
  pugi::xml_node scheme = child(elements, "clrScheme");
  for (size_t i = 0; i < 12; ++i) {
    pugi::xml_node slot = child(scheme, kSlots[i]);
    if (pugi::xml_node srgb = child(slot, "srgbClr")) t.colors[i] = ParseHex(srgb.attribute("val").value()).value_or(0);
    else if (pugi::xml_node sys = child(slot, "sysClr")) t.colors[i] = ParseHex(sys.attribute("lastClr").value()).value_or(0);
  }
  pugi::xml_node fonts = child(elements, "fontScheme");
  for (auto [which, local] : {std::pair{&t.major, "majorFont"}, std::pair{&t.minor, "minorFont"}}) {
    pugi::xml_node f = child(fonts, local);
    which->latin = child(f, "latin").attribute("typeface").value();
    which->eastAsia = child(f, "ea").attribute("typeface").value();
    which->cs = child(f, "cs").attribute("typeface").value();
    for (auto c : f.children())
      if (pkg::IsElement(c, kA, "font")) which->scripts.emplace_back(c.attribute("script").value(), c.attribute("typeface").value());
  }
}

void Reader::ReadCommentsExtended(std::string_view name) {
  std::vector<uint8_t> bytes;
  if (!doc_.package_->FindPart(name) || !doc_.package_->ReadPart(name, bytes).Ok()) return;
  pkg::XmlDom dom;
  if (!dom.Parse(AsText(bytes), doc_.package_->GetLimits()).Ok()) return;
  for (auto c : dom.Root().children()) {
    if (!pkg::IsElement(c, kW15, "commentEx")) continue;
    Document::CommentEx ex;
    ex.paraId = pkg::FindAttribute(c, kW15, "paraId").value();
    ex.parentParaId = pkg::FindAttribute(c, kW15, "paraIdParent").value();
    ex.done = ParseOnOff(pkg::FindAttribute(c, kW15, "done").value()).value_or(false);
    doc_.commentsEx_.push_back(std::move(ex));
  }
}

Status Reader::Run() {
  const pkg::Package& pkg = *doc_.package_;
  const pkg::RelationshipSet* rootRels = pkg.RelationshipsOf("/");
  std::string mainName;
  if (rootRels)
    for (const auto& r : rootRels->items)
      if (!r.external && RelKind(r.type) == "officeDocument") {
        mainName = r.targetPart;
        break;
      }
  if (mainName.empty() || !pkg.FindPart(mainName))
    return Status::Fail(Status::Code::Unsupported, "This file isn't a Word document: it has no main document part.");
  std::string_view type = pkg.ContentTypeOf(mainName);
  if (std::find(std::begin(kWordMainTypes), std::end(kWordMainTypes), type) == std::end(kWordMainTypes))
    return Status::Fail(Status::Code::Unsupported, "This file isn't a Word document.");

  Status st = ReadPart(mainName, PartKind::Document);
  if (!st.Ok()) return st;
  if (doc_.main_ == kNone)
    return Status::Fail(Status::Code::Damaged, "This Word document is damaged: it has no body.");

  if (const pkg::RelationshipSet* rels = pkg.RelationshipsOf(mainName)) {
    struct Kind {
      std::string_view rel;
      PartKind part;
    };
    static constexpr Kind kKinds[] = {
        {"styles", PartKind::Styles},       {"numbering", PartKind::Numbering}, {"settings", PartKind::Settings},
        {"fontTable", PartKind::FontTable}, {"header", PartKind::Header},       {"footer", PartKind::Footer},
        {"footnotes", PartKind::Footnotes}, {"endnotes", PartKind::Endnotes},   {"comments", PartKind::Comments},
    };
    // Definitions first, then the stories in a stable order.
    for (const Kind& k : kKinds)
      for (const auto& r : rels->items)
        if (!r.external && RelKind(r.type) == k.rel && !(st = ReadPart(r.targetPart, k.part)).Ok()) return st;
    for (const auto& r : rels->items) {
      if (r.external) continue;
      if (RelKind(r.type) == "theme" && !doc_.theme_.present) ReadTheme(r.targetPart);
      else if (RelKind(r.type) == "commentsExtended") ReadCommentsExtended(r.targetPart);
    }
  }
  doc_.BuildIndexes();
  return Status{};
}

std::unique_ptr<Document> Reader::Read(std::unique_ptr<pkg::Package> package, Status& status, const ReadOptions& options) {
  if (!package) {
    status = Status::Fail(Status::Code::NotFound, "There is no package to read.");
    return nullptr;
  }
  std::unique_ptr<Document> doc(new Document());
  doc->package_ = std::move(package);
  if (options.registry) {
    doc->registry_ = options.registry;
  } else {
    doc->ownRegistry_ = std::make_unique<pkg::ReferenceRegistry>(pkg::ReferenceRegistry::Default());
    doc->registry_ = doc->ownRegistry_.get();
  }
  Reader reader(*doc, options);
  status = reader.Run();
  if (!status.Ok()) return nullptr;
  return doc;
}

std::unique_ptr<Document> Read(std::unique_ptr<pkg::Package> package, Status& status, const ReadOptions& options) {
  return Reader::Read(std::move(package), status, options);
}

}  // namespace fw::doc::docx
