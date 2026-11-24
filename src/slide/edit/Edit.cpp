// The edit layer's core (P2, p2-model): calls and steps, publishing parts, undo and redo, the
// model refreshed after each change, and the helpers the operations share.
#include <algorithm>
#include <charconv>

#include "core/Strings.h"
#include "slide/edit/EditInternal.h"
#include "slide/edit/Journal.h"

namespace fw::slide::edit {

namespace {

const pkg::ReferenceRegistry& Registry() {
  static const pkg::ReferenceRegistry r = pkg::ReferenceRegistry::Default();
  return r;
}

uint32_t U32(pugi::xml_node n, const char* attr) {
  double d = Number(n, attr, 0);
  return d >= 0 && d <= 4294967295.0 ? static_cast<uint32_t>(d) : 0;
}

}  // namespace

// ---- Helpers

std::wstring XmlText(std::wstring_view s, bool breaks) {
  std::wstring out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    const wchar_t c = s[i];
    if (c >= 0xD800 && c <= 0xDBFF && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
      out += c;
      out += s[++i];
    } else if (c >= 0xD800 && c <= 0xDFFF) {
      out += L'\xFFFD';
    } else if (c == 0x9 || c == 0xA || c == 0xD || (breaks && c == 0xB) || (c >= 0x20 && c <= 0xFFFD)) {
      out += c;
    }
  }
  return out;
}

std::string Utf8(std::wstring_view s) { return fw::WideToUtf8(XmlText(s)); }
std::wstring Wide(std::string_view s) { return fw::Utf8ToWide(s); }

bool ParseFragment(std::string_view xml, pugi::xml_document& doc) {
  // Checked like a part first (limits, no DTD, well-formed, namespaces declared).
  pkg::XmlDom dom;
  if (!dom.Parse(xml, pkg::Limits::Default(), "fragment").Ok() || !dom.Root()) return false;
  // What a parser decoded from references (&#1;, &#xFFFE;, &#xD800;) never reaches the writer.
  CleanXmlTree(dom.Doc());
  doc.reset(dom.Doc());
  return static_cast<bool>(doc.document_element());
}

namespace {

// Namespace declarations in scope at `n` (nearest first wins).
void ScopeOf(pugi::xml_node n, std::vector<std::pair<std::string, std::string>>& out) {
  for (; n && n.type() == pugi::node_element; n = n.parent()) {
    for (pugi::xml_attribute a = n.first_attribute(); a; a = a.next_attribute()) {
      std::string_view name = a.name();
      if (name != "xmlns" && !name.starts_with("xmlns:")) continue;
      if (std::none_of(out.begin(), out.end(), [&](const auto& p) { return p.first == name; }))
        out.emplace_back(std::string(name), a.value());
    }
  }
}

}  // namespace

pugi::xml_node InsertCopy(pugi::xml_node parent, pugi::xml_node src, pugi::xml_node before) {
  pugi::xml_node c = before ? parent.insert_copy_before(src, before) : parent.append_copy(src);
  std::vector<std::pair<std::string, std::string>> scope;
  ScopeOf(src.parent(), scope);
  for (const auto& [name, uri] : scope) {
    if (c.attribute(name.c_str())) continue;
    std::string_view prefix = name == "xmlns" ? std::string_view() : std::string_view(name).substr(6);
    std::optional<std::string_view> there = pkg::LookupNamespace(parent, prefix);
    if (there && *there == uri) continue;
    c.append_attribute(name.c_str()) = uri.c_str();
  }
  return c;
}

std::string PrefixFor(pugi::xml_node node, std::string_view ns, std::string_view fallback) {
  for (pugi::xml_node n = node; n && n.type() == pugi::node_element; n = n.parent()) {
    for (pugi::xml_attribute a = n.first_attribute(); a; a = a.next_attribute()) {
      std::string_view name = a.name();
      if (a.value() != ns) continue;
      if (name.starts_with("xmlns:")) {
        std::string prefix(name.substr(6));
        // Shadowed by a nearer declaration of the same prefix?
        auto bound = pkg::LookupNamespace(node, prefix);
        if (bound && *bound == ns) return prefix;
      } else if (name == "xmlns") {
        auto bound = pkg::LookupNamespace(node, "");
        if (bound && *bound == ns) return {};
      }
    }
  }
  pugi::xml_node root = node;
  while (root.parent() && root.parent().type() == pugi::node_element) root = root.parent();
  std::string prefix(fallback);
  for (int i = 2; pkg::LookupNamespace(root, prefix).has_value(); ++i) prefix = std::string(fallback) + std::to_string(i);
  root.append_attribute(("xmlns:" + prefix).c_str()) = std::string(ns).c_str();
  return prefix;
}

