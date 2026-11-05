// The comments' companion parts (plan §4.3 derived data: "comment companions"), written again
// from the model whenever comments.xml is: commentsExtended (replies and resolved state, by each
// comment's last paragraph's w14:paraId), commentsIds (a durable id for each) and
// commentsExtensible (when each was made, in UTC). Durable ids and UTC dates the parts already
// had are kept; new comments get new ones. Entries of deleted comments go.
#include <windows.h>

#include <cstdio>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "doc/docx/Save.h"
#include "pkg/Package.h"
#include "pkg/XmlDom.h"

namespace fw::doc::docx {

namespace {

constexpr std::string_view kW15 = "http://schemas.microsoft.com/office/word/2012/wordml";
constexpr std::string_view kW16Cid = "http://schemas.microsoft.com/office/word/2016/wordml/cid";
constexpr std::string_view kW16Cex = "http://schemas.microsoft.com/office/word/2018/wordml/cex";
constexpr std::string_view kMc = "http://schemas.openxmlformats.org/markup-compatibility/2006";

struct Companion {
  const char* local;
  const char* relType;
  const char* contentType;
};
constexpr Companion kExtended{"commentsExtended", "http://schemas.microsoft.com/office/2011/relationships/commentsExtended",
                              "application/vnd.openxmlformats-officedocument.wordprocessingml.commentsExtended+xml"};
constexpr Companion kIds{"commentsIds", "http://schemas.microsoft.com/office/2016/09/relationships/commentsIds",
                         "application/vnd.openxmlformats-officedocument.wordprocessingml.commentsIds+xml"};
constexpr Companion kExtensible{"commentsExtensible",
                                "http://schemas.microsoft.com/office/2018/08/relationships/commentsExtensible",
                                "application/vnd.openxmlformats-officedocument.wordprocessingml.commentsExtensible+xml"};

// The companion's part as the main part's relationship names it; empty when there is none.
std::string Existing(const pkg::Package& package, const std::string& main, const Companion& c) {
  if (const pkg::RelationshipSet* rels = package.RelationshipsOf(main))
    for (const pkg::Relationship& r : rels->items)
      if (!r.external && r.type == c.relType) return r.targetPart;
  return {};
}

// A new companion part: its content type and its relationship from the main part.
std::string Add(pkg::Package& package, const std::string& main, const Companion& c) {
  const std::string dir = main.substr(0, main.rfind('/') + 1);
  std::string name = dir + c.local + ".xml";
  for (int i = 2; package.FindPart(name); ++i) name = dir + c.local + std::to_string(i) + ".xml";
  pkg::ContentTypes types = package.Types();
  types.overrides.emplace_back(name, c.contentType);
  package.SetContentTypes(std::move(types));
  pkg::RelationshipSet rels;
  if (const pkg::RelationshipSet* have = package.RelationshipsOf(main)) {
    rels = *have;
  } else {
    rels.source = main;
    rels.relsPart = pkg::RelsPartFor(main);
  }
  pkg::Relationship rel;
  for (size_t n = rels.items.size() + 1;; ++n) {
    rel.id = "rId" + std::to_string(n);
    if (!rels.Find(rel.id)) break;
  }
  rel.type = c.relType;
  rel.target = name.substr(dir.size());
  rel.targetPart = name;
  rels.items.push_back(std::move(rel));
  package.SetRelationships(std::move(rels));
  return name;
}

// The part's root element children, read; nothing when it isn't there or can't be read.
template <class Fn>
void ReadEntries(const pkg::Package& package, const std::string& name, Fn fn) {
  std::vector<uint8_t> bytes;
  if (name.empty() || !package.FindPart(name) || !package.ReadPart(name, bytes).Ok()) return;
  pkg::XmlDom dom;
  if (!dom.Parse(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), package.GetLimits()).Ok())
    return;
  for (pugi::xml_node c : dom.Root().children())
    if (c.type() == pugi::node_element) fn(c);
}

// Word writes a comment's w:date in local time (with a Z); commentsExtensible has it in UTC.
std::string UtcOf(std::string_view local) {
  SYSTEMTIME st{}, utc{};
  unsigned y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
  const std::string text(local);
  if (sscanf_s(text.c_str(), "%4u-%2u-%2uT%2u:%2u:%2u", &y, &mo, &d, &h, &mi, &s) != 6) return W3cTime();
  st.wYear = static_cast<WORD>(y);
  st.wMonth = static_cast<WORD>(mo);
  st.wDay = static_cast<WORD>(d);
  st.wHour = static_cast<WORD>(h);
  st.wMinute = static_cast<WORD>(mi);
  st.wSecond = static_cast<WORD>(s);
  if (!TzSpecificLocalTimeToSystemTime(nullptr, &st, &utc)) return W3cTime();
  char buf[32];
  std::snprintf(buf, sizeof buf, "%04u-%02u-%02uT%02u:%02u:%02uZ", utc.wYear, utc.wMonth, utc.wDay, utc.wHour, utc.wMinute,
                utc.wSecond);
  return buf;
}

constexpr const char* kHead = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n";

}  // namespace

