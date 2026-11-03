// Fuzz target: the relationships and content types parsers, and the part-name helpers.
// Seeds: tests/fuzz/seeds/opc (relationships and content types parts from the corpus).
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

#include "pkg/Package.h"

using namespace fw::pkg;

namespace {

bool SameItems(const RelationshipSet& a, const RelationshipSet& b) {
  if (a.items.size() != b.items.size()) return false;
  for (size_t i = 0; i < a.items.size(); ++i) {
    const Relationship& x = a.items[i];
    const Relationship& y = b.items[i];
    if (x.id != y.id || x.type != y.type || x.target != y.target || x.external != y.external ||
        x.targetPart != y.targetPart)
      return false;
  }
  return true;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  Limits limits;
  limits.maxStringLength = 1u << 20;
  std::string_view xml(reinterpret_cast<const char*>(data), size);

  // The same bytes as the relationships of a part, and of the package.
  for (std::string_view source : {std::string_view("/word/document.xml"), std::string_view("/")}) {
    RelationshipSet set;
    if (!ParseRelationships(xml, source, limits, "fuzz.rels", set).Ok()) continue;
    for (const auto& r : set.items) {
      // A resolved target that can name a part (not empty, no trailing slash) has a relationships
      // part of its own, which leads back to it.
      if (r.external || r.targetPart.empty() || r.targetPart.ends_with('/')) continue;
      std::string rels = RelsPartFor(r.targetPart);
      if (SourceOfRelsPart(rels) != r.targetPart) std::abort();
      PartKey(r.targetPart);
      ResolveTarget(r.targetPart, r.target);
    }
    // What Featherweight writes reads back the same.
    RelationshipSet again;
    if (!ParseRelationships(set.Serialize(), source, limits, "fuzz.rels", again).Ok() || !SameItems(set, again))
      std::abort();
  }

  ContentTypes types;
  if (ParseContentTypes(xml, limits, types).Ok()) {
    for (const auto& [name, type] : types.overrides) types.Lookup(name);
    for (const auto& [ext, type] : types.defaults) types.Lookup("/part." + ext);
    ContentTypes again;
    if (!ParseContentTypes(types.Serialize(), limits, again).Ok() || again.defaults != types.defaults ||
        again.overrides != types.overrides)
      std::abort();
  }
  return 0;
}
