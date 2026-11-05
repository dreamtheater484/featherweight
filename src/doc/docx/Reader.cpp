// The .docx reader: parts into the model's element arena, everything else into the round-trip
// store (plan §4.3 rule 1), Markup Compatibility (ECMA-376 Part 3), pairs through the reference
// registry (rule 2).
//
// Each part is read in one pass with XmlPull, without a DOM: a long document's DOM would be
// several times the size of its model (plan §11). The model is built as the events arrive, and
// content the model doesn't know is written into self-contained fragments as it streams by,
// attached as pkg::RoundTripStore::CaptureChildren attaches them: after the previous known
// sibling, else before the next known one, else inside the parent. Only a drawing's own content
// is parsed into a small DOM, for its properties and its text boxes.
#include <algorithm>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <unordered_set>

#include "doc/docx/Docx.h"
#include "doc/model/Utf.h"
#include "pkg/Mce.h"
#include "pkg/XmlDom.h"
#include "pkg/XmlPull.h"
#include "pkg/XmlWriter.h"

namespace fw::doc::docx {

using pkg::Status;
using Event = pkg::XmlPull::Event;

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

bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
bool IsWhitespace(std::string_view s) { return std::all_of(s.begin(), s.end(), IsSpace); }

std::string_view AsText(const std::vector<uint8_t>& b) {
  return std::string_view(reinterpret_cast<const char*>(b.data()), b.size());
}

// Calls fn for each whitespace-separated token of a list ("w14 wp14").
template <class F>
void ForEachToken(std::string_view list, F&& fn) {
  size_t i = 0;
  while (i < list.size()) {
    while (i < list.size() && IsSpace(list[i])) ++i;
    size_t start = i;
    while (i < list.size() && !IsSpace(list[i])) ++i;
    if (i > start) fn(list.substr(start, i - start));
  }
}

// Content that isn't an element, as a fragment.
std::string TextFragment(std::string_view text, bool cdata) {
  pkg::XmlWriter w;
  if (cdata) w.CData(text);
  else w.Text(text);
  return w.Take();
}

std::string CommentFragment(std::string_view text) {
  pkg::XmlWriter w;
  w.Comment(text);
  return w.Take();
}

std::string PIFragment(std::string_view target, std::string_view data) {
  pkg::XmlWriter w;
  w.ProcessingInstruction(target, data);
  return w.Take();
}

// The XML declaration at the start of a part, as a fragment (it now says UTF-8, as the part will
// be written), or empty when there is none. XmlPull has checked it.
std::string DeclarationFragment(std::string_view src) {
  if (!src.starts_with("<?xml") || src.size() < 6 || !IsSpace(src[5])) return {};
  size_t end = src.find("?>");
  if (end == std::string_view::npos) return {};
  std::string_view body = src.substr(5, end - 5);
  std::vector<std::pair<std::string_view, std::string_view>> attrs;
  size_t i = 0;
  while (i < body.size()) {
    while (i < body.size() && IsSpace(body[i])) ++i;
    size_t name = i;
    while (i < body.size() && body[i] != '=' && !IsSpace(body[i])) ++i;
    std::string_view n = body.substr(name, i - name);
    while (i < body.size() && body[i] != '"' && body[i] != '\'') ++i;
    if (n.empty() || i >= body.size()) break;
    char quote = body[i++];
    size_t close = body.find(quote, i);
    if (close == std::string_view::npos) break;
    attrs.emplace_back(n, n == "encoding" ? std::string_view("UTF-8") : body.substr(i, close - i));
    i = close + 1;
  }
  pkg::XmlWriter w;
  w.Declaration(attrs);
  return w.Take();
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

// An element's subtree as a self-contained fragment, recorded from the event stream the way
// pkg::RoundTripStore::SerializeFragment writes one from a DOM: the prefixes the subtree uses
// (in mc prefix lists too) but doesn't declare on its root are declared there, as bound around it.
class Recorder {
 public:
  struct Attr {
    std::string qname, nsUri, value;
  };
  static std::vector<Attr> AttrsOf(const pkg::XmlPull& pull) {
    std::vector<Attr> out;
    out.reserve(pull.Attributes().size());
    for (const auto& a : pull.Attributes()) out.push_back({std::string(a.qname), std::string(a.nsUri), std::string(a.value)});
    return out;
  }

  Recorder(std::string_view qname, std::string_view nsUri, std::vector<Attr> attrs) : root_(qname), attrs_(std::move(attrs)) {
    for (const auto& a : attrs_)
      if (a.qname == "xmlns" || a.qname.starts_with("xmlns:")) declared_.emplace_back(pkg::QNameLocal(a.qname == "xmlns" ? "" : a.qname));
    Use(qname, nsUri, attrs_);
  }

  // An event inside the root (not its own start or end).
  void Feed(const pkg::XmlPull& pull, Event e) {
    switch (e) {
      case Event::StartElement:
        body_.StartElement(pull.QName());
        for (const auto& a : pull.Attributes()) body_.Attribute(a.qname, a.value);
        Use(pull.QName(), pull.NamespaceUri(), pull.Attributes());
        break;
      case Event::EndElement: body_.EndElement(); break;
      case Event::Text: body_.Text(pull.Text()); break;
      case Event::CData: body_.CData(pull.Text()); break;
      case Event::Comment: body_.Comment(pull.Text()); break;
      case Event::ProcessingInstruction: body_.ProcessingInstruction(pull.QName(), pull.Text()); break;
      default: break;
    }
  }
  void FeedText(std::string_view text, bool cdata) {
    if (cdata) body_.CData(text);
    else body_.Text(text);
  }

  // At the root's end, while the bindings around it are still in effect.
  std::string Finish(const pkg::XmlPull& pull) {
    pkg::XmlWriter w;
    w.StartElement(root_);
    for (const auto& prefix : used_) {
      if (std::find(declared_.begin(), declared_.end(), prefix) != declared_.end()) continue;
      auto ns = pull.LookupNamespace(prefix);
      if (!ns || (prefix.empty() && ns->empty())) continue;
      w.Attribute(prefix.empty() ? std::string("xmlns") : "xmlns:" + prefix, *ns);
    }
    for (const auto& a : attrs_) w.Attribute(a.qname, a.value);
    std::string body = body_.Take();
    if (!body.empty()) w.Raw(body);
    w.EndElement();
    return w.Take();
  }

 private:
  template <class Attrs>
  void Use(std::string_view qname, std::string_view nsUri, const Attrs& attrs) {
    Add(pkg::QNamePrefix(qname));
    bool isChoice = nsUri == pkg::mce::kNamespace && pkg::QNameLocal(qname) == "Choice";
    for (const auto& a : attrs) {
      std::string_view name = a.qname, value = a.value;
      if (name == "xmlns" || name.starts_with("xmlns:")) continue;
      std::string_view prefix = pkg::QNamePrefix(name), local = pkg::QNameLocal(name);
      if (!prefix.empty()) Add(prefix);
      if (isChoice && prefix.empty() && local == "Requires") ForEachToken(value, [&](std::string_view t) { Add(t); });
      if (prefix.empty() || std::string_view(a.nsUri) != pkg::mce::kNamespace) continue;
      if (local == "Ignorable" || local == "MustUnderstand")
        ForEachToken(value, [&](std::string_view t) { Add(t); });
      else if (local == "ProcessContent" || local == "PreserveElements" || local == "PreserveAttributes")
        ForEachToken(value, [&](std::string_view t) { Add(pkg::QNamePrefix(t)); });
    }
  }
  void Add(std::string_view prefix) {
    if (prefix == "xml" || prefix == "xmlns") return;
    if (std::find(used_.begin(), used_.end(), prefix) == used_.end()) used_.emplace_back(prefix);
  }

  std::string root_;
  std::vector<Attr> attrs_;
  std::vector<std::string> declared_, used_;
  pkg::XmlWriter body_;
};

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

  // One open element of the part being read.
  struct Frame {
    enum Kind : uint8_t {
      Known,      // an element of the model
      Text,       // a w:t (or w:instrText...), until markup inside it means it must be kept whole
      Skip,       // content the model doesn't read (recorded when it's kept)
      Alternate,  // an mc:AlternateContent block
      Branch,     // its chosen branch: what's in it is shown in the block's place
    };
    Kind kind = Skip;
    bool capture = false;      // what the model doesn't know goes to the store
    bool displayOnly = false;  // the elements made here are shown, not written
    bool direct = false;       // Known and Text: a child of the owner's element, not of a branch in it
    bool object = false;       // Known: a drawing, picture or equation, whose content is kept as it is
    bool chosen = false;       // Alternate: a branch was chosen
    bool pairEnd = false;      // Skip: the kept element is one end of a pair
    bool sawKnown = false, onlySpace = true;  // Known: as CaptureChildren tracks them
    bool kept = false;         // Known and Text: it or a child has content in the store
    int recorder = -1;         // the recorder whose root this frame's element is
    uint32_t attrMark = 0;     // Known: where the children's attributes start in the arena
    ElemId elem = kNoElem;
    uint32_t owner = 0;        // the Known frame whose element this frame's elements are linked into
    ElemId last = kNoElem;     // Known: the element's last child so far
    ElemId prevLast = kNoElem; // Text: the owner's last child before this one
    uint32_t children = 0;     // Known: child nodes so far
    pkg::NodeId prevNode = pkg::kNoNode;
    std::vector<pkg::Fragment> pending;  // Known: unknown children before the first known one
    std::string space;         // Known: its whitespace, while that is all it holds
    std::string objectXml;     // Known, object: its content, for the drawing's properties
    // Text: what it held so far, in case it must become a fragment after all.
    struct Piece {
      bool cdata;
      uint32_t start, length;  // in the paragraph's text
    };
    std::vector<Piece> pieces;
    std::string qname, nsUri;
    std::vector<Recorder::Attr> attrs;
  };

  Status ReadPart(const std::string& name, PartKind kind);
  // Reads the element `pull` is on, made as `root`, and everything in it.
  Status Stream(pkg::XmlPull& pull, ElemId root, bool capture, bool checkRootBindings);
  void OnStart(const pkg::XmlPull& pull);
  void OnEnd(const pkg::XmlPull& pull);
  void OnText(const pkg::XmlPull& pull, Event e);
  void OnMarkup(const pkg::XmlPull& pull, Event e);
  void StartKnown(const pkg::XmlPull& pull, size_t at, bool checkBindings);
  ElemId KnownChild(const pkg::XmlPull& pull, ElemId parent, ElemId& last, bool displayOnly);
  bool ReadAttributes(const pkg::XmlPull& pull, ElemId elem, bool capture);
  void InternProperties(const Frame& f);
  int StartRecorder(const pkg::XmlPull& pull);
  void PushSkip(int recorder = -1, bool pairEnd = false);
  void Unknown(size_t at, std::string xml, bool pairEnd);
  void SawKnown(size_t at, pkg::NodeId id);
  void TextToFragment(size_t at);
  bool ChoiceUnderstood(const pkg::XmlPull& pull) const;
  void CheckMustUnderstand(const pkg::XmlPull& pull);
  void CheckBindings(const pkg::XmlPull& pull);
  void Enter(Frame& f);
  void Exit(const Frame& f);
  void ReadDrawing(Frame& f);
  void FindTextBoxes(pugi::xml_node from, uint32_t drawing);
  void ReadTextBoxes();
  void SetUpPrefixes(Part& part, const pkg::XmlPull& pull);
  void ReadTheme(std::string_view name);
  void ReadCommentsExtended(std::string_view name);
  std::u16string& Text() { return doc_.paras_[curPara_].text; }
  Elem& E(ElemId id) { return doc_.tree_.elems[id]; }

  Document& doc_;
  pkg::mce::Understood understood_;
  std::unordered_set<std::string> pairSites_;  // "{ns}local" of elements that open or close a pair
  pkg::NodeId nextNode_ = 1;
  uint32_t curPart_ = kNone, curStory_ = kNone, curPara_ = kNone;
  bool checkMust_ = false;  // mc:MustUnderstand is checked while a part is read, not its text boxes again
  std::vector<Frame> frames_;
  std::vector<std::unique_ptr<Recorder>> recorders_;  // innermost last; each sees every event inside its root
  struct PendingTextBox {
    std::string xml;  // the w:txbxContent, self-contained
    uint32_t drawing;
  };
  std::vector<PendingTextBox> textBoxes_;
  std::unordered_map<std::string, uint32_t> partsRead_;  // PartKey -> index in doc_.parts_
  std::unordered_multimap<uint64_t, ElemId> propertySets_;  // hash -> the first element of an interned set
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

// ---------- The event loop ----------

Status Reader::Stream(pkg::XmlPull& pull, ElemId root, bool capture, bool checkRootBindings) {
  frames_.clear();
  recorders_.clear();
  Frame& f = frames_.emplace_back();
  f.kind = Frame::Known;
  f.elem = root;
  f.capture = capture;
  f.displayOnly = (E(root).flags & kDisplayOnly) != 0;
  StartKnown(pull, 0, checkRootBindings);
  while (!frames_.empty()) {
    Event e = pull.Next();
    switch (e) {
      case Event::StartElement: OnStart(pull); break;
      case Event::EndElement: OnEnd(pull); break;
      case Event::Text:
      case Event::CData: OnText(pull, e); break;
      case Event::Comment:
      case Event::ProcessingInstruction: OnMarkup(pull, e); break;
      case Event::Error: return pull.GetStatus();
      case Event::EndDocument: return Status::Fail(Status::Code::Damaged, "This part ends unexpectedly.");
    }
  }
  return Status{};
}

void Reader::StartKnown(const pkg::XmlPull& pull, size_t at, bool checkBindings) {
  Frame& f = frames_[at];
  f.kept = ReadAttributes(pull, f.elem, f.capture);
  f.attrMark = static_cast<uint32_t>(doc_.tree_.attrs.size());
  if (checkBindings) CheckBindings(pull);
  const Schema& s = Schema::Get();
  f.object = s.Type(s.Elem(E(f.elem).def).type).role == Role::Object;
  Enter(f);
}

void Reader::OnStart(const pkg::XmlPull& pull) {
  for (auto& r : recorders_) r->Feed(pull, Event::StartElement);
  if (checkMust_) CheckMustUnderstand(pull);
  size_t top = frames_.size() - 1;
  switch (frames_[top].kind) {
    case Frame::Skip: PushSkip(); return;
    case Frame::Text:
      // Markup inside text: the whole element is kept as it is.
      TextToFragment(top);
      if (frames_[top].recorder >= 0) recorders_.back()->Feed(pull, Event::StartElement);
      PushSkip();
      return;
    case Frame::Alternate: {
      bool mce = pull.NamespaceUri() == pkg::mce::kNamespace;
      bool take = !frames_[top].chosen && mce &&
                  ((pull.LocalName() == "Choice" && ChoiceUnderstood(pull)) || pull.LocalName() == "Fallback");
      if (!take) {
        PushSkip();
        return;
      }
      frames_[top].chosen = true;
      uint32_t owner = frames_[top].owner;
      Frame& branch = frames_.emplace_back();
      branch.kind = Frame::Branch;
      branch.owner = owner;
      return;
    }
    default: break;
  }
  // A child of a Known element, or of the chosen branch of a block in one.
  bool direct = frames_[top].kind == Frame::Known;
  uint32_t owner = direct ? static_cast<uint32_t>(top) : frames_[top].owner;
  bool capture = direct && frames_[top].capture;
  bool displayOnly = !direct || frames_[owner].displayOnly;
  if (direct) {
    frames_[top].onlySpace = false;
    ++frames_[top].children;
    if (frames_[top].object) {  // an object's content is kept as it is
      PushSkip(StartRecorder(pull));
      return;
    }
  }
  if (pull.NamespaceUri() == pkg::mce::kNamespace && pull.LocalName() == "AlternateContent") {
    // The whole block is kept and written back; the chosen branch is shown.
    int recorder = capture ? StartRecorder(pull) : -1;
    Frame& ac = frames_.emplace_back();
    ac.kind = Frame::Alternate;
    ac.owner = owner;
    ac.recorder = recorder;
    return;
  }
  ElemId prevLast = frames_[owner].last;
  ElemId id = KnownChild(pull, frames_[owner].elem, frames_[owner].last, displayOnly);
  if (id == kNoElem) {
    if (!capture) {
      PushSkip();
      return;
    }
    std::string expanded = "{" + std::string(pull.NamespaceUri()) + "}" + std::string(pull.LocalName());
    PushSkip(StartRecorder(pull), pairSites_.contains(expanded));
    return;
  }
  const Schema& s = Schema::Get();
  bool text = s.Type(s.Elem(E(id).def).type).role == Role::Text;
  // A w:t is known once its end shows it held only text; until then, CaptureChildren hasn't seen it.
  if (direct && !text) SawKnown(top, E(id).node);
  Frame& f = frames_.emplace_back();
  f.kind = text ? Frame::Text : Frame::Known;
  f.elem = id;
  f.owner = owner;
  f.direct = direct;
  f.capture = capture;
  f.displayOnly = displayOnly;
  f.prevLast = prevLast;
  if (text) {
    f.qname = pull.QName();
    f.nsUri = pull.NamespaceUri();
    if (!pull.Attributes().empty()) f.attrs = Recorder::AttrsOf(pull);
  }
  StartKnown(pull, frames_.size() - 1, true);
}

void Reader::OnEnd(const pkg::XmlPull& pull) {
  size_t at = frames_.size() - 1;
  std::string xml;
  bool recorded = frames_[at].recorder >= 0;
  if (recorded) {
    xml = recorders_.back()->Finish(pull);
    recorders_.pop_back();
  }
  for (auto& r : recorders_) r->Feed(pull, Event::EndElement);
  Frame& f = frames_[at];
  switch (f.kind) {
    case Frame::Skip:
    case Frame::Alternate:
      if (recorded) Unknown(at - 1, std::move(xml), f.pairEnd);
      break;
    case Frame::Branch: break;
    case Frame::Text:
      E(f.elem).length = static_cast<uint32_t>(Text().size()) - E(f.elem).start;
      if (f.direct) SawKnown(at - 1, E(f.elem).node);
      Exit(f);
      break;
    case Frame::Known: {
      pkg::NodeId node = E(f.elem).node;
      if (f.capture) {
        for (auto& frag : f.pending) {
          frag.slot = pkg::Slot::Inside;
          doc_.store_.Add(node, std::move(frag));
        }
        // Whitespace that is an element's only content is content (canonical XML keeps it).
        if (f.children > 0 && f.onlySpace) {
          doc_.store_.Add(node, pkg::Fragment{pkg::Slot::Inside, {}, TextFragment(f.space, false)});
          f.kept = true;
        }
        if (!f.kept) InternProperties(f);
      }
      if (f.object) ReadDrawing(f);
      Exit(f);
      break;
    }
  }
  if (frames_[at].kept && at > 0) frames_[at - 1].kept = true;
  frames_.pop_back();
}

void Reader::OnText(const pkg::XmlPull& pull, Event e) {
  for (auto& r : recorders_) r->Feed(pull, e);
  size_t at = frames_.size() - 1;
  Frame& f = frames_[at];
  std::string_view text = pull.Text();
  if (f.kind == Frame::Text) {
    auto start = static_cast<uint32_t>(Text().size());
    AppendUtf16(Text(), text);
    f.pieces.push_back({e == Event::CData, start, static_cast<uint32_t>(Text().size()) - start});
    return;
  }
  if (f.kind != Frame::Known) return;  // recorded, or in a branch that isn't shown
  ++f.children;
  if (e == Event::Text && IsWhitespace(text)) {  // formatting between elements, not content
    f.space = text;
    if (f.object) f.objectXml += TextFragment(text, false);
    return;
  }
  f.onlySpace = false;
  Unknown(at, TextFragment(text, e == Event::CData), false);
}

void Reader::OnMarkup(const pkg::XmlPull& pull, Event e) {
  for (auto& r : recorders_) r->Feed(pull, e);
  size_t at = frames_.size() - 1;
  if (frames_[at].kind == Frame::Text) {
    TextToFragment(at);
    if (frames_[at].recorder >= 0) recorders_.back()->Feed(pull, e);
    return;
  }
  if (frames_[at].kind != Frame::Known) return;
  frames_[at].onlySpace = false;
  ++frames_[at].children;
  Unknown(at, e == Event::Comment ? CommentFragment(pull.Text()) : PIFragment(pull.QName(), pull.Text()), false);
}

// ---------- Unknown content ----------

int Reader::StartRecorder(const pkg::XmlPull& pull) {
  recorders_.push_back(std::make_unique<Recorder>(pull.QName(), pull.NamespaceUri(), Recorder::AttrsOf(pull)));
  return static_cast<int>(recorders_.size()) - 1;
}

void Reader::PushSkip(int recorder, bool pairEnd) {
  Frame& f = frames_.emplace_back();
  f.kind = Frame::Skip;
  f.recorder = recorder;
  f.pairEnd = pairEnd;
}

// Content the model doesn't know, in the element of frame `at`: attached to its nearest known
// neighbor, as RoundTripStore::CaptureChildren does.
void Reader::Unknown(size_t at, std::string xml, bool pairEnd) {
  Frame& p = frames_[at];
  if (p.object) p.objectXml += xml;
  if (!p.capture) return;
  p.kept = true;
  pkg::Fragment frag;
  frag.xml = std::move(xml);
  frag.pairEnd = pairEnd;
  if (!p.sawKnown) {
    p.pending.push_back(std::move(frag));
  } else if (p.prevNode != pkg::kNoNode) {
    frag.slot = pkg::Slot::After;
    doc_.store_.Add(p.prevNode, std::move(frag));
  } else {
    frag.slot = pkg::Slot::Inside;
    doc_.store_.Add(E(p.elem).node, std::move(frag));
  }
}

void Reader::SawKnown(size_t at, pkg::NodeId id) {
  Frame& p = frames_[at];
  if (!p.capture) return;
  if (!p.sawKnown) {
    p.sawKnown = true;
    for (auto& frag : p.pending) {
      frag.slot = pkg::Slot::Before;
      doc_.store_.Add(id, std::move(frag));
    }
    p.pending.clear();
  }
  p.prevNode = id;
}

void Reader::TextToFragment(size_t at) {
  Frame& f = frames_[at];
  Frame& owner = frames_[f.owner];
  // Undo the element. It is the arena's last: a w:t has no elements of its own.
  ElemId id = f.elem;
  Elem& e = E(id);
  std::u16string& text = Text();
  std::vector<std::pair<std::string, bool>> pieces;
  for (const auto& p : f.pieces) pieces.emplace_back(ToUtf8(std::u16string_view(text).substr(p.start, p.length)), p.cdata);
  text.resize(e.start);
  if (e.attrCount) doc_.tree_.attrs.resize(e.attrs);
  if (e.node) doc_.store_.Delete(e.node);
  owner.last = f.prevLast;
  if (f.prevLast == kNoElem) E(owner.elem).first = kNoElem;
  else E(f.prevLast).next = kNoElem;
  doc_.tree_.elems.pop_back();
  f.kind = Frame::Skip;
  f.elem = kNoElem;
  if (!f.direct || !f.capture) return;
  // Kept whole, from its start tag on.
  recorders_.push_back(std::make_unique<Recorder>(f.qname, f.nsUri, std::move(f.attrs)));
  f.recorder = static_cast<int>(recorders_.size()) - 1;
  for (const auto& [piece, cdata] : pieces) recorders_.back()->FeedText(piece, cdata);
}

// ---------- Markup Compatibility and namespaces ----------

// An mc:Choice is taken when every namespace its Requires names is understood.
bool Reader::ChoiceUnderstood(const pkg::XmlPull& pull) const {
  const auto* required = pull.FindAttribute({}, "Requires");
  bool all = true;
  ForEachToken(required ? required->value : std::string_view(), [&](std::string_view prefix) {
    auto ns = pull.LookupNamespace(prefix);
    if (!ns || ns->empty() || !understood_.Has(*ns)) all = false;
  });
  return all;
}

// A part with an mc:MustUnderstand namespace that isn't understood is shown but not edited.
void Reader::CheckMustUnderstand(const pkg::XmlPull& pull) {
  const auto* must = pull.FindAttribute(pkg::mce::kNamespace, "MustUnderstand");
  if (!must) return;
  auto& list = doc_.parts_[curPart_].mustUnderstand;
  ForEachToken(must->value, [&](std::string_view prefix) {
    auto ns = pull.LookupNamespace(prefix);
    if (!ns || ns->empty() || understood_.Has(*ns)) return;
    if (std::find(list.begin(), list.end(), *ns) == list.end()) list.emplace_back(*ns);
  });
}

// An inner element that rebinds a prefix the writer uses would make the writer's names mean
// something else below it.
void Reader::CheckBindings(const pkg::XmlPull& pull) {
  if (curPart_ == kNone) return;
  Part& part = doc_.parts_[curPart_];
  for (const auto& a : pull.Attributes()) {
    if (!a.qname.starts_with("xmlns:")) continue;
    for (size_t ns = 0; ns < kNsCount; ++ns)
      if (!part.prefix[ns].empty() && part.prefix[ns] == a.local && NsFromUri(a.value) != static_cast<Ns>(ns))
        part.writable = false;
  }
}

// ---------- Known elements ----------

// Returns whether unknown attributes went to the store.
bool Reader::ReadAttributes(const pkg::XmlPull& pull, ElemId elem, bool capture) {
  const Schema& s = Schema::Get();
  TypeId type = s.Elem(E(elem).def).type;
  std::vector<pkg::XmlAttr> unknown;
  auto first = static_cast<uint32_t>(doc_.tree_.attrs.size());
  uint16_t count = 0;
  for (const auto& a : pull.Attributes()) {
    const Ns ns = NsFromUri(a.nsUri);
    AttrDefId def = a.IsNamespaceDecl() ? kNoDef : s.AttrOf(type, NameOf(ns, a.local));
    // NameOf spells w: and unqualified names alike: one in another namespace than its
    // definition isn't that attribute, and is kept as it is.
    if (def != kNoDef && s.Attr(def).ns != ns) def = kNoDef;
    if (def != kNoDef && count < 0xFFFF) {
      doc_.tree_.attrs.push_back(AttrVal{def, doc_.tree_.strings.Intern(a.value)});
      ++count;
    } else if (capture) {
      unknown.push_back({std::string(a.qname), std::string(a.nsUri), std::string(a.value)});
    }
  }
  E(elem).attrs = count ? first : 0;
  E(elem).attrCount = count;
  if (unknown.empty()) return false;
  doc_.store_.SetNodeAttributes(E(elem).node, doc_.store_.Intern(std::move(unknown)));
  return true;
}

// Documents repeat the same few property sets on every run, paragraph and cell (half of a long
// report's elements). A container whose children are plain properties shares them with the first
// identical set: its new children are dropped from the arena's end, where they were just made.
void Reader::InternProperties(const Frame& f) {
  static constexpr uint32_t kContainers[] = {Name("rPr"),     Name("pPr"),   Name("tabs"),       Name("numPr"),
                                             Name("pBdr"),    Name("tcPr"),  Name("tcBorders"),  Name("tcMar"),
                                             Name("trPr"),    Name("tblPr"), Name("tblBorders"), Name("tblCellMar")};
  const Schema& s = Schema::Get();
  ElemId c = f.elem;
  const Elem& e = E(c);
  uint32_t name = s.Elem(e.def).name;
  if (e.first == kNoElem || std::find(std::begin(kContainers), std::end(kContainers), name) == std::end(kContainers)) return;
  // Only plain properties: elements without children, made one after another just now, none of
  // them a revision mark or anything else with an id of its own.
  const Tree& t = doc_.tree_;
  uint64_t hash = 1469598103934665603ull;
  auto mix = [&](uint64_t v) { hash = (hash ^ v) * 1099511628211ull; };
  mix(e.def);
  size_t n = 0;
  for (ElemId k = e.first; k != kNoElem; k = t.elems[k].next, ++n) {
    const Elem& ke = t.elems[k];
    if (k != c + 1 + n || ke.first != kNoElem || ke.flags != 0) return;
    mix(ke.def);
    mix(ke.attrCount);
    for (uint32_t i = 0; i < ke.attrCount; ++i) {
      const AttrVal& a = t.attrs[ke.attrs + i];
      if (s.Attr(a.def).name == Name("id")) return;
      mix((uint64_t{a.def} << 32) | a.value);
    }
  }
  if (c + 1 + n != t.elems.size()) return;
  auto same = [&](ElemId first) {
    ElemId k = first;
    for (size_t i = 0; i < n; ++i, k = t.elems[k].next) {
      if (k == kNoElem) return false;
      const Elem &a = t.elems[k], &b = t.elems[c + 1 + i];
      if (a.def != b.def || a.attrCount != b.attrCount) return false;
      for (uint32_t j = 0; j < a.attrCount; ++j)
        if (t.attrs[a.attrs + j].def != t.attrs[b.attrs + j].def || t.attrs[a.attrs + j].value != t.attrs[b.attrs + j].value)
          return false;
    }
    return k == kNoElem;
  };
  for (auto [it, end] = propertySets_.equal_range(hash); it != end; ++it) {
    if (!same(it->second)) continue;
    E(c).first = it->second;
    E(c).flags |= kSharedKids;
    doc_.tree_.elems.resize(c + 1);
    doc_.tree_.attrs.resize(f.attrMark);
    return;
  }
  propertySets_.emplace(hash, c + 1);
}

ElemId Reader::KnownChild(const pkg::XmlPull& pull, ElemId parent, ElemId& last, bool displayOnly) {
  const Schema& s = Schema::Get();
  TypeId ptype = s.Elem(E(parent).def).type;
  const Ns ns = NsFromUri(pull.NamespaceUri());
  ElemDefId def = s.Child(ptype, NameOf(ns, pull.LocalName()));
  // NameOf spells w: and unqualified names alike: an element in another namespace than its
  // definition (an unprefixed "tmpl" in no namespace) isn't Word's, and is kept as it is.
  if (def == kNoDef || s.Elem(def).ns != ns) return kNoElem;
  Role role = s.Type(s.Elem(def).type).role;
  bool inPara = curPara_ != kNone;
  bool textual = role == Role::Text || role == Role::Char || role == Role::Object || role == Role::Marker;
  if ((textual || role == Role::Run) && !inPara) return kNoElem;
  if ((role == Role::Paragraph || role == Role::Story) && inPara) return kNoElem;
  ElemId id = doc_.tree_.Add(def, parent);
  if (displayOnly) E(id).flags |= kDisplayOnly;
  else E(id).node = nextNode_++;
  if (last == kNoElem) E(parent).first = id;
  else E(last).next = id;
  last = id;
  return id;
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
    case Role::Object:  // the drawing is read at its end (ReadDrawing)
      E(f.elem).start = static_cast<uint32_t>(Text().size());
      Text() += u'\xFFFC';
      E(f.elem).length = 1;
      break;
    default:  // Role::Text too: its text arrives as events
      if (inPara) E(f.elem).start = static_cast<uint32_t>(Text().size());
      break;
  }
}

void Reader::Exit(const Frame& f) {
  const Schema& s = Schema::Get();
  Role role = s.Type(s.Elem(E(f.elem).def).type).role;
  if (role == Role::Paragraph) {
    E(f.elem).length = static_cast<uint32_t>(Text().size());
    Text().shrink_to_fit();  // the text grew piece by piece; a quarter of it would be spare capacity
    curPara_ = kNone;
  } else if (role == Role::Story) {
    curStory_ = kNone;
  } else if (curPara_ != kNone && role != Role::Text && role != Role::Char && role != Role::Object) {
    E(f.elem).length = static_cast<uint32_t>(Text().size()) - E(f.elem).start;
  }
}

// ---------- Drawings ----------

void ReadDrawingFrame(pugi::xml_node frame, Drawing& d) {
  auto attrEmu = [](pugi::xml_node n, const char* name) { return Emu(ParseInt(n.attribute(name).value()).value_or(0)); };
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

void Reader::ReadDrawing(Frame& f) {
  ElemId elem = f.elem;
  Drawing d;
  d.node = Node(&doc_.tree_, elem);
  d.pos = Pos{curPara_, E(elem).start};
  d.part = curPart_;
  d.displayOnly = f.displayOnly;
  // The drawing's content as a small DOM: its fragments are self-contained, so a plain wrapper
  // holds them.
  pkg::XmlDom dom;
  pugi::xml_node obj;
  if (dom.Parse("<fw>" + f.objectXml + "</fw>", doc_.package_->GetLimits()).Ok()) obj = dom.Root();
  std::string_view local = Schema::Get().Elem(E(elem).def).local;
  if (local == "drawing") {
    d.kind = DrawingKind::DrawingML;
    pugi::xml_node frame;
    for (auto c : obj.children())
      if (pkg::IsElement(c, kWp, "inline") || pkg::IsElement(c, kWp, "anchor")) frame = c;
    if (frame) ReadDrawingFrame(frame, d);
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
  auto index = static_cast<uint32_t>(doc_.drawings_.size());
  if (local == "drawing" || local == "pict" || local == "object") FindTextBoxes(obj, index);
  // Nothing of it is in the store when it's shown from a kept block or a text box: keep it here.
  if (!f.capture) d.keptXml = std::move(f.objectXml);
  doc_.drawings_.push_back(std::move(d));
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
      textBoxes_.push_back({pkg::RoundTripStore::SerializeFragment(n), drawing});
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
  bool checkMust = checkMust_;
  checkMust_ = false;  // the part's own pass saw these elements
  for (size_t i = 0; i < textBoxes_.size(); ++i) {
    std::string xml = std::move(textBoxes_[i].xml);
    uint32_t drawing = textBoxes_[i].drawing;
    pkg::XmlPull pull(xml, doc_.package_->GetLimits());
    Event e;
    do e = pull.Next();
    while (e != Event::StartElement && e != Event::Error && e != Event::EndDocument);
    if (e != Event::StartElement) continue;
    ElemId root = doc_.tree_.Add(def, kNoElem);
    E(root).flags |= kDetached;
    uint32_t savedPara = curPara_;
    curPara_ = kNone;
    Stream(pull, root, false, true);  // our own serialization: it can't be damaged
    curPara_ = savedPara;
    // Stream made the story; link it to its drawing.
    for (size_t k = doc_.stories_.size(); k-- > 0;) {
      if (doc_.stories_[k].root != root) continue;
      doc_.stories_[k].drawing = drawing;
      doc_.drawings_[drawing].textBoxes.push_back(static_cast<uint32_t>(k));
      break;
    }
  }
  checkMust_ = checkMust;
  textBoxes_.clear();
}

// ---------- Parts ----------

void Reader::SetUpPrefixes(Part& part, const pkg::XmlPull& pull) {
  // The writer names each namespace with the prefix the root declares for it, and declares the
  // ones the root doesn't.
  bool strict = false;
  // A prefix is taken when the root declares it, or when the part names it: an undeclared prefix
  // in a qualified name, or in an mc:Ignorable or mc:MustUnderstand list, would change meaning if
  // the writer declared it.
  const std::string_view source = pull.Source();
  auto nameChar = [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' ||
           c == '_' || static_cast<unsigned char>(c) >= 0x80;
  };
  auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
  auto named = [&](std::string_view prefix) {
    const std::string colon = std::string(prefix) + ':';
    for (size_t at = source.find(colon); at != std::string_view::npos; at = source.find(colon, at + 1))
      if (at == 0 || !nameChar(source[at - 1])) return true;
    // Markup Compatibility lists name prefixes without a colon.
    for (std::string_view list : {std::string_view("Ignorable"), std::string_view("MustUnderstand")}) {
      for (size_t at = source.find(list); at != std::string_view::npos; at = source.find(list, at + 1)) {
        size_t q = at + list.size();
        while (q < source.size() && space(source[q])) ++q;
        if (q >= source.size() || source[q] != '=') continue;
        ++q;
        while (q < source.size() && space(source[q])) ++q;
        if (q >= source.size() || (source[q] != '"' && source[q] != '\'')) continue;
        const size_t close = source.find(source[q], q + 1);
        if (close == std::string_view::npos) break;
        for (size_t p = q + 1; p < close;) {
          while (p < close && space(source[p])) ++p;
          size_t e = p;
          while (e < close && !space(source[e])) ++e;
          if (source.substr(p, e - p) == prefix) return true;
          p = e;
        }
      }
    }
    return false;
  };
  auto declared = [&](std::string_view prefix) {
    for (const auto& a : pull.Attributes())
      if (a.IsNamespaceDecl() && (prefix.empty() ? a.qname == "xmlns" : a.qname != "xmlns" && a.local == prefix)) return true;
    return !prefix.empty() && named(prefix);
  };
  for (const auto& a : pull.Attributes()) {
    if (!a.qname.starts_with("xmlns:")) continue;
    Ns ns = NsFromUri(a.value);
    if (ns == Ns::W && IsStrictUri(a.value)) strict = true;
    if (ns == Ns::W || ns == Ns::R || ns == Ns::W14 || ns == Ns::M) {
      size_t i = static_cast<size_t>(ns);
      if (part.prefix[i].empty()) {
        part.prefix[i] = std::string(a.local);
        part.uri[i] = a.value;
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
  pkg::XmlPull pull(AsText(bytes), pkg.GetLimits(), name.substr(1));

  // What comes before the root (the declaration, comments) is kept before it.
  std::vector<std::string> prolog;
  Event e;
  for (;;) {
    e = pull.Next();
    if (e == Event::Error) return pull.GetStatus();
    if (e == Event::StartElement || e == Event::EndDocument) break;
    if (e == Event::Comment) prolog.push_back(CommentFragment(pull.Text()));
    else if (e == Event::ProcessingInstruction) prolog.push_back(PIFragment(pull.QName(), pull.Text()));
  }
  if (e != Event::StartElement) return Status::Fail(Status::Code::Damaged, "Part " + name.substr(1) + " is damaged.");
  if (std::string decl = DeclarationFragment(pull.Source()); !decl.empty()) prolog.insert(prolog.begin(), std::move(decl));

  const Schema& s = Schema::Get();
  const Ns rootNs = NsFromUri(pull.NamespaceUri());
  ElemDefId rootDef = s.Child(s.RootType(), NameOf(rootNs, pull.LocalName()));
  if (rootDef != kNoDef && s.Elem(rootDef).ns != rootNs) rootDef = kNoDef;  // as in KnownChild
  if (rootDef == kNoDef || s.Elem(rootDef).local == "txbxContent") {
    if (kind == PartKind::Document)
      return Status::Fail(Status::Code::Unsupported, "This file isn't a Word document: its main part is something else.");
    // Not WordprocessingML: the part stays as it is, but it must still be well-formed.
    while ((e = pull.Next()) != Event::EndDocument)
      if (e == Event::Error) return pull.GetStatus();
    return Status{};
  }

  Part part;
  part.name = name;
  part.kind = kind;
  part.docNode = nextNode_++;
  part.root = doc_.tree_.Add(rootDef, kNoElem);
  doc_.tree_.elems[part.root].node = nextNode_++;
  curPart_ = static_cast<uint32_t>(doc_.parts_.size());
  partsRead_[key] = curPart_;
  doc_.parts_.push_back(std::move(part));
  ElemId root = doc_.parts_[curPart_].root;
  pkg::NodeId rootNode = E(root).node;
  for (auto& xml : prolog) doc_.store_.Add(rootNode, pkg::Fragment{pkg::Slot::Before, {}, std::move(xml)});

  // The prefixes the writer uses must be known before inner elements are checked against them.
  SetUpPrefixes(doc_.parts_[curPart_], pull);
  checkMust_ = true;
  CheckMustUnderstand(pull);
  if (!(st = Stream(pull, root, true, false)).Ok()) return st;
  // What comes after the root is kept after it.
  while ((e = pull.Next()) != Event::EndDocument) {
    if (e == Event::Error) return pull.GetStatus();
    if (e == Event::Comment) doc_.store_.Add(rootNode, pkg::Fragment{pkg::Slot::After, {}, CommentFragment(pull.Text())});
    else if (e == Event::ProcessingInstruction)
      doc_.store_.Add(rootNode, pkg::Fragment{pkg::Slot::After, {}, PIFragment(pull.QName(), pull.Text())});
  }
  ReadTextBoxes();
  checkMust_ = false;
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
  // The arena grew in steps; read-only from here on (until D2 edits), it needs no spare room.
  doc_.tree_.elems.shrink_to_fit();
  doc_.tree_.attrs.shrink_to_fit();
  doc_.paras_.shrink_to_fit();
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
