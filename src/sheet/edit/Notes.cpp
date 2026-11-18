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

// One entry: a note (the author's name in bold, then the text, in runs), or a thread's note for
// older versions (`uid` set: the author "tc={id}", plain text).
struct Entry {
  std::string author, text, uid;
};

// The prefix the root gives `ns`, declaring it as `want` when it hasn't one.
std::string Declare(pugi::xml_node root, const char* ns, const char* want) {
  for (pugi::xml_attribute a : root.attributes()) {
    std::string_view n = a.name();
    if (n.rfind("xmlns:", 0) == 0 && std::string_view(a.value()) == ns) return std::string(n.substr(6)) + ":";
  }
  root.append_attribute((std::string("xmlns:") + want).c_str()).set_value(ns);
  return std::string(want) + ":";
}

// xr:uid needs the revision namespace, ignorable for readers that don't know it (as Excel writes).
std::string RevisionPrefix(pugi::xml_node root) {
  const std::string mc = Declare(root, "http://schemas.openxmlformats.org/markup-compatibility/2006", "mc");
  const std::string xr = Declare(root, "http://schemas.microsoft.com/office/spreadsheetml/2014/revision", "xr");
  const std::string token = xr.substr(0, xr.size() - 1);
  pugi::xml_attribute ign = root.attribute((mc + "Ignorable").c_str());
  if (!ign) {
    root.append_attribute((mc + "Ignorable").c_str()).set_value(token.c_str());
  } else {
    std::string v = ign.value();
    if ((" " + v + " ").find(" " + token + " ") == std::string::npos) ign.set_value((v.empty() ? token : v + " " + token).c_str());
  }
  return xr;
}

// Authors no comment names go, and the comments' authorIds follow.
void DropUnusedAuthors(pugi::xml_node authors, pugi::xml_node list) {
  std::vector<pugi::xml_node> nodes;
  for (pugi::xml_node a = authors.first_child(); a; a = a.next_sibling())
    if (a.type() == pugi::node_element && xlsx::Local(a) == "author") nodes.push_back(a);
  std::vector<int> map(nodes.size(), -1);
  for (pugi::xml_node c = list.first_child(); c; c = c.next_sibling()) {
    const int id = xlsx::IntAttr(c, "authorId", -1);
    if (id >= 0 && static_cast<size_t>(id) < map.size()) map[static_cast<size_t>(id)] = 0;
  }
  int next = 0;
  bool dropped = false;
  for (size_t i = 0; i < nodes.size(); ++i) {
    if (map[i] < 0) {
      authors.remove_child(nodes[i]);
      dropped = true;
    } else {
      map[i] = next++;
    }
  }
  if (!dropped) return;
  for (pugi::xml_node c = list.first_child(); c; c = c.next_sibling()) {
    const int id = xlsx::IntAttr(c, "authorId", -1);
    if (id >= 0 && static_cast<size_t>(id) < map.size()) c.attribute("authorId").set_value(map[static_cast<size_t>(id)]);
  }
}

