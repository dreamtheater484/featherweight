// The .docx writer: a part from the model's elements and the round-trip store's fragments
// (plan §4.3 rule 1). Known elements come from the model; what the model doesn't know is put back
// where it was; display-only elements (an AlternateContent branch shown in place of the kept
// block) and text-box stories (kept inside their drawing) aren't written.
#include "doc/docx/Docx.h"
#include "doc/model/Utf.h"
#include "pkg/XmlWriter.h"

namespace fw::doc::docx {

using pkg::Status;

class Writer {
 public:
  Writer(const Document& doc, const Part& part) : doc_(doc), part_(part) {}
  Status Write(std::string& out);

 private:
  std::string QName(Ns ns, std::string_view local) const;
  void Open(ElemId id);
  void Close(ElemId id);

  const Document& doc_;
  const Part& part_;
  pkg::XmlWriter w_;
  std::string utf8_;
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

void Writer::Open(ElemId id) {
  const Schema& s = Schema::Get();
  const Tree& t = doc_.tree_;
  const pkg::RoundTripStore& store = doc_.store_;
  const Elem& e = t.elems[id];
  const ElemDef& def = s.Elem(e.def);
  store.WriteFragments(w_, e.node, pkg::Slot::Before);
  w_.StartElement(QName(def.ns, def.local));
  for (uint32_t i = 0; i < e.attrCount; ++i) {
    const AttrVal& a = t.attrs[e.attrs + i];
    const AttrDef& ad = s.Attr(a.def);
    w_.Attribute(QName(ad.ns, ad.local), t.strings.Get(a.value));
  }
  store.WriteAttributes(w_, store.NodeAttributes(e.node));
  if (id == part_.root)
    for (const auto& [prefix, uri] : part_.addDecls) w_.Attribute("xmlns:" + prefix, uri);
  store.WriteFragmentsAfterChild(w_, e.node, "");
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
  doc_.store_.WriteFragments(w_, e.node, pkg::Slot::End);
  w_.EndElement();
  doc_.store_.WriteFragments(w_, e.node, pkg::Slot::After);
}

Status Writer::Write(std::string& out) {
  if (!part_.writable)
    return Status::Fail(Status::Code::Unsupported,
                        "Part " + part_.name.substr(1) + " redeclares a namespace prefix inside it, so it is kept as it is.");
  const Tree& t = doc_.tree_;
  // Depth first, iteratively: (element, closing?).
  std::vector<std::pair<ElemId, bool>> stack{{part_.root, false}};
  while (!stack.empty()) {
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
