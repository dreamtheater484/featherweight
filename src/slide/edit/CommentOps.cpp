// Comments on slides (P3, p3-review): reading PowerPoint's modern and legacy comment parts, and the
// operations on them, written as PowerPoint 365 writes them (Comments.h; checked against the files
// PowerPoint on the reference PC saves: modernComment_100_51B5B541.xml for slide id 256, creation id
// 1370862913, positions in EMUs, times in UTC).

#include "slide/edit/Comments.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cwctype>
#include <map>

#include "pkg/OfficeCrypto.h"
#include "slide/edit/EditInternal.h"

namespace fw::slide::edit {

namespace {

constexpr std::string_view kP188 = "http://schemas.microsoft.com/office/powerpoint/2018/8/main";
constexpr std::string_view kP15 = "http://schemas.microsoft.com/office/powerpoint/2012/main";
constexpr std::string_view kP14 = "http://schemas.microsoft.com/office/powerpoint/2010/main";
constexpr std::string_view kPc = "http://schemas.microsoft.com/office/powerpoint/2013/main/command";
constexpr std::string_view kRelModern = "http://schemas.microsoft.com/office/2018/10/relationships/comments";
constexpr std::string_view kRelModernAuthors = "http://schemas.microsoft.com/office/2018/10/relationships/authors";
constexpr std::string_view kRelLegacy = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/comments";
constexpr std::string_view kRelLegacyAuthors = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/commentAuthors";
constexpr std::string_view kTypeModern = "application/vnd.ms-powerpoint.comments+xml";
constexpr std::string_view kTypeModernAuthors = "application/vnd.ms-powerpoint.authors+xml";
constexpr std::string_view kTypeLegacyAuthors =
    "application/vnd.openxmlformats-officedocument.presentationml.commentAuthors+xml";
constexpr std::string_view kExtCommentRel = "{6950BFC3-D8DA-4A85-94F7-54DA5524770B}";
constexpr std::string_view kExtCreationId = "{BB962C8B-B14F-4D97-AF65-F5344CB8AC3E}";
constexpr std::string_view kExtThreading = "{C676402C-5697-4E1C-873F-D02D1690AC5C}";
constexpr std::string_view kExtPresence = "{19B8F6BF-5375-455C-9EA6-DF929625EA0E}";
constexpr std::string_view kLegacy = "legacy:";
constexpr float kLegacyPerPoint = 8;  // legacy p:pos is in master units, 576 to the inch
constexpr double kEmuPerPoint = 12700;
constexpr char kDecl[] = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n";
constexpr char kRootNs[] =
    " xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\""
    " xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\"";

const pkg::Relationship* RelOfType(const pkg::RelationshipSet* rels, std::string_view type) {
  if (!rels) return nullptr;
  for (const pkg::Relationship& r : rels->items)
    if (!r.external && r.type == type) return &r;
  return nullptr;
}

std::string PartOfType(const pkg::RelationshipSet* rels, std::string_view type) {
  const pkg::Relationship* r = RelOfType(rels, type);
  return r ? r->targetPart : std::string();
}

template <class F>
void Children(pugi::xml_node parent, std::string_view ns, std::string_view local, F&& f) {
  for (pugi::xml_node c = parent.first_child(); c; c = c.next_sibling())
    if (c.type() == pugi::node_element && IsElement(c, ns, local)) f(c);
}

pugi::xml_node First(pugi::xml_node parent, std::string_view ns, std::string_view local) {
  for (pugi::xml_node c = parent.first_child(); c; c = c.next_sibling())
    if (c.type() == pugi::node_element && IsElement(c, ns, local)) return c;
  return {};
}

// A text body's paragraphs, joined with '\n' (a line break is one too).
std::wstring BodyText(pugi::xml_node txBody) {
  std::wstring out;
  bool first = true;
  Children(txBody, kA, "p", [&](pugi::xml_node p) {
    if (!first) out += L'\n';
    first = false;
    for (pugi::xml_node c = p.first_child(); c; c = c.next_sibling()) {
      if (IsElement(c, kA, "br")) out += L'\n';
      else if (IsElement(c, kA, "r") || IsElement(c, kA, "fld")) out += Wide(First(c, kA, "t").child_value());
    }
  });
  return out;
}

// The text as paragraphs of one run each, as PowerPoint writes a comment's.
void SetBodyText(pugi::xml_node txBody, std::wstring_view text) {
  const std::string a = PrefixFor(txBody, kA, "a");
  for (pugi::xml_node c = txBody.first_child(); c;) {
    pugi::xml_node next = c.next_sibling();
    if (IsElement(c, kA, "p")) txBody.remove_child(c);
    c = next;
  }
  if (!First(txBody, kA, "bodyPr")) txBody.prepend_child((a + ":bodyPr").c_str());
  if (!First(txBody, kA, "lstStyle")) txBody.insert_child_after((a + ":lstStyle").c_str(), First(txBody, kA, "bodyPr"));
  const std::wstring clean = XmlText(text);
  size_t from = 0;
  while (from <= clean.size()) {
    size_t end = clean.find(L'\n', from);
    if (end == std::wstring::npos) end = clean.size();
    std::wstring line = clean.substr(from, end - from);
    if (!line.empty() && line.back() == L'\r') line.pop_back();
    pugi::xml_node p = txBody.append_child((a + ":p").c_str());
    if (line.empty()) {
      p.append_child((a + ":endParaRPr").c_str()).append_attribute("lang") = "en-US";
    } else {
      pugi::xml_node r = p.append_child((a + ":r").c_str());
      r.append_child((a + ":rPr").c_str()).append_attribute("lang") = "en-US";
      r.append_child((a + ":t").c_str()).text() = Utf8(line).c_str();
    }
    from = end + 1;
  }
}

std::wstring Trimmed(std::wstring_view s) {
  size_t b = 0, e = s.size();
  while (b < e && (s[b] == L' ' || s[b] == L'\n' || s[b] == L'\r' || s[b] == L'\t')) ++b;
  while (e > b && (s[e - 1] == L' ' || s[e - 1] == L'\n' || s[e - 1] == L'\r' || s[e - 1] == L'\t')) --e;
  return std::wstring(s.substr(b, e - b));
}

// Ids as Office writes them ("{8-4-4-4-12}", version 4): pkg/OfficeCrypto's, Windows' random numbers.
std::string NewGuid() { return pkg::crypto::NewGuid(); }

// A random 32-bit number, not 0 (a slide's creation id): from a new GUID's first eight hex digits.
uint32_t NewCreationId() {
  uint32_t id = 0;
  while (id == 0) id = static_cast<uint32_t>(std::strtoul(NewGuid().substr(1, 8).c_str(), nullptr, 16));
  return id;
}

// Now as xsd:dateTime with milliseconds, in UTC (modern) or local time (legacy dt).
std::string Now(bool utc) {
  using namespace std::chrono;
  const auto now = system_clock::now();
  const std::time_t t = system_clock::to_time_t(now);
  const int ms = static_cast<int>(duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000);
  std::tm tm{};
  utc ? gmtime_s(&tm, &t) : localtime_s(&tm, &t);
  char buf[40];
  std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d.%03d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                tm.tm_hour, tm.tm_min, tm.tm_sec, ms);
  return buf;
}

// Minutes from local time to UTC (Windows' bias: UTC = local + bias), as p15:threadingInfo keeps it.
long TimeZoneBias() {
  const std::time_t t = std::time(nullptr);
  std::tm local{}, utc{};
  localtime_s(&local, &t);
  gmtime_s(&utc, &t);
  utc.tm_isdst = local.tm_isdst;
  return static_cast<long>(std::difftime(std::mktime(&utc), std::mktime(&local)) / 60);
}

std::string Hex(uint32_t v) {
  char buf[16];
  std::snprintf(buf, sizeof buf, "%X", v);
  return buf;
}

// ---- Authors

// The comment's author (authorId) from the presentation's authors part of that kind.
void SetAuthor(Editor::Impl& m, std::string_view relType, std::string_view ns, pugi::xml_node cm, Comment& c) {
  const std::string part = PartOfType(m.Rels(m.PresentationPart()), relType);
  pugi::xml_node root = part.empty() ? pugi::xml_node() : m.Root(part);
  const std::string_view id = cm.attribute("authorId").value();
  for (pugi::xml_node a = root.first_child(); a; a = a.next_sibling()) {
    if (a.type() != pugi::node_element || !(IsElement(a, ns, "author") || IsElement(a, ns, "cmAuthor"))) continue;
    if (id != a.attribute("id").value()) continue;
    c.author = Wide(a.attribute("name").value());
    c.initials = Wide(a.attribute("initials").value());
    return;
  }
}

// A new part holding just its root element `<qname ...namespaces/>`.
std::shared_ptr<pkg::XmlDom> NewPart(Editor& ed, std::string_view qname, std::string_view ns) {
  const std::string prefix(qname.substr(0, qname.find(':')));
  std::string xml = std::string(kDecl) + "<" + std::string(qname) + kRootNs;
  if (prefix != "p")
    xml += " xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\"";
  xml += " xmlns:" + prefix + "=\"" + std::string(ns) + "\"/>";
  return ParseDom(xml, ed.Deck().GetPackage().GetLimits(), qname);
}

// The modern author entry for `who` (the first with that name), added when there's none: its id.
std::string ModernAuthor(Editor& ed, const CommentAuthor& who) {
  Editor::Impl& m = ed.Internals();
  const std::string pres = m.PresentationPart();
  std::string part = PartOfType(m.Rels(pres), kRelModernAuthors);
  const std::string name = Utf8(who.name);
  if (!part.empty()) {
    pugi::xml_node root = m.Root(part);
    for (pugi::xml_node a = root.first_child(); a; a = a.next_sibling())
      if (IsElement(a, kP188, "author") && name == a.attribute("name").value()) return a.attribute("id").value();
  } else {
    part = m.Exists("/ppt/authors.xml") ? m.NewName("/ppt/authors", ".xml") : "/ppt/authors.xml";
    auto dom = NewPart(ed, "p188:authorLst", kP188);
    if (!dom) return {};
    m.PutXml(part, dom, kTypeModernAuthors);
    m.AddRel(pres, kRelModernAuthors, part);
  }
  pugi::xml_node root = m.Edit(part);
  if (!root || !IsElement(root, kP188, "authorLst")) return {};
  const std::string id = NewGuid();
  pugi::xml_node a = root.append_child((PrefixFor(root, kP188, "p188") + ":author").c_str());
  a.append_attribute("id") = id.c_str();
  a.append_attribute("name") = name.c_str();
  a.append_attribute("initials") = Utf8(who.initials.empty() ? CommentInitials(who.name) : who.initials).c_str();
  a.append_attribute("userId") = name.c_str();
  a.append_attribute("providerId") = "None";
  return id;
}

// The legacy author entry for `who`, added when there's none, with its next comment index.
bool LegacyAuthor(Editor& ed, const CommentAuthor& who, std::string& id, uint32_t& idx) {
  Editor::Impl& m = ed.Internals();
  const std::string pres = m.PresentationPart();
  std::string part = PartOfType(m.Rels(pres), kRelLegacyAuthors);
  if (part.empty()) {
    part = m.Exists("/ppt/commentAuthors.xml") ? m.NewName("/ppt/commentAuthors", ".xml") : "/ppt/commentAuthors.xml";
    auto dom = NewPart(ed, "p:cmAuthorLst", kP);
    if (!dom) return false;
    m.PutXml(part, dom, kTypeLegacyAuthors);
    m.AddRel(pres, kRelLegacyAuthors, part);
  }
  pugi::xml_node root = m.Edit(part);
  if (!root || !IsElement(root, kP, "cmAuthorLst")) return false;
  const std::string name = Utf8(who.name);
  pugi::xml_node mine;
  uint32_t maxId = 0, count = 0;
  Children(root, kP, "cmAuthor", [&](pugi::xml_node a) {
    ++count;
    maxId = std::max(maxId, static_cast<uint32_t>(std::strtoul(a.attribute("id").value(), nullptr, 10)));
    if (!mine && name == a.attribute("name").value()) mine = a;
  });
  if (!mine) {
    const std::string p = PrefixFor(root, kP, "p");
    mine = root.append_child((p + ":cmAuthor").c_str());
    mine.append_attribute("id") = std::to_string(maxId + 1).c_str();
    mine.append_attribute("name") = name.c_str();
    mine.append_attribute("initials") = Utf8(who.initials.empty() ? CommentInitials(who.name) : who.initials).c_str();
    mine.append_attribute("lastIdx") = "0";
    mine.append_attribute("clrIdx") = std::to_string(count).c_str();
    pugi::xml_node ext = mine.append_child((p + ":extLst").c_str()).append_child((p + ":ext").c_str());
    ext.append_attribute("uri") = std::string(kExtPresence).c_str();
    pugi::xml_node presence = ext.append_child("p15:presenceInfo");
    presence.append_attribute("xmlns:p15") = std::string(kP15).c_str();
    presence.append_attribute("userId") = name.c_str();
    presence.append_attribute("providerId") = "None";
  }
  id = mine.attribute("id").value();
  idx = static_cast<uint32_t>(std::strtoul(mine.attribute("lastIdx").value(), nullptr, 10)) + 1;
  if (!mine.attribute("lastIdx")) mine.append_attribute("lastIdx");
  mine.attribute("lastIdx") = std::to_string(idx).c_str();
  return true;
}

// ---- The slide's parts

std::string ModernPart(Editor::Impl& m, std::string_view slidePart) { return PartOfType(m.Rels(slidePart), kRelModern); }
std::string LegacyPart(Editor::Impl& m, std::string_view slidePart) { return PartOfType(m.Rels(slidePart), kRelLegacy); }

std::string LegacyId(pugi::xml_attribute author, pugi::xml_attribute idx) {
  return std::string(kLegacy) + author.value() + ":" + idx.value();
}

// The thread a legacy comment answers (p15:threadingInfo/p15:parentCm), its id; empty: none.
std::string LegacyParent(pugi::xml_node cm) {
  pugi::xml_node list = First(cm, kP, "extLst");
  for (pugi::xml_node ext = list.first_child(); ext; ext = ext.next_sibling()) {
    pugi::xml_node info = First(ext, kP15, "threadingInfo");
    if (pugi::xml_node parent = First(info, kP15, "parentCm"))
      return LegacyId(parent.attribute("authorId"), parent.attribute("idx"));
  }
  return {};
}

uint32_t ShapeAnchor(pugi::xml_node anchor) {
  if (!anchor) return 0;
  if (LocalNameOf(anchor) == "spMk") return static_cast<uint32_t>(std::strtoul(anchor.attribute("id").value(), nullptr, 10));
  for (pugi::xml_node c = anchor.first_child(); c; c = c.next_sibling())
    if (c.type() == pugi::node_element)
      if (uint32_t id = ShapeAnchor(c)) return id;
  return 0;
}

void ReadLegacy(Editor::Impl& m, std::string_view part, std::vector<Comment>& out) {
  pugi::xml_node root = m.Root(part);
  if (!IsElement(root, kP, "cmLst")) return;
  std::vector<Comment> all;
  Children(root, kP, "cm", [&](pugi::xml_node cm) {
    Comment c;
    c.legacy = true;
    c.id = LegacyId(cm.attribute("authorId"), cm.attribute("idx"));
    c.parent = LegacyParent(cm);
    SetAuthor(m, kRelLegacyAuthors, kP, cm, c);
    c.created = cm.attribute("dt").value();
    c.text = Wide(First(cm, kP, "text").child_value());
    if (pugi::xml_node pos = First(cm, kP, "pos")) {
      c.pos = {static_cast<float>(Number(pos, "x", 0)) / kLegacyPerPoint, static_cast<float>(Number(pos, "y", 0)) / kLegacyPerPoint};
      c.hasPos = true;
    }
    all.push_back(std::move(c));
  });
  // Threads in order, each with its replies after it; a reply whose thread isn't there is a thread.
  auto rootOf = [&](const Comment& c) {
    std::string id = c.parent;
    for (int depth = 0; depth < 8 && !id.empty(); ++depth) {
      auto it = std::find_if(all.begin(), all.end(), [&](const Comment& o) { return o.id == id; });
      if (it == all.end()) return std::string();
      if (it->parent.empty()) return it->id;
      id = it->parent;
    }
    return std::string();
  };
  for (Comment& c : all) c.parent = rootOf(c);
  for (const Comment& t : all) {
    if (!t.parent.empty()) continue;
    out.push_back(t);
    for (const Comment& r : all)
      if (r.parent == t.id) out.push_back(r);
  }
}

void ReadModern(Editor::Impl& m, std::string_view part, std::vector<Comment>& out) {
  pugi::xml_node root = m.Root(part);
  if (!IsElement(root, kP188, "cmLst")) return;
  auto fill = [&](pugi::xml_node n, Comment& c) {
    c.id = n.attribute("id").value();
    SetAuthor(m, kRelModernAuthors, kP188, n, c);
    c.created = n.attribute("created").value();
    c.utc = true;
    c.text = BodyText(First(n, kP188, "txBody"));
  };
  Children(root, kP188, "cm", [&](pugi::xml_node cm) {
    Comment c;
    fill(cm, c);
    const std::string_view status = cm.attribute("status").value();
    c.resolved = status == "resolved" || status == "closed";
    if (pugi::xml_node pos = First(cm, kP188, "pos")) {
      c.pos = {static_cast<float>(Number(pos, "x", 0) / kEmuPerPoint), static_cast<float>(Number(pos, "y", 0) / kEmuPerPoint)};
      c.hasPos = true;
    }
    for (pugi::xml_node k = cm.first_child(); k; k = k.next_sibling()) {
      if (k.type() != pugi::node_element || NamespaceOf(k) == kP188) continue;
      c.shape = ShapeAnchor(k);  // the anchor (pc:sldMkLst, ac:deMkLst, ac:txMkLst...)
      break;
    }
    out.push_back(c);
    Children(First(cm, kP188, "replyLst"), kP188, "reply", [&](pugi::xml_node reply) {
      Comment r = c;
      fill(reply, r);
      r.parent = c.id;
      out.push_back(std::move(r));
    });
  });
}

// Where a comment is, for changing it: the parts edited (Edit), the element, its thread's first.
struct Found {
  std::string part;
  bool legacy = false;
  pugi::xml_node node, thread;
};

Found Locate(Editor::Impl& m, std::string_view slidePart, std::string_view id) {
  Found f;
  if (id.starts_with(kLegacy)) {
    f.legacy = true;
    f.part = LegacyPart(m, slidePart);
    pugi::xml_node root = f.part.empty() ? pugi::xml_node() : m.Edit(f.part);
    if (!IsElement(root, kP, "cmLst")) return {};
    auto find = [&](std::string_view want) {
      pugi::xml_node hit;
      Children(root, kP, "cm", [&](pugi::xml_node cm) {
        if (!hit && LegacyId(cm.attribute("authorId"), cm.attribute("idx")) == want) hit = cm;
      });
      return hit;
    };
    f.node = find(id);
    if (!f.node) return {};
    f.thread = f.node;
    for (int depth = 0; depth < 8; ++depth) {
      const std::string parent = LegacyParent(f.thread);
      pugi::xml_node up = parent.empty() ? pugi::xml_node() : find(parent);
      if (!up) break;
      f.thread = up;
    }
    return f;
  }
  f.part = ModernPart(m, slidePart);
  pugi::xml_node root = f.part.empty() ? pugi::xml_node() : m.Edit(f.part);
  if (!IsElement(root, kP188, "cmLst")) return {};
  Children(root, kP188, "cm", [&](pugi::xml_node cm) {
    if (f.node) return;
    if (id == cm.attribute("id").value()) f.node = f.thread = cm;
    Children(First(cm, kP188, "replyLst"), kP188, "reply", [&](pugi::xml_node r) {
      if (!f.node && id == r.attribute("id").value()) {
        f.node = r;
        f.thread = cm;
      }
    });
  });
  return f.node ? f : Found{};
}

// The slide's creation id (p14:creationId), made when it has none, as PowerPoint gives each slide.
uint32_t CreationId(Editor::Impl& m, std::string_view slidePart) {
  auto read = [](pugi::xml_node root) -> uint32_t {
    pugi::xml_node list = First(First(root, kP, "cSld"), kP, "extLst");
    for (pugi::xml_node ext = list.first_child(); ext; ext = ext.next_sibling())
      if (pugi::xml_node id = First(ext, kP14, "creationId"))
        return static_cast<uint32_t>(std::strtoul(id.attribute("val").value(), nullptr, 10));
    return 0;
  };
  if (uint32_t id = read(m.Root(slidePart))) return id;
  pugi::xml_node root = m.Edit(slidePart);
  pugi::xml_node cSld = First(root, kP, "cSld");
  if (!cSld) return 0;
  const std::string p = PrefixFor(root, kP, "p");
  pugi::xml_node list = First(cSld, kP, "extLst");
  if (!list) list = cSld.append_child((p + ":extLst").c_str());
  pugi::xml_node ext = list.append_child((p + ":ext").c_str());
  ext.append_attribute("uri") = std::string(kExtCreationId).c_str();
  pugi::xml_node c = ext.append_child("p14:creationId");
  c.append_attribute("xmlns:p14") = std::string(kP14).c_str();
  const uint32_t id = NewCreationId();
  c.append_attribute("val") = std::to_string(id).c_str();
  return id;
}

// The slide's p188:commentRel naming relationship `rid` (made in its p:extLst).
void AddCommentRel(pugi::xml_node slide, const std::string& rid) {
  const std::string p = PrefixFor(slide, kP, "p");
  const std::string r = PrefixFor(slide, kRelNs, "r");
  pugi::xml_node list = First(slide, kP, "extLst");
  if (!list) list = slide.append_child((p + ":extLst").c_str());
  pugi::xml_node ext = list.append_child((p + ":ext").c_str());
  ext.append_attribute("uri") = std::string(kExtCommentRel).c_str();
  pugi::xml_node rel = ext.append_child("p188:commentRel");
  rel.append_attribute("xmlns:p188") = std::string(kP188).c_str();
  rel.append_attribute((r + ":id").c_str()) = rid.c_str();
}

// The slide's p188:commentRel for `rid` (every one when empty) gone, and an empty p:extLst with it.
void RemoveCommentRel(pugi::xml_node slide, std::string_view rid, bool onlyEmpty) {
  pugi::xml_node list = First(slide, kP, "extLst");
  if (!list) return;
  for (pugi::xml_node ext = list.first_child(); ext;) {
    pugi::xml_node next = ext.next_sibling();
    if (pugi::xml_node rel = First(ext, kP188, "commentRel")) {
      const std::string_view id = FindAttribute(rel, kRelNs, "id").value();
      if (onlyEmpty ? id.empty() : (rid.empty() || id == rid)) list.remove_child(ext);
    }
    ext = next;
  }
  if (!First(list, kP, "ext")) slide.remove_child(list);
}

// The slide's comments part of a format gone, with its relationship (and, modern, the slide's link).
void RemovePart(Editor::Impl& m, std::string_view slidePart, std::string_view relType) {
  const pkg::RelationshipSet* rels = m.Rels(slidePart);
  const pkg::Relationship* r = RelOfType(rels, relType);
  if (!r) return;
  const std::string part = r->targetPart, rid = r->id;
  m.Remove(part);
  pkg::RelationshipSet& edit = m.EditRels(slidePart);
  std::erase_if(edit.items, [&](const pkg::Relationship& x) { return x.id == rid; });
  if (relType == kRelModern) RemoveCommentRel(m.Edit(slidePart), rid, false);
}

Result CheckSlide(Editor::Impl& m, size_t slide, std::string& part) {
  part = m.SlidePart(slide);
  if (part.empty()) return Result::Fail(u"That slide isn't in the presentation.");
  return {};
}

}  // namespace