pugi::xml_node DirectChild(pugi::xml_node parent, std::string_view ns, std::string_view local) {
  for (pugi::xml_node c = parent.first_child(); c; c = c.next_sibling()) {
    if (c.type() == pugi::node_element && Is(c, ns, local)) return c;
  }
  return {};
}

pugi::xml_node AddChildOrdered(pugi::xml_node parent, std::string_view qname, std::initializer_list<std::string_view> after) {
  for (pugi::xml_node c = parent.first_child(); c; c = c.next_sibling()) {
    if (c.type() != pugi::node_element) continue;
    std::string_view k = LocalNameOf(c);
    if (std::find(after.begin(), after.end(), k) != after.end()) return parent.insert_child_before(std::string(qname).c_str(), c);
  }
  return parent.append_child(std::string(qname).c_str());
}

// ---- Editor::Impl: reading and writing parts

Editor::Impl::Impl(Editor& editor, PartStore& s) : ed(editor), deck(editor.Deck()), store(s), registry(Registry()) {}

pugi::xml_node Editor::Impl::Root(std::string_view part) const {
  std::string key = pkg::PartKey(part);
  if (auto it = work.doms.find(key); it != work.doms.end()) return it->second->Root();
  if (auto it = work.parts.find(key); it != work.parts.end()) return {};
  DomPtr dom = Current(part);
  return dom ? dom->Root() : pugi::xml_node();
}

DomPtr Editor::Impl::Current(std::string_view part) const {
  std::string key = pkg::PartKey(part);
  if (auto it = work.doms.find(key); it != work.doms.end()) return it->second;
  if (auto it = work.parts.find(key); it != work.parts.end()) return nullptr;
  if (const PartState* s = store.Find(part)) {
    if (s->dom) return s->dom;
    if (!s->bytes) return nullptr;
  } else if (DomPtr model = ed.ModelDom(part)) {
    return model;
  }
  auto it = parsed.find(key);
  if (it != parsed.end()) {
    // A part the store holds as bytes may have changed since it was parsed.
    const PartState* s = store.Find(part);
    if (!s || it->second.second == s->bytes.get()) return it->second.first;
  }
  DomPtr dom = store.Dom(part);
  const PartState* s = store.Find(part);
  parsed[key] = {dom, s ? s->bytes.get() : nullptr};
  return dom;
}

bool Editor::Impl::Exists(std::string_view part) const {
  std::string key = pkg::PartKey(part);
  if (work.doms.count(key)) return true;
  if (auto it = work.parts.find(key); it != work.parts.end()) return !it->second.Removed();
  return store.Has(part);
}

const pkg::RelationshipSet* Editor::Impl::Rels(std::string_view source) const {
  std::string key = pkg::PartKey(source);
  if (auto it = work.rels.find(key); it != work.rels.end()) return it->second.get();
  if (auto it = work.parts.find(key); it != work.parts.end() && it->second.Removed()) return nullptr;
  return store.Relationships(source);
}

std::string Editor::Impl::ContentType(std::string_view part) const {
  if (work.types) return std::string(work.types->Lookup(part));
  return store.ContentTypeOf(part);
}

bool Editor::Impl::Read(std::string_view part, std::vector<uint8_t>& out) const {
  std::string key = pkg::PartKey(part);
  if (auto it = work.doms.find(key); it != work.doms.end()) {
    std::string xml = it->second->Serialize();
    out.assign(xml.begin(), xml.end());
    return true;
  }
  if (auto it = work.parts.find(key); it != work.parts.end()) {
    if (!it->second.bytes) return false;
    out = *it->second.bytes;
    return true;
  }
  return store.ReadPart(part, out);
}

pugi::xml_node Editor::Impl::Edit(std::string_view part) {
  std::string key = pkg::PartKey(part);
  if (auto it = work.doms.find(key); it != work.doms.end()) return it->second->Root();
  DomPtr cur = Current(part);
  if (!cur) return {};
  std::shared_ptr<pkg::XmlDom> copy = CloneDom(*cur);
  work.doms[key] = copy;
  work.names[key] = std::string(part);
  work.parts.erase(key);
  return copy->Root();
}

pkg::RelationshipSet& Editor::Impl::EditRels(std::string_view source) {
  std::string key = pkg::PartKey(source);
  auto it = work.rels.find(key);
  if (it == work.rels.end()) {
    auto set = std::make_shared<pkg::RelationshipSet>();
    if (work.originalRels.erase(key)) {
      if (const pkg::RelationshipSet* cur = store.Package().RelationshipsOf(source)) *set = *cur;
    } else if (const pkg::RelationshipSet* cur = Rels(source)) {
      *set = *cur;
    }
    set->source.assign(source);
    set->relsPart = pkg::RelsPartFor(source);
    it = work.rels.emplace(key, std::move(set)).first;
  }
  return *it->second;
}

