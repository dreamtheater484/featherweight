#include "doc/edit/Kept.h"

#include <algorithm>
#include <iterator>

#include "doc/model/Schema.h"

namespace fw::doc::edit {

namespace {

bool Real(pkg::NodeId n) { return n != pkg::kNoNode && n != pkg::kFieldNode; }

// An element from outside the main namespace (w14:ligatures, mc:AlternateContent): schemas put
// these extensions after the properties they know.
bool Extension(const pkg::Fragment& f) { return f.xml.size() > 1 && f.xml[0] == '<' && f.xml.compare(0, 3, "<w:") != 0; }

// Takes the fragments of `from` that `which` picks, out of its list into `moved` (in order), and
// records `from`'s new list.
template <typename Which>
void Take(const pkg::RoundTripStore& store, pkg::NodeId from, Which which, std::vector<pkg::Fragment>& moved,
          FragmentChanges& out) {
  if (!Real(from)) return;
  std::vector<pkg::Fragment> stay;
  bool any = false;
  for (pkg::Fragment& f : store.FragmentsCopy(from)) {
    if (which(f)) {
      moved.push_back(std::move(f));
      any = true;
    } else {
      stay.push_back(std::move(f));
    }
  }
  if (any) out.emplace_back(from, std::move(stay));
}

// Gives `to` the fragments in `slot`: before its own when `first`, after them otherwise.
void Give(const pkg::RoundTripStore& store, pkg::NodeId to, pkg::Slot slot, bool first, std::vector<pkg::Fragment> moved,
          FragmentChanges& out) {
  if (moved.empty()) return;
  for (pkg::Fragment& f : moved) {
    f.slot = slot;
    f.afterChild.clear();
  }
  std::vector<pkg::Fragment> there = store.FragmentsCopy(to);
  there.insert(first ? there.begin() : there.end(), std::make_move_iterator(moved.begin()),
               std::make_move_iterator(moved.end()));
  out.emplace_back(to, std::move(there));
}

}  // namespace

FragmentChanges FragmentsAfterLink(const pkg::RoundTripStore& store, const Tree& t, ElemId prev, ElemId child) {
  FragmentChanges out;
  const Elem& c = t.elems[child];
  if (!Real(c.node) || c.parent == kNoElem) return out;
  const Schema& s = Schema::Get();
  const TypeDef& parentType = s.Type(s.Elem(t.elems[c.parent].def).type);
  const bool leads = !parentType.children.empty() && parentType.children.front() == c.def;
  std::vector<pkg::Fragment> moved;
  if (prev == kNoElem) {
    // Kept at the parent's start (with no known child before it): after a leading element, and
    // extensions after any.
    Take(store, t.elems[c.parent].node,
         [&](const pkg::Fragment& f) {
           return f.slot == pkg::Slot::Inside && f.afterChild.empty() && !f.pairEnd && (leads || Extension(f));
         },
         moved, out);
  } else if (c.next == kNoElem) {
    // Extensions after the known child that was last.
    Take(store, t.elems[prev].node,
         [](const pkg::Fragment& f) { return f.slot == pkg::Slot::After && !f.pairEnd && Extension(f); }, moved, out);
  }
  Give(store, c.node, pkg::Slot::After, true, std::move(moved), out);
  return out;
}

FragmentChanges FragmentsBeforeUnlink(const pkg::RoundTripStore& store, const Tree& t, ElemId prev, ElemId elem,
                                      ElemId next) {
  FragmentChanges out;
  const Elem& e = t.elems[elem];
  if (!Real(e.node)) return out;
  std::vector<pkg::Fragment> moved;
  Take(store, e.node, [](const pkg::Fragment& f) { return f.slot == pkg::Slot::Before || f.slot == pkg::Slot::After; },
       moved, out);
  if (moved.empty()) return out;
  // Before fragments first, then After ones: their order around the element.
  std::stable_partition(moved.begin(), moved.end(), [](const pkg::Fragment& f) { return f.slot == pkg::Slot::Before; });
  if (prev != kNoElem && Real(t.elems[prev].node)) {
    Give(store, t.elems[prev].node, pkg::Slot::After, false, std::move(moved), out);
  } else if (next != kNoElem && Real(t.elems[next].node)) {
    Give(store, t.elems[next].node, pkg::Slot::Before, true, std::move(moved), out);
  } else if (e.parent != kNoElem && Real(t.elems[e.parent].node)) {
    // After what the parent keeps first: the element came after that.
    const pkg::NodeId to = t.elems[e.parent].node;
    for (pkg::Fragment& f : moved) {
      f.slot = pkg::Slot::Inside;
      f.afterChild.clear();
    }
    std::vector<pkg::Fragment> there = store.FragmentsCopy(to);
    auto at = std::find_if(there.begin(), there.end(),
                           [](const pkg::Fragment& f) { return !(f.slot == pkg::Slot::Inside && f.afterChild.empty()); });
    there.insert(at, std::make_move_iterator(moved.begin()), std::make_move_iterator(moved.end()));
    out.emplace_back(to, std::move(there));
  } else {
    out.clear();  // nowhere to keep them: leave them with the element
  }
  return out;
}

}  // namespace fw::doc::edit
