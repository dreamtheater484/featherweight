#pragma once

#include <utility>
#include <vector>

#include "doc/model/Tree.h"
#include "pkg/RoundTrip.h"

// Kept content (the round-trip store's fragments) beside the known elements an edit links in or
// takes out, so that a part keeps its schema order and loses nothing (plan §4.3 rules 1 and 2,
// d2-torture finding 3). The editor and TreeEdit both use it. Each function returns the nodes
// whose fragments change, with their new lists; the caller stores and journals them.
namespace fw::doc::edit {

using FragmentChanges = std::vector<std::pair<pkg::NodeId, std::vector<pkg::Fragment>>>;

// `child` has just been linked into its parent, after the known sibling `prev` (kNoElem: first).
// - A parent's first element by the schema (w:rPr in w:r, w:pPr in w:p) goes before everything
//   kept at the parent's start, such as a run's mc:AlternateContent.
// - Any other goes before the extension elements (w14:ligatures) kept at the start of its parent,
//   or after the known child before it when it's now the last: schemas put them last.
FragmentChanges FragmentsAfterLink(const pkg::RoundTripStore& store, const Tree& t, ElemId prev, ElemId child);

// `elem` is about to be taken out from between its known siblings `prev` and `next` (kNoElem when
// none). What was kept beside it stays where it was: after `prev`, before `next`, or first in the
// parent.
FragmentChanges FragmentsBeforeUnlink(const pkg::RoundTripStore& store, const Tree& t, ElemId prev, ElemId elem,
                                      ElemId next);

}  // namespace fw::doc::edit
