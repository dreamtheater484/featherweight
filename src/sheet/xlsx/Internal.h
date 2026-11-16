#pragma once

#include <pugixml.hpp>

#include <cstdint>
#include <string>
#include <string_view>

#include "pkg/Limits.h"
#include "pkg/Status.h"
#include "pkg/XmlPull.h"
#include "sheet/model/Styles.h"
#include "sheet/model/Workbook.h"

// Shared by the xlsx reader and writer.
namespace fw::sheet::xlsx {

inline constexpr std::string_view kMainNs = "http://schemas.openxmlformats.org/spreadsheetml/2006/main";
inline constexpr std::string_view kStrictMainNs = "http://purl.oclc.org/ooxml/spreadsheetml/main";
inline bool IsMainNs(std::string_view ns) { return ns == kMainNs || ns == kStrictMainNs; }

// Where the XML declaration ends in a part's text (0 when there is none).
size_t DeclarationEnd(std::string_view src);
// Where the tag that ends just before `end` begins: its "<". Attribute values never hold "<"
// (XmlPull refuses them), so the last one before `end` is it.
size_t TagBegin(std::string_view src, size_t end);
// Skips the element whose StartElement was just read, through its EndElement. False on an error.
bool SkipElement(pkg::XmlPull& pull);

// Attributes of pugi elements, by their unprefixed name.
std::string_view Attr(pugi::xml_node n, const char* name);
bool BoolAttr(pugi::xml_node n, const char* name, bool def);
uint32_t UIntAttr(pugi::xml_node n, const char* name, uint32_t def);
int IntAttr(pugi::xml_node n, const char* name, int def);
double NumAttr(pugi::xml_node n, const char* name, double def);
// r:id: an attribute named id with a prefix (the relationships namespace, which slices of a part
// don't declare).
std::string_view RelIdAttr(pugi::xml_node n);
// The local name of an element, without its prefix.
std::string_view Local(pugi::xml_node n);
pugi::xml_node Child(pugi::xml_node n, std::string_view local);

// "1", "true" -> true; "0", "false" -> false.
bool ParseBool(std::string_view s, bool def);
bool ParseUInt(std::string_view s, uint32_t& out);
bool ParseDouble(std::string_view s, double& out);

Color ReadColor(pugi::xml_node n);
// A font (styles' font, a rich run's rPr): its children.
Font ReadFont(pugi::xml_node n);
Fill ReadFill(pugi::xml_node n, bool differential);
Border ReadBorder(pugi::xml_node n);
Alignment ReadAlignment(pugi::xml_node n);

// Numbers in <v> (Numbers.cpp): `v` written in `style` (0-6) into `out` (48 bytes), and the style
// that writes `v` as `text`, if one does.
size_t FormatNumber(double v, uint8_t style, char* out);
bool NumberStyle(std::string_view text, double v, uint8_t& style);

// Parses an XML fragment kept from a part (well-formed: XmlPull read it) as a DOM.
bool ParseSlice(pugi::xml_document& doc, std::string_view xml);
// The text of an element: its text and CDATA children, joined.
std::string TextOf(pugi::xml_node n);

pkg::Status ReadStyles(std::string_view xml, const pkg::Limits& limits, std::string_view label, Styles& styles);
pkg::Status ReadStrings(std::string_view xml, const pkg::Limits& limits, std::string_view label, StringTable& table);
// Reads a sheet's part into it (rows, cells and what it shows).
pkg::Status LoadSheetPart(Workbook& book, size_t index);

}  // namespace fw::sheet::xlsx
