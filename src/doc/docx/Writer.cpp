// The .docx writer: a part from the model's elements and the round-trip store's fragments
// (plan §4.3 rule 1). Known elements come from the model; what the model doesn't know is put back
// where it was; display-only elements (an AlternateContent branch shown in place of the kept
// block) aren't written. Text-box stories live inside their drawing's kept XML: an edited one is
// written in place of each copy of its text there (a DrawingML choice and its VML fallback alike),
// the others as they were read.
#include <unordered_map>

#include "doc/docx/Docx.h"
#include "doc/model/Utf.h"
#include "pkg/Mce.h"
#include "pkg/RoundTrip.h"
#include "pkg/XmlDom.h"
#include "pkg/XmlWriter.h"

namespace fw::doc::docx {

using pkg::Status;

namespace {

// The namespace `prefix` ("" for the default) has at `node`, from the declarations there and above.
std::string_view InScope(pugi::xml_node node, std::string_view prefix) {
  const std::string decl = prefix.empty() ? std::string("xmlns") : "xmlns:" + std::string(prefix);
  for (pugi::xml_node n = node; n; n = n.parent())
    if (pugi::xml_attribute a = n.attribute(decl.c_str())) return a.value();
  return {};
}

std::string_view PrefixOfName(std::string_view qname) {
  const size_t colon = qname.find(':');
  return colon == std::string_view::npos ? std::string_view() : qname.substr(0, colon);
}

// Puts a copy of `box` (a w:txbxContent read with the declarations it needs above it) in place of
// `old`, declaring on the copy what isn't the same where it goes.
void Replace(pugi::xml_node old, pugi::xml_node box) {
  pugi::xml_node parent = old.parent();
  pugi::xml_node put = parent.insert_copy_before(box, old);
  parent.remove_child(old);
  std::vector<std::string> prefixes;
  auto use = [&](std::string_view qname, bool element) {
    std::string_view p = PrefixOfName(qname);
    if (p == "xml" || p == "xmlns" || qname == "xmlns" || (!element && p.empty())) return;
    if (std::find(prefixes.begin(), prefixes.end(), p) == prefixes.end()) prefixes.emplace_back(p);
  };
  std::vector<pugi::xml_node> work{box};
  while (!work.empty()) {
    pugi::xml_node n = work.back();
    work.pop_back();
    if (n.type() != pugi::node_element) continue;
    use(n.name(), true);
    for (pugi::xml_attribute a : n.attributes()) use(a.name(), false);
    for (pugi::xml_node c = n.first_child(); c; c = c.next_sibling()) work.push_back(c);
  }
  for (const std::string& p : prefixes) {
    const std::string_view want = InScope(box, p);
    if (want.empty() || InScope(put, p) == want) continue;
    const std::string decl = p.empty() ? std::string("xmlns") : "xmlns:" + p;
    put.prepend_attribute(decl.c_str()) = std::string(want).c_str();
  }
}

}  // namespace

class Writer {
 public:
  Writer(const Document& doc, const Part& part, int depth = 0) : doc_(doc), part_(part), depth_(depth) {}
  // The part.
  Status Write(std::string& out);

 private:
  std::string QName(Ns ns, std::string_view local) const;
  void Walk(ElemId root);
  void Open(ElemId id);
  void Close(ElemId id);
  // Kept content of `owner` in `slot` (Inside: the fragments first among its children).
  void Fragments(pkg::NodeId owner, pkg::Slot slot);
  void FindEditedBoxes();
  bool WithTextBoxes(uint32_t drawing, std::vector<std::string>& xmls);