bool EditComments(std::string_view in, CellAddr cell, const Entry* note, std::string& out) {
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
    if (!note->uid.empty()) c.append_attribute((RevisionPrefix(root) + "uid").c_str()).set_value(note->uid.c_str());
    pugi::xml_node text = c.append_child((p + "text").c_str());
    // As Excel's New Note: the author's name in bold, then the text on the next line.
    if (!note->uid.empty()) {
      pugi::xml_node t = text.append_child((p + "t").c_str());
      if (!note->text.empty() && note->text.back() == ' ') t.append_attribute("xml:space").set_value("preserve");
      t.text().set(note->text.c_str());
    } else if (!note->author.empty()) {
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
  DropUnusedAuthors(authors, list);
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
  std::optional<Entry> e;
  if (note) e = Entry{note->author, note->text, {}};
  return EditComments(comments, cell, e ? &*e : nullptr, commentsOut) && EditVml(vml, cell, note, vmlOut);
}

bool HasEntry(std::string_view comments, CellAddr cell) {
  pugi::xml_document doc;
  if (comments.empty() || !Load(doc, comments)) return false;
  for (pugi::xml_node c = xlsx::Child(doc.document_element(), "commentList").first_child(); c; c = c.next_sibling()) {
    CellAddr a;
    if (c.type() == pugi::node_element && xlsx::Local(c) == "comment" && ParseRef(xlsx::Attr(c, "ref"), a) &&
        a.row == cell.row && a.col == cell.col)
      return true;
  }
  return false;
}

// ---- Threaded comments

namespace {

constexpr const char* kThreads = "http://schemas.microsoft.com/office/spreadsheetml/2018/threadedcomments";

bool OnCell(pugi::xml_node c, CellAddr cell) {
  CellAddr a;
  return ParseRef(xlsx::Attr(c, "ref"), a) && a.row == cell.row && a.col == cell.col;
}

pugi::xml_node NewRoot(pugi::xml_document& doc, const char* name) {
  pugi::xml_node root = doc.append_child(name);
  root.append_attribute("xmlns").set_value(kThreads);
  root.append_attribute("xmlns:x").set_value(kMain);
  return root;
}

// A person's id when the persons part hasn't one for the name: the same for the same name, so
// replaying an edit gives the same file (Excel's are random; any GUID does).
std::string PersonId(const std::string& name) {
  uint64_t h[2] = {1469598103934665603ull, 1099511628211ull * 31};
  for (int k = 0; k < 2; ++k)
    for (unsigned char ch : name) h[k] = (h[k] ^ ch) * 1099511628211ull;
  uint8_t b[16];
  for (int i = 0; i < 16; ++i) b[i] = static_cast<uint8_t>(h[i / 8] >> (8 * (i % 8)));
  b[6] = static_cast<uint8_t>((b[6] & 0x0F) | 0x50);
  b[8] = static_cast<uint8_t>((b[8] & 0x3F) | 0x80);
  std::string s = "{";
  for (int i = 0; i < 16; ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) s += '-';
    s += "0123456789ABCDEF"[b[i] >> 4];
    s += "0123456789ABCDEF"[b[i] & 15];
  }
  return s + "}";
}

// The persons part with each author in it: their ids in `ids`, in the thread's order.
bool EditPersons(std::string_view in, const ThreadText& t, std::vector<std::string>& ids, std::string& out) {
  pugi::xml_document doc;
  if (in.empty()) NewRoot(doc, "personList");
  else if (!Load(doc, in) || xlsx::Local(doc.document_element()) != "personList") return false;
  pugi::xml_node root = doc.document_element();
  const std::string p = PrefixOf(root);
  bool changed = in.empty();
  for (const ThreadEntry& e : t.comments) {
    std::string id;
    for (pugi::xml_node q = root.first_child(); q && id.empty(); q = q.next_sibling())
      if (q.type() == pugi::node_element && xlsx::Local(q) == "person" && e.author == xlsx::Attr(q, "displayName"))
        id = xlsx::Attr(q, "id");
    if (id.empty()) {
      id = PersonId(e.author);
      pugi::xml_node q = root.append_child((p + "person").c_str());
      q.append_attribute("displayName").set_value(e.author.c_str());
      q.append_attribute("id").set_value(id.c_str());
      q.append_attribute("userId").set_value(e.author.c_str());
      q.append_attribute("providerId").set_value("None");
      changed = true;
    }
    ids.push_back(id);
  }
  out = changed ? kDeclaration + Print(doc) : std::string(in);
  return true;
}

bool EditThreads(std::string_view in, CellAddr cell, const std::optional<ThreadText>& t, const std::vector<std::string>& people,
                 std::string& out) {
  pugi::xml_document doc;
  if (in.empty()) {
    if (!t) {
      out.clear();
      return true;
    }
    NewRoot(doc, "ThreadedComments");
  } else if (!Load(doc, in) || xlsx::Local(doc.document_element()) != "ThreadedComments") {
    return false;
  }
  pugi::xml_node root = doc.document_element();
  const std::string p = PrefixOf(root);
  // The cell's comments go (kept aside, so ones that don't change are written back as they were,
  // with what else they hold); the new ones take their place, or their place in row order.
  pugi::xml_document old;
  pugi::xml_node before;
  bool removed = false;
  for (pugi::xml_node c = root.first_child(); c;) {
    pugi::xml_node next = c.next_sibling();
    if (c.type() == pugi::node_element && xlsx::Local(c) == "threadedComment") {
      CellAddr a;
      if (OnCell(c, cell)) {
        old.append_copy(c);
        root.remove_child(c);
        removed = true;
      } else if (!before && ParseRef(xlsx::Attr(c, "ref"), a) &&
                 (removed || a.row > cell.row || (a.row == cell.row && a.col > cell.col))) {
        before = c;
      }
    }
    c = next;
  }
  if (t) {
    const std::string ref = CellName(cell.row, cell.col), first = t->comments.empty() ? std::string() : t->comments[0].id;
    for (size_t i = 0; i < t->comments.size(); ++i) {
      const ThreadEntry& e = t->comments[i];
      pugi::xml_node kept;
      for (pugi::xml_node o = old.first_child(); o && !kept; o = o.next_sibling())
        if (e.id == xlsx::Attr(o, "id") && e.text == xlsx::Child(o, "text").text().get() && people[i] == xlsx::Attr(o, "personId"))
          kept = o;
      pugi::xml_node c;
      if (kept) {
        c = before ? root.insert_copy_before(kept, before) : root.append_copy(kept);
        c.remove_attribute("parentId");
        c.remove_attribute("done");
      } else {
        c = before ? root.insert_child_before((p + "threadedComment").c_str(), before)
                   : root.append_child((p + "threadedComment").c_str());
        c.append_attribute("ref").set_value(ref.c_str());
        c.append_attribute("dT").set_value(e.date.c_str());
        c.append_attribute("personId").set_value(people[i].c_str());
        c.append_attribute("id").set_value(e.id.c_str());
        c.append_child((p + "text").c_str()).text().set(e.text.c_str());
      }
      if (i) c.insert_attribute_after("parentId", c.attribute("id")).set_value(first.c_str());
      else if (t->resolved) c.insert_attribute_after("done", c.attribute("id")).set_value("1");
    }
  }
  bool any = false;
  for (pugi::xml_node c = root.first_child(); c && !any; c = c.next_sibling())
    any = c.type() == pugi::node_element && xlsx::Local(c) == "threadedComment";
  if (!any) {
    out.clear();
    return true;
  }
  out = kDeclaration + Print(doc);
  return true;
}

// The note Excel 365 writes for older versions (measured in files it saved).
std::string LegacyText(const ThreadText& t) {
  std::string s =
      "[Threaded comment]\n\nYour version of Excel allows you to read this threaded comment; however, any edits to it will "
      "get removed if the file is opened in a newer version of Excel. Learn more: "
      "https://go.microsoft.com/fwlink/?linkid=870924\n\n";
  for (size_t i = 0; i < t.comments.size(); ++i) s += (i ? "\nReply:\n    " : "Comment:\n    ") + t.comments[i].text;
  return s;
}

}  // namespace

