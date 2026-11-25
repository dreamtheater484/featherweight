#include "slide/edit/Save.h"

#include <algorithm>
#include <deque>
#include <set>

#include "core/Lang.h"
#include "slide/edit/EditInternal.h"
#include "slide/edit/Journal.h"

namespace fw::slide::edit {

namespace {

std::vector<uint8_t> Bytes(const std::string& s) { return std::vector<uint8_t>(s.begin(), s.end()); }

// Relationships of a source in the plan's state: the store's, else the package's (none for a part
// the edits removed).
const pkg::RelationshipSet* RelsIn(const SavePlan& plan, std::string_view source) {
  std::string key = pkg::PartKey(source);
  if (auto it = plan.rels.find(key); it != plan.rels.end()) return it->second.get();
  if (auto it = plan.parts.find(key); it != plan.parts.end() && it->second.Removed()) return nullptr;
  return plan.package->RelationshipsOf(source);
}

bool ExistsIn(const SavePlan& plan, std::string_view part) {
  std::string key = pkg::PartKey(part);
  if (auto it = plan.parts.find(key); it != plan.parts.end()) return !it->second.Removed();
  const pkg::Part* p = plan.package->FindPart(part);
  return p && !p->deleted;
}

// Every part reachable from the package's relationships, by PartKey.
std::set<std::string> Reachable(const SavePlan& plan, bool original) {
  std::set<std::string> seen;
  std::deque<std::string> queue{"/"};
  while (!queue.empty()) {
    std::string source = std::move(queue.front());
    queue.pop_front();
    const pkg::RelationshipSet* rels = original ? plan.package->RelationshipsOf(source) : RelsIn(plan, source);
    if (!rels) continue;
    for (const pkg::Relationship& r : rels->items) {
      if (r.external || r.targetPart.empty()) continue;
      if (original ? !plan.package->FindPart(r.targetPart) : !ExistsIn(plan, r.targetPart)) continue;
      if (seen.insert(pkg::PartKey(r.targetPart)).second) queue.push_back(r.targetPart);
    }
  }
  return seen;
}

}  // namespace

std::string_view MainContentType(FileFormat format) {
  switch (format) {
    case FileFormat::Pptx: return "application/vnd.openxmlformats-officedocument.presentationml.presentation.main+xml";
    case FileFormat::Pptm: return "application/vnd.ms-powerpoint.presentation.macroEnabled.main+xml";
    case FileFormat::Ppsx: return "application/vnd.openxmlformats-officedocument.presentationml.slideshow.main+xml";
    case FileFormat::Ppsm: return "application/vnd.ms-powerpoint.slideshow.macroEnabled.main+xml";
    case FileFormat::Potx: return "application/vnd.openxmlformats-officedocument.presentationml.template.main+xml";
    case FileFormat::Potm: return "application/vnd.ms-powerpoint.template.macroEnabled.main+xml";
  }
  return {};
}

pkg::Status PrepareSave(const Editor& editor, SavePlan& plan, FileFormat format) {
  const Presentation& deck = editor.Deck();
  const PartStore& store = *deck.Edits();
  bool macros = format == FileFormat::Pptm || format == FileFormat::Ppsm || format == FileFormat::Potm;
  if (deck.HasMacros() && !macros) {
    return pkg::Status::Fail(pkg::Status::Code::Unsupported,
                             T("This presentation has macros, which this format can't keep. Save it as .pptm."));
  }
  plan = SavePlan{};
  plan.package = &deck.GetPackage();
  plan.parts = store.Parts();
  plan.rels = store.RelationshipSets();
  plan.types = store.GetTypes();
  plan.presentationPart = deck.PresentationPart();
  if (format != deck.Format()) plan.mainType = std::string(MainContentType(format));
  plan.state = editor.StateId();
  return {};
}

pkg::Status WriteSave(SavePlan& plan, pkg::ByteSink& sink) {
  plan.written.clear();
  plan.leftOut.clear();
  pkg::Package::SaveChanges changes;
  // Parts the edits left unused: reachable in the file as opened, not now; or added and not used.
  std::set<std::string> was = Reachable(plan, true), now = Reachable(plan, false);
  std::set<std::string> out;
  for (const std::string& key : was) {
    if (!now.count(key)) out.insert(key);
  }
  for (const auto& [key, state] : plan.parts) {
    if (state.Removed() || !now.count(key)) out.insert(key);
  }
  for (const auto& [key, state] : plan.parts) {
    if (!out.count(key)) continue;
    const pkg::Part* p = plan.package->FindPart(state.name);
    if (p && !p->deleted) changes.leaveOut.push_back(p->name);
  }
  for (const std::string& key : out) {
    const pkg::Part* p = plan.package->FindPart(key);
    if (p && !p->deleted) changes.leaveOut.push_back(p->name);
    // Its relationships go with it.
    std::string rels = pkg::RelsPartFor(p ? p->name : key);
    const pkg::Part* rp = plan.package->FindPart(rels);
    if (rp && !rp->deleted) changes.leaveOut.push_back(rp->name);
  }
  // Parts written from the store.
  for (const auto& [key, state] : plan.parts) {
    if (out.count(key) || state.Removed()) continue;
    if (state.dom) {
      changes.replace.emplace_back(state.name, Bytes(state.dom->Serialize()));
    } else {
      changes.replace.emplace_back(state.name, *state.bytes);
    }
    plan.written.push_back(state.name);
  }
  for (const auto& [key, set] : plan.rels) {
    if (out.count(key) || (key != "/" && !ExistsIn(plan, set->source))) continue;
    std::string relsPart = set->relsPart.empty() ? pkg::RelsPartFor(set->source) : set->relsPart;
    if (set->items.empty()) {
      const pkg::Part* rp = plan.package->FindPart(relsPart);
      if (rp && !rp->deleted) changes.leaveOut.push_back(rp->name);
      continue;
    }
    changes.replace.emplace_back(relsPart, Bytes(set->Serialize()));
    plan.written.push_back(relsPart);
  }
  // Content types: overrides of parts left out go; the main part's for a format change.
  const pkg::ContentTypes& base = plan.types ? *plan.types : plan.package->Types();
  pkg::ContentTypes types = base;
  bool typesChanged = plan.types != nullptr;
  std::erase_if(types.overrides, [&](const auto& o) {
    std::string key = pkg::PartKey(o.first);
    bool gone = out.count(key) > 0 ||
                std::any_of(changes.leaveOut.begin(), changes.leaveOut.end(), [&](const std::string& n) { return pkg::PartKey(n) == key; });
    typesChanged = typesChanged || gone;
    return gone;
  });
  if (!plan.mainType.empty()) {
    std::string key = pkg::PartKey(plan.presentationPart);
    auto it = std::find_if(types.overrides.begin(), types.overrides.end(),
                           [&](const auto& o) { return pkg::PartKey(o.first) == key; });
    if (it != types.overrides.end()) {
      it->second = plan.mainType;
    } else {
      types.overrides.emplace_back(plan.presentationPart, plan.mainType);
    }
    typesChanged = true;
  }
  if (typesChanged) {
    changes.replace.emplace_back(std::string(pkg::kContentTypesPart), Bytes(types.Serialize()));
    plan.written.push_back(std::string(pkg::kContentTypesPart));
  }
  std::sort(changes.leaveOut.begin(), changes.leaveOut.end());
  changes.leaveOut.erase(std::unique(changes.leaveOut.begin(), changes.leaveOut.end()), changes.leaveOut.end());
  plan.leftOut = changes.leaveOut;
  return plan.package->Save(sink, changes);
}

void CommitSave(Editor& editor, const SavePlan& plan) {
  editor.MarkSavedAt(plan.state);
  if (Journal* j = editor.GetJournal()) j->Saved();
}

pkg::Status SaveToBytes(Editor& editor, std::vector<uint8_t>& out, std::vector<std::string>* written) {
  SavePlan plan;
  pkg::Status st = PrepareSave(editor, plan, editor.Deck().Format());
  if (!st.Ok()) return st;
  pkg::MemorySink sink;
  st = WriteSave(plan, sink);
  if (!st.Ok()) return st;
  out = std::move(sink.bytes);
  if (written) {
    *written = plan.written;
    written->insert(written->end(), plan.leftOut.begin(), plan.leftOut.end());
  }
  CommitSave(editor, plan);
  return {};
}

}  // namespace fw::slide::edit
