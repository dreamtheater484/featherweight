#include "pkg/References.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "pkg/Formula.h"
#include "pkg/Mce.h"
#include "pkg/RoundTrip.h"
#include "pkg/XmlDom.h"
#include "pkg/XmlPull.h"
#include "pkg/XmlWriter.h"

namespace fw::pkg {
namespace {

char Lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; }

bool EqualNoCase(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (Lower(a[i]) != Lower(b[i])) return false;
  return true;
}

std::string LowerAscii(std::string_view s) {
  std::string out(s);
  for (auto& c : out) c = Lower(c);
  return out;
}

bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
bool IsIdChar(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
         static_cast<unsigned char>(c) >= 0x80;
}

// Strict OOXML (ISO/IEC 29500 Strict) names the same vocabularies differently; sites use the
// transitional names.
std::string_view Canonical(std::string_view ns) {
  static constexpr std::pair<std::string_view, std::string_view> kStrict[] = {
      {kStrictRelationshipsNs, kOfficeRelationshipsNs},
      {"http://purl.oclc.org/ooxml/wordprocessingml/main", "http://schemas.openxmlformats.org/wordprocessingml/2006/main"},
      {"http://purl.oclc.org/ooxml/spreadsheetml/main", "http://schemas.openxmlformats.org/spreadsheetml/2006/main"},
      {"http://purl.oclc.org/ooxml/presentationml/main", "http://schemas.openxmlformats.org/presentationml/2006/main"},
      {"http://purl.oclc.org/ooxml/drawingml/main", "http://schemas.openxmlformats.org/drawingml/2006/main"},
      {"http://purl.oclc.org/ooxml/drawingml/wordprocessingDrawing",
       "http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing"},
      {"http://purl.oclc.org/ooxml/drawingml/spreadsheetDrawing",
       "http://schemas.openxmlformats.org/drawingml/2006/spreadsheetDrawing"},
      {"http://purl.oclc.org/ooxml/drawingml/chart", "http://schemas.openxmlformats.org/drawingml/2006/chart"},
      {"http://purl.oclc.org/ooxml/drawingml/picture", "http://schemas.openxmlformats.org/drawingml/2006/picture"},
      {"http://purl.oclc.org/ooxml/officeDocument/customXml",
       "http://schemas.openxmlformats.org/officeDocument/2006/customXml"},
  };
  if (!ns.starts_with("http://purl.oclc.org/")) return ns;
  for (const auto& [strict, transitional] : kStrict)
    if (strict == ns) return transitional;
  return ns;
}

pugi::xml_node FirstElement(pugi::xml_node n) {
  pugi::xml_node c = n.first_child();
  while (c && c.type() != pugi::node_element) c = c.next_sibling();
  return c;
}

pugi::xml_node NextElement(pugi::xml_node n) {
  pugi::xml_node c = n.next_sibling();
  while (c && c.type() != pugi::node_element) c = c.next_sibling();
  return c;
}

// Walks the elements of a subtree in document order, iteratively, with the namespaces in scope.
// It also knows when it is inside a later branch of an mc:AlternateContent block.
class Walker {
 public:
  explicit Walker(pugi::xml_node root) : root_(root.type() == pugi::node_document ? FirstElement(root) : root) {
    std::vector<pugi::xml_node> above;  // declarations above the root are in scope too
    for (pugi::xml_node n = root_.parent(); n && n.type() == pugi::node_element; n = n.parent()) above.push_back(n);
    for (auto it = above.rbegin(); it != above.rend(); ++it) Declare(*it);
  }

  template <class Fn>
  void Run(Fn&& fn) {
    pugi::xml_node cur = root_;
    if (!cur || cur.type() != pugi::node_element) return;
    Enter(cur);
    fn(cur);
    for (;;) {
      if (pugi::xml_node child = FirstElement(cur)) {
        cur = child;
        Enter(cur);
        fn(cur);
        continue;
      }
      for (;;) {
        Leave();
        if (cur == root_) return;
        if (pugi::xml_node next = NextElement(cur)) {
          cur = next;
          Enter(cur);
          fn(cur);
          break;
        }
        cur = cur.parent();
      }
    }
  }

  std::string_view Lookup(std::string_view prefix) const {
    if (prefix == "xml") return kXmlNamespace;
    for (auto it = bindings_.rbegin(); it != bindings_.rend(); ++it)
      if (it->first == prefix) return it->second;
    return {};
  }
  bool InAlternate() const { return alternate_ > 0; }