pkg::ContentTypes& Editor::Impl::EditTypes() {
  if (!work.types) {
    work.types = std::make_shared<pkg::ContentTypes>(work.typesOriginal ? store.Package().Types() : store.Types());
    work.typesOriginal = false;
  }
  return *work.types;
}

namespace {

std::string ExtensionOf(std::string_view part) {
  size_t slash = part.rfind('/'), dot = part.rfind('.');
  if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash)) return {};
  std::string ext(part.substr(dot + 1));
  for (char& c : ext) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
  return ext;
}

void SetType(pkg::ContentTypes& types, std::string_view part, std::string_view contentType, bool preferDefault) {
  if (contentType.empty()) return;
  std::string key = pkg::PartKey(part);
  auto over = std::find_if(types.overrides.begin(), types.overrides.end(),
                           [&](const auto& o) { return pkg::PartKey(o.first) == key; });
  if (over != types.overrides.end()) {
    over->second.assign(contentType);
    return;
  }
  if (types.LookupDefault(part) == contentType) return;
  std::string ext = ExtensionOf(part);
  bool hasDefault = std::any_of(types.defaults.begin(), types.defaults.end(), [&](const auto& d) {
    std::string e = d.first;
    for (char& c : e) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
    return e == ext;
  });
  if (preferDefault && !ext.empty() && !hasDefault) {
    types.defaults.emplace_back(ext, std::string(contentType));
    return;
  }
  types.overrides.emplace_back(std::string(part), std::string(contentType));
}

}  // namespace

void Editor::Impl::PutXml(std::string_view part, std::shared_ptr<pkg::XmlDom> dom, std::string_view contentType) {
  std::string key = pkg::PartKey(part);
  work.doms[key] = std::move(dom);
  work.names[key] = std::string(part);
  work.parts.erase(key);
  work.originalParts.erase(key);
  if (!contentType.empty() && ContentType(part) != contentType) SetType(EditTypes(), part, contentType, false);
}

void Editor::Impl::PutBytes(std::string_view part, std::vector<uint8_t> bytes, std::string_view contentType) {
  std::string key = pkg::PartKey(part);
  work.doms.erase(key);
  work.parts[key] = PartState{std::string(part), nullptr, std::make_shared<const std::vector<uint8_t>>(std::move(bytes))};
  work.names[key] = std::string(part);
  work.originalParts.erase(key);
  if (!contentType.empty() && ContentType(part) != contentType) SetType(EditTypes(), part, contentType, true);
}

void Editor::Impl::Original(std::string_view part) {
  std::string key = pkg::PartKey(part);
  work.doms.erase(key);
  work.parts.erase(key);
  work.names[key] = std::string(part);
  work.originalParts[key] = std::string(part);
}

void Editor::Impl::OriginalRels(std::string_view source) {
  std::string key = pkg::PartKey(source);
  work.rels.erase(key);
  work.originalRels[key] = std::string(source);
}

void Editor::Impl::OriginalTypes() {
  work.types.reset();
  work.typesOriginal = true;
}

void Editor::Impl::Remove(std::string_view part) {
  std::string key = pkg::PartKey(part);
  work.originalParts.erase(key);
  work.doms.erase(key);
  work.parts[key] = PartState{std::string(part), nullptr, nullptr};
  work.names[key] = std::string(part);
  if (Rels(part)) {
    pkg::RelationshipSet& rels = EditRels(part);
    rels.items.clear();
  }
  pkg::ContentTypes& types = EditTypes();
  std::erase_if(types.overrides, [&](const auto& o) { return pkg::PartKey(o.first) == key; });
}

void Editor::Impl::Touched(std::string_view slidePart, uint32_t shape) { work.shapes[pkg::PartKey(slidePart)].insert(shape); }

void Editor::Impl::CommentsChanged(std::string_view slidePart) { work.comments.insert(pkg::PartKey(slidePart)); }

namespace {

// p3-review: slide `id`'s comments changed (the slide is reported with kComments).
void ReportComments(Changes& out, uint32_t id, int index) {
  auto it = std::find_if(out.slides.begin(), out.slides.end(), [&](const SlideChange& c) { return c.id == id; });
  if (it == out.slides.end()) out.slides.push_back({id, index, kComments, {}});
  else it->what |= kComments;
}

}  // namespace

// ---- Calls and steps

void Editor::Impl::BeginCall() {
  if (calls++ == 0) work = Work{};
}