std::wstring CommentInitials(std::wstring_view name) {
  std::wstring out;
  bool start = true;
  for (wchar_t ch : name) {
    const bool space = ch == L' ' || ch == L'\t' || ch == L'-' || ch == L'.';
    if (!space && start && out.size() < 4) out += static_cast<wchar_t>(std::towupper(ch));
    start = space;
  }
  return out;
}

std::vector<Comment> SlideComments(Editor& ed, size_t slide) {
  std::vector<Comment> out;
  Editor::Impl& m = ed.Internals();
  const std::string part = m.SlidePart(slide);
  if (part.empty()) return out;
  if (std::string legacy = LegacyPart(m, part); !legacy.empty()) ReadLegacy(m, legacy, out);
  if (std::string modern = ModernPart(m, part); !modern.empty()) ReadModern(m, modern, out);
  return out;
}

std::vector<size_t> CommentCounts(Editor& ed) {
  std::vector<size_t> out(ed.Deck().SlideCount());
  for (size_t i = 0; i < out.size(); ++i) out[i] = SlideComments(ed, i).size();
  return out;
}

Result AddComment(Editor& ed, size_t slide, draw::Point at, std::wstring_view text, const CommentAuthor& who, std::string* id) {
  Call call(ed, u"New Comment");
  Editor::Impl& m = ed.Internals();
  std::string sp;
  if (Result r = CheckSlide(m, slide, sp); !r) return call.End(r);
  const std::wstring body = Trimmed(text);
  if (body.empty()) return call.End(Result::Fail(u"A comment needs some text."));
  const std::vector<Editor::Impl::SlideEntry> list = m.SlideList();
  const uint32_t sldId = slide < list.size() ? list[slide].id : 0;
  const uint32_t cId = CreationId(m, sp);
  const std::string author = ModernAuthor(ed, who);
  if (!cId || author.empty()) return call.End(Result::Fail(u"The comment couldn't be added to this slide."));
  std::string part = ModernPart(m, sp);
  if (part.empty()) {
    part = "/ppt/comments/modernComment_" + Hex(sldId) + "_" + Hex(cId) + ".xml";
    if (m.Exists(part)) part = m.NewName("/ppt/comments/modernComment_" + Hex(sldId) + "_" + Hex(cId) + "_", ".xml");
    auto dom = NewPart(ed, "p188:cmLst", kP188);
    if (!dom) return call.End(Result::Fail(u"The comment couldn't be added to this slide."));
    m.PutXml(part, dom, kTypeModern);
    AddCommentRel(m.Edit(sp), m.AddRel(sp, kRelModern, part));
  }
  pugi::xml_node root = m.Edit(part);
  if (!IsElement(root, kP188, "cmLst")) return call.End(Result::Fail(u"This slide's comments can't be read."));
  const std::string p188 = PrefixFor(root, kP188, "p188");
  const std::string newId = NewGuid();
  pugi::xml_node cm = root.append_child((p188 + ":cm").c_str());
  cm.append_attribute("id") = newId.c_str();
  cm.append_attribute("authorId") = author.c_str();
  cm.append_attribute("created") = Now(true).c_str();
  pugi::xml_node anchor = cm.append_child("pc:sldMkLst");
  anchor.append_attribute("xmlns:pc") = std::string(kPc).c_str();
  anchor.append_child("pc:docMk");
  pugi::xml_node mk = anchor.append_child("pc:sldMk");
  mk.append_attribute("cId") = std::to_string(cId).c_str();
  mk.append_attribute("sldId") = std::to_string(sldId).c_str();
  pugi::xml_node pos = cm.append_child((p188 + ":pos").c_str());
  pos.append_attribute("x") = std::to_string(std::llround(std::max(0.f, at.x) * kEmuPerPoint)).c_str();
  pos.append_attribute("y") = std::to_string(std::llround(std::max(0.f, at.y) * kEmuPerPoint)).c_str();
  SetBodyText(cm.append_child((p188 + ":txBody").c_str()), body);
  m.CommentsChanged(sp);
  if (id) *id = newId;
  return call.End({});
}