 private:
  void Declare(pugi::xml_node e) {
    for (auto a : e.attributes()) {
      std::string_view n = a.name();
      if (n == "xmlns") bindings_.emplace_back(std::string_view(), a.value());
      else if (n.starts_with("xmlns:")) bindings_.emplace_back(n.substr(6), a.value());
    }
  }
  void Enter(pugi::xml_node e) {
    marks_.push_back(bindings_.size());
    Declare(e);
    bool later = false;
    std::string_view local = QNameLocal(e.name());
    if ((local == "Choice" || local == "Fallback") && Lookup(QNamePrefix(e.name())) == mce::kNamespace) {
      for (pugi::xml_node s = e.previous_sibling(); s && !later; s = s.previous_sibling())
        later = s.type() == pugi::node_element;
    }
    later_.push_back(later);
    alternate_ += later ? 1 : 0;
  }
  void Leave() {
    bindings_.resize(marks_.back());
    marks_.pop_back();
    alternate_ -= later_.back() ? 1 : 0;
    later_.pop_back();
  }

  pugi::xml_node root_;
  std::vector<std::pair<std::string_view, std::string_view>> bindings_;
  std::vector<size_t> marks_;
  std::vector<bool> later_;
  int alternate_ = 0;
};

bool ElementMatches(const RefSite& site, pugi::xml_node e, std::string_view ns, std::string_view local) {
  if (!site.elementLocal.empty() && site.elementLocal != local) return false;
  if ((!site.elementLocal.empty() || !site.elementNs.empty()) && site.elementNs != ns) return false;
  if (!site.parentLocal.empty()) {
    pugi::xml_node p = e.parent();
    if (p.type() != pugi::node_element || QNameLocal(p.name()) != site.parentLocal ||
        Canonical(NamespaceOf(p)) != site.elementNs)
      return false;
  }
  return true;
}

// Field instructions: REF, PAGEREF and NOTEREF name a bookmark first; HYPERLINK names one after \l
// when it has no address (with one, \l is a place in that address: HYPERLINK "https://..." \l "frag").
void ForEachFieldBookmark(std::string_view code, const std::function<void(std::string_view)>& fn) {
  std::vector<std::string_view> tokens;
  size_t i = 0;
  while (i < code.size()) {
    while (i < code.size() && IsSpace(code[i])) ++i;
    if (i >= code.size()) break;
    if (code[i] == '"') {
      size_t end = code.find('"', i + 1);
      if (end == std::string_view::npos) end = code.size();
      tokens.push_back(code.substr(i + 1, end - i - 1));
      i = end + 1;
    } else {
      size_t end = i;
      while (end < code.size() && !IsSpace(code[end])) ++end;
      tokens.push_back(code.substr(i, end - i));
      i = end;
    }
  }
  if (tokens.empty()) return;
  std::string command = LowerAscii(tokens[0]);
  if ((command == "ref" || command == "pageref" || command == "noteref") && tokens.size() > 1 &&
      !tokens[1].starts_with('\\') && !tokens[1].empty()) {
    fn(tokens[1]);
  } else if (command == "hyperlink") {
    // \l, \o and \t take an argument; the address is the first token that isn't a switch or one.
    std::string_view location;
    bool address = false;
    for (size_t t = 1; t < tokens.size(); ++t) {
      const bool takesArgument = EqualNoCase(tokens[t], "\\l") || EqualNoCase(tokens[t], "\\o") || EqualNoCase(tokens[t], "\\t");
      if (takesArgument && t + 1 < tokens.size()) {
        if (EqualNoCase(tokens[t], "\\l")) location = tokens[t + 1];
        ++t;
      } else if (!tokens[t].starts_with('\\') && !tokens[t].empty()) {
        address = true;
      }
    }
    if (!address && !location.empty()) fn(location);
  }
}

// The value with every reference to `from` renamed, or nothing when there is none.
std::optional<std::string> RenamedValue(const RefSite& site, const RefRule& rule, std::string_view value,
                                        std::string_view from, std::string_view to) {
  if (rule.kind == RefKind::CellRange || rule.kind == RefKind::VmlAnchor) return std::nullopt;  // shifted, not renamed
  if (site.syntax == RefSyntax::Formula) {
    if (rule.kind != RefKind::SheetName) return std::nullopt;
    bool changed = false;
    std::string s = RenameSheetInFormula(value, from, to, &changed);
    return changed ? std::optional<std::string>(std::move(s)) : std::nullopt;
  }
  std::string out;
  size_t copied = 0;
  bool any = false;
  ForEachRefValue(site, rule.kind, value, [&](std::string_view v) {
    if (!(rule.ignoreCase ? EqualNoCase(v, from) : v == from)) return;
    size_t at = static_cast<size_t>(v.data() - value.data());
    out.append(value.substr(copied, at - copied));
    out += to;
    copied = at + v.size();
    any = true;
  });
  if (!any) return std::nullopt;
  out.append(value.substr(copied));
  return out;
}

// Whether `text` contains `value` as a whole word (compared without ASCII case).
bool MentionsWord(std::string_view text, std::string_view value) {
  if (value.empty() || text.size() < value.size()) return false;
  for (size_t i = 0; i + value.size() <= text.size(); ++i) {
    if (!EqualNoCase(text.substr(i, value.size()), value)) continue;
    bool before = i == 0 || !IsIdChar(text[i - 1]);
    bool after = i + value.size() == text.size() || !IsIdChar(text[i + value.size()]);
    if (before && after) return true;
  }
  return false;
}

bool SubtreeMentions(pugi::xml_node ext, std::string_view value) {
  pugi::xml_node cur = ext;
  for (;;) {
    if (cur.type() == pugi::node_element) {
      for (auto a : cur.attributes()) {
        std::string_view n = a.name();
        if (n == "xmlns" || n.starts_with("xmlns:") || (cur == ext && n == "uri")) continue;
        if (MentionsWord(a.value(), value)) return true;
      }
    } else if ((cur.type() == pugi::node_pcdata || cur.type() == pugi::node_cdata) && MentionsWord(cur.value(), value)) {
      return true;
    }
    if (pugi::xml_node child = cur.first_child()) {
      cur = child;
      continue;
    }
    while (cur != ext && !cur.next_sibling()) cur = cur.parent();
    if (cur == ext) return false;
    cur = cur.next_sibling();
  }
}

bool ParseUnsigned(std::string_view v, uint64_t& out) {
  if (v.empty() || v.size() > 10) return false;
  out = 0;
  for (char c : v) {
    if (c < '0' || c > '9') return false;
    out = out * 10 + static_cast<uint64_t>(c - '0');
  }
  return true;
}

}  // namespace

