#include "pkg/Check.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <memory>
#include <set>
#include <unordered_map>

#include "pkg/Formula.h"
#include "pkg/Snapshot.h"
#include "pkg/XmlDom.h"
#include "pkg/XmlPull.h"

namespace fw::pkg {
namespace {

constexpr std::string_view kX = "http://schemas.openxmlformats.org/spreadsheetml/2006/main";
constexpr std::string_view kC = "http://schemas.openxmlformats.org/drawingml/2006/chart";
constexpr std::string_view kXv = "urn:schemas-microsoft-com:office:excel";
constexpr std::string_view kVmlContentType = "application/vnd.openxmlformats-officedocument.vmlDrawing";
constexpr size_t kMaxMessages = 200;  // per list, so a ruined file still gives a readable report
constexpr int kMaxDepth = 2;          // packages embedded in embedded packages
constexpr uint32_t kMaxSeriesPoints = 65536;

std::string LowerAscii(std::string_view s) {
  std::string out(s);
  for (auto& c : out)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
  return out;
}

bool EndsWithNoCase(std::string_view s, std::string_view suffix) {
  return s.size() >= suffix.size() && LowerAscii(s.substr(s.size() - suffix.size())) == suffix;
}

std::string_view AsText(const std::vector<uint8_t>& bytes) {
  return std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

std::string Quote(std::string_view v) {
  return "\"" + (v.size() <= 60 ? std::string(v) : std::string(v.substr(0, 57)) + "...") + "\"";
}

// Relationship types end with their name: ".../relationships/calcChain".
bool TypeIs(std::string_view type, std::string_view name) {
  return type.size() > name.size() && type.ends_with(name) && type[type.size() - name.size() - 1] == '/';
}

std::string Describe(const FoundRef& ref) {
  std::string out = ref.element.name();
  if (ref.attr) out += std::string(" ") + ref.attr.name() + "=" + Quote(ref.attr.value());
  else out += " " + Quote(ref.element.child_value());
  return out;
}

std::string Times(size_t n) { return n == 2 ? "twice" : std::to_string(n) + " times"; }

template <class Fn>
void ForEachElement(pugi::xml_node root, Fn&& fn) {
  pugi::xml_node cur = root;
  for (;;) {
    if (cur.type() == pugi::node_element) fn(cur);
    if (pugi::xml_node child = cur.first_child()) {
      cur = child;
      continue;
    }
    while (cur != root && !cur.next_sibling()) cur = cur.parent();
    if (cur == root) return;
    cur = cur.next_sibling();
  }
}

pugi::xml_node Child(pugi::xml_node n, std::string_view ns, std::string_view local) {
  for (auto c : n.children())
    if (IsElement(c, ns, local)) return c;
  return {};
}

// The text of a shared or inline string: its x:t elements, without phonetic runs (x:rPh).
std::string StringText(pugi::xml_node si) {
  std::string out;
  ForEachElement(si, [&](pugi::xml_node e) {
    if (!IsElement(e, kX, "t")) return;
    for (pugi::xml_node a = e.parent(); a && a != si; a = a.parent())
      if (IsElement(a, kX, "rPh")) return;
    out += e.child_value();
  });
  return out;
}

bool ParseNumber(std::string_view s, double& out) {
  if (s.empty() || s.size() > 64) return false;
  std::string buf(s);
  char* end = nullptr;
  out = std::strtod(buf.c_str(), &end);
  return end == buf.c_str() + buf.size() && std::isfinite(out);
}

bool SameValue(std::string_view a, std::string_view b) {
  if (a == b) return true;
  double x = 0, y = 0;
  return ParseNumber(a, x) && ParseNumber(b, y) && std::fabs(x - y) <= 1e-9 * std::max({1.0, std::fabs(x), std::fabs(y)});
}

std::string NormalCell(std::string_view ref) {
  uint32_t col = 0, row = 0;
  return ParseCellRef(ref, col, row) ? CellName(col, row) : std::string();
}

class Checker;

struct Sheet {
  std::string name, id, part;
  bool loaded = false;
  std::unordered_map<std::string, std::string> values;  // "B2" -> the value as stored
  std::set<std::string> formulas;                         // cells with a formula
};

struct Workbook {
  bool present = false;
  std::vector<Sheet> sheets;
  std::vector<std::string> strings;  // shared strings
  Checker* owner = nullptr;

  Sheet* ByName(std::string_view name) {
    for (auto& s : sheets)
      if (LowerAscii(s.name) == LowerAscii(name)) return &s;
    return nullptr;
  }
  Sheet* ById(std::string_view id) {
    for (auto& s : sheets)
      if (s.id == id) return &s;
    return nullptr;
  }
};

class Checker {
 public:
  Checker(const Package& pkg, const ReferenceRegistry& registry, CheckReport& report, std::string prefix, int depth)
      : pkg_(pkg), reg_(registry), rep_(report), prefix_(std::move(prefix)), depth_(depth) {
    book_.owner = this;
  }

  void Run();
  Workbook& Book() { return book_; }
  void LoadSheet(Sheet& sheet);

 private:
  struct PartDom {
    std::string name;
    std::string scope;  // the document it belongs to (RefScope::Document)
    XmlDom dom;
  };
  struct Embedded {
    std::unique_ptr<Package> package;
    std::unique_ptr<Checker> checker;
  };

  void Add(std::vector<std::string>& list, std::string_view part, const std::string& msg) {
    if (list.size() >= kMaxMessages) {
      if (list.size() == kMaxMessages) list.push_back(prefix_ + "(more not shown)");
      return;
    }
    if (part.starts_with('/')) part.remove_prefix(1);
    list.push_back(prefix_ + (part.empty() ? std::string() : std::string(part) + ": ") + msg);
  }
  void Error(std::string_view part, const std::string& msg) { Add(rep_.errors, part, msg); }
  void Warn(std::string_view part, const std::string& msg) { Add(rep_.warnings, part, msg); }
  void Issue(bool lenient, std::string_view part, const std::string& msg) { lenient ? Warn(part, msg) : Error(part, msg); }

  std::string MainPart() const;
  PartDom* Dom(std::string_view part) {
    auto it = index_.find(PartKey(part));
    return it == index_.end() ? nullptr : it->second;
  }

  void CheckRelationshipsAndTypes();
  void LoadParts();
  void AssignScopes();
  void CheckRules();
  void CheckAnchor(const FoundRef& ref, const PartDom& part);
  void LoadWorkbook();
  void CheckCalcChain();
  void CheckNotes();
  void CheckCharts();
  void CompareCache(const PartDom& part, std::string_view formula, pugi::xml_node cache, Workbook& book,
                    std::string_view bookName);
  Checker* EmbeddedChecker(const std::string& part);

  const Package& pkg_;
  const ReferenceRegistry& reg_;
  CheckReport& rep_;
  std::string prefix_;  // names the embedded package in messages
  int depth_;
  DocFormat format_ = DocFormat::Any;
  std::vector<std::unique_ptr<PartDom>> parts_;
  std::unordered_map<std::string, PartDom*> index_;  // PartKey -> part
  Workbook book_;
  std::map<std::string, Embedded> embedded_;  // PartKey -> package
};

std::string Checker::MainPart() const {
  if (const auto* rels = pkg_.RelationshipsOf("/"))
    for (const auto& r : rels->items)
      if (TypeIs(r.type, "officeDocument") && !r.external) return r.targetPart;
  return {};
}

void Checker::Run() {
  Status st = pkg_.Verify();
  if (!st.Ok()) {
    Error({}, st.message);
    return;
  }
  format_ = FormatOf(pkg_);
  CheckRelationshipsAndTypes();
  LoadParts();
  AssignScopes();
  CheckRules();
  if (format_ == DocFormat::Excel) {
    LoadWorkbook();
    CheckCalcChain();
    CheckNotes();
  }
  CheckCharts();
  for (const auto& part : pkg_.Parts()) {
    if (part.deleted) continue;
    for (std::string_view ext : {".xlsx", ".xlsm", ".docx", ".docm", ".pptx", ".pptm"})
      if (EndsWithNoCase(part.name, ext)) EmbeddedChecker(part.name);
  }
  for (const auto& p : parts_)
    for (const auto& uri : reg_.UnknownExtensions(p->dom.Root())) rep_.unknownExtensions.push_back(prefix_ + p->name.substr(1) + ": " + uri);
}

// ---------- Relationships and content types ----------

void Checker::CheckRelationshipsAndTypes() {
  if (!pkg_.RelationshipsOf("/")) Error({}, "the package has no relationships (_rels/.rels)");
  for (const auto& set : pkg_.Relationships()) {
    const std::string& where = set.relsPart;
    if (set.source != "/" && !pkg_.FindPart(set.source))
      Warn(where, "relationships for " + set.source + ", which doesn't exist");
    std::set<std::string> ids;
    for (const auto& r : set.items) {
      ++rep_.relationships;
      if (r.id.empty()) Error(where, "a relationship has no id");
      else if (!ids.insert(r.id).second) Error(where, "id " + r.id + " is used twice");
      if (r.external) continue;
      if (r.targetPart.empty()) Error(where, r.id + " has no target");
      else if (!pkg_.FindPart(r.targetPart)) Error(where, r.id + " points to " + r.targetPart + ", which doesn't exist");
    }
  }
  for (const auto& part : pkg_.Parts()) {
    if (part.deleted || part.name == kContentTypesPart) continue;
    if (pkg_.ContentTypeOf(part.name).empty()) Error(part.name, "it has no content type");
  }
  for (const auto& [name, type] : pkg_.Types().overrides)
    if (!pkg_.FindPart(name)) Warn({}, "a content type is given for " + name + ", which doesn't exist");
}

// ---------- Parts and documents ----------

void Checker::LoadParts() {
  std::vector<uint8_t> bytes;
  for (const auto& part : pkg_.Parts()) {
    if (part.deleted || part.name == kContentTypesPart || !SourceOfRelsPart(part.name).empty()) continue;
    bool vml = pkg_.ContentTypeOf(part.name) == kVmlContentType || EndsWithNoCase(part.name, ".vml");
    if (!vml && !pkg_.IsXmlPart(part.name)) continue;
    auto p = std::make_unique<PartDom>();
    p->name = part.name;
    Status st = pkg_.ReadPart(part.name, bytes);
    if (st.Ok()) st = p->dom.Parse(AsText(bytes), pkg_.GetLimits(), std::string_view(part.name).substr(1));
    if (!st.Ok()) {
      // Old VML isn't always well-formed XML; Office reads it with a forgiving parser.
      Issue(vml, part.name, (vml ? "not checked: " : "") + st.message);
      continue;
    }
    if (!p->dom.Root()) continue;
    index_.emplace(PartKey(part.name), p.get());
    parts_.push_back(std::move(p));
  }
}

// A package holds one document, except that a Word glossary (building blocks) is a document of its
// own, with its own ids: parts belong to the document whose main part reaches them first.
void Checker::AssignScopes() {
  std::unordered_map<std::string, std::string> scopeOf;
  std::vector<std::string> roots;
  auto walk = [&](const std::string& start) {
    std::vector<std::string> queue{start};
    while (!queue.empty()) {
      std::string part = std::move(queue.back());
      queue.pop_back();
      const RelationshipSet* rels = pkg_.RelationshipsOf(part);
      if (!rels) continue;
      for (const auto& r : rels->items) {
        if (r.external || r.targetPart.empty()) continue;
        if (TypeIs(r.type, "glossaryDocument")) {
          roots.push_back(r.targetPart);
          continue;
        }
        if (scopeOf.emplace(PartKey(r.targetPart), start).second) queue.push_back(r.targetPart);
      }
    }
  };
  std::string main = MainPart();
  if (!main.empty()) roots.push_back(main);
  for (size_t i = 0; i < roots.size(); ++i) {
    std::string root = roots[i];  // walk() may grow `roots`
    if (scopeOf.emplace(PartKey(root), root).second) walk(root);
  }
  for (auto& p : parts_) {
    auto it = scopeOf.find(PartKey(p->name));
    p->scope = it != scopeOf.end() ? it->second : main.empty() ? "/" : main;  // unreached parts: the main document's
  }
}

// ---------- The registry's rules ----------

void Checker::CheckRules() {
  const auto& rules = reg_.Rules();
  struct Def {
    std::string value, part;
    size_t count = 0;
  };
  struct Ref {
    std::string key, value, part, scope, desc;
    bool lenient = false;
  };
  struct RuleState {
    std::map<std::string, std::map<std::string, Def>> defs, opens;  // scope -> key -> first seen
    std::vector<Ref> refs;
  };
  struct PairState {
    std::string value;
    size_t opens = 0, closes = 0;
    bool endedFirst = false;
  };
  std::vector<RuleState> state(rules.size());

  for (auto& p : parts_) {
    std::map<size_t, std::map<std::string, PairState>> pairs;  // rule -> key -> state
    std::map<size_t, std::vector<bool>> fields;                 // rule -> open fields (separator seen)
    const RelationshipSet* rels = pkg_.RelationshipsOf(p->name);
    for (const auto& ref : reg_.Find(p->dom.Root())) {
      const RefRule& rule = *ref.rule;
      if (rule.format != DocFormat::Any && rule.format != format_) continue;
      const RefSite& site = *ref.site;
      size_t ri = static_cast<size_t>(&rule - rules.data());
      bool lenient = rule.lenient || site.lenient;
      if (rule.kind == RefKind::RelationshipId) {
        if (rels && rels->Find(ref.Value())) ++rep_.references;
        else Issue(lenient, p->name, std::string(ref.attr.name()) + "=" + Quote(ref.Value()) + " doesn't match any of its relationships");
        continue;
      }
      if (rule.nested) {
        if (ref.alternate) continue;
        auto& open = fields[ri];
        if (site.role & kOpens) {
          open.push_back(false);
        } else if (site.role & kInside) {
          if (open.empty()) Issue(lenient, p->name, "a " + std::string(rule.name) + " separator is outside any " + std::string(rule.name));
          else if (open.back()) Issue(lenient, p->name, "a " + std::string(rule.name) + " has two separators");
          else open.back() = true;
        } else if (site.role & kCloses) {
          if (open.empty()) {
            Issue(lenient, p->name, "a " + std::string(rule.name) + " ends but doesn't begin");
          } else {
            open.pop_back();
            ++rep_.pairs;
          }
        }
        continue;
      }
      if (site.role == kAnchors) {
        CheckAnchor(ref, *p);
        continue;
      }
      RuleState& rs = state[ri];
      const std::string& scope = rule.scope == RefScope::Part ? p->name : p->scope;
      ForEachRefValue(site, rule.kind, ref.Value(), [&](std::string_view v) {
        std::string key = rule.ignoreCase ? LowerAscii(v) : std::string(v);
        if (site.role & kDefines) {
          // A later AlternateContent branch repeats the first: its definitions resolve references,
          // but don't count twice.
          auto [it, fresh] = rs.defs[scope].try_emplace(key);
          Def& d = it->second;
          if (fresh) {
            d.value = v;
            d.part = p->name;
            ++rep_.ids;
            if (rule.valid && !rule.valid(v)) Issue(rule.lenient, p->name, std::string(rule.name) + " " + Quote(v) + " isn't valid");
          }
          if (!ref.alternate) ++d.count;
        }
        if ((site.role & kOpens) && !ref.alternate) {
          Def& d = rs.opens[scope][key];
          if (d.count++ == 0) {
            d.value = v;
            d.part = p->name;
          }
          PairState& ps = pairs[ri][key];
          ps.value = v;
          ++ps.opens;
        }
        if ((site.role & kCloses) && !ref.alternate) {
          PairState& ps = pairs[ri][key];
          ps.value = v;
          if (ps.closes == ps.opens) ps.endedFirst = true;
          ++ps.closes;
        }
        if (site.role & kRefers) rs.refs.push_back({key, std::string(v), p->name, scope, Describe(ref), lenient});
      });
    }
    for (const auto& [ri, byKey] : pairs) {
      const RefRule& rule = rules[ri];
      for (const auto& [key, ps] : byKey) {
        std::string what = std::string(rule.name) + " " + Quote(ps.value);
        if (ps.opens > ps.closes) Issue(rule.lenient, p->name, what + " starts but doesn't end");
        else if (ps.closes > ps.opens) Issue(rule.lenient, p->name, what + " ends but doesn't start");
        else if (ps.endedFirst) Issue(rule.lenient, p->name, what + " ends before it starts");
        else rep_.pairs += ps.opens;
      }
    }
    for (const auto& [ri, open] : fields) {
      if (open.empty()) continue;
      std::string name(rules[ri].name);
      Issue(rules[ri].lenient, p->name,
            open.size() == 1 ? "a " + name + " begins but doesn't end" : std::to_string(open.size()) + " " + name + "s begin but don't end");
    }
  }

  for (size_t ri = 0; ri < rules.size(); ++ri) {
    const RefRule& rule = rules[ri];
    RuleState& rs = state[ri];
    if (rule.unique) {
      for (const auto* table : {&rs.defs, &rs.opens})
        for (const auto& [scope, byKey] : *table)
          for (const auto& [key, d] : byKey)
            if (d.count > 1) Issue(rule.lenient, d.part, std::string(rule.name) + " " + Quote(d.value) + " is used " + Times(d.count));
    }
    for (const auto& ref : rs.refs) {
      auto it = rs.defs.find(ref.scope);
      if (it != rs.defs.end() && it->second.count(ref.key)) ++rep_.references;
      else Issue(ref.lenient, ref.part, ref.desc + " names " + std::string(rule.name) + " " + Quote(ref.value) + ", which doesn't exist");
    }
  }
}

void Checker::CheckAnchor(const FoundRef& ref, const PartDom& part) {
  bool lenient = ref.rule->lenient || ref.site->lenient;
  if (ref.kind == RefKind::CellRange && ref.site->syntax != RefSyntax::Formula) {
    bool ok = true;
    ForEachRefValue(*ref.site, ref.kind, ref.Value(), [&](std::string_view v) { ok = ok && IsCellRange(v); });
    if (ok) ++rep_.references;
    else Issue(lenient, part.name, Describe(ref) + " isn't a cell reference");
  } else if (ref.kind == RefKind::VmlAnchor) {
    bool ok = true;
    ForEachRefValue(*ref.site, ref.kind, ref.Value(), [&](std::string_view v) {
      std::string_view n = v.ends_with(',') ? v.substr(0, v.size() - 1) : v;
      ok = ok && !n.empty() && n.size() <= 9 && std::all_of(n.begin(), n.end(), [](char c) { return c >= '0' && c <= '9'; });
    });
    if (!ok) Issue(lenient, part.name, Describe(ref) + " isn't a row or column number");
  }
}

// ---------- Excel: the calculation chain and comment notes ----------

void Checker::LoadWorkbook() {
  std::string main = MainPart();
  PartDom* wb = Dom(main);
  if (!wb) return;
  book_.present = true;
  const RelationshipSet* rels = pkg_.RelationshipsOf(main);
  for (auto sheets : wb->dom.Root().children()) {
    if (!IsElement(sheets, kX, "sheets")) continue;
    for (auto s : sheets.children()) {
      if (!IsElement(s, kX, "sheet")) continue;
      Sheet sheet;
      sheet.name = s.attribute("name").value();
      sheet.id = s.attribute("sheetId").value();
      pugi::xml_attribute rid = FindAttribute(s, kOfficeRelationshipsNs, "id");
      if (const Relationship* r = rels && rid ? rels->Find(rid.value()) : nullptr; r && !r->external) sheet.part = r->targetPart;
      book_.sheets.push_back(std::move(sheet));
    }
  }
  if (!rels) return;
  for (const auto& r : rels->items) {
    if (!TypeIs(r.type, "sharedStrings") || r.external) continue;
    if (PartDom* sst = Dom(r.targetPart))
      for (auto si : sst->dom.Root().children())
        if (IsElement(si, kX, "si")) book_.strings.push_back(StringText(si));
  }
}

void Checker::LoadSheet(Sheet& sheet) {
  if (sheet.loaded) return;
  sheet.loaded = true;
  PartDom* dom = Dom(sheet.part);
  if (!dom) return;
  pugi::xml_node data = Child(dom->dom.Root(), kX, "sheetData");
  for (auto row : data.children()) {
    if (!IsElement(row, kX, "row")) continue;
    for (auto c : row.children()) {
      if (!IsElement(c, kX, "c")) continue;
      std::string ref = NormalCell(c.attribute("r").value());
      if (ref.empty()) continue;  // cells without r: TODO(S1), with the sheet reader
      if (Child(c, kX, "f")) sheet.formulas.insert(ref);
      std::string_view type = c.attribute("t").value();
      std::string value;
      if (type == "inlineStr") {
        value = StringText(Child(c, kX, "is"));
      } else {
        value = Child(c, kX, "v").child_value();
        if (type == "s") {
          char* end = nullptr;
          unsigned long i = std::strtoul(value.c_str(), &end, 10);
          value = !value.empty() && *end == '\0' && i < book_.strings.size() ? book_.strings[i] : std::string();
        }
      }
      if (!value.empty()) sheet.values[ref] = std::move(value);
    }
  }
}

// Excel repairs a file whose calculation chain names a cell without a formula, so a chain left
// stale by an edit is an error.
void Checker::CheckCalcChain() {
  const RelationshipSet* rels = pkg_.RelationshipsOf(MainPart());
  if (!rels) return;
  for (const auto& r : rels->items) {
    if (!TypeIs(r.type, "calcChain") || r.external) continue;
    PartDom* chain = Dom(r.targetPart);
    if (!chain) continue;
    std::string sheetId;  // an entry without i is on the previous entry's sheet
    for (auto c : chain->dom.Root().children()) {
      if (!IsElement(c, kX, "c")) continue;
      if (pugi::xml_attribute i = c.attribute("i")) sheetId = i.value();
      Sheet* sheet = book_.ById(sheetId);
      if (!sheet) {
        Error(chain->name, "the calculation chain names sheet id " + Quote(sheetId) + ", which doesn't exist");
        continue;
      }
      LoadSheet(*sheet);
      std::string ref = NormalCell(c.attribute("r").value());
      if (sheet->formulas.count(ref)) ++rep_.derived;
      else Error(chain->name, "the calculation chain is stale: " + QuoteSheetName(sheet->name) + "!" + std::string(c.attribute("r").value()) + " has no formula");
    }
  }
}

// Excel shows each comment through a VML note anchored to the same cell (0-based row and column).
void Checker::CheckNotes() {
  for (auto& sheet : book_.sheets) {
    const RelationshipSet* rels = pkg_.RelationshipsOf(sheet.part);
    if (!rels) continue;
    std::vector<PartDom*> comments, drawings;
    for (const auto& r : rels->items) {
      if (r.external) continue;
      PartDom* dom = Dom(r.targetPart);
      if (!dom) continue;
      if (TypeIs(r.type, "comments")) comments.push_back(dom);
      else if (TypeIs(r.type, "vmlDrawing")) drawings.push_back(dom);
    }
    if (comments.empty()) continue;
    std::set<std::string> notes;
    for (PartDom* vml : drawings) {
      ForEachElement(vml->dom.Root(), [&](pugi::xml_node e) {
        if (!IsElement(e, kXv, "ClientData") || std::string_view(e.attribute("ObjectType").value()) != "Note") return;
        char* end = nullptr;
        std::string rowText = Child(e, kXv, "Row").child_value(), colText = Child(e, kXv, "Column").child_value();
        unsigned long row = std::strtoul(rowText.c_str(), &end, 10);
        unsigned long col = std::strtoul(colText.c_str(), &end, 10);
        if (!rowText.empty() && !colText.empty() && row < kMaxRow && col < kMaxColumn)
          notes.insert(CellName(static_cast<uint32_t>(col + 1), static_cast<uint32_t>(row + 1)));
      });
    }
    std::set<std::string> commented;
    for (PartDom* part : comments) {
      ForEachElement(part->dom.Root(), [&](pugi::xml_node e) {
        if (!IsElement(e, kX, "comment")) return;
        std::string ref = NormalCell(e.attribute("ref").value());
        commented.insert(ref);
        if (drawings.empty()) return;
        if (notes.count(ref)) ++rep_.references;
        else Warn(part->name, "the comment on " + ref + " has no note in the sheet's VML drawing");
      });
    }
    if (drawings.empty()) Warn(comments[0]->name, "the comments have no VML drawing to show them");
    for (const auto& note : notes)
      if (!commented.count(note)) Warn(drawings[0]->name, "a note on " + note + " has no comment");
  }
}

// ---------- Charts ----------

// A chart's caches are what Office draws; its workbook (embedded, or the one it is in) is where the
// data comes from. They must agree (derived data).
void Checker::CheckCharts() {
  for (auto& p : parts_) {
    pugi::xml_node root = p->dom.Root();
    if (!IsElement(root, kC, "chartSpace")) continue;
    Workbook* book = nullptr;
    std::string bookName;
    if (pugi::xml_node ext = Child(root, kC, "externalData")) {
      const RelationshipSet* rels = pkg_.RelationshipsOf(p->name);
      pugi::xml_attribute rid = FindAttribute(ext, kOfficeRelationshipsNs, "id");
      const Relationship* r = rels && rid ? rels->Find(rid.value()) : nullptr;
      if (r && !r->external) {
        if (Checker* nested = EmbeddedChecker(r->targetPart)) {
          book = &nested->Book();
          bookName = "its embedded workbook";
        }
      }
    } else if (format_ == DocFormat::Excel) {
      book = &book_;
      bookName = "the workbook";
    }
    if (!book || !book->present) continue;
    ForEachElement(root, [&](pugi::xml_node e) {
      bool num = IsElement(e, kC, "numRef");
      if (!num && !IsElement(e, kC, "strRef")) return;
      pugi::xml_node f = Child(e, kC, "f"), cache = Child(e, kC, num ? "numCache" : "strCache");
      if (f && cache) CompareCache(*p, f.child_value(), cache, *book, bookName);
    });
  }
}

void Checker::CompareCache(const PartDom& part, std::string_view formula, pugi::xml_node cache, Workbook& book,
                           std::string_view bookName) {
  // One area on one sheet: 'Sales'!$B$2:$B$7. Several areas, 3D references and two-dimensional
  // ranges (multi-level categories) aren't compared yet.
  SheetToken token;
  int tokens = 0;
  ForEachSheetToken(formula, [&](const SheetToken& t) {
    token = t;
    ++tokens;
  });
  if (tokens != 1 || !token.last.empty() || token.begin != 0 || token.end + 1 > formula.size()) return;
  std::string_view range = formula.substr(token.end + 1);
  size_t colon = range.find(':');
  uint32_t c1 = 0, r1 = 0, c2 = 0, r2 = 0;
  if (!ParseCellRef(range.substr(0, colon), c1, r1)) return;
  if (colon == std::string_view::npos) {
    c2 = c1;
    r2 = r1;
  } else if (!ParseCellRef(range.substr(colon + 1), c2, r2)) {
    return;
  }
  if ((c1 != c2 && r1 != r2) || c2 < c1 || r2 < r1) return;
  Sheet* sheet = book.ByName(token.first);
  if (!sheet) {
    Warn(part.name, "the chart names sheet " + Quote(token.first) + ", which isn't in " + std::string(bookName));
    return;
  }
  book.owner->LoadSheet(*sheet);
  std::map<uint32_t, std::string> cached;
  for (auto pt : cache.children())
    if (IsElement(pt, kC, "pt")) cached[static_cast<uint32_t>(pt.attribute("idx").as_uint())] = Child(pt, kC, "v").child_value();
  uint32_t n = std::min(c1 == c2 ? r2 - r1 + 1 : c2 - c1 + 1, kMaxSeriesPoints);
  for (uint32_t i = 0; i < n; ++i) {
    std::string cell = c1 == c2 ? CellName(c1, r1 + i) : CellName(c1 + i, r1);
    auto want = sheet->values.find(cell);
    auto have = cached.find(i);
    std::string_view w = want == sheet->values.end() ? std::string_view() : std::string_view(want->second);
    std::string_view h = have == cached.end() ? std::string_view() : std::string_view(have->second);
    if (!SameValue(h, w)) {
      Warn(part.name, "the chart's cached value " + Quote(h) + " for " + QuoteSheetName(sheet->name) + "!" + cell +
                          " doesn't match " + std::string(bookName) + " (" + Quote(w) + ")");
      return;  // one message per series
    }
    ++rep_.derived;
  }
}

Checker* Checker::EmbeddedChecker(const std::string& part) {
  std::string key = PartKey(part);
  if (auto it = embedded_.find(key); it != embedded_.end()) return it->second.checker.get();
  Embedded& slot = embedded_[key];
  if (depth_ >= kMaxDepth) return nullptr;
  std::vector<uint8_t> bytes;
  if (!pkg_.ReadPart(part, bytes).Ok() || bytes.size() < 4 || bytes[0] != 'P' || bytes[1] != 'K') return nullptr;
  Status st;
  slot.package = Package::Open(Snapshot::FromBytes(std::move(bytes)), pkg_.GetLimits(), st);
  if (!slot.package) {
    Error(part, "the embedded file doesn't open: " + st.message);
    return nullptr;
  }
  ++rep_.embedded;
  slot.checker = std::make_unique<Checker>(*slot.package, reg_, rep_, prefix_ + part.substr(1) + ": ", depth_ + 1);
  slot.checker->Run();
  return slot.checker.get();
}

}  // namespace

DocFormat FormatOf(const Package& pkg) {
  if (const auto* rels = pkg.RelationshipsOf("/")) {
    for (const auto& r : rels->items) {
      if (!TypeIs(r.type, "officeDocument") || r.external) continue;
      std::string type = LowerAscii(pkg.ContentTypeOf(r.targetPart));
      if (type.find("wordprocessingml") != std::string::npos || type.find("ms-word") != std::string::npos)
        return DocFormat::Word;
      if (type.find("spreadsheetml") != std::string::npos || type.find("ms-excel") != std::string::npos)
        return DocFormat::Excel;
      if (type.find("presentationml") != std::string::npos || type.find("ms-powerpoint") != std::string::npos)
        return DocFormat::PowerPoint;
    }
  }
  return DocFormat::Any;
}

CheckReport CheckPackage(const Package& pkg, const ReferenceRegistry& registry) {
  CheckReport report;
  Checker(pkg, registry, report, {}, 0).Run();
  return report;
}

}  // namespace fw::pkg
