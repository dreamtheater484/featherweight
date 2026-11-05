#include "doc/edit/TableStyles.h"

#include <algorithm>
#include <cctype>

#include "doc/edit/TreeEdit.h"

namespace fw::doc::edit {

namespace {

// Word's definitions of the basics, cut to what shows: borders, the header row, banded rows.
struct Basic {
  std::string_view name, id;
};

std::string Borders(std::string_view color, int eighths = 4) {
  std::string xml = "<w:tblBorders>";
  for (const char* edge : {"top", "left", "bottom", "right", "insideH", "insideV"})
    xml += std::string("<w:") + edge + " w:val=\"single\" w:sz=\"" + std::to_string(eighths) + "\" w:space=\"0\" w:color=\"" +
           std::string(color) + "\"/>";
  return xml + "</w:tblBorders>";
}

const std::string& BasicXml(std::string_view id) {
  static const std::string tight = "<w:pPr><w:spacing w:after=\"0\" w:line=\"240\" w:lineRule=\"auto\"/></w:pPr>";
  static const std::string bands = "<w:tblStyleRowBandSize w:val=\"1\"/><w:tblStyleColBandSize w:val=\"1\"/>";
  static const std::string grid = "<w:uiPriority w:val=\"39\"/>" + tight + "<w:tblPr>" + Borders("auto") + "</w:tblPr>";
  static const std::string plain =
      "<w:uiPriority w:val=\"41\"/>" + tight + "<w:tblPr>" + bands + Borders("BFBFBF") +
      "</w:tblPr><w:tblStylePr w:type=\"firstRow\"><w:rPr><w:b/><w:bCs/></w:rPr></w:tblStylePr>"
      "<w:tblStylePr w:type=\"band1Horz\"><w:tcPr><w:shd w:val=\"clear\" w:color=\"auto\" w:fill=\"F2F2F2\"/></w:tcPr></w:tblStylePr>";
  static const std::string light =
      "<w:uiPriority w:val=\"46\"/>" + tight + "<w:tblPr>" + bands + Borders("999999") +
      "</w:tblPr><w:tblStylePr w:type=\"firstRow\"><w:rPr><w:b/><w:bCs/></w:rPr><w:tcPr><w:tcBorders>"
      "<w:bottom w:val=\"single\" w:sz=\"12\" w:space=\"0\" w:color=\"666666\"/></w:tcBorders></w:tcPr></w:tblStylePr>";
  static const std::string accent =
      "<w:uiPriority w:val=\"49\"/>" + tight + "<w:tblPr>" + bands + Borders("8EAADB") +
      "</w:tblPr><w:tblStylePr w:type=\"firstRow\"><w:rPr><w:b/><w:bCs/><w:color w:val=\"FFFFFF\"/></w:rPr><w:tcPr>"
      "<w:shd w:val=\"clear\" w:color=\"auto\" w:fill=\"4472C4\"/></w:tcPr></w:tblStylePr>"
      "<w:tblStylePr w:type=\"band1Horz\"><w:tcPr><w:shd w:val=\"clear\" w:color=\"auto\" w:fill=\"D9E2F3\"/></w:tcPr></w:tblStylePr>";
  static const std::string none;
  return id == "TableGrid" ? grid : id == "PlainTable1" ? plain : id == "GridTable1Light" ? light : id == "GridTable4-Accent1" ? accent : none;
}

constexpr Basic kBasics[] = {
    {"Table Grid", "TableGrid"},
    {"Plain Table 1", "PlainTable1"},
    {"Grid Table 1 Light", "GridTable1Light"},
    {"Grid Table 4 - Accent 1", "GridTable4-Accent1"},
};

bool SameName(std::string_view a, std::string_view b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
           return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
         });
}

const Style* FindTableStyle(const Document& doc, std::string_view name) {
  for (const Style& s : doc.Styles())
    if (s.type == StyleType::Table && (SameName(s.name, name) || s.id == name)) return &s;
  return nullptr;
}

// The default table style ("Normal Table", id "TableNormal" in English) isn't one to choose.
bool Hidden(const Style& s) { return s.isDefault || SameName(s.name, "Normal Table") || SameName(s.name, "Table Normal"); }

}  // namespace

std::vector<std::string> TableStyleNames(const Document& doc) {
  std::vector<std::string> out;
  for (const Style& s : doc.Styles())
    if (s.type == StyleType::Table && !Hidden(s) && !s.name.empty()) out.emplace_back(s.name);
  for (const Basic& b : kBasics)
    if (!FindTableStyle(doc, b.name)) out.emplace_back(b.name);
  return out;
}

Result ApplyTableStyle(Editor& editor, Pos at, std::string_view name) {
  const Document& doc = editor.Doc();
  if (!editor.CellAt(at)) {
    Result r;
    r.ok = false;
    r.message = u"Put the cursor in a table first.";
    return r;
  }
  std::string id;
  if (const Style* s = FindTableStyle(doc, name)) id = std::string(s->id);
  const Basic* basic = nullptr;
  for (const Basic& b : kBasics)
    if (SameName(b.name, name)) basic = &b;
  if (id.empty() && !basic) {
    Result r;
    r.ok = false;
    r.message = u"The document has no table style of that name.";
    return r;
  }
  editor.BeginGroup(u"Table style");
  bool added = false;
  if (id.empty()) {
    // Word's definition, under a free id, based on the default table style when there is one.
    TreeEdit te(editor, at);
    ElemId root = te.EnsurePart(PartKind::Styles);
    if (root != kNoElem) {
      id = std::string(basic->id);
      for (int n = 1; doc.FindStyle(id); ++n) id = std::string(basic->id) + std::to_string(n);
      const Style* normal = doc.DefaultStyle(StyleType::Table);
      std::string xml = "<w:style w:type=\"table\" w:styleId=\"" + id + "\"><w:name w:val=\"" + std::string(basic->name) + "\"/>";
      if (normal) xml += "<w:basedOn w:val=\"" + std::string(normal->id) + "\"/>";
      xml += BasicXml(basic->id) + "</w:style>";
      te.Begin(u"Table style");
      if (te.AddXml(root, xml) == kNoElem) id.clear();
      else added = true;
      te.Reindex();
      te.Commit();
    } else {
      id.clear();
    }
  }
  Result r;
  if (id.empty()) {
    r.ok = false;
    r.message = u"This table style can't be added to the document.";
  } else {
    r = editor.SetTableProps(at, PropEdit().Set("tblStyle", {{"val", id}}));
  }
  if (r) editor.EndGroup();
  else editor.CancelGroup();  // a style added for it goes again
  return r;
}

}  // namespace fw::doc::edit
