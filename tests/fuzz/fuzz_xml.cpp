// Fuzz target: the streaming reader, the DOM and the canonical form.
// Seeds: tests/fuzz/seeds/xml, plus XML parts from tests/office.
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

#include "pkg/Canonical.h"
#include "pkg/Mce.h"
#include "pkg/References.h"
#include "pkg/RoundTrip.h"
#include "pkg/XmlDom.h"
#include "pkg/XmlPull.h"

using namespace fw::pkg;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  Limits limits;
  limits.maxStringLength = 1u << 20;
  std::string_view xml(reinterpret_cast<const char*>(data), size);

  Status pulled = XmlPull::Check(xml, limits);
  XmlDom dom;
  Status parsed = dom.Parse(xml, limits);
  if (pulled.Ok() != parsed.Ok()) std::abort();  // the DOM accepts exactly what XmlPull accepts
  if (!parsed.Ok()) return 0;

  // Writing the DOM back must not change what the document says.
  std::string before, after;
  if (!CanonicalizeXml(xml, before, limits).Ok()) std::abort();
  std::string written = dom.Serialize();
  if (!CanonicalizeXml(written, after, limits).Ok() || before != after) std::abort();

  // The helpers that walk whole parts.
  mce::CheckPart(dom.Root(), mce::Understood{});
  ReferenceRegistry::Default().Find(dom.Root());
  RoundTripStore store;
  for (auto child : dom.Root().children()) {
    std::string fragment = RoundTripStore::SerializeFragment(child);
    if (child.type() == pugi::node_element) {
      XmlDom alone;
      if (!alone.Parse(fragment, limits).Ok()) std::abort();  // fragments stand alone
    }
  }
  return 0;
}