Result Editor::Impl::EndCall(Result r) {
  if (--calls > 0) return r;
  if (r.ok) {
    Publish();
  } else {
    work = Work{};
  }
  return r;
}

void Editor::Impl::Record(Step& step, const std::string& name, bool rels) {
  std::string key = pkg::PartKey(name);
  if (rels) {
    if (!step.rels.count(key)) {
      step.rels[key] = store.GetRels(name);
      step.names["rels:" + key] = name;
    }
  } else if (!step.parts.count(key)) {
    step.parts[key] = store.Get(name);
    step.names[key] = name;
  }
}

void Editor::Impl::Publish() {
  Work w = std::move(work);
  work = Work{};
  if (w.doms.empty() && w.parts.empty() && w.rels.empty() && !w.types && w.originalParts.empty() &&
      w.originalRels.empty() && !w.typesOriginal)
    return;
  store.Bury();
  Step& step = open.back();
  std::set<std::string> keys;
  for (auto& [key, dom] : w.doms) {
    const std::string& name = w.names[key];
    Record(step, name, false);
    store.Set(name, PartState{name, std::move(dom), nullptr});
    keys.insert(key);
  }
  for (auto& [key, state] : w.parts) {
    Record(step, state.name, false);
    store.Set(state.name, state);
    keys.insert(key);
  }
  // Back to the file's own: no edit of them in the store (a save copies them as they are).
  for (const auto& [key, name] : w.originalParts) {
    Record(step, name, false);
    store.Set(name, std::nullopt);
    keys.insert(key);
  }
  for (const auto& [key, name] : w.originalRels) {
    Record(step, name, true);
    store.SetRels(name, std::nullopt);
    keys.insert(key);
  }
  if (w.typesOriginal) {
    if (!step.typesSaved) {
      step.typesSaved = true;
      step.types = store.GetTypes();
    }
    store.SetTypes(nullptr);
  }
  for (auto& [key, set] : w.rels) {
    Record(step, set->source, true);
    for (pkg::Relationship& r : set->items) {
      r.targetPart = r.external ? std::string() : pkg::ResolveTarget(set->source, r.target);
    }
    if (set->relsPart.empty()) set->relsPart = pkg::RelsPartFor(set->source);
    // A relationships part needs a content type (Package::SetRelationships does the same).
    keys.insert(key);
    std::string relsPart = set->relsPart, source = set->source;
    store.SetRels(source, RelsPtr(std::move(set)));
    if ((w.types ? std::string(w.types->Lookup(relsPart)) : store.ContentTypeOf(relsPart)).empty()) {
      if (!w.types) w.types = std::make_shared<pkg::ContentTypes>(store.Types());
      w.types->overrides.emplace_back(relsPart, "application/vnd.openxmlformats-package.relationships+xml");
    }
  }
  if (w.types) {
    if (!step.typesSaved) {
      step.typesSaved = true;
      step.types = store.GetTypes();
    }
    store.SetTypes(std::move(w.types));
  }
  step.full = step.full || w.full;
  Changes& out = changes;
  // The shapes touched, by slide id, for the view.
  std::vector<SlideEntry> list = SlideList();
  for (const auto& [partKey, ids] : w.shapes) {
    for (const SlideEntry& e : list) {
      if (pkg::PartKey(e.part) == partKey) step.shapes[e.id].insert(ids.begin(), ids.end());
    }
  }
  Refresh(keys, w.full, out);
  for (const auto& [partKey, ids] : w.shapes) {
    for (const SlideEntry& e : list) {
      if (pkg::PartKey(e.part) != partKey) continue;
      auto it = std::find_if(out.slides.begin(), out.slides.end(), [&](const SlideChange& c) { return c.id == e.id; });
      if (it == out.slides.end()) {
        out.slides.push_back({e.id, deck.SlideIndexOfId(e.id), kContent, {}});
        it = out.slides.end() - 1;
      }
      for (uint32_t id : ids) {
        if (std::find(it->shapes.begin(), it->shapes.end(), id) == it->shapes.end()) it->shapes.push_back(id);
      }
    }
  }
  for (const std::string& partKey : w.comments) {
    for (const SlideEntry& e : list) {
      if (pkg::PartKey(e.part) != partKey) continue;
      step.comments.insert(e.id);
      ReportComments(out, e.id, deck.SlideIndexOfId(e.id));
    }
  }
  if (ed.journal_) ed.journal_->Changed(store, keys);
}

void Editor::Impl::Refresh(const std::set<std::string>& keys, bool full, Changes& out) {
  RefreshModel(deck, keys, full, out);
}

void Editor::Impl::Open(std::u16string name) {
  if (open.empty()) changes = Changes{};
  Step s;
  s.name = std::move(name);
  open.push_back(std::move(s));
}

