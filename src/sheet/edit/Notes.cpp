#include "sheet/edit/Notes.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <pugixml.hpp>

#include "sheet/model/Workbook.h"
#include "sheet/xlsx/Internal.h"

namespace fw::sheet::edit::notes {

namespace {

constexpr const char* kMain = "http://schemas.openxmlformats.org/spreadsheetml/2006/main";
constexpr const char* kDeclaration = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n";

struct StringWriter : pugi::xml_writer {
  std::string out;
  void write(const void* data, size_t size) override { out.append(static_cast<const char*>(data), size); }
};

std::string Print(const pugi::xml_document& doc) {
  StringWriter w;
  doc.save(w, "", pugi::format_raw | pugi::format_no_declaration);
  return w.out;
}

bool Load(pugi::xml_document& doc, std::string_view xml) {
  return doc.load_buffer(xml.data(), xml.size(), pugi::parse_default | pugi::parse_ws_pcdata_single) &&
         doc.document_element();
}

std::string PrefixOf(pugi::xml_node n) {
  std::string_view name = n.name();
  const size_t colon = name.find(':');
  return colon == std::string_view::npos ? std::string() : std::string(name.substr(0, colon + 1));
}

// Excel's run properties for note text: Tahoma 9, the note's text color.
void RunProps(pugi::xml_node r, const std::string& p, bool bold) {
  pugi::xml_node pr = r.append_child((p + "rPr").c_str());
  if (bold) pr.append_child((p + "b").c_str());
  pr.append_child((p + "sz").c_str()).append_attribute("val").set_value("9");
  pr.append_child((p + "color").c_str()).append_attribute("indexed").set_value("81");
  pr.append_child((p + "rFont").c_str()).append_attribute("val").set_value("Tahoma");
  pr.append_child((p + "family").c_str()).append_attribute("val").set_value("2");
}

void AddRun(pugi::xml_node text, const std::string& p, bool bold, const std::string& s) {
  pugi::xml_node r = text.append_child((p + "r").c_str());
  RunProps(r, p, bold);
  pugi::xml_node t = r.append_child((p + "t").c_str());
  if (!s.empty() && (s.front() == ' ' || s.front() == '\n' || s.back() == ' ' || s.find('\n') != std::string::npos))
    t.append_attribute("xml:space").set_value("preserve");
  t.text().set(s.c_str());
}

bool ParseRef(std::string_view ref, CellAddr& a) {
  Range r;
  if (!ParseRange(ref, r)) return false;
  a = {r.firstRow, r.firstCol};
  return true;
}

// ---- The comments part

bool EditComments(std::string_view in, CellAddr cell, const std::optional<NoteText>& note, std::string& out) {
  pugi::xml_document doc;
  if (in.empty()) {
    if (!note) {
      out.clear();
      return true;
    }
    doc.append_child("comments").append_attribute("xmlns").set_value(kMain);
  } else if (!Load(doc, in) || xlsx::Local(doc.document_element()) != "comments") {
    return false;
  }
  pugi::xml_node root = doc.document_element();
  const std::string p = PrefixOf(root);
  pugi::xml_node authors = xlsx::Child(root, "authors");
  if (!authors) authors = root.prepend_child((p + "authors").c_str());
  pugi::xml_node list = xlsx::Child(root, "commentList");
  if (!list) list = root.insert_child_after((p + "commentList").c_str(), authors);
  const std::string ref = CellName(cell.row, cell.col);
  // The cell's note goes; the new one takes its place in row order.
  pugi::xml_node before;
  for (pugi::xml_node c = list.first_child(); c;) {
    pugi::xml_node next = c.next_sibling();
    CellAddr a;
    if (c.type() == pugi::node_element && xlsx::Local(c) == "comment" && ParseRef(xlsx::Attr(c, "ref"), a)) {
      if (a.row == cell.row && a.col == cell.col) list.remove_child(c);
      else if (!before && (a.row > cell.row || (a.row == cell.row && a.col > cell.col))) before = c;
    }
    c = next;
  }
  if (note) {
    int authorId = -1, n = 0;
    for (pugi::xml_node a = authors.first_child(); a; a = a.next_sibling()) {
      if (a.type() != pugi::node_element || xlsx::Local(a) != "author") continue;
      if (authorId < 0 && note->author == a.text().get()) authorId = n;
      ++n;
    }
    if (authorId < 0) {
      authors.append_child((p + "author").c_str()).text().set(note->author.c_str());
      authorId = n;
    }
    pugi::xml_node c = before ? list.insert_child_before((p + "comment").c_str(), before) : list.append_child((p + "comment").c_str());
    c.append_attribute("ref").set_value(ref.c_str());
    c.append_attribute("authorId").set_value(authorId);
    c.append_attribute("shapeId").set_value("0");
    pugi::xml_node text = c.append_child((p + "text").c_str());
    // As Excel's New Note: the author's name in bold, then the text on the next line.
    if (!note->author.empty()) {
      AddRun(text, p, true, note->author + ":");
      AddRun(text, p, false, "\n" + note->text);
    } else {
      AddRun(text, p, false, note->text);
    }
  }
  bool any = false;
  for (pugi::xml_node c = list.first_child(); c && !any; c = c.next_sibling())
    any = c.type() == pugi::node_element && xlsx::Local(c) == "comment";
  if (!any) {
    out.clear();
    return true;
  }
  out = kDeclaration + Print(doc);
  return true;
}

// ---- The VML drawing

bool IsNoteShape(pugi::xml_node s, CellAddr* at) {
  if (xlsx::Local(s) != "shape") return false;
  pugi::xml_node data = xlsx::Child(s, "ClientData");
  if (!data || std::string_view(data.attribute("ObjectType").value()) != "Note") return false;
  if (at) {
    at->row = static_cast<uint32_t>(std::strtoul(xlsx::Child(data, "Row").text().get(), nullptr, 10));
    at->col = static_cast<uint32_t>(std::strtoul(xlsx::Child(data, "Column").text().get(), nullptr, 10));
  }
  return true;
}

std::string Num(double v) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%g", v);
  return buf;
}

bool EditVml(std::string_view in, CellAddr cell, const std::optional<NoteText>& note, std::string& out) {
  pugi::xml_document doc;
  if (in.empty()) {
    if (!note) {
      out.clear();
      return true;
    }
    pugi::xml_node root = doc.append_child("xml");
    root.append_attribute("xmlns:v").set_value("urn:schemas-microsoft-com:vml");
    root.append_attribute("xmlns:o").set_value("urn:schemas-microsoft-com:office:office");
    root.append_attribute("xmlns:x").set_value("urn:schemas-microsoft-com:office:excel");
    pugi::xml_node layout = root.append_child("o:shapelayout");
    layout.append_attribute("v:ext").set_value("edit");
    pugi::xml_node idmap = layout.append_child("o:idmap");
    idmap.append_attribute("v:ext").set_value("edit");
    idmap.append_attribute("data").set_value("1");
  } else if (!Load(doc, in)) {
    return false;
  }
  pugi::xml_node root = doc.document_element();
  uint32_t maxId = 1024;
  bool hasType = false;
  for (pugi::xml_node s = root.first_child(); s;) {
    pugi::xml_node next = s.next_sibling();
    CellAddr at;
    if (s.type() == pugi::node_element) {
      if (xlsx::Local(s) == "shapetype" && std::string_view(s.attribute("id").value()) == "_x0000_t202") hasType = true;
      if (IsNoteShape(s, &at) && at.row == cell.row && at.col == cell.col) {
        root.remove_child(s);
      } else if (xlsx::Local(s) == "shape") {
        std::string_view id = s.attribute("id").value();
        if (id.rfind("_x0000_s", 0) == 0) maxId = std::max(maxId, static_cast<uint32_t>(std::strtoul(id.data() + 8, nullptr, 10)));
      }
    }
    s = next;
  }
  if (note) {
    if (!hasType) {
      pugi::xml_document t;
      const char* type =
          "<v:shapetype id=\"_x0000_t202\" coordsize=\"21600,21600\" o:spt=\"202\" path=\"m,l,21600r21600,l21600,xe\">"
          "<v:stroke joinstyle=\"miter\"/><v:path gradientshapeok=\"t\" o:connecttype=\"rect\"/></v:shapetype>";
      if (t.load_string(type)) {
        pugi::xml_node after;
        for (pugi::xml_node s = root.first_child(); s; s = s.next_sibling())
          if (s.type() == pugi::node_element && xlsx::Local(s) == "shapelayout") after = s;
        after ? root.insert_copy_after(t.document_element(), after) : root.prepend_copy(t.document_element());
      }
    }
    // Excel's box: two columns right of the cell, from the row above, 104 by 61.5 points (measured).
    const uint32_t top = cell.row ? cell.row - 1 : 0, topOffset = cell.row ? 12 : 2;
    const std::string anchor = std::to_string(cell.col + 1) + ", 10, " + std::to_string(top) + ", " + std::to_string(topOffset) +
                               ", " + std::to_string(cell.col + 3) + ", 20, " + std::to_string(top + 4) + ", 17";
    const std::string style = "position:absolute;margin-left:" + Num((cell.col + 1) * 48.0 + 7.5) +
                              "pt;margin-top:" + Num(top * 15.0 + topOffset * 0.75) +
                              "pt;width:104pt;height:61.5pt;z-index:" + std::to_string(maxId - 1023) +
                              ";visibility:" + (note->shown ? "visible" : "hidden");
    const std::string id = "_x0000_s" + std::to_string(maxId + 1);
    std::string shape =
        "<v:shape id=\"" + id + "\" type=\"#_x0000_t202\" style=\"" + style +
        "\" fillcolor=\"infoBackground [80]\" strokecolor=\"none [81]\" o:insetmode=\"auto\">"
        "<v:fill color2=\"infoBackground [80]\"/><v:shadow color=\"none [81]\" obscured=\"t\"/>"
        "<v:path o:connecttype=\"none\"/><v:textbox style=\"mso-direction-alt:auto\"><div style=\"text-align:left\"/></v:textbox>"
        "<x:ClientData ObjectType=\"Note\"><x:MoveWithCells/><x:SizeWithCells/><x:Anchor>" + anchor +
        "</x:Anchor><x:AutoFill>False</x:AutoFill><x:Row>" + std::to_string(cell.row) + "</x:Row><x:Column>" +
        std::to_string(cell.col) + "</x:Column>" + (note->shown ? "<x:Visible/>" : "") + "</x:ClientData></v:shape>";
    pugi::xml_document s;
    if (!s.load_string(shape.c_str())) return false;
    root.append_copy(s.document_element());
  }
  bool any = false;
  for (pugi::xml_node s = root.first_child(); s && !any; s = s.next_sibling())
    any = s.type() == pugi::node_element && xlsx::Local(s) == "shape";
  if (!any) {
    out.clear();
    return true;
  }
  out = Print(doc);
  return true;
}

}  // namespace

bool Edit(std::string_view comments, std::string_view vml, CellAddr cell, const std::optional<NoteText>& note,
          std::string& commentsOut, std::string& vmlOut) {
  return EditComments(comments, cell, note, commentsOut) && EditVml(vml, cell, note, vmlOut);
}

}  // namespace fw::sheet::edit::notes