Result ReplyToComment(Editor& ed, size_t slide, std::string_view id, std::wstring_view text, const CommentAuthor& who,
                      std::string* replyId) {
  Call call(ed, u"Reply to Comment");
  Editor::Impl& m = ed.Internals();
  std::string sp;
  if (Result r = CheckSlide(m, slide, sp); !r) return call.End(r);
  const std::wstring body = Trimmed(text);
  if (body.empty()) return call.End(Result::Fail(u"A reply needs some text."));
  Found f = Locate(m, sp, id);
  if (!f.node) return call.End(Result::Fail(u"That comment isn't on this slide."));
  std::string newId;
  if (f.legacy) {
    std::string author;
    uint32_t idx = 0;
    if (!LegacyAuthor(ed, who, author, idx)) return call.End(Result::Fail(u"The reply couldn't be added."));
    pugi::xml_node root = m.Edit(f.part);
    const std::string p = PrefixFor(root, kP, "p");
    pugi::xml_node cm = root.append_child((p + ":cm").c_str());
    cm.append_attribute("authorId") = author.c_str();
    cm.append_attribute("dt") = Now(false).c_str();
    cm.append_attribute("idx") = std::to_string(idx).c_str();
    if (pugi::xml_node pos = First(f.thread, kP, "pos")) cm.append_copy(pos);
    else {
      pugi::xml_node zero = cm.append_child((p + ":pos").c_str());
      zero.append_attribute("x") = "0";
      zero.append_attribute("y") = "0";
    }
    cm.append_child((p + ":text").c_str()).text() = Utf8(body).c_str();
    pugi::xml_node ext = cm.append_child((p + ":extLst").c_str()).append_child((p + ":ext").c_str());
    ext.append_attribute("uri") = std::string(kExtThreading).c_str();
    pugi::xml_node info = ext.append_child("p15:threadingInfo");
    info.append_attribute("xmlns:p15") = std::string(kP15).c_str();
    info.append_attribute("timeZoneBias") = std::to_string(TimeZoneBias()).c_str();
    pugi::xml_node parent = info.append_child("p15:parentCm");
    parent.append_attribute("authorId") = f.thread.attribute("authorId").value();
    parent.append_attribute("idx") = f.thread.attribute("idx").value();
    newId = std::string(kLegacy) + author + ":" + std::to_string(idx);
  } else {
    const std::string author = ModernAuthor(ed, who);
    if (author.empty()) return call.End(Result::Fail(u"The reply couldn't be added."));
    const std::string p188 = PrefixFor(f.thread, kP188, "p188");
    pugi::xml_node list = First(f.thread, kP188, "replyLst");
    if (!list) {
      // Schema order: the anchor, pos, replyLst, txBody, extLst.
      pugi::xml_node body2 = First(f.thread, kP188, "txBody");
      list = body2 ? f.thread.insert_child_before((p188 + ":replyLst").c_str(), body2)
                   : f.thread.append_child((p188 + ":replyLst").c_str());
    }
    newId = NewGuid();
    pugi::xml_node reply = list.append_child((p188 + ":reply").c_str());
    reply.append_attribute("id") = newId.c_str();
    reply.append_attribute("authorId") = author.c_str();
    reply.append_attribute("created") = Now(true).c_str();
    SetBodyText(reply.append_child((p188 + ":txBody").c_str()), body);
  }
  m.CommentsChanged(sp);
  if (replyId) *replyId = newId;
  return call.End({});
}

