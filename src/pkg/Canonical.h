#pragma once

#include <string>
#include <string_view>

#include "pkg/Limits.h"
#include "pkg/Status.h"

namespace fw::pkg {

// The canonical form that re-serialized parts are compared in (plan §10, "Re-serialize all").
// Two documents with the same canonical form say the same thing:
//  - names become {namespace}local, so prefixes don't matter, and namespace declarations go;
//  - prefix lists in Markup Compatibility attributes (mc:Ignorable, Requires, ...) are resolved
//    to namespaces too, since they name prefixes;
//  - attributes are sorted; empty elements, CDATA sections and character references become
//    plain text; the XML declaration goes;
//  - whitespace-only text is dropped where it sits between elements (indentation), and kept
//    where it is an element's only content.
// It reads with XmlPull, independently of the DOM, so it can catch the DOM's own mistakes.
Status CanonicalizeXml(std::string_view xml, std::string& out, const Limits& limits = Limits::Default(),
                       std::string_view label = {});

}  // namespace fw::pkg