namespace {

void Merge(Step& outer, Step& inner) {
  for (auto& [key, state] : inner.parts) outer.parts.try_emplace(key, std::move(state));
  for (auto& [key, rels] : inner.rels) outer.rels.try_emplace(key, std::move(rels));
  for (auto& [key, name] : inner.names) outer.names.try_emplace(key, std::move(name));
  if (inner.typesSaved && !outer.typesSaved) {
    outer.typesSaved = true;
    outer.types = std::move(inner.types);
  }
  outer.full = outer.full || inner.full;
  for (auto& [id, shapes] : inner.shapes) outer.shapes[id].insert(shapes.begin(), shapes.end());
  outer.comments.insert(inner.comments.begin(), inner.comments.end());
}

}  // namespace

void Editor::Impl::Close() {
  Step s = std::move(open.back());
  open.pop_back();
  if (!open.empty()) {
    Merge(open.back(), s);
    return;
  }
  bool typing = s.typing;
  if (!s.Empty()) {
    s.serial = ++serial;
    redo.clear();
    undo.push_back(std::move(s));
    if (undo.size() > kMaxSteps) undo.erase(undo.begin());
    typingOpen = typing;
  }
  Changes c = std::move(changes);
  changes = Changes{};
  if (!c.Empty()) {
    c.cause = Changes::Cause::Edit;
    c.step = undo.empty() ? std::u16string() : undo.back().name;
    Notify(std::move(c));
  }
}

void Editor::Impl::Cancel() {
  Step s = std::move(open.back());
  open.pop_back();
  // Back to the state before the group: its own changes swapped out and dropped.
  Swap(s, changes);
  if (open.empty()) {
    Changes c = std::move(changes);
    changes = Changes{};
    if (!c.Empty()) Notify(std::move(c));
  }
}

void Editor::Impl::Reshelve() {
  Step s = std::move(open.back());
  open.pop_back();
  undo.push_back(std::move(s));
  typingOpen = true;
  changes = Changes{};
}

void Editor::Impl::Swap(Step& s, Changes& out) {
  store.Bury();
  std::set<std::string> keys;
  for (auto& [key, state] : s.parts) {
    const std::string& name = s.names[key];
    std::optional<PartState> now = store.Get(name);
    store.Set(name, std::move(state));
    state = std::move(now);
    keys.insert(key);
  }
  for (auto& [key, rels] : s.rels) {
    const std::string& name = s.names["rels:" + key];
    std::optional<RelsPtr> now = store.GetRels(name);
    store.SetRels(name, std::move(rels));
    rels = std::move(now);
    keys.insert(key);
  }
  if (s.typesSaved) {
    TypesPtr now = store.GetTypes();
    store.SetTypes(std::move(s.types));
    s.types = std::move(now);
  }
  Refresh(keys, s.full, out);
  for (const auto& [id, shapes] : s.shapes) {
    int index = deck.SlideIndexOfId(id);
    if (index < 0) continue;
    auto it = std::find_if(out.slides.begin(), out.slides.end(), [&](const SlideChange& c) { return c.id == id; });
    if (it == out.slides.end()) {
      out.slides.push_back({id, index, kContent, {}});
      it = out.slides.end() - 1;
    }
    it->shapes.insert(it->shapes.end(), shapes.begin(), shapes.end());
  }
  for (uint32_t id : s.comments) {
    const int index = deck.SlideIndexOfId(id);
    if (index >= 0) ReportComments(out, id, index);
  }
  if (ed.journal_) ed.journal_->Changed(store, keys);
}

void Editor::Impl::Notify(Changes c) {
  for (const auto& l : ed.listeners_) l(c);
}

// ---- Helpers for the operations

std::vector<Editor::Impl::SlideEntry> Editor::Impl::SlideList() const {
  std::vector<SlideEntry> list;
  pugi::xml_node root = Root(PresentationPart());
  const pkg::RelationshipSet* rels = Rels(PresentationPart());
  for (pugi::xml_node id = P(root, "sldIdLst").first_child(); id; id = id.next_sibling()) {
    if (!Is(id, kP, "sldId")) continue;
    std::string rel(RelId(id, "id"));
    std::string part = TargetOf(rels, rel);
    if (part.empty()) continue;
    list.push_back({U32(id, "id"), rel, part, id});
  }
  return list;
}

std::string Editor::Impl::SlidePart(size_t index) const {
  std::vector<SlideEntry> list = SlideList();
  return index < list.size() ? list[index].part : std::string();
}

Result Editor::Impl::CheckSlides(const std::vector<size_t>& slides, size_t count) const {
  if (slides.empty()) return Result::Fail(u"No slides are selected.");
  for (size_t s : slides) {
    if (s >= count) return Result::Fail(u"That slide isn't in the presentation.");
  }
  return {};
}