Result EditComment(Editor& ed, size_t slide, std::string_view id, std::wstring_view text) {
  Call call(ed, u"Edit Comment");
  Editor::Impl& m = ed.Internals();
  std::string sp;
  if (Result r = CheckSlide(m, slide, sp); !r) return call.End(r);
  const std::wstring body = Trimmed(text);
  if (body.empty()) return call.End(Result::Fail(u"A comment needs some text."));
  Found f = Locate(m, sp, id);
  if (!f.node) return call.End(Result::Fail(u"That comment isn't on this slide."));
  if (f.legacy) {
    pugi::xml_node t = First(f.node, kP, "text");
    if (!t) {
      const std::string p = PrefixFor(f.node, kP, "p");
      pugi::xml_node pos = First(f.node, kP, "pos");
      t = pos ? f.node.insert_child_after((p + ":text").c_str(), pos) : f.node.prepend_child((p + ":text").c_str());
    }
    t.text() = Utf8(body).c_str();
  } else {
    pugi::xml_node t = First(f.node, kP188, "txBody");
    if (!t) {
      const std::string p188 = PrefixFor(f.node, kP188, "p188");
      pugi::xml_node ext = First(f.node, kP188, "extLst");
      t = ext ? f.node.insert_child_before((p188 + ":txBody").c_str(), ext) : f.node.append_child((p188 + ":txBody").c_str());
    }
    SetBodyText(t, body);
  }
  m.CommentsChanged(sp);
  return call.End({});
}