std::optional<ThreadText> ReadThread(std::string_view threads, std::string_view persons, CellAddr cell) {
  pugi::xml_document doc, people;
  if (threads.empty() || !Load(doc, threads)) return std::nullopt;
  auto name = [&](std::string_view id) -> std::string {
    if (!people.document_element() && (persons.empty() || !Load(people, persons))) return {};
    for (pugi::xml_node q = people.document_element().first_child(); q; q = q.next_sibling())
      if (xlsx::Local(q) == "person" && xlsx::Attr(q, "id") == id) return std::string(xlsx::Attr(q, "displayName"));
    return {};
  };
  std::optional<ThreadText> t;
  std::string first;
  for (pugi::xml_node c = doc.document_element().first_child(); c; c = c.next_sibling()) {
    if (c.type() != pugi::node_element || xlsx::Local(c) != "threadedComment") continue;
    const std::string_view parent = xlsx::Attr(c, "parentId");
    if (t ? parent != first : (!parent.empty() || !OnCell(c, cell))) continue;
    if (!t) {
      t.emplace();
      first = xlsx::Attr(c, "id");
      t->resolved = xlsx::BoolAttr(c, "done", false);
    }
    t->comments.push_back({name(xlsx::Attr(c, "personId")), xlsx::Child(c, "text").text().get(),
                           std::string(xlsx::Attr(c, "id")), std::string(xlsx::Attr(c, "dT"))});
  }
  return t;
}

bool EditThread(const ThreadParts& in, CellAddr cell, const std::optional<ThreadText>& thread, ThreadParts& out) {
  if (thread && (thread->comments.empty() ||
                 std::any_of(thread->comments.begin(), thread->comments.end(), [](const ThreadEntry& e) { return e.id.empty(); })))
    return false;
  std::vector<std::string> people;
  if (!thread) out.persons = in.persons;
  else if (!EditPersons(in.persons, *thread, people, out.persons)) return false;
  std::optional<Entry> e;
  if (thread) e = Entry{"tc=" + thread->comments[0].id, LegacyText(*thread), thread->comments[0].id};
  std::optional<NoteText> box;
  if (thread) box = NoteText{};
  return EditThreads(in.threads, cell, thread, people, out.threads) &&
         EditComments(in.comments, cell, e ? &*e : nullptr, out.comments) && EditVml(in.vml, cell, box, out.vml);
}

}  // namespace fw::sheet::edit::notes