// ---------- Values ----------

std::string_view FoundRef::Value() const { return attr ? std::string_view(attr.value()) : element.child_value(); }

void ForEachRefValue(const RefSite& site, RefKind kind, std::string_view value,
                     const std::function<void(std::string_view)>& fn) {
  switch (site.syntax) {
    case RefSyntax::Value:
      fn(value);
      break;
    case RefSyntax::List:
    case RefSyntax::Ranges: {
      size_t i = 0;
      while (i < value.size()) {
        while (i < value.size() && IsSpace(value[i])) ++i;
        size_t end = i;
        while (end < value.size() && !IsSpace(value[end])) ++end;
        if (end > i) fn(value.substr(i, end - i));
        i = end;
      }
      break;
    }
    case RefSyntax::Fragment:
      fn(value.starts_with('#') ? value.substr(1) : value);
      break;
    case RefSyntax::Formula:
      if (kind == RefKind::SheetName) {
        ForEachSheetToken(value, [&](const SheetToken& t) {
          fn(t.first);
          if (!t.last.empty()) fn(t.last);
        });
      } else if (kind == RefKind::CellRange) {
        ForEachCellToken(value, fn);
      }
      break;
    case RefSyntax::FieldCode:
      ForEachFieldBookmark(value, fn);
      break;
    case RefSyntax::None:
      break;
  }
}

bool IsLongHexId(std::string_view v) {
  if (v.size() != 8 || !((v[0] >= '0' && v[0] <= '7'))) return false;
  return std::all_of(v.begin(), v.end(), [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
  });
}

bool IsUnsignedId(std::string_view v) {
  uint64_t n = 0;
  return ParseUnsigned(v, n) && n <= 0xFFFFFFFFull;
}

bool IsSlideId(std::string_view v) {
  uint64_t n = 0;
  return ParseUnsigned(v, n) && n >= 256 && n <= 0x7FFFFFFFull;
}

bool IsSlideMasterId(std::string_view v) {
  uint64_t n = 0;
  return ParseUnsigned(v, n) && n >= 0x80000000ull && n <= 0xFFFFFFFFull;
}

// ---------- Registry ----------

void ReferenceRegistry::AddRule(RefRule rule) {
  rules_.push_back(std::move(rule));
  Index(rules_.size() - 1);
}