  const Document& doc_;
  const Part& part_;
  int depth_;  // text boxes inside text boxes
  pkg::XmlWriter w_;
  std::string utf8_;
  Status failed_;
  // Drawings whose text boxes are written from their stories: by the node whose Inside fragments
  // are the drawing (a w:drawing or w:pict), or by the tag of the mc:AlternateContent block it is.
  std::unordered_map<pkg::NodeId, uint32_t> byNode_;
  std::unordered_map<uint32_t, uint32_t> byTag_;
};

std::string Writer::QName(Ns ns, std::string_view local) const {
  if (ns == Ns::None) return std::string(local);
  const std::string& prefix = part_.prefix[static_cast<size_t>(ns)];
  if (prefix.empty()) return std::string(local);
  std::string q;
  q.reserve(prefix.size() + 1 + local.size());
  q += prefix;
  q += ':';
  q += local;
  return q;
}

void Writer::FindEditedBoxes() {
  for (uint32_t i = 0; i < doc_.drawings_.size(); ++i) {
    const Drawing& d = doc_.drawings_[i];
    if (d.textBoxes.empty() || d.pos.para == kNone || d.part >= doc_.parts_.size() || &doc_.parts_[d.part] != &part_)
      continue;
    bool edited = false;
    for (uint32_t s : d.textBoxes) edited = edited || (s < doc_.stories_.size() && doc_.stories_[s].edits > 0);
    if (!edited) continue;
    if (d.kept != 0) byTag_[d.kept] = i;
    else if (!d.displayOnly && d.node) byNode_[d.node.Raw().node] = i;
  }
}

// The kept XML of drawing `i` (`xmls`, its fragments in order) with each copy of its text boxes'
// text written from their stories: in each branch of its mc:AlternateContent block, or in the
// drawing's own content. False when it doesn't hold them as the reader found them.
bool Writer::WithTextBoxes(uint32_t i, std::vector<std::string>& xmls) {
  const Drawing& d = doc_.drawings_[i];
  if (!d.boxesWritable || depth_ > 16) return false;
  // Each story, read back with the part's declarations around it.
  std::string decls;
  for (Ns which : {Ns::W, Ns::R, Ns::W14, Ns::M}) {  // the model's own (SetUpPrefixes names each)
    const size_t ns = static_cast<size_t>(which);
    const std::string& p = part_.prefix[ns];
    if (p.empty() || part_.uri[ns].empty()) continue;
    decls += " xmlns:" + p + "=\"" + part_.uri[ns] + '"';
  }
  std::vector<pkg::XmlDom> stories(d.textBoxes.size());
  std::vector<pugi::xml_node> boxes;
  for (size_t k = 0; k < d.textBoxes.size(); ++k) {
    const Story& st = doc_.stories_[d.textBoxes[k]];
    Writer inner(doc_, part_, depth_ + 1);
    inner.Walk(st.root);
    if (!inner.failed_.Ok()) return false;
    const std::string xml = "<fw" + decls + ">" + inner.w_.Take() + "</fw>";
    if (!stories[k].Parse(xml, doc_.Package().GetLimits()).Ok()) return false;
    pugi::xml_node box = stories[k].Root().first_child();
    while (box && box.type() != pugi::node_element) box = box.next_sibling();
    if (!box) return false;
    boxes.push_back(box);
  }
  std::vector<pkg::XmlDom> doms(xmls.size());
  for (size_t k = 0; k < xmls.size(); ++k)
    if (!doms[k].Parse(xmls[k], doc_.Package().GetLimits()).Ok()) return false;
  // Where each copy goes: per branch of a block, else through the drawing's fragments in order.
  std::vector<std::vector<pugi::xml_node>> copies;
  if (doms.size() == 1 && pkg::IsElement(doms[0].Root(), pkg::mce::kNamespace, "AlternateContent")) {
    for (pugi::xml_node b = doms[0].Root().first_child(); b; b = b.next_sibling())
      if (b.type() == pugi::node_element) copies.push_back(TextBoxesIn(b));
  } else {
    std::vector<pugi::xml_node> all;
    for (pkg::XmlDom& dom : doms) {
      pugi::xml_node holder = dom.Doc();  // the fragment's root may itself be a text box's holder
      for (pugi::xml_node n : TextBoxesIn(holder)) all.push_back(n);
    }
    copies.push_back(std::move(all));
  }
  if (copies.empty()) return false;
  for (const auto& places : copies)
    if (places.size() != boxes.size()) return false;
  for (const auto& places : copies)
    for (size_t k = 0; k < places.size(); ++k) Replace(places[k], boxes[k]);
  for (size_t k = 0; k < xmls.size(); ++k) xmls[k] = pkg::RoundTripStore::SerializeFragment(doms[k].Root());
  return true;
}

void Writer::Fragments(pkg::NodeId owner, pkg::Slot slot) {
  const pkg::RoundTripStore& store = doc_.store_;
  std::vector<const pkg::Fragment*> list =
      slot == pkg::Slot::Inside ? store.FragmentsAfterChild(owner, "") : store.FragmentsOf(owner, slot);
  if (list.empty()) return;
  if (slot == pkg::Slot::Inside && !byNode_.empty()) {
    if (auto it = byNode_.find(owner); it != byNode_.end()) {
      std::vector<std::string> xmls;
      for (const pkg::Fragment* f : list) xmls.push_back(f->xml);
      if (!WithTextBoxes(it->second, xmls)) {
        failed_ = Status::Fail(Status::Code::Unsupported, "A text box's text couldn't be written into its drawing.");
        return;
      }
      for (const std::string& x : xmls) w_.Raw(x);
      return;
    }
  }
  for (const pkg::Fragment* f : list) {
    if (f->tag != 0 && !byTag_.empty())
      if (auto it = byTag_.find(f->tag); it != byTag_.end()) {
        std::vector<std::string> xmls{f->xml};
        if (!WithTextBoxes(it->second, xmls)) {
          failed_ = Status::Fail(Status::Code::Unsupported, "A text box's text couldn't be written into its drawing.");
          return;
        }
        w_.Raw(xmls[0]);
        continue;
      }
    w_.Raw(f->xml);
  }
}

void Writer::Open(ElemId id) {
  const Schema& s = Schema::Get();
  const Tree& t = doc_.tree_;
  const pkg::RoundTripStore& store = doc_.store_;
  const Elem& e = t.elems[id];
  const ElemDef& def = s.Elem(e.def);
  Fragments(e.node, pkg::Slot::Before);
  w_.StartElement(QName(def.ns, def.local));
  for (uint32_t i = 0; i < e.attrCount; ++i) {
    const AttrVal& a = t.attrs[e.attrs + i];
    const AttrDef& ad = s.Attr(a.def);
    w_.Attribute(QName(ad.ns, ad.local), t.strings.Get(a.value));
  }
  store.WriteAttributes(w_, store.NodeAttributes(e.node));
  if (id == part_.root)
    for (const auto& [prefix, uri] : part_.addDecls) w_.Attribute("xmlns:" + prefix, uri);
  Fragments(e.node, pkg::Slot::Inside);
  if (s.Type(def.type).role == Role::Text) {
    uint32_t para = doc_.ParagraphOf(Node(&t, id));
    if (para != kNone) {
      std::u16string_view text = doc_.paras_[para].text;
      utf8_.clear();
      AppendUtf8(utf8_, text.substr(std::min<size_t>(e.start, text.size()), e.length));
      if (!utf8_.empty()) w_.Text(utf8_);
    }
  }
}

void Writer::Close(ElemId id) {
  const Elem& e = doc_.tree_.elems[id];
  Fragments(e.node, pkg::Slot::End);
  w_.EndElement();
  Fragments(e.node, pkg::Slot::After);
}

void Writer::Walk(ElemId root) {
  FindEditedBoxes();
  const Tree& t = doc_.tree_;
  // Depth first, iteratively: (element, closing?).
  std::vector<std::pair<ElemId, bool>> stack{{root, false}};
  while (!stack.empty() && failed_.Ok()) {
    auto [id, closing] = stack.back();
    stack.pop_back();
    if (closing) {
      Close(id);
      continue;
    }
    if (t.elems[id].flags & kDisplayOnly) continue;
    Open(id);
    stack.emplace_back(id, true);
    size_t mark = stack.size();
    for (ElemId c = t.elems[id].first; c != kNoElem; c = t.elems[c].next) stack.emplace_back(c, false);
    std::reverse(stack.begin() + static_cast<std::ptrdiff_t>(mark), stack.end());
  }
}

Status Writer::Write(std::string& out) {
  if (!part_.writable)
    return Status::Fail(Status::Code::Unsupported,
                        "Part " + part_.name.substr(1) + " redeclares a namespace prefix inside it, so it is kept as it is.");
  Walk(part_.root);
  if (!failed_.Ok()) return failed_;
  out = w_.Take();
  return Status{};
}

Status WritePart(const Document& doc, uint32_t part, std::string& out) {
  if (part >= doc.Parts().size()) return Status::Fail(Status::Code::NotFound, "There is no such part.");
  Writer w(doc, doc.Parts()[part]);
  return w.Write(out);
}

Status WriteAllParts(Document& doc, size_t* written, size_t* skipped) {
  size_t n = 0, s = 0;
  for (uint32_t i = 0; i < doc.Parts().size(); ++i) {
    if (!doc.Parts()[i].writable) {
      ++s;
      continue;
    }
    std::string xml;
    Status st = WritePart(doc, i, xml);
    if (!st.Ok()) return st;
    doc.Package().SetPart(doc.Parts()[i].name, std::vector<uint8_t>(xml.begin(), xml.end()));
    ++n;
  }
  if (written) *written = n;
  if (skipped) *skipped = s;
  return Status{};
}

}  // namespace fw::doc::docx