std::string Editor::Impl::NewRelId(const pkg::RelationshipSet& rels) {
  uint64_t max = 0;
  for (const pkg::Relationship& r : rels.items) {
    std::string_view id = r.id;
    if (id.size() > 3 && id.starts_with("rId")) {
      uint64_t n = 0;
      auto [p, ec] = std::from_chars(id.data() + 3, id.data() + id.size(), n);
      if (ec == std::errc() && p == id.data() + id.size()) max = std::max(max, n);
    }
  }
  for (uint64_t n = max + 1;; ++n) {
    std::string id = "rId" + std::to_string(n);
    if (!rels.Find(id)) return id;
  }
}

std::string Editor::Impl::RelativeTarget(std::string_view source, std::string_view part) {
  // Directories of the source ("/ppt/slides/" for "/ppt/slides/slide1.xml"; "/" for the package).
  std::string_view dir = source == "/" ? std::string_view("/") : source.substr(0, source.rfind('/') + 1);
  std::vector<std::string_view> from, to;
  auto split = [](std::string_view s, std::vector<std::string_view>& out) {
    size_t i = 1;
    while (i < s.size()) {
      size_t j = s.find('/', i);
      if (j == std::string_view::npos) {
        out.push_back(s.substr(i));
        break;
      }
      out.push_back(s.substr(i, j - i));
      i = j + 1;
    }
  };
  split(dir, from);
  split(part, to);
  size_t common = 0;
  while (common < from.size() && common + 1 < to.size() && pkg::PartKey(from[common]) == pkg::PartKey(to[common])) ++common;
  std::string out;
  for (size_t i = common; i < from.size(); ++i) out += "../";
  for (size_t i = common; i < to.size(); ++i) {
    out += to[i];
    if (i + 1 < to.size()) out += '/';
  }
  return out;
}

std::string Editor::Impl::AddRel(std::string_view source, std::string_view type, std::string_view target, bool external) {
  pkg::RelationshipSet& rels = EditRels(source);
  pkg::Relationship r;
  r.id = NewRelId(rels);
  r.type.assign(type);
  r.external = external;
  if (external) {
    r.target.assign(target);
  } else {
    r.target = RelativeTarget(source, target);
    r.targetPart.assign(target);
  }
  rels.items.push_back(r);
  return r.id;
}

std::string Editor::Impl::NewName(std::string_view stem, std::string_view ext) const {
  for (uint32_t n = 1;; ++n) {
    std::string name = std::string(stem) + std::to_string(n) + std::string(ext);
    if (!Exists(name) && !store.Find(name)) return name;
  }
}

pugi::xml_node Editor::Impl::FindShape(pugi::xml_node root, uint32_t id) {
  pugi::xml_node tree = P(P(root, "cSld"), "spTree");
  pugi::xml_node found;
  std::vector<pugi::xml_node> stack{tree};
  while (!stack.empty() && !found) {
    pugi::xml_node parent = stack.back();
    stack.pop_back();
    ForEachChosenChild(parent, [&](pugi::xml_node c) {
      std::string_view k = LocalNameOf(c);
      if (k != "sp" && k != "pic" && k != "cxnSp" && k != "graphicFrame" && k != "grpSp" && k != "contentPart") return true;
      pugi::xml_node nv;
      for (pugi::xml_node n = c.first_child(); n && !nv; n = n.next_sibling()) {
        std::string_view nk = LocalNameOf(n);
        if (nk.size() > 4 && nk.starts_with("nv") && nk.ends_with("Pr")) nv = n;
      }
      pugi::xml_node cNvPr = P(nv, "cNvPr");
      if (cNvPr && U32(cNvPr, "id") == id) {
        found = c;
        return false;
      }
      if (k == "grpSp") stack.push_back(c);
      return true;
    });
  }
  return found;
}

uint32_t Editor::Impl::NextShapeId(pugi::xml_node root) {
  uint32_t max = 1;
  std::vector<pugi::xml_node> stack{root};
  while (!stack.empty()) {
    pugi::xml_node n = stack.back();
    stack.pop_back();
    for (pugi::xml_node c = n.first_child(); c; c = c.next_sibling()) {
      if (c.type() != pugi::node_element) continue;
      if (LocalNameOf(c) == "cNvPr") max = std::max(max, U32(c, "id"));
      stack.push_back(c);
    }
  }
  return max + 1;
}

bool HoldsMath(pugi::xml_node body) {
  return body && body.find_node([](pugi::xml_node n) { return Is(n, kA14, "m"); });
}