Result DeleteComment(Editor& ed, size_t slide, std::string_view id) {
  Call call(ed, u"Delete Comment");
  Editor::Impl& m = ed.Internals();
  std::string sp;
  if (Result r = CheckSlide(m, slide, sp); !r) return call.End(r);
  Found f = Locate(m, sp, id);
  if (!f.node) return call.End(Result::Fail(u"That comment isn't on this slide."));
  pugi::xml_node root = m.Edit(f.part);
  if (f.legacy) {
    const std::string thread = f.node == f.thread ? LegacyId(f.node.attribute("authorId"), f.node.attribute("idx")) : "";
    if (!thread.empty()) {
      // Its replies, and replies to them, go too.
      for (pugi::xml_node c = root.first_child(); c;) {
        pugi::xml_node next = c.next_sibling();
        if (IsElement(c, kP, "cm") && c != f.node) {
          pugi::xml_node up = c;
          for (int depth = 0; depth < 8 && up; ++depth) {
            const std::string parent = LegacyParent(up);
            if (parent.empty()) {
              up = {};
              break;
            }
            if (parent == thread) break;
            pugi::xml_node hit;
            Children(root, kP, "cm", [&](pugi::xml_node o) {
              if (!hit && LegacyId(o.attribute("authorId"), o.attribute("idx")) == parent) hit = o;
            });
            up = hit;
          }
          if (up) root.remove_child(c);
        }
        c = next;
      }
    }
    root.remove_child(f.node);
    if (!First(root, kP, "cm")) RemovePart(m, sp, kRelLegacy);
  } else {
    pugi::xml_node parent = f.node.parent();
    parent.remove_child(f.node);
    if (IsElement(parent, kP188, "replyLst") && !First(parent, kP188, "reply")) parent.parent().remove_child(parent);
    if (!First(root, kP188, "cm")) RemovePart(m, sp, kRelModern);
  }
  m.CommentsChanged(sp);
  return call.End({});
}

