#pragma once

#include <string_view>
#include <vector>

#include "pkg/References.h"

namespace fw::pkg {

// Derived data (plan §4.3 rule 2): data a file holds that is computed from other data, and what
// happens to it on save. Engines follow this list when they write a file; fwtool check verifies
// the entries it can see.
enum class OnSave : uint8_t {
  Drop,        // removed when its sources change, with its relationship and content type; the application rebuilds it
  Regenerate,  // written again from its sources
  MarkDirty,   // kept, and marked so the application updates it when it opens the file
  Keep,        // kept as is: it stays valid, or the application recomputes it anyway
};

struct DerivedData {
  DocFormat format;       // Any: every format
  std::string_view what;  // e.g. "calculation chain (xl/calcChain.xml)"
  std::string_view from;  // what it is computed from
  OnSave onSave;
  std::string_view when;  // when that happens, and how
  bool checked;           // fwtool check compares it with its sources
};

const std::vector<DerivedData>& DerivedDataList();
// The entries for one format, including those for every format.
std::vector<const DerivedData*> DerivedDataFor(DocFormat format);

}  // namespace fw::pkg