pugi::xml_node Editor::Impl::Unwrap(pugi::xml_node shape) {
  // The nearest mc:AlternateContent around the shape, below the shape tree.
  pugi::xml_node branch, ac;
  for (pugi::xml_node n = shape; n.parent() && n.parent().type() == pugi::node_element; n = n.parent()) {
    pugi::xml_node p = n.parent();
    if (Is(p, pkg::mce::kNamespace, "Choice") || Is(p, pkg::mce::kNamespace, "Fallback")) {
      branch = p;
      ac = p.parent();
      break;
    }
    if (LocalNameOf(p) == "spTree") break;
  }
  if (!ac) return shape;
  // A Choice's shape is edited where it is: the block keeps its wrapper (math or a newer shape needs it
  // to stay valid), its other Choices and its Fallback (what other apps show, never dropped). The
  // operations that apply to the Fallback's shape too keep it in step (ShapeOps.cpp, SyncTwins).
  if (Is(branch, pkg::mce::kNamespace, "Choice")) return shape;
  // The Fallback was chosen (no Choice is understood): its content is plain markup and replaces the
  // block, so what was edited is what every app shows.
  // The path from the branch's child down to the shape, to find its copy.
  std::vector<int> path;
  pugi::xml_node top = shape;
  while (top.parent() != branch) {
    int i = 0;
    for (pugi::xml_node s = top.parent().first_child(); s != top; s = s.next_sibling()) ++i;
    path.push_back(i);
    top = top.parent();
  }
  pugi::xml_node parent = ac.parent(), mapped;
  for (pugi::xml_node c = branch.first_child(); c; c = c.next_sibling()) {
    if (c.type() != pugi::node_element) continue;
    pugi::xml_node copy = InsertCopy(parent, c, ac);
    if (c == top) {
      mapped = copy;
      for (auto it = path.rbegin(); it != path.rend(); ++it) {
        pugi::xml_node s = mapped.first_child();
        for (int i = 0; i < *it && s; ++i) s = s.next_sibling();
        mapped = s;
      }
    }
  }
  parent.remove_child(ac);
  return mapped ? mapped : shape;
}

// ---- Editor

Editor::Editor(Presentation& deck) : deck_(deck) {
  if (!deck_.store_) deck_.store_ = std::make_unique<PartStore>(*deck_.package_);
  impl_ = std::make_unique<Impl>(*this, *deck_.store_);
}

Editor::~Editor() = default;

DomPtr Editor::ModelDom(std::string_view part) const {
  std::string key = pkg::PartKey(part);
  if (key == pkg::PartKey(deck_.presentationPart_)) return deck_.presentationDom_;
  for (const auto& r : deck_.slideRefs_) {
    if (r.slide && pkg::PartKey(r.part) == key) return r.slide->dom_;
  }
  for (const auto& l : deck_.layouts_) {
    if (pkg::PartKey(l->part) == key) return l->dom_;
  }
  for (const auto& m : deck_.masters_) {
    if (pkg::PartKey(m->part) == key) return m->dom_;
  }
  for (const auto& n : deck_.notes_) {
    if (pkg::PartKey(n->part) == key) return n->dom_;
  }
  if (deck_.notesMaster_ && pkg::PartKey(deck_.notesMaster_->part) == key) return deck_.notesMaster_->dom_;
  return nullptr;
}

bool Editor::CanEdit(std::u16string* why) const {
  pugi::xml_node root = impl_->Root(deck_.PresentationPart());
  if (!root || !Is(root, kP, "presentation")) {
    if (why) *why = u"This presentation can't be edited, because its main part is damaged.";
    return false;
  }
  return true;
}

void Editor::BeginGroup(std::u16string name) {
  impl_->Open(std::move(name));
}

void Editor::EndGroup() {
  if (!impl_->open.empty()) impl_->Close();
}

void Editor::CancelGroup() {
  if (!impl_->open.empty()) impl_->Cancel();
}

Result Editor::Run(std::u16string name, const std::function<Result()>& fn) {
  BeginGroup(std::move(name));
  Result r = fn();
  r ? EndGroup() : CancelGroup();
  return r;
}

bool Editor::CanUndo() const { return !impl_->undo.empty() && impl_->open.empty(); }
bool Editor::CanRedo() const { return !impl_->redo.empty() && impl_->open.empty(); }
std::u16string Editor::UndoName() const { return impl_->undo.empty() ? std::u16string() : impl_->undo.back().name; }
std::u16string Editor::RedoName() const { return impl_->redo.empty() ? std::u16string() : impl_->redo.back().name; }

std::vector<std::u16string> Editor::UndoNames() const {
  std::vector<std::u16string> out;
  for (auto it = impl_->undo.rbegin(); it != impl_->undo.rend(); ++it) out.push_back(it->name);
  return out;
}