void ReferenceRegistry::Index(size_t r) {
  for (size_t s = 0; s < rules_[r].sites.size(); ++s) {
    const RefSite& site = rules_[r].sites[s];
    SiteRef ref{static_cast<uint16_t>(r), static_cast<uint16_t>(s)};
    if (site.attrLocal.empty() && site.attrNs.empty()) textSites_[site.elementLocal].push_back(ref);
    else if (site.attrLocal.empty()) nsAttrSites_.push_back(ref);
    else attrSites_[site.attrLocal].push_back(ref);
  }
}

void ReferenceRegistry::AddExtension(std::string uri, ExtensionClass c) { extensions_.emplace_back(std::move(uri), c); }

ExtensionClass ReferenceRegistry::ClassifyExtension(std::string_view uri) const {
  for (const auto& [u, c] : extensions_)
    if (EqualNoCase(u, uri)) return c;
  return ExtensionClass::Unknown;
}

void ReferenceRegistry::AddHook(Hook hook) { hooks_.push_back(std::move(hook)); }

std::vector<FoundRef> ReferenceRegistry::Find(pugi::xml_node root, std::optional<RefKind> kind) const {
  std::vector<FoundRef> out;
  if (!root) return out;
  Walker walker(root);
  walker.Run([&](pugi::xml_node e) {
    std::string_view qname = e.name();
    std::string_view local = QNameLocal(qname);
    std::string_view ns;
    bool nsKnown = false;
    auto elementNs = [&] {
      if (!nsKnown) ns = Canonical(walker.Lookup(QNamePrefix(qname)));
      nsKnown = true;
      return ns;
    };
    auto consider = [&](const SiteRef& ref, pugi::xml_attribute a, std::string_view value) {
      const RefRule& rule = rules_[ref.rule];
      const RefSite& site = rule.sites[ref.site];
      if (kind && rule.kind != *kind) return;
      if (!ElementMatches(site, e, elementNs(), local)) return;
      if (!site.whenValue.empty() && site.whenValue != value) return;
      if (!site.skipValue.empty() && site.skipValue == value) return;
      out.push_back({rule.kind, &rule, &site, e, a, walker.InAlternate()});
    };
    for (auto a : e.attributes()) {
      std::string_view value = a.value();
      if (value.empty()) continue;
      std::string_view name = a.name();
      std::string_view prefix = QNamePrefix(name);
      if (name == "xmlns" || prefix == "xmlns") continue;
      auto it = attrSites_.find(QNameLocal(name));
      bool any = !prefix.empty() && !nsAttrSites_.empty();
      if (it == attrSites_.end() && !any) continue;
      std::string_view ans = prefix.empty() ? std::string_view() : Canonical(walker.Lookup(prefix));
      if (it != attrSites_.end())
        for (const auto& ref : it->second)
          if (rules_[ref.rule].sites[ref.site].attrNs == ans) consider(ref, a, value);
      if (any)
        for (const auto& ref : nsAttrSites_)
          if (rules_[ref.rule].sites[ref.site].attrNs == ans) consider(ref, a, value);
    }
    if (auto it = textSites_.find(local); it != textSites_.end()) {
      std::string_view value = e.child_value();
      if (!value.empty())
        for (const auto& ref : it->second) consider(ref, pugi::xml_attribute(), value);
    }
  });
  return out;
}

size_t ReferenceRegistry::RenameDom(RefKind kind, std::string_view from, std::string_view to,
                                    pugi::xml_node root) const {
  size_t changed = 0;
  for (auto& ref : Find(root, kind)) {
    auto value = RenamedValue(*ref.site, *ref.rule, ref.Value(), from, to);
    if (!value) continue;
    if (ref.attr) ref.attr.set_value(value->c_str());
    else ref.element.text().set(value->c_str());
    ++changed;
  }
  return changed;
}

size_t ReferenceRegistry::RenameInTree(RefKind kind, std::string_view from, std::string_view to,
                                       pugi::xml_node root) const {
  return RenameDom(kind, from, to, root);
}

