#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "pkg/Limits.h"
#include "pkg/Status.h"

namespace fw::pkg {

class XmlDom;
class ZipReader;

// What the OpenDocument importers share (D5's .odt, S4's .ods): reading a package's XML parts with
// ODF's usual prefixes, and the values ODF writes in its style properties.
namespace odf {

// Reads an XML part (content.xml, styles.xml, settings.xml) into `dom`, its elements and attributes
// renamed to ODF's usual prefixes ("office:", "table:", "number:", ...) whatever the file declares,
// since pugixml keeps names as written. A missing part fails as Damaged with `missing` when
// `required`, and is an empty DOM otherwise.
Status LoadPart(const ZipReader& zip, const char* name, XmlDom& dom, const Limits& limits, bool required,
                std::string_view missing);
// The package's mimetype entry ("application/vnd.oasis.opendocument.spreadsheet"), or empty.
std::string MimeType(const ZipReader& zip);

// ODF lengths ("2.5cm", "0.5in", "12pt") in points.
std::optional<double> Points(std::string_view v);
// "#1f4e79" as "1F4E79"; empty for "transparent" or anything else.
std::string Color(std::string_view v);
// An encoded ODF name as people read it: "_20_" is a space.
std::string DisplayName(std::string_view name);
// Text for XML content or an attribute value.
std::string Escape(std::string_view s);

// A border ("0.5pt solid #000000", "none"): its width in points, line style ("solid", "double",
// "dashed", "dotted", ...) and color ("1F4E79", empty when not given). `none` for "none" or a
// zero width.
struct Border {
  double width = 0.5;
  std::string style = "solid";
  std::string color;
  bool none = false;
};
std::optional<Border> ParseBorder(std::string_view v);  // nullopt when `v` is empty

}  // namespace odf
}  // namespace fw::pkg