Result DeleteAllComments(Editor& ed, const std::vector<size_t>& slides) {
  Call call(ed, u"Delete All Comments");
  Editor::Impl& m = ed.Internals();
  std::vector<size_t> which = slides;
  if (which.empty())
    for (size_t i = 0; i < ed.Deck().SlideCount(); ++i) which.push_back(i);
  bool any = false;
  for (size_t s : which) {
    std::string sp;
    if (Result r = CheckSlide(m, s, sp); !r) return call.End(r);
    const bool has = !ModernPart(m, sp).empty() || !LegacyPart(m, sp).empty();
    if (!has) continue;
    RemovePart(m, sp, kRelModern);
    RemovePart(m, sp, kRelLegacy);
    m.CommentsChanged(sp);
    any = true;
  }
  if (!any) return call.End(Result::Fail(u"There are no comments to delete."));
  return call.End({});
}

Result ResolveComment(Editor& ed, size_t slide, std::string_view id, bool resolved) {
  Call call(ed, resolved ? u"Resolve Thread" : u"Reopen Thread");
  Editor::Impl& m = ed.Internals();
  std::string sp;
  if (Result r = CheckSlide(m, slide, sp); !r) return call.End(r);
  Found f = Locate(m, sp, id);
  if (!f.node) return call.End(Result::Fail(u"That comment isn't on this slide."));
  if (f.legacy) return call.End(Result::Fail(u"This comment is in PowerPoint's older format, which can't mark it resolved."));
  f.thread.remove_attribute("status");
  if (resolved) f.thread.append_attribute("status") = "resolved";
  m.CommentsChanged(sp);
  return call.End({});
}