size_t ReferenceRegistry::Rename(RefKind kind, std::string_view from, std::string_view to, pugi::xml_node root,
                                 RoundTripStore* store) const {
  size_t changed = RenameDom(kind, from, to, root);
  if (store) {
    bool ignoreCase = false;
    for (const auto& rule : rules_) ignoreCase |= rule.kind == kind && rule.ignoreCase;
    std::string needle = ignoreCase ? LowerAscii(from) : std::string(from);
    // Kept fragments are XML text: those that mention the old value are parsed and rewritten.
    store->ForEachFragment([&](NodeId, Fragment& f) {
      if ((ignoreCase ? LowerAscii(f.xml) : f.xml).find(needle) == std::string::npos) return;
      XmlDom dom;
      if (!dom.Parse(f.xml).Ok() || !dom.Root()) return;  // a comment or text: no references
      size_t n = RenameDom(kind, from, to, dom.Root());
      if (n == 0) return;
      XmlWriter w;
      WriteNode(w, dom.Root());
      f.xml = w.Take();
      changed += n;
    });
    // Interned attributes have no element, so only sites on any element apply to them.
    store->ForEachAttrSet([&](std::vector<XmlAttr>& attrs) {
      for (auto& a : attrs) {
        std::string_view local = QNameLocal(a.qname);
        std::string_view ns = Canonical(a.nsUri);
        for (const auto& rule : rules_) {
          if (rule.kind != kind) continue;
          bool done = false;
          for (const auto& site : rule.sites) {
            if (!site.elementLocal.empty() || !site.elementNs.empty() || !site.parentLocal.empty()) continue;
            if (site.attrNs != ns || (!site.attrLocal.empty() && site.attrLocal != local)) continue;
            if (site.attrNs.empty() && site.attrLocal.empty()) continue;
            if (auto value = RenamedValue(site, rule, a.value, from, to)) {
              a.value = std::move(*value);
              ++changed;
              done = true;
              break;
            }
          }
          if (done) break;
        }
      }
    });
  }
  for (const auto& hook : hooks_) hook(kind, from, to);
  return changed;
}

// ---------- Extensions ----------

std::vector<std::string> ReferenceRegistry::UnknownExtensions(pugi::xml_node root) const {
  std::vector<std::string> out;
  Walker walker(root);
  walker.Run([&](pugi::xml_node e) {
    if (QNameLocal(e.name()) != "ext" || QNameLocal(e.parent().name()) != "extLst") return;
    pugi::xml_attribute uri = e.attribute("uri");
    if (uri && ClassifyExtension(uri.value()) == ExtensionClass::Unknown) out.emplace_back(uri.value());
  });
  return out;
}

std::vector<StrandedExtension> ReferenceRegistry::WouldStrand(pugi::xml_node root,
                                                              const std::vector<EditImpact>& edit) const {
  std::vector<StrandedExtension> out;
  if (edit.empty()) return out;
  Walker walker(root);
  walker.Run([&](pugi::xml_node e) {
    if (QNameLocal(e.name()) != "ext" || QNameLocal(e.parent().name()) != "extLst") return;
    pugi::xml_attribute uri = e.attribute("uri");
    if (!uri || ClassifyExtension(uri.value()) != ExtensionClass::Unknown) return;
    for (const auto& impact : edit) {
      if (impact.values.empty()) {
        out.push_back({uri.value(), impact.kind, {}});
        continue;
      }
      for (const auto& value : impact.values) {
        if (SubtreeMentions(e, value)) {
          out.push_back({uri.value(), impact.kind, value});
          break;
        }
      }
    }
  });
  return out;
}

// ---------- Pasting ----------

namespace {

enum class Gen { Number, Hex, Name, Spid };
struct RenewSpec {
  RefKind kind;
  Gen gen;
  bool everywhere;  // definitions live in another part: renew what the pasted content names, too
};
constexpr RenewSpec kRenewed[] = {
    {RefKind::ParagraphId, Gen::Hex, false},       {RefKind::BookmarkId, Gen::Number, false},
    {RefKind::BookmarkName, Gen::Name, false},     {RefKind::CommentId, Gen::Number, true},
    {RefKind::CommentDurableId, Gen::Hex, true},   {RefKind::PermissionId, Gen::Number, false},
    {RefKind::MoveRangeId, Gen::Number, false},    {RefKind::CustomXmlRangeId, Gen::Number, false},
    {RefKind::FootnoteId, Gen::Number, true},      {RefKind::EndnoteId, Gen::Number, true},
    {RefKind::DrawingId, Gen::Number, false},      {RefKind::VmlShapeId, Gen::Spid, false},
    {RefKind::ContentControlId, Gen::Number, false},
};

const RenewSpec* SpecOf(RefKind kind) {
  for (const auto& s : kRenewed)
    if (s.kind == kind) return &s;
  return nullptr;
}

constexpr std::string_view kSpidPrefix = "_x0000_s";
constexpr size_t kMaxBookmarkName = 40;  // Word's limit

}  // namespace