pkg::Status WriteCommentParts(Document& doc) {
  const Part* main = doc.FindPart(PartKind::Document);
  if (!main || !doc.FindPart(PartKind::Comments)) return {};
  pkg::Package& package = doc.Package();
  const std::vector<CommentInfo> comments = doc.Comments();
  std::string extended = Existing(package, main->name, kExtended), ids = Existing(package, main->name, kIds),
              extensible = Existing(package, main->name, kExtensible);
  if (comments.empty() && extended.empty() && ids.empty() && extensible.empty()) return {};

  // What the parts say now: each paragraph's durable id, each durable id's time.
  std::unordered_map<std::string, std::string> durable, dates;
  std::unordered_set<std::string> used;
  ReadEntries(package, ids, [&](pugi::xml_node c) {
    const std::string para = pkg::FindAttribute(c, kW16Cid, "paraId").value(),
                      id = pkg::FindAttribute(c, kW16Cid, "durableId").value();
    if (!para.empty() && !id.empty()) durable.emplace(para, id);
  });
  ReadEntries(package, extensible, [&](pugi::xml_node c) {
    const std::string id = pkg::FindAttribute(c, kW16Cex, "durableId").value(),
                      utc = pkg::FindAttribute(c, kW16Cex, "dateUtc").value();
    if (!id.empty() && !utc.empty()) dates.emplace(id, utc);
  });
  for (const auto& [para, id] : durable) used.insert(id);

  std::unordered_map<std::string_view, std::string_view> paraOf;  // comment id -> its paraId
  for (const CommentInfo& c : comments) paraOf.emplace(c.id, c.paraId);
  uint32_t seed = GetTickCount() ^ 0x9E3779B9u;
  auto fresh = [&]() {
    for (;;) {
      seed ^= seed << 13;
      seed ^= seed >> 17;
      seed ^= seed << 5;
      const uint32_t v = seed & 0x7FFFFFFFu;  // Word's durable ids stay below 0x7FFFFFFF
      if (v == 0 || v == 0x7FFFFFFFu) continue;
      char buf[9];
      std::snprintf(buf, sizeof buf, "%08X", v);
      if (used.insert(buf).second) return std::string(buf);
    }
  };

  std::string ex = std::string(kHead) + "<w15:commentsEx xmlns:mc=\"" + std::string(kMc) + "\" xmlns:w15=\"" +
                   std::string(kW15) + "\" mc:Ignorable=\"w15\">";
  std::string id = std::string(kHead) + "<w16cid:commentsIds xmlns:mc=\"" + std::string(kMc) + "\" xmlns:w16cid=\"" +
                   std::string(kW16Cid) + "\" mc:Ignorable=\"w16cid\">";
  std::string ext = std::string(kHead) + "<w16cex:commentsExtensible xmlns:mc=\"" + std::string(kMc) +
                    "\" xmlns:w16cex=\"" + std::string(kW16Cex) + "\" mc:Ignorable=\"w16cex\">";
  for (const CommentInfo& c : comments) {
    if (c.paraId.empty()) continue;  // nothing names it
    const std::string para(c.paraId);
    ex += "<w15:commentEx w15:paraId=\"" + para + "\"";
    if (auto it = paraOf.find(c.parentId); !c.parentId.empty() && it != paraOf.end() && !it->second.empty())
      ex += " w15:paraIdParent=\"" + std::string(it->second) + "\"";
    ex += std::string(" w15:done=\"") + (c.done ? "1" : "0") + "\"/>";
    auto d = durable.find(para);
    const std::string dur = d != durable.end() ? d->second : fresh();
    id += "<w16cid:commentId w16cid:paraId=\"" + para + "\" w16cid:durableId=\"" + dur + "\"/>";
    auto t = dates.find(dur);
    ext += "<w16cex:commentExtensible w16cex:durableId=\"" + dur + "\" w16cex:dateUtc=\"" +
           (t != dates.end() ? t->second : UtcOf(c.date)) + "\"/>";
  }
  ex += "</w15:commentsEx>";
  id += "</w16cid:commentsIds>";
  ext += "</w16cex:commentsExtensible>";

  if (extended.empty()) extended = Add(package, main->name, kExtended);
  if (ids.empty()) ids = Add(package, main->name, kIds);
  if (extensible.empty()) extensible = Add(package, main->name, kExtensible);
  package.SetPart(extended, std::vector<uint8_t>(ex.begin(), ex.end()));
  package.SetPart(ids, std::vector<uint8_t>(id.begin(), id.end()));
  package.SetPart(extensible, std::vector<uint8_t>(ext.begin(), ext.end()));
  return {};
}

}  // namespace fw::doc::docx
