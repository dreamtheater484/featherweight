#include "slide/edit/Journal.h"

#include <cstring>

#include "slide/edit/EditInternal.h"

namespace fw::slide::edit {

namespace {

void PutU(std::string& out, uint64_t v) {
  do {
    uint8_t b = v & 0x7f;
    v >>= 7;
    if (v) b |= 0x80;
    out.push_back(static_cast<char>(b));
  } while (v);
}

void PutS(std::string& out, std::string_view s) {
  PutU(out, s.size());
  out.append(s);
}

bool GetU(std::string_view& in, uint64_t& v) {
  v = 0;
  for (int shift = 0; shift < 64; shift += 7) {
    if (in.empty()) return false;
    uint8_t b = static_cast<uint8_t>(in.front());
    in.remove_prefix(1);
    v |= static_cast<uint64_t>(b & 0x7f) << shift;
    if (!(b & 0x80)) return true;
  }
  return false;
}

bool GetS(std::string_view& in, std::string_view& s) {
  uint64_t n = 0;
  if (!GetU(in, n) || n > in.size()) return false;
  s = in.substr(0, static_cast<size_t>(n));
  in.remove_prefix(static_cast<size_t>(n));
  return true;
}

}  // namespace

void Journal::Changed(const PartStore& store, const std::set<std::string>& keys) {
  (void)store;
  dirty_.insert(keys.begin(), keys.end());
  ever_.insert(keys.begin(), keys.end());
  // Content types change with most structural edits; cheap to record each time.
  typesDirty_ = typesEver_ = true;
}

std::string Journal::Records(const PartStore& store, const std::set<std::string>& keys, bool types) const {
  std::string out;
  for (const std::string& key : keys) {
    const auto& parts = store.Parts();
    if (auto it = parts.find(key); it != parts.end()) {
      const PartState& s = it->second;
      if (s.Removed()) {
        out.push_back(static_cast<char>(JournalOp::PartRemoved));
        PutS(out, s.name);
      } else {
        std::vector<uint8_t> bytes;
        store.ReadPart(s.name, bytes);
        out.push_back(static_cast<char>(JournalOp::Part));
        PutS(out, s.name);
        PutS(out, std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
      }
    } else {
      out.push_back(static_cast<char>(JournalOp::PartOriginal));
      PutS(out, key);
    }
    const auto& rels = store.RelationshipSets();
    if (auto it = rels.find(key); it != rels.end()) {
      out.push_back(static_cast<char>(JournalOp::Rels));
      PutS(out, it->second->source);
      PutS(out, it->second->Serialize());
    } else {
      out.push_back(static_cast<char>(JournalOp::RelsOriginal));
      PutS(out, key);
    }
  }
  if (types) {
    if (TypesPtr t = store.GetTypes()) {
      out.push_back(static_cast<char>(JournalOp::Types));
      PutS(out, t->Serialize());
    } else {
      out.push_back(static_cast<char>(JournalOp::TypesOriginal));
    }
  }
  return out;
}

std::string Journal::All(const Editor& editor) const {
  std::string out;
  out.push_back(static_cast<char>(JournalOp::Version));
  PutU(out, kVersion);
  const PartStore& store = *editor.Deck().Edits();
  std::set<std::string> keys = ever_;
  for (const auto& [key, s] : store.Parts()) keys.insert(key);
  for (const auto& [key, r] : store.RelationshipSets()) keys.insert(key);
  return out + Records(store, keys, true);
}

std::string Journal::Unwritten(const Editor& editor) const {
  return Records(*editor.Deck().Edits(), dirty_, typesDirty_);
}

void Journal::Written() {
  dirty_.clear();
  typesDirty_ = false;
}

void Journal::Saved() {
  dirty_.clear();
  ever_.clear();
  typesDirty_ = typesEver_ = false;
}

Result Replay(Editor& editor, std::string_view records, size_t* applied) {
  size_t count = 0;
  if (applied) *applied = 0;
  Editor::Impl& m = editor.Internals();
  const PartStore& store = m.store;
  Call call(editor, u"Recover");
  std::string_view in = records;
  bool ok = true;
  while (!in.empty() && ok) {
    auto op = static_cast<JournalOp>(static_cast<uint8_t>(in.front()));
    in.remove_prefix(1);
    std::string_view name, bytes;
    uint64_t version = 0;
    switch (op) {
      case JournalOp::Version:
        ok = GetU(in, version) && version >= 1 && version <= Journal::kVersion;
        break;
      case JournalOp::Part:
        ok = GetS(in, name) && GetS(in, bytes) && name.starts_with("/");
        if (ok) {
          if (name.ends_with(".xml") || name.ends_with(".rels")) {
            auto dom = ParseDom(bytes, store.Package().GetLimits(), name);
            ok = dom != nullptr;
            if (ok) m.PutXml(name, std::move(dom), {});
          } else {
            m.PutBytes(name, std::vector<uint8_t>(bytes.begin(), bytes.end()), {});
          }
        }
        break;
      case JournalOp::PartRemoved:
        ok = GetS(in, name) && name.starts_with("/");
        if (ok) {
          std::string key = pkg::PartKey(name);
          m.work.doms.erase(key);
          m.work.parts[key] = PartState{std::string(name), nullptr, nullptr};
          m.work.names[key] = std::string(name);
        }
        break;
      case JournalOp::PartOriginal: {
        ok = GetS(in, name) && name.starts_with("/");
        if (!ok) break;
        // Back to the file's own: the package's bytes, as a part of the store.
        std::vector<uint8_t> original;
        std::string key = pkg::PartKey(name);
        const pkg::Part* p = store.Package().FindPart(name);
        if (p && !p->deleted && store.Package().ReadPart(name, original).Ok()) {
          std::string_view text(reinterpret_cast<const char*>(original.data()), original.size());
          if (store.Package().IsXmlPart(name)) {
            if (auto dom = ParseDom(text, store.Package().GetLimits(), name)) m.PutXml(p->name, std::move(dom), {});
          } else {
            m.PutBytes(p->name, std::move(original), {});
          }
        } else {
          m.work.doms.erase(key);
          m.work.parts[key] = PartState{std::string(name), nullptr, nullptr};
          m.work.names[key] = std::string(name);
        }
        break;
      }
      case JournalOp::Rels: {
        ok = GetS(in, name) && GetS(in, bytes);
        if (!ok) break;
        pkg::RelationshipSet set;
        ok = pkg::ParseRelationships(bytes, name, store.Package().GetLimits(), "relationships", set).Ok();
        if (ok) {
          pkg::RelationshipSet& rels = m.EditRels(name);
          rels.items = std::move(set.items);
        }
        break;
      }
      case JournalOp::RelsOriginal: {
        ok = GetS(in, name);
        if (!ok) break;
        pkg::RelationshipSet& rels = m.EditRels(name);
        const pkg::RelationshipSet* original = store.Package().RelationshipsOf(name);
        rels.items = original ? original->items : std::vector<pkg::Relationship>{};
        break;
      }
      case JournalOp::Types: {
        ok = GetS(in, bytes);
        if (!ok) break;
        pkg::ContentTypes types;
        ok = pkg::ParseContentTypes(bytes, store.Package().GetLimits(), types).Ok();
        if (ok) m.EditTypes() = std::move(types);
        break;
      }
      case JournalOp::TypesOriginal:
        m.EditTypes() = store.Package().Types();
        break;
      case JournalOp::Saved:
        break;
      default:
        ok = false;
        break;
    }
    if (ok) ++count;
  }
  m.Full();
  if (applied) *applied = count;
  Result r = call.End({});
  if (!ok && r) return Result::Fail(u"Some of the changes couldn't be recovered.");
  return r;
}

}  // namespace fw::slide::edit