std::vector<std::u16string> Editor::RedoNames() const {
  std::vector<std::u16string> out;
  for (auto it = impl_->redo.rbegin(); it != impl_->redo.rend(); ++it) out.push_back(it->name);
  return out;
}

Result Editor::Undo(size_t steps) {
  Impl& m = *impl_;
  if (!m.open.empty() || m.calls) return Result::Fail(u"Finish the current edit first.");
  if (m.undo.empty()) return Result::Fail(u"There's nothing to undo.");
  m.typingOpen = false;
  Changes c;
  c.cause = Changes::Cause::Undo;
  for (size_t i = 0; i < steps && !m.undo.empty(); ++i) {
    Step s = std::move(m.undo.back());
    m.undo.pop_back();
    c.step = s.name;
    m.Swap(s, c);
    s.typing = false;
    m.redo.push_back(std::move(s));
  }
  m.Notify(std::move(c));
  return {};
}

Result Editor::Redo(size_t steps) {
  Impl& m = *impl_;
  if (!m.open.empty() || m.calls) return Result::Fail(u"Finish the current edit first.");
  if (m.redo.empty()) return Result::Fail(u"There's nothing to redo.");
  m.typingOpen = false;
  Changes c;
  c.cause = Changes::Cause::Redo;
  for (size_t i = 0; i < steps && !m.redo.empty(); ++i) {
    Step s = std::move(m.redo.back());
    m.redo.pop_back();
    c.step = s.name;
    m.Swap(s, c);
    m.undo.push_back(std::move(s));
  }
  m.Notify(std::move(c));
  return {};
}

bool Editor::Modified() const { return impl_->StateSerial() != impl_->savedSerial; }

void Editor::MarkSaved() {
  impl_->savedSerial = impl_->StateSerial();
  impl_->typingOpen = false;
}

uint64_t Editor::StateId() const { return impl_->StateSerial(); }

void Editor::MarkSavedAt(uint64_t state) {
  impl_->savedSerial = state;
  if (state == impl_->StateSerial()) impl_->typingOpen = false;
}

void Editor::EndTyping() { impl_->typingOpen = false; }

void Editor::SetJournal(Journal* journal) { journal_ = journal; }

// ---- Package parts

std::string Editor::AddPart(std::string_view source, std::string_view relType, std::string_view name,
                            std::string_view contentType, std::vector<uint8_t> bytes) {
  Impl& m = *impl_;
  if (m.open.empty() || name.empty() || !name.starts_with("/") || m.Exists(name)) return {};
  if (source != "/" && !m.Exists(source)) return {};
  Call call(*this, u"Insert");
  bool xml = name.ends_with(".xml") || name.ends_with(".rels");
  std::shared_ptr<pkg::XmlDom> dom;
  if (xml) {
    dom = ParseDom(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()),
                   deck_.GetPackage().GetLimits(), name);
    if (!dom) {
      call.End(Result::Fail(u"The part isn't well-formed XML."));
      return {};
    }
    m.PutXml(name, std::move(dom), contentType);
  } else {
    m.PutBytes(name, std::move(bytes), contentType);
  }
  std::string id = m.AddRel(source, relType, name);
  call.End({});
  return id;
}

std::string Editor::AddRelationship(std::string_view source, std::string_view relType, std::string_view target,
                                    bool external) {
  Impl& m = *impl_;
  if (m.open.empty()) return {};
  if (!external && !m.Exists(target)) return {};
  Call call(*this, u"Insert");
  std::string id = m.AddRel(source, relType, target, external);
  call.End({});
  return id;
}

bool Editor::WritePart(std::string_view name, std::vector<uint8_t> bytes) {
  Impl& m = *impl_;
  if (m.open.empty() || !m.Exists(name)) return false;
  Call call(*this, u"Edit");
  if (name.ends_with(".xml")) {
    auto dom = ParseDom(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()),
                        deck_.GetPackage().GetLimits(), name);
    if (!dom) {
      call.End(Result::Fail(u"The part isn't well-formed XML."));
      return false;
    }
    m.PutXml(name, std::move(dom), {});
  } else {
    m.PutBytes(name, std::move(bytes), {});
  }
  return call.End({}).ok;
}

std::string Editor::NewPartName(std::string_view stem, std::string_view ext) const { return impl_->NewName(stem, ext); }

bool Editor::ReadPart(std::string_view name, std::vector<uint8_t>& out) const { return impl_->Read(name, out); }

std::string Editor::SlidePart(size_t slide) const { return impl_->SlidePart(slide); }

}  // namespace fw::slide::edit