Result MoveComment(Editor& ed, size_t slide, std::string_view id, draw::Point at) {
  Call call(ed, u"Move Comment");
  Editor::Impl& m = ed.Internals();
  std::string sp;
  if (Result r = CheckSlide(m, slide, sp); !r) return call.End(r);
  Found f = Locate(m, sp, id);
  if (!f.node) return call.End(Result::Fail(u"That comment isn't on this slide."));
  at = {std::max(0.f, at.x), std::max(0.f, at.y)};
  if (f.legacy) {
    const std::string p = PrefixFor(f.thread, kP, "p");
    pugi::xml_node pos = First(f.thread, kP, "pos");
    if (!pos) pos = f.thread.prepend_child((p + ":pos").c_str());
    pos.remove_attribute("x");
    pos.remove_attribute("y");
    pos.append_attribute("x") = std::to_string(std::lround(at.x * kLegacyPerPoint)).c_str();
    pos.append_attribute("y") = std::to_string(std::lround(at.y * kLegacyPerPoint)).c_str();
  } else {
    const std::string p188 = PrefixFor(f.thread, kP188, "p188");
    pugi::xml_node pos = First(f.thread, kP188, "pos");
    if (!pos) {
      pugi::xml_node before = First(f.thread, kP188, "replyLst");
      if (!before) before = First(f.thread, kP188, "txBody");
      pos = before ? f.thread.insert_child_before((p188 + ":pos").c_str(), before) : f.thread.append_child((p188 + ":pos").c_str());
    }
    pos.remove_attribute("x");
    pos.remove_attribute("y");
    pos.append_attribute("x") = std::to_string(std::llround(at.x * kEmuPerPoint)).c_str();
    pos.append_attribute("y") = std::to_string(std::llround(at.y * kEmuPerPoint)).c_str();
  }
  m.CommentsChanged(sp);
  return call.End({});
}

void DropCommentRel(pugi::xml_node slideRoot) {
  if (IsElement(slideRoot, kP, "sld")) RemoveCommentRel(slideRoot, {}, true);
}

}  // namespace fw::slide::edit