IdRenewer::IdRenewer(const ReferenceRegistry& registry) : registry_(registry) {}

bool IdRenewer::Renews(RefKind kind, uint8_t role) const {
  const RenewSpec* spec = SpecOf(kind);
  if (!spec) return false;
  return (role & (kDefines | kOpens)) != 0 || spec->everywhere;
}

std::string IdRenewer::Key(RefKind kind, std::string_view value) const {
  for (const auto& rule : registry_.Rules())
    if (rule.kind == kind && rule.ignoreCase) return LowerAscii(value);
  return std::string(value);
}

void IdRenewer::Take(RefKind kind, std::string_view value) {
  taken_[static_cast<size_t>(kind)].insert(Key(kind, value));
}

void IdRenewer::Take(pugi::xml_node root) {
  for (const auto& ref : registry_.Find(root)) {
    if (!SpecOf(ref.kind)) continue;
    ForEachRefValue(*ref.site, ref.kind, ref.Value(), [&](std::string_view v) { Take(ref.kind, v); });
  }
}

std::string IdRenewer::NewValue(RefKind kind, std::string_view old, const std::set<std::string>& avoid) {
  const auto& taken = taken_[static_cast<size_t>(kind)];
  auto used = [&](const std::string& v) { return taken.count(Key(kind, v)) || avoid.count(Key(kind, v)); };
  switch (SpecOf(kind)->gen) {
    case Gen::Number: {
      long long next = 0;
      for (const auto* set : {&taken, &avoid})
        for (const auto& v : *set) {
          char* end = nullptr;
          long long n = std::strtoll(v.c_str(), &end, 10);
          if (end && *end == '\0' && !v.empty()) next = std::max(next, n + 1);
        }
      while (used(std::to_string(next))) ++next;
      return std::to_string(next);
    }
    case Gen::Hex:
      for (;;) {
        uint32_t n = (0x1F2E3D4Cu + ++counter_ * 0x9E3779B1u) & 0x7FFFFFFFu;
        char buf[16];
        std::snprintf(buf, sizeof buf, "%08X", n);
        if (n != 0 && !used(buf)) return buf;
      }
    case Gen::Name:
      for (int n = 1;; ++n) {
        std::string suffix = "_" + std::to_string(n);
        size_t cut = std::min(old.size(), kMaxBookmarkName - suffix.size());
        while (cut > 0 && cut < old.size() && (static_cast<unsigned char>(old[cut]) & 0xC0) == 0x80) --cut;  // whole characters
        std::string candidate = std::string(old.substr(0, cut)) + suffix;
        if (!used(candidate)) return candidate;
      }
    case Gen::Spid: {
      unsigned long next = 1025;  // Office's first shape id
      for (const auto* set : {&taken, &avoid})
        for (const auto& v : *set)
          if (v.starts_with(kSpidPrefix)) next = std::max(next, std::strtoul(v.c_str() + kSpidPrefix.size(), nullptr, 10) + 1);
      while (used(std::string(kSpidPrefix) + std::to_string(next))) ++next;
      return std::string(kSpidPrefix) + std::to_string(next);
    }
  }
  return std::string(old);
}

std::vector<IdRenewer::Renamed> IdRenewer::Renew(pugi::xml_node pasted) {
  std::array<std::vector<std::string>, kRefKindCount> mine;  // in document order
  std::array<std::set<std::string>, kRefKindCount> mineKeys;
  for (const auto& ref : registry_.Find(pasted)) {
    if (!Renews(ref.kind, ref.site->role)) continue;
    size_t k = static_cast<size_t>(ref.kind);
    ForEachRefValue(*ref.site, ref.kind, ref.Value(), [&](std::string_view v) {
      if (mineKeys[k].insert(Key(ref.kind, v)).second) mine[k].emplace_back(v);
    });
  }
  std::vector<Renamed> renamed;
  for (size_t k = 0; k < kRefKindCount; ++k) {
    auto kind = static_cast<RefKind>(k);
    for (const auto& value : mine[k]) {
      if (!taken_[k].count(Key(kind, value))) {
        taken_[k].insert(Key(kind, value));
        continue;
      }
      std::string to = NewValue(kind, value, mineKeys[k]);
      registry_.RenameInTree(kind, value, to, pasted);
      taken_[k].insert(Key(kind, to));
      mineKeys[k].insert(Key(kind, to));
      renamed.push_back({kind, value, to});
    }
  }
  return renamed;
}

}  // namespace fw::pkg
