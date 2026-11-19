#pragma once

#include <pugixml.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

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
// An element without its children, with its attributes as they are: "<name a="v"/>" (the workbook
// keeps workbookProtection so, and saving compares it with the file's).
std::string EmptyElementXml(pugi::xml_node n);
// The top-level elements of kept XML (a part's text around sheetData): where each is and its local
// name; comments, processing instructions and whitespace between them skipped.
struct XmlPiece {
  size_t begin = 0, end = 0;
  std::string local;
};
std::vector<XmlPiece> TopLevel(std::string_view xml);
// Puts `xml` (a whole element) where the sheet's element `local` is (empty removes it), or where the
// schema puts it; and the prefix the sheet's elements are written with ("x:" or "").
void PutSheetElement(detail::PartText& t, std::string_view local, const std::string& xml);
std::string SheetPrefix(const detail::PartText& t);
// The sheet's element `local` as its kept XML has it (empty: none).
std::string SheetElement(const detail::PartText& t, std::string_view local);
// The prefix the sheet's root gives the relationships namespace ("r"), declared on the root when
// it has none (openpyxl and others write sheets without it).
std::string RelPrefix(detail::PartText& t);
// The sheet's conditional formats (`validations` false: every conditionalFormatting element and the
// x14 extension {78C0D931-...}) or validations (the dataValidations element and the extension
// {CCE6A557-...}), as written: `xml` the top-level elements, `ext` the extension element's content.
// Put replaces them (empty removes them; an extLst left empty goes too).
void GetSheetRules(const detail::PartText& t, bool validations, std::string& xml, std::string& ext);
void PutSheetRules(detail::PartText& t, bool validations, const std::string& xml, const std::string& ext);
// Tables (TableWrite.cpp): a new table's part as Excel writes it; an existing part with the table's
// name, size, rows, filter, columns and style set from `t`, the rest kept (false: not a table);
// the sheet's tableParts with the relationship `relId` added or taken out.
std::string TableXml(const Table& t);
// A table's filter as its part has it (false: none), and the part with its autoFilter element
// replaced by `autoFilter` (Editor's FilterXml); an autoFilter element (a sheet's or a table's) read.
bool ReadTableFilter(std::string_view tableXml, SheetFilter& f);
bool PutTableFilter(std::string& tableXml, const std::string& autoFilter);
bool ReadAutoFilter(pugi::xml_node n, SheetFilter& f);
bool UpdateTableXml(std::string& xml, const Table& t);
void PutTablePart(detail::PartText& t, const std::string& relId, bool add);
// A differential format (dxfs) as styles.xml has it, and read back.
std::string StyleItemXml(const Dxf& d);
bool ReadStyleItem(std::string_view xml, Dxf& out);
// One font, fill, border or cell format as styles.xml has it, and read back from that (the edit
// journal records the formats added this way, as faithful as saving them).
std::string StyleItemXml(const Font& f);
std::string StyleItemXml(const Fill& f);
std::string StyleItemXml(const Border& b);
std::string StyleItemXml(const Xf& x);
bool ReadStyleItem(std::string_view xml, Font& out);
bool ReadStyleItem(std::string_view xml, Fill& out);
bool ReadStyleItem(std::string_view xml, Border& out);
bool ReadStyleItem(std::string_view xml, Xf& out);

pkg::Status ReadStyles(std::string_view xml, const pkg::Limits& limits, std::string_view label, Styles& styles);
pkg::Status ReadTable(std::string_view xml, const pkg::Limits& limits, const std::string& part, Table& table);
pkg::Status ReadStrings(std::string_view xml, const pkg::Limits& limits, std::string_view label, StringTable& table);
// Reads a sheet's part into it (rows, cells and what it shows).
pkg::Status LoadSheetPart(Workbook& book, size_t index);

// The writer (Writer.cpp): a sheet's part with `text` around its sheetData (Sheet::Text(), or one
// with elements the model rewrote), and the shared strings part.
void WriteSheet(const Sheet& sheet, const detail::PartText& text, std::string& out);
void WriteStringsPart(const StringTable& table, std::string& out);

}  // namespace fw::sheet::xlsx
