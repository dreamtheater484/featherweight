#include "sheet/edit/Refs.h"

#include <algorithm>
#include <charconv>
#include <cstring>

#include "core/Lang.h"
#include "sheet/xlsx/Internal.h"

namespace fw::sheet::edit::refs {

using xlsx::Local;

namespace {

struct Out : pugi::xml_writer {
  std::string s;
  void write(const void* d, size_t n) override { s.append(static_cast<const char*>(d), n); }
};

std::string Print(pugi::xml_node n) {
  Out w;
  n.print(w, "", pugi::format_raw);
  return w.s;
}

constexpr unsigned kParse = pugi::parse_default | pugi::parse_ws_pcdata_single | pugi::parse_comments |
                            pugi::parse_declaration;

bool LoadDoc(pugi::xml_document& d, const std::string& xml) { return d.load_buffer(xml.data(), xml.size(), kParse); }

std::string SaveDoc(const pugi::xml_document& d) {
  Out w;
  d.save(w, "", pugi::format_raw | pugi::format_no_declaration);
  return w.s;
}

uint32_t ToUInt(std::string_view s, uint32_t def = kNone) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\n' || s.front() == '\r' || s.front() == '\t')) s.remove_prefix(1);
  uint32_t v = 0;
  auto r = std::from_chars(s.data(), s.data() + s.size(), v);
  return r.ec == std::errc() ? v : def;
}

void SetAttr(pugi::xml_node n, const char* name, const std::string& v) {
  pugi::xml_attribute a = n.attribute(name);
  if (!a) a = n.append_attribute(name);
  a.set_value(v.c_str());
}

void SetText(pugi::xml_node n, const std::string& v) { n.text().set(v.c_str()); }

std::vector<Range> ParseSqref(std::string_view s) {
  std::vector<Range> out;
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && s[i] == ' ') ++i;
    size_t j = s.find(' ', i);
    if (j == std::string_view::npos) j = s.size();
    Range r;
    if (j > i && ParseRange(s.substr(i, j - i), r)) out.push_back(r);
    i = j;
  }
  return out;
}

std::string JoinSqref(const std::vector<Range>& areas) {
  std::string out;
  for (const Range& r : areas) {
    if (!out.empty()) out += ' ';
    out += RangeName(r);
  }
  return out;
}

// Rewrites the top-level elements of kept XML that `fn` cares about: fn(local name, element)
// returns 0 when it left the element alone, 1 when it changed it, -1 to remove it. Elements it adds
// after the one it's given (a rule split in two) are written there.
template <class Fn>
bool EachElement(std::string& seg, std::initializer_list<std::string_view> names, Fn&& fn) {
  bool changed = false;
  auto pieces = xlsx::TopLevel(seg);
  for (auto it = pieces.rbegin(); it != pieces.rend(); ++it) {
    if (std::find(names.begin(), names.end(), std::string_view(it->local)) == names.end()) continue;
    pugi::xml_document d;
    if (!xlsx::ParseSlice(d, std::string_view(seg).substr(it->begin, it->end - it->begin))) continue;
    int r = fn(std::string_view(it->local), d.document_element());
    if (r == 0) continue;
    std::string now;
    if (r > 0)
      for (pugi::xml_node x = d.first_child(); x; x = x.next_sibling())
        if (x.type() == pugi::node_element) now += Print(x);
    seg.replace(it->begin, it->end - it->begin, now);
    changed = true;
  }
  return changed;
}

// Every descendant element named `local` (any prefix), in document order, not looking inside them.
void Collect(pugi::xml_node n, std::string_view local, std::vector<pugi::xml_node>& out) {
  for (pugi::xml_node c = n.first_child(); c; c = c.next_sibling())
    if (c.type() == pugi::node_element) {
      if (Local(c) == local) out.push_back(c);
      else Collect(c, local, out);
    }
}
template <class Fn>
void Each(pugi::xml_node n, std::string_view local, Fn&& fn) {
  std::vector<pugi::xml_node> found;
  Collect(n, local, found);
  for (pugi::xml_node x : found) fn(x);
}

pugi::xml_node FirstElement(pugi::xml_node n) {
  for (pugi::xml_node c = n.first_child(); c; c = c.next_sibling())
    if (c.type() == pugi::node_element) return c;
  return {};
}

// A formula child's text through the context's formula function; true when it changed.
bool RewriteText(pugi::xml_node n, int sheet, CellAddr at, const Context& c) {
  std::string text = xlsx::TextOf(n);
  if (text.empty()) return false;
  std::string now = c.formula(text, sheet, at.row, at.col);
  if (now == text) return false;
  SetText(n, now);
  return true;
}

bool RewriteAttr(pugi::xml_node n, const char* name, int sheet, CellAddr at, const Context& c) {
  pugi::xml_attribute a = n.attribute(name);
  if (!a || !*a.value()) return false;
  std::string text = a.value();
  bool eq = text.front() == '=';
  std::string now = c.formula(eq ? std::string_view(text).substr(1) : std::string_view(text), sheet, at.row, at.col);
  if (eq) now.insert(0, 1, '=');
  if (now == text) return false;
  a.set_value(now.c_str());
  return true;
}

// ---- Rules (conditional formats, validations): an area list and formulas relative to its first
// area's top-left cell.

// Where the rule's formulas are read from after the edit: the old top-left cell, or (when it went)
// the cell before the edit that is the new top-left one.
struct Anchor {
  CellAddr old, before, after;
  bool moved = false;  // the formulas must be read from `before` (a copy) first
};

CellAddr Before(const Mover& m, CellAddr a) {
  uint32_t& line = m.Rows() ? a.row : a.col;
  if (line < m.sh.at) return a;
  if (m.sh.count > 0) line = line >= m.sh.at + static_cast<uint32_t>(m.sh.count) ? line - m.sh.count : m.sh.at;
  else line += m.Deleted();
  return a;
}

Anchor AnchorOf(const Mover& m, const std::vector<Range>& before, const std::vector<Range>& after) {
  Anchor a;
  if (!before.empty()) a.old = {before[0].firstRow, before[0].firstCol};
  a.before = a.after = a.old;
  if (after.empty()) return a;
  a.after = {after[0].firstRow, after[0].firstCol};
  CellAddr moved = a.old;
  if (m.Cell(moved.row, moved.col) && moved == a.after) return a;
  a.before = Before(m, a.after);
  a.moved = !(a.before == a.old);
  return a;
}

std::string RuleFormula(std::string_view text, const Anchor& a, int sheet, const Context& c) {
  if (!a.moved) return c.formula(text, sheet, a.old.row, a.old.col);
  return c.formula(c.copy(text, sheet, a.old, a.before), sheet, a.before.row, a.before.col);
}

CellAddr TopLeft(const Range& r) { return {r.firstRow, r.firstCol}; }

// Areas holding the line just before an insertion take in the inserted cells beside it (Excel copies
// that line's formats into them): an area ending there grows, or gets another area for those cells.
void ExtendInto(const Mover& m, std::vector<Range>& areas) {
  if (m.sh.count <= 0 || m.sh.at == 0) return;
  const bool rows = m.Rows();
  const uint32_t bf = rows ? m.sh.band.firstCol : m.sh.band.firstRow, bl = rows ? m.sh.band.lastCol : m.sh.band.lastRow;
  const uint32_t lastNew = std::min<uint32_t>(m.sh.at - 1 + static_cast<uint32_t>(m.sh.count), m.max - 1);
  const size_t n = areas.size();
  for (size_t i = 0; i < n; ++i) {
    Range r = areas[i];
    uint32_t last = rows ? r.lastRow : r.lastCol, cf = rows ? r.firstCol : r.firstRow, cl = rows ? r.lastCol : r.lastRow;
    int across = m.Across(cf, cl);
    if (last + 1 != m.sh.at || across == 0) continue;
    if (across == 1) {
      (rows ? areas[i].lastRow : areas[i].lastCol) = lastNew;
      continue;
    }
    Range add = r;
    (rows ? add.firstRow : add.firstCol) = m.sh.at;
    (rows ? add.lastRow : add.lastCol) = lastNew;
    (rows ? add.firstCol : add.firstRow) = std::max(cf, bf);
    (rows ? add.lastCol : add.lastRow) = std::min(cl, bl);
    areas.push_back(add);
  }
}

// The parts of an area list the edit moves (as they are before it), and the parts it leaves.
void SplitMoved(const Mover& m, const std::vector<Range>& in, std::vector<Range>& still, std::vector<Range>& moved) {
  const bool rows = m.Rows();
  const uint32_t bf = rows ? m.sh.band.firstCol : m.sh.band.firstRow, bl = rows ? m.sh.band.lastCol : m.sh.band.lastRow;
  for (const Range& r : in) {
    uint32_t first = rows ? r.firstRow : r.firstCol, last = rows ? r.lastRow : r.lastCol;
    uint32_t cf = rows ? r.firstCol : r.firstRow, cl = rows ? r.lastCol : r.lastRow;
    if (last < m.sh.at || m.Across(cf, cl) == 0) {
      still.push_back(r);
      continue;
    }
    auto piece = [&](uint32_t c1, uint32_t c2, uint32_t l1, uint32_t l2) {
      Range p;
      (rows ? p.firstRow : p.firstCol) = l1;
      (rows ? p.lastRow : p.lastCol) = l2;
      (rows ? p.firstCol : p.firstRow) = c1;
      (rows ? p.lastCol : p.lastRow) = c2;
      return p;
    };
    if (cf < bf) still.push_back(piece(cf, bf - 1, first, last));
    if (cl > bl) still.push_back(piece(bl + 1, cl, first, last));
    uint32_t c1 = std::max(cf, bf), c2 = std::min(cl, bl);
    if (first < m.sh.at) still.push_back(piece(c1, c2, first, m.sh.at - 1));
    moved.push_back(piece(c1, c2, std::max(first, m.sh.at), last));
  }
}

// The rule's area list: its sqref attribute, or its xm:sqref child.
struct Sqref {
  pugi::xml_attribute attr;
  pugi::xml_node child;
  std::string Get() const { return attr ? std::string(attr.value()) : child ? xlsx::TextOf(child) : std::string(); }
  void Set(const std::string& v) {
    if (attr) attr.set_value(v.c_str());
    else if (child) SetText(child, v);
  }
};
Sqref SqrefOf(pugi::xml_node rule) {
  Sqref s;
  s.attr = rule.attribute("sqref");
  if (!s.attr) Each(rule, "sqref", [&](pugi::xml_node x) { if (!s.child) s.child = x; });
  return s;
}

// The rule's formula elements with text, in document order (an x14 formula element holds xm:f).
std::vector<pugi::xml_node> FormulasOf(pugi::xml_node rule, std::initializer_list<std::string_view> names) {
  std::vector<pugi::xml_node> out;
  for (std::string_view name : names)
    Each(rule, name, [&](pugi::xml_node f) {
      pugi::xml_node holder = f;
      if (pugi::xml_node first = FirstElement(f); first && Local(first) == "f") holder = first;
      if (!xlsx::TextOf(holder).empty()) out.push_back(holder);
    });
  return out;
}

// One rule element (conditionalFormatting, dataValidation, their x14 forms): its area list and its
// formulas, which are written for the first area's top-left cell. When the edit moves some of its
// cells and not others and its formulas then read differently in the two (a relative reference to
// cells that stay), it's split in two as Excel does: this one keeps the cells that stay, a copy after
// it (with `priority`, the sheet's next free priority, for conditional formats) the moved ones.
// -1: nothing is left of it; 2: it was split.
int MoveRule(pugi::xml_node rule, bool edited, int sheet, const Context& c,
             std::initializer_list<std::string_view> formulaNames, uint32_t* priority) {
  const Mover& m = c.move;
  Sqref sq = SqrefOf(rule);
  const std::string sqref = sq.Get();
  std::vector<Range> before = ParseSqref(sqref);
  std::vector<pugi::xml_node> formulas = FormulasOf(rule, formulaNames);
  if (edited && priority && !formulas.empty() && !before.empty()) {
    std::vector<Range> still, moved, now;
    SplitMoved(m, before, still, moved);
    for (Range r : moved)
      if (m.Area(r)) now.push_back(r);
    if (!still.empty() && !now.empty()) {
      CellAddr a = TopLeft(before[0]), u = TopLeft(still[0]), from = TopLeft(moved[0]), to = TopLeft(now[0]);
      std::vector<std::string> stay, go;
      bool even = true;
      for (pugi::xml_node f : formulas) {
        std::string text = xlsx::TextOf(f);
        std::string fs = c.formula(u == a ? text : c.copy(text, sheet, a, u), sheet, u.row, u.col);
        std::string fm = c.formula(from == a ? text : c.copy(text, sheet, a, from), sheet, from.row, from.col);
        even = even && c.copy(fs, sheet, u, to) == fm;
        stay.push_back(std::move(fs));
        go.push_back(std::move(fm));
      }
      if (!even) {
        pugi::xml_node copy = rule.parent().insert_copy_after(rule, rule);
        ExtendInto(m, still);
        sq.Set(JoinSqref(still));
        for (size_t i = 0; i < formulas.size(); ++i) SetText(formulas[i], stay[i]);
        Sqref theirSqref = SqrefOf(copy);
        theirSqref.Set(JoinSqref(now));
        std::vector<pugi::xml_node> theirs = FormulasOf(copy, formulaNames);
        for (size_t i = 0; i < theirs.size() && i < go.size(); ++i) SetText(theirs[i], go[i]);
        Each(copy, "cfRule", [&](pugi::xml_node r) { SetAttr(r, "priority", std::to_string(++*priority)); });
        copy.remove_attribute("xr:uid");
        return 2;
      }
    }
  }
  bool changed = false;
  std::vector<Range> after = before;
  if (edited) {
    after = m.Areas(before);
    ExtendInto(m, after);
    if (after.empty()) return -1;
    std::string now = JoinSqref(after);
    if (now != sqref) {
      sq.Set(now);
      changed = true;
    }
  }
  Anchor anchor = edited ? AnchorOf(m, before, after) : Anchor{};
  if (!edited && !before.empty()) anchor.old = anchor.before = TopLeft(before[0]);
  for (pugi::xml_node f : formulas) {
    std::string text = xlsx::TextOf(f);
    std::string now = RuleFormula(text, anchor, sheet, c);
    if (now != text) {
      SetText(f, now);
      changed = true;
    }
  }
  return changed ? 1 : 0;
}

// A list element (dataValidations, x14:dataValidations, ignoredErrors...) whose items each hold an
// area list: items left with nothing go, and so does the list when it's empty.
int MoveList(pugi::xml_node list, std::string_view item, bool edited, int sheet, const Context& c,
             std::initializer_list<std::string_view> formulaNames, uint32_t* priority = nullptr) {
  bool changed = false;
  std::vector<pugi::xml_node> items;
  for (pugi::xml_node x = list.first_child(); x; x = x.next_sibling())
    if (x.type() == pugi::node_element && Local(x) == item) items.push_back(x);
  size_t left = items.size();
  for (pugi::xml_node x : items) {
    int r = MoveRule(x, edited, sheet, c, formulaNames, priority);
    if (r < 0) {
      list.remove_child(x);
      --left;
    }
    if (r == 2) ++left;
    changed = changed || r != 0;
  }
  if (!changed) return 0;
  if (left == 0) return -1;
  if (list.attribute("count")) SetAttr(list, "count", std::to_string(left));
  return 1;
}

// ---- Anchors (drawings, controls, VML): a corner at a line and an offset along the axis.

struct Corner {
  uint32_t line = 0;
  int64_t off = 0;
};

// Where a corner goes. The top-left corner moves with the line it's in; the bottom-right one only
// when it reaches into a line at or after the edit.
Corner MoveCorner(const Mover& m, Corner p, bool from) {
  if (m.sh.count > 0) {
    bool moves = from ? p.line >= m.sh.at : (p.line > m.sh.at || (p.line == m.sh.at && p.off > 0));
    if (moves) p.line = std::min<uint32_t>(p.line + static_cast<uint32_t>(m.sh.count), m.max - 1);
    return p;
  }
  uint32_t d = m.Deleted();
  if (p.line < m.sh.at) return p;
  if (p.line >= m.sh.at + d) {
    p.line -= d;
    return p;
  }
  return Corner{m.sh.at, 0};
}

enum class Follow : uint8_t { MoveAndSize, Move, None };

// An object's two corners after the edit, by how it follows its cells; false when nothing changed.
// `crossFrom` and `crossTo` are its corners on the other axis: with cells shifted in a band, an object
// that moves and sizes with its cells moves when its middle is in the band (Excel, measured), one
// that only moves when its top-left cell is.
bool MoveBox(const Mover& m, Follow follow, Corner& from, Corner* to, uint32_t crossFrom, uint32_t crossTo) {
  if (m.Sorting()) {
    // An object inside one line of the sorted cells goes with it, however it follows cells; one
    // over several lines stays (Excel, measured).
    bool one = !to || to->line == from.line || (to->line == from.line + 1 && to->off == 0);
    if (!one || m.Across(crossFrom, std::max(crossFrom, crossTo)) != 1) return false;
    uint32_t n = m.Line(from.line);
    if (n == from.line) return false;
    if (to) to->line = n + (to->line - from.line);
    from.line = n;
    return true;
  }
  if (follow == Follow::None) return false;
  uint32_t probe = follow == Follow::MoveAndSize ? (crossFrom + crossTo) / 2 : crossFrom;
  if (m.Across(probe, probe) != 1) return false;
  Corner f = MoveCorner(m, from, true);
  bool changed = f.line != from.line || f.off != from.off;
  if (to) {
    Corner t = *to;
    if (follow == Follow::MoveAndSize) {
      t = MoveCorner(m, *to, false);
    } else {
      int64_t delta = static_cast<int64_t>(f.line) - static_cast<int64_t>(from.line);
      t.line = static_cast<uint32_t>(std::clamp<int64_t>(static_cast<int64_t>(to->line) + delta, 0, m.max - 1));
    }
    changed = changed || t.line != to->line || t.off != to->off;
    *to = t;
  }
  from = f;
  return changed;
}

// An xdr from/to element's line and offset along the mover's axis.
struct XdrCorner {
  pugi::xml_node line, off, cross;
  bool ok = false;
};
XdrCorner ReadXdr(pugi::xml_node n, const Mover& m) {
  XdrCorner c;
  if (!n) return c;
  for (pugi::xml_node x = n.first_child(); x; x = x.next_sibling()) {
    std::string_view l = Local(x);
    if (l == (m.Rows() ? "row" : "col")) c.line = x;
    else if (l == (m.Rows() ? "rowOff" : "colOff")) c.off = x;
    else if (l == (m.Rows() ? "col" : "row")) c.cross = x;
  }
  c.ok = c.line && c.cross;
  return c;
}
Corner CornerOf(const XdrCorner& x) {
  Corner c;
  c.line = ToUInt(x.line.text().get(), 0);
  if (x.off) c.off = x.off.text().as_llong();
  return c;
}
void PutCorner(const XdrCorner& x, Corner c) {
  x.line.text().set(std::to_string(c.line).c_str());
  if (x.off) x.off.text().set(std::to_string(c.off).c_str());
}

bool MoveXdrAnchor(const Mover& m, pugi::xml_node fromNode, pugi::xml_node toNode, Follow follow) {
  XdrCorner f = ReadXdr(fromNode, m), t = ReadXdr(toNode, m);
  if (!f.ok) return false;
  uint32_t cf = ToUInt(f.cross.text().get(), 0), cl = t.ok ? ToUInt(t.cross.text().get(), 0) : cf;
  Corner from = CornerOf(f), to = t.ok ? CornerOf(t) : Corner{};
  if (!MoveBox(m, follow, from, t.ok ? &to : nullptr, cf, cl)) return false;
  PutCorner(f, from);
  if (t.ok) PutCorner(t, to);
  return true;
}

pugi::xml_node ChildNamed(pugi::xml_node n, std::string_view local) {
  for (pugi::xml_node c = n.first_child(); c; c = c.next_sibling())
    if (c.type() == pugi::node_element && Local(c) == local) return c;
  return {};
}

// Controls' and OLE objects' anchors in the sheet part (x14: <anchor moveWithCells sizeWithCells>).
bool MoveObjectAnchors(pugi::xml_node root, bool edited, int sheet, const Context& c) {
  bool changed = false;
  if (edited)
    Each(root, "anchor", [&](pugi::xml_node a) {
      Follow follow = xlsx::BoolAttr(a, "sizeWithCells", false) ? Follow::MoveAndSize
                      : xlsx::BoolAttr(a, "moveWithCells", false) ? Follow::Move
                                                                  : Follow::None;
      changed = MoveXdrAnchor(c.move, ChildNamed(a, "from"), ChildNamed(a, "to"), follow) || changed;
    });
  Each(root, "controlPr", [&](pugi::xml_node p) {
    changed = RewriteAttr(p, "linkedCell", sheet, {}, c) || changed;
    changed = RewriteAttr(p, "listFillRange", sheet, {}, c) || changed;
  });
  return changed;
}

bool SameName(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    char x = a[i], y = b[i];
    if (x >= 'a' && x <= 'z') x = static_cast<char>(x - 32);
    if (y >= 'a' && y <= 'z') y = static_cast<char>(y - 32);
    if (x != y) return false;
  }
  return true;
}

}  // namespace

// ---------- Mover ----------

Mover::Mover(const Shift& s) : sh(s), max(s.axis == Shift::Axis::Rows ? kMaxRows : kMaxCols) {}

uint32_t Mover::Line(uint32_t line) const {
  if (line < sh.at) return line;
  if (Sorting()) return line - sh.at < dest.size() ? sh.at + dest[line - sh.at] : line;
  if (sh.count >= 0) {
    uint64_t n = static_cast<uint64_t>(line) + static_cast<uint64_t>(sh.count);
    return n < max ? static_cast<uint32_t>(n) : kNone;
  }
  uint32_t d = Deleted();
  return line < sh.at + d ? kNone : line - d;
}

bool Mover::Span(uint32_t& first, uint32_t& last) const {
  if (Sorting() || last < sh.at || (first == 0 && last == max - 1)) return true;  // before the edit; whole lines
  bool toEnd = last == max - 1;
  if (sh.count > 0) {
    if (first >= sh.at) {
      if (static_cast<uint64_t>(first) + static_cast<uint64_t>(sh.count) >= max) return false;
      first += static_cast<uint32_t>(sh.count);
    }
    last = static_cast<uint32_t>(std::min<uint64_t>(static_cast<uint64_t>(last) + static_cast<uint64_t>(sh.count), max - 1));
    return true;
  }
  uint32_t d = Deleted(), end = sh.at + d;
  uint32_t f = first < sh.at ? first : (first >= end ? first - d : sh.at);
  uint32_t l;
  if (last < end) {
    if (first >= sh.at) return false;
    l = sh.at - 1;
  } else {
    l = toEnd ? max - 1 : last - d;
  }
  if (f > l) return false;
  first = f;
  last = l;
  return true;
}

int Mover::Across(uint32_t first, uint32_t last) const {
  uint32_t bf = Rows() ? sh.band.firstCol : sh.band.firstRow, bl = Rows() ? sh.band.lastCol : sh.band.lastRow;
  if (last < bf || first > bl) return 0;
  return first >= bf && last <= bl ? 1 : -1;
}

bool Mover::Cell(uint32_t& row, uint32_t& col) const {
  uint32_t& line = Rows() ? row : col;
  uint32_t cross = Rows() ? col : row;
  if (line < sh.at || Across(cross, cross) == 0) return true;
  uint32_t n = Line(line);
  if (n == kNone) return false;
  line = n;
  return true;
}

bool Mover::Area(Range& r) const {
  uint32_t& first = Rows() ? r.firstRow : r.firstCol;
  uint32_t& last = Rows() ? r.lastRow : r.lastCol;
  if (Across(Rows() ? r.firstCol : r.firstRow, Rows() ? r.lastCol : r.lastRow) != 1) return true;
  return Span(first, last);
}

std::vector<Range> Mover::Areas(const std::vector<Range>& in) const {
  if (Sorting()) return in;
  std::vector<Range> out, moved;
  for (Range r : in) {
    uint32_t last = Rows() ? r.lastRow : r.lastCol;
    uint32_t cf = Rows() ? r.firstCol : r.firstRow, cl = Rows() ? r.lastCol : r.lastRow;
    int across = Across(cf, cl);
    if (last < sh.at || across == 0) {
      out.push_back(r);
      continue;
    }
    if (across == 1) {
      if (Area(r)) out.push_back(r);
      continue;
    }
    // Partly in the band: the parts beside it stay, the part inside moves (after the others).
    uint32_t bf = Rows() ? sh.band.firstCol : sh.band.firstRow, bl = Rows() ? sh.band.lastCol : sh.band.lastRow;
    auto piece = [&](uint32_t f, uint32_t l) {
      Range p = r;
      (Rows() ? p.firstCol : p.firstRow) = f;
      (Rows() ? p.lastCol : p.lastRow) = l;
      return p;
    };
    if (cf < bf) out.push_back(piece(cf, bf - 1));
    if (cl > bl) out.push_back(piece(bl + 1, cl));
    Range inside = piece(std::max(cf, bf), std::min(cl, bl));
    if (Area(inside)) moved.push_back(inside);
  }
  out.insert(out.end(), moved.begin(), moved.end());
  return out;
}

std::string Mover::Sqref(std::string_view sqref) const { return JoinSqref(Areas(ParseSqref(sqref))); }

// ---------- The sheet part ----------

std::u16string CheckSheetXml(const detail::PartText& t) {
  static constexpr std::string_view kKnown[] = {
      "sheetPr", "dimension", "sheetViews", "sheetFormatPr", "cols", "sheetCalcPr", "sheetProtection",
      "protectedRanges", "autoFilter", "sortState", "mergeCells", "phoneticPr", "conditionalFormatting",
      "dataValidations", "hyperlinks", "printOptions", "pageMargins", "pageSetup", "headerFooter", "rowBreaks",
      "colBreaks", "customProperties", "cellWatches", "ignoredErrors", "drawing", "legacyDrawing",
      "legacyDrawingHF", "drawingHF", "picture", "oleObjects", "controls", "tableParts", "extLst",
      "AlternateContent"};
  static constexpr std::pair<std::string_view, UiText16> kBlocked[] = {
      {"scenarios", N_(u"scenarios")},
      {"dataConsolidate", N_(u"a data consolidation")},
      {"customSheetViews", N_(u"custom views")},
      {"smartTags", N_(u"smart tags")},
      {"webPublishItems", N_(u"items published to the web")}};
  static constexpr std::string_view kKnownExt[] = {"conditionalFormattings", "dataValidations", "sparklineGroups",
                                                   "slicerList", "protectedRanges", "ignoredErrors",
                                                   "webExtensions", "timelineRefs"};
  for (const std::string* seg : {&t.beforeData, &t.afterData})
    for (const xlsx::XmlPiece& p : xlsx::TopLevel(*seg)) {
      for (const auto& [name, what] : kBlocked)
        if (p.local == name) return std::u16string(what);
      if (std::find(std::begin(kKnown), std::end(kKnown), p.local) == std::end(kKnown))
        return T(u"parts Featherweight doesn't know");
      if (p.local != "extLst") continue;
      pugi::xml_document d;
      if (!xlsx::ParseSlice(d, std::string_view(*seg).substr(p.begin, p.end - p.begin))) return T(u"parts it can't read");
      for (pugi::xml_node ext = d.document_element().first_child(); ext; ext = ext.next_sibling()) {
        if (ext.type() != pugi::node_element) continue;
        pugi::xml_node inner = FirstElement(ext);
        if (inner && std::find(std::begin(kKnownExt), std::end(kKnownExt), Local(inner)) == std::end(kKnownExt))
          return T(u"an extension Featherweight doesn't know");
      }
    }
  return {};
}

void MoveSheetXml(detail::PartText& t, int index, const Context& c) {
  const bool edited = index == c.sheet;
  const Mover& m = c.move;
  // Conditional formats split in two take the sheet's next free priorities.
  uint32_t priority = 0;
  for (size_t at = t.afterData.find("priority=\""); at != std::string::npos; at = t.afterData.find("priority=\"", at + 1))
    priority = std::max(priority, ToUInt(std::string_view(t.afterData).substr(at + 10), 0));
  EachElement(
      t.afterData,
      {"conditionalFormatting", "dataValidations", "autoFilter", "sortState", "rowBreaks", "colBreaks",
       "ignoredErrors", "protectedRanges", "cellWatches", "extLst", "controls", "oleObjects", "AlternateContent"},
      [&](std::string_view local, pugi::xml_node n) -> int {
        if (local == "conditionalFormatting") return std::min(MoveRule(n, edited, index, c, {"formula"}, &priority), 1);
        if (local == "dataValidations")
          return MoveList(n, "dataValidation", edited, index, c, {"formula1", "formula2"}, &priority);
        if (local == "controls" || local == "oleObjects" || local == "AlternateContent")
          return MoveObjectAnchors(n, edited, index, c) ? 1 : 0;
        if (!edited && local != "extLst") return 0;
        if (local == "ignoredErrors") return MoveList(n, "ignoredError", true, index, c, {});
        if (local == "protectedRanges") return MoveList(n, "protectedRange", true, index, c, {});
        if (local == "autoFilter" || local == "sortState") {
          bool changed = false;
          Range before;
          bool filter = local == "autoFilter";
          if (ParseRange(xlsx::Attr(n, "ref"), before)) {
            Range after = before;
            if (!m.Area(after)) return -1;
            if (!(after == before)) {
              SetAttr(n, "ref", RangeName(after));
              changed = true;
            }
            // Filter columns are counted from the range's first column.
            if (filter && !m.Rows() && m.Across(before.firstRow, before.lastRow) == 1) {
              std::vector<pugi::xml_node> cols;
              for (pugi::xml_node fc = n.first_child(); fc; fc = fc.next_sibling())
                if (Local(fc) == "filterColumn") cols.push_back(fc);
              for (pugi::xml_node fc : cols) {
                uint32_t id = ToUInt(xlsx::Attr(fc, "colId"), kNone);
                if (id == kNone) continue;
                uint32_t col = m.Line(before.firstCol + id);
                if (col == kNone || col < after.firstCol || col > after.lastCol) {
                  n.remove_child(fc);
                } else if (col - after.firstCol != id) {
                  SetAttr(fc, "colId", std::to_string(col - after.firstCol));
                }
                changed = true;
              }
            }
          }
          Each(n, "sortCondition", [&](pugi::xml_node sc) {
            Range r;
            if (ParseRange(xlsx::Attr(sc, "ref"), r) && m.Area(r)) {
              std::string now = RangeName(r);
              if (now != xlsx::Attr(sc, "ref")) {
                SetAttr(sc, "ref", now);
                changed = true;
              }
            }
          });
          if (filter)
            if (pugi::xml_node ss = ChildNamed(n, "sortState")) {
              Range r;
              if (ParseRange(xlsx::Attr(ss, "ref"), r) && m.Area(r) && RangeName(r) != xlsx::Attr(ss, "ref")) {
                SetAttr(ss, "ref", RangeName(r));
                changed = true;
              }
            }
          return changed ? 1 : 0;
        }
        if (local == "rowBreaks" || local == "colBreaks") {
          // Page breaks move with whole rows or columns only.
          if (m.Across(0, m.Rows() ? kMaxCols - 1 : kMaxRows - 1) != 1) return 0;
          bool changed = false;
          std::vector<pugi::xml_node> brks;
          for (pugi::xml_node b = n.first_child(); b; b = b.next_sibling())
            if (Local(b) == "brk") brks.push_back(b);
          if ((local == "rowBreaks") != m.Rows()) return 0;  // how far they reach stays (Excel)
          size_t left = brks.size(), manual = 0;
          for (pugi::xml_node b : brks) {
            uint32_t id = ToUInt(xlsx::Attr(b, "id"), 0);
            uint32_t now = m.Line(id);
            if (now == kNone) {
              n.remove_child(b);
              --left;
              changed = true;
              continue;
            }
            manual += xlsx::BoolAttr(b, "man", false);
            if (now != id) {
              SetAttr(b, "id", std::to_string(now));
              changed = true;
            }
          }
          if (!changed) return 0;
          if (left == 0) return -1;
          SetAttr(n, "count", std::to_string(left));
          if (n.attribute("manualBreakCount")) SetAttr(n, "manualBreakCount", std::to_string(manual));
          return 1;
        }
        if (local == "cellWatches") {
          bool changed = false;
          std::vector<pugi::xml_node> watches;
          for (pugi::xml_node w = n.first_child(); w; w = w.next_sibling())
            if (Local(w) == "cellWatch") watches.push_back(w);
          size_t left = watches.size();
          for (pugi::xml_node w : watches) {
            Range r;
            if (!ParseRange(xlsx::Attr(w, "r"), r)) continue;
            uint32_t row = r.firstRow, col = r.firstCol;
            if (!m.Cell(row, col)) {
              n.remove_child(w);
              --left;
              changed = true;
            } else if (row != r.firstRow || col != r.firstCol) {
              SetAttr(w, "r", CellName(row, col));
              changed = true;
            }
          }
          return !changed ? 0 : left ? 1 : -1;
        }
        // extLst: the x14 forms, and sparklines.
        bool changed = false;
        Each(n, "conditionalFormatting", [&](pugi::xml_node cf) {
          int r = MoveRule(cf, edited, index, c, {"f"}, nullptr);
          if (r < 0) cf.parent().remove_child(cf);
          changed = changed || r != 0;
        });
        Each(n, "dataValidations", [&](pugi::xml_node list) {
          int r = MoveList(list, "dataValidation", edited, index, c, {"formula1", "formula2"}, &priority);
          if (r < 0) list.parent().remove_child(list);
          changed = changed || r != 0;
        });
        for (std::string_view item : {"ignoredError", "protectedRange"})
          Each(n, item, [&](pugi::xml_node x) {
            int r = MoveRule(x, edited, index, c, {}, nullptr);
            if (r < 0) x.parent().remove_child(x);
            changed = changed || r != 0;
          });
        Each(n, "webExtensions", [&](pugi::xml_node w) {
          Each(w, "f", [&](pugi::xml_node f) { changed = RewriteText(f, index, {}, c) || changed; });
        });
        Each(n, "sparklineGroup", [&](pugi::xml_node g) {
          // A date axis range (an xm:f straight under the group) and each sparkline's data and cell.
          for (pugi::xml_node x = g.first_child(); x; x = x.next_sibling())
            if (Local(x) == "f") changed = RewriteText(x, index, {}, c) || changed;
          pugi::xml_node list = ChildNamed(g, "sparklines");
          if (!list) return;
          std::vector<pugi::xml_node> lines;
          for (pugi::xml_node s = list.first_child(); s; s = s.next_sibling())
            if (Local(s) == "sparkline") lines.push_back(s);
          for (pugi::xml_node s : lines) {
            pugi::xml_node f = ChildNamed(s, "f"), at = ChildNamed(s, "sqref");
            Range cell;
            bool hasCell = at && ParseRange(xlsx::TextOf(at), cell);
            CellAddr where = hasCell ? CellAddr{cell.firstRow, cell.firstCol} : CellAddr{};
            if (f) changed = RewriteText(f, index, where, c) || changed;
            if (f && xlsx::TextOf(f) == "#REF!") s.remove_child(f);  // its data's sheet went (Excel)
            if (!edited || !hasCell) continue;
            uint32_t row = cell.firstRow, col = cell.firstCol;
            if (!m.Cell(row, col)) {
              list.remove_child(s);
              changed = true;
              continue;
            }
            if (row != cell.firstRow || col != cell.firstCol) {
              SetText(at, CellName(row, col));
              changed = true;
            }
            // A sparkline in the line just before an insertion is copied into the new lines.
            uint32_t line = m.Rows() ? row : col, cross = m.Rows() ? col : row;
            if (m.sh.count > 0 && m.sh.at > 0 && line + 1 == m.sh.at && m.Across(cross, cross) == 1 && f) {
              pugi::xml_node after = s;
              for (int32_t k = 1; k <= m.sh.count && line + k < m.max; ++k) {
                CellAddr to = m.Rows() ? CellAddr{row + k, col} : CellAddr{row, col + k};
                pugi::xml_node copy = list.insert_copy_after(s, after);
                after = copy;
                SetText(ChildNamed(copy, "f"), c.copy(xlsx::TextOf(ChildNamed(copy, "f")), index, {row, col}, to));
                SetText(ChildNamed(copy, "sqref"), CellName(to.row, to.col));
              }
            }
          }
          if (!list.first_child()) g.parent().remove_child(g);
        });
        if (!changed) return 0;
        // Lists left empty go, then extensions left empty, then the extLst itself (Excel repairs
        // an empty x14:conditionalFormattings).
        for (pugi::xml_node ext = n.first_child(); ext;) {
          pugi::xml_node next = ext.next_sibling();
          if (ext.type() == pugi::node_element) {
            pugi::xml_node inner = FirstElement(ext);
            if (inner && !FirstElement(inner) && Local(inner) != "slicerList" && Local(inner) != "timelineRefs")
              ext.remove_child(inner);
            if (!FirstElement(ext)) n.remove_child(ext);
          }
          ext = next;
        }
        return FirstElement(n) ? 1 : -1;
      });
}

// ---------- Other parts ----------

bool MoveDrawing(std::string& xml, int index, const Context& c) {
  pugi::xml_document d;
  if (!LoadDoc(d, xml)) return false;
  bool changed = false;
  const bool edited = index == c.sheet;
  pugi::xml_node root = d.document_element();
  if (edited) {
    for (std::string_view kind : {"twoCellAnchor", "oneCellAnchor"})
      Each(root, kind, [&](pugi::xml_node a) {
        Follow follow = Follow::MoveAndSize;
        if (kind == "oneCellAnchor") {
          follow = Follow::Move;
        } else {
          std::string_view as = xlsx::Attr(a, "editAs");
          if (as == "oneCell") follow = Follow::Move;
          else if (as == "absolute") follow = Follow::None;
        }
        changed = MoveXdrAnchor(c.move, ChildNamed(a, "from"), ChildNamed(a, "to"), follow) || changed;
      });
  }
  // Shapes whose text comes from a cell.
  Each(root, "sp", [&](pugi::xml_node sp) { changed = RewriteAttr(sp, "textlink", index, {}, c) || changed; });
  if (changed) xml = SaveDoc(d);
  return changed;
}

// A numRef's or strRef's cache, written anew with `points` (only when they differ from what it holds,
// numbers compared as numbers, when `ifDifferent`); whether it changed.
bool PutCache(pugi::xml_node ref, const std::vector<CachePoint>& points, bool ifDifferent) {
  const bool numbers = Local(ref) == "numRef";
  pugi::xml_node held = ChildNamed(ref, numbers ? "numCache" : "strCache");
  if (!held) return false;
  if (ifDifferent) {
    std::vector<std::pair<uint32_t, std::string>> was;
    uint32_t count = kNone;
    for (pugi::xml_node x = held.first_child(); x; x = x.next_sibling())
      if (Local(x) == "ptCount") count = ToUInt(xlsx::Attr(x, "val"), kNone);
      else if (Local(x) == "pt") was.emplace_back(ToUInt(xlsx::Attr(x, "idx"), kNone), xlsx::TextOf(ChildNamed(x, "v")));
    std::vector<std::pair<uint32_t, std::string>> now;
    for (size_t i = 0; i < points.size(); ++i)
      if (points[i].kind != CachePoint::Empty && (!numbers || points[i].kind == CachePoint::Number))
        now.emplace_back(static_cast<uint32_t>(i), points[i].text);
    auto same = [&](const std::string& a, const std::string& b) {
      if (a == b) return true;
      double x = 0, y = 0;
      return numbers && xlsx::ParseDouble(a, x) && xlsx::ParseDouble(b, y) && x == y;
    };
    bool equal = count == points.size() && was.size() == now.size();
    for (size_t i = 0; equal && i < was.size(); ++i) equal = was[i].first == now[i].first && same(was[i].second, now[i].second);
    if (equal) return false;
  }
  std::string p(held.name());
  p = p.find(':') == std::string::npos ? std::string() : p.substr(0, p.find(':') + 1);
  for (pugi::xml_node x = held.first_child(); x;) {
    pugi::xml_node next = x.next_sibling();
    if (Local(x) == "ptCount" || Local(x) == "pt") held.remove_child(x);
    x = next;
  }
  pugi::xml_node before = ChildNamed(held, "extLst");
  auto add = [&](const char* name) {
    return before ? held.insert_child_before((p + name).c_str(), before) : held.append_child((p + name).c_str());
  };
  add("ptCount").append_attribute("val").set_value(std::to_string(points.size()).c_str());
  for (size_t i = 0; i < points.size(); ++i) {
    const CachePoint& pt = points[i];
    if (pt.kind == CachePoint::Empty || (numbers && pt.kind != CachePoint::Number)) continue;
    pugi::xml_node x = add("pt");
    x.append_attribute("idx").set_value(std::to_string(i).c_str());
    x.append_child((p + "v").c_str()).text().set(pt.text.c_str());
  }
  return true;
}

bool RefreshChart(std::string& xml, const CacheFn& cache) {
  if (xml.find("Cache") == std::string::npos) return false;
  pugi::xml_document d;
  if (!LoadDoc(d, xml)) return false;
  bool changed = false, visibleOnly = true;
  pugi::xml_node root = d.document_element();
  Each(root, "plotVisOnly", [&](pugi::xml_node p) { visibleOnly = xlsx::BoolAttr(p, "val", true); });
  Each(root, "f", [&](pugi::xml_node f) {
    pugi::xml_node ref = f.parent();
    const std::string_view kind = Local(ref);
    std::vector<CachePoint> points;
    if ((kind == "numRef" || kind == "strRef") && cache(xlsx::TextOf(f), visibleOnly, points))
      changed = PutCache(ref, points, true) || changed;
  });
  if (changed) xml = SaveDoc(d);
  return changed;
}

bool MoveChart(std::string& xml, int index, const Context& c, const CacheFn& cache) {
  pugi::xml_document d;
  if (!LoadDoc(d, xml)) return false;
  bool changed = false;
  pugi::xml_node root = d.document_element();
  bool visibleOnly = false;
  Each(root, "plotVisOnly", [&](pugi::xml_node p) { visibleOnly = xlsx::BoolAttr(p, "val", true); });
  Each(root, "f", [&](pugi::xml_node f) {
    std::string text = xlsx::TextOf(f);
    if (text.empty()) return;
    std::string now = c.formula(text, index, 0, 0);
    if (now == text) return;
    SetText(f, now);
    changed = true;
    // The values cached beside it, read again (Excel keeps them when the reference is gone).
    pugi::xml_node ref = f.parent();
    std::string_view kind = Local(ref);
    if ((kind != "numRef" && kind != "strRef") || now.find("#REF!") != std::string::npos || !cache) return;
    std::vector<CachePoint> points;
    if (cache(now, visibleOnly, points)) PutCache(ref, points, false);
  });
  if (changed) xml = SaveDoc(d);
  return changed;
}

bool MoveComments(std::string& xml, const Context& c) {
  pugi::xml_document d;
  if (!LoadDoc(d, xml)) return false;
  bool changed = false;
  for (std::string_view kind : {"comment", "threadedComment"})
    Each(d.document_element(), kind, [&](pugi::xml_node n) {
      Range r;
      if (!ParseRange(xlsx::Attr(n, "ref"), r)) return;
      uint32_t row = r.firstRow, col = r.firstCol;
      if (!c.move.Cell(row, col)) {
        n.parent().remove_child(n);
        changed = true;
      } else if (row != r.firstRow || col != r.firstCol) {
        SetAttr(n, "ref", CellName(row, col));
        changed = true;
      }
    });
  if (changed) xml = SaveDoc(d);
  return changed;
}

namespace {

// The text between <tag> and </tag> inside `s` (VML, kept as text: Excel's isn't always XML).
bool TagText(const std::string& s, std::string_view tag, size_t& begin, size_t& end) {
  std::string open = "<" + std::string(tag) + ">", close = "</" + std::string(tag) + ">";
  size_t a = s.find(open);
  if (a == std::string::npos) return false;
  size_t b = s.find(close, a);
  if (b == std::string::npos) return false;
  begin = a + open.size();
  end = b;
  return true;
}

// -1: the shape goes (a note on a deleted cell); 1 changed; 0 not.
// The prefix a VML part gives Excel's elements in a shape ("x:", or another a writer chose: "ns2:").
std::string ExcelPrefix(const std::string& shape) {
  const size_t at = shape.find("ClientData");
  if (at == std::string::npos) return "x:";
  size_t lt = shape.rfind('<', at);
  if (lt == std::string::npos || lt + 1 > at) return "x:";
  return shape.substr(lt + 1, at - lt - 1);
}

int MoveVmlShape(std::string& shape, bool edited, int index, const Context& c) {
  const Mover& m = c.move;
  const std::string x = ExcelPrefix(shape);
  bool changed = false;
  size_t b = 0, e = 0;
  bool note = shape.find("ObjectType=\"Note\"") != std::string::npos;
  int64_t noteDelta = 0;  // a note's box moves as far as its cell does
  if (edited && note) {
    size_t rb = 0, re = 0, cb = 0, ce = 0;
    if (TagText(shape, x + "Row", rb, re) && TagText(shape, x + "Column", cb, ce)) {
      uint32_t row0 = ToUInt(std::string_view(shape).substr(rb, re - rb), 0);
      uint32_t col0 = ToUInt(std::string_view(shape).substr(cb, ce - cb), 0);
      uint32_t row = row0, col = col0;
      if (!m.Cell(row, col)) return -1;
      noteDelta = m.Rows() ? static_cast<int64_t>(row) - row0 : static_cast<int64_t>(col) - col0;
      if (row != row0 || col != col0) {
        // The later tag first, so the earlier one's offsets hold.
        std::string rs = std::to_string(row), cs = std::to_string(col);
        if (rb > cb) {
          shape.replace(rb, re - rb, rs);
          shape.replace(cb, ce - cb, cs);
        } else {
          shape.replace(cb, ce - cb, cs);
          shape.replace(rb, re - rb, rs);
        }
        changed = true;
      }
    }
  }
  if (edited && TagText(shape, x + "Anchor", b, e)) {
    // LeftColumn, LeftOffset, TopRow, TopOffset, RightColumn, RightOffset, BottomRow, BottomOffset
    uint32_t v[8];
    std::string_view s = std::string_view(shape).substr(b, e - b);
    size_t n = 0;
    for (size_t i = 0; i < s.size() && n < 8;) {
      while (i < s.size() && (s[i] < '0' || s[i] > '9')) ++i;
      size_t j = i;
      while (j < s.size() && s[j] >= '0' && s[j] <= '9') ++j;
      if (j > i) v[n++] = ToUInt(s.substr(i, j - i), 0);
      i = j;
    }
    if (n == 8) {
      int line = m.Rows() ? 2 : 0, cross = m.Rows() ? 0 : 2;
      Corner from{v[line], v[line + 1]}, to{v[line + 4], v[line + 5]};
      bool moveWithCells = shape.find("<" + x + "MoveWithCells") == std::string::npos;
      bool sizeWithCells = shape.find("<" + x + "SizeWithCells") == std::string::npos;
      Follow follow = moveWithCells && sizeWithCells ? Follow::MoveAndSize
                      : moveWithCells                ? Follow::Move
                                                     : Follow::None;
      bool moved;
      if (note) {
        moved = noteDelta != 0;
        from.line = static_cast<uint32_t>(std::clamp<int64_t>(from.line + noteDelta, 0, m.max - 1));
        to.line = static_cast<uint32_t>(std::clamp<int64_t>(to.line + noteDelta, 0, m.max - 1));
      } else {
        moved = MoveBox(m, follow, from, &to, v[cross], v[cross + 4]);
      }
      if (moved) {
        v[line] = from.line;
        v[line + 1] = static_cast<uint32_t>(from.off);
        v[line + 4] = to.line;
        v[line + 5] = static_cast<uint32_t>(to.off);
        std::string text;
        for (int k = 0; k < 8; ++k) text += (k ? ", " : "") + std::to_string(v[k]);
        // Keep the whitespace around the numbers as written.
        size_t lead = s.find_first_not_of(" \t\r\n"), trail = s.find_last_not_of(" \t\r\n");
        if (lead != std::string_view::npos) shape.replace(b + lead, trail - lead + 1, text);
        changed = true;
      }
    }
  }
  for (const std::string& tag : {x + "FmlaLink", x + "FmlaRange", x + "FmlaTxbx"})
    if (TagText(shape, tag, b, e)) {
      std::string text = shape.substr(b, e - b);
      std::string now = c.formula(text, index, 0, 0);
      if (now != text) {
        shape.replace(b, e - b, now);
        changed = true;
      }
    }
  return changed ? 1 : 0;
}

}  // namespace

bool MoveVml(std::string& xml, int index, const Context& c) {
  const bool edited = index == c.sheet;
  bool changed = false;
  std::string out;
  size_t pos = 0;
  // Shapes under any prefix (v:, or one a writer chose: ns0:), shapetypes left alone.
  auto nameChar = [](char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.';
  };
  for (size_t s = xml.find('<', pos); s != std::string::npos; s = xml.find('<', pos)) {
    size_t n = s + 1, name = n;
    while (n < xml.size() && nameChar(xml[n])) ++n;
    if (n < xml.size() && xml[n] == ':') {
      name = ++n;
      while (n < xml.size() && nameChar(xml[n])) ++n;
    }
    const char after = n < xml.size() ? xml[n] : '>';
    if (xml.compare(name, n - name, "shape") != 0 ||
        (after != ' ' && after != '>' && after != '\r' && after != '\n' && after != '\t')) {
      out.append(xml, pos, s + 1 - pos);
      pos = s + 1;
      continue;
    }
    const std::string close = "</" + xml.substr(s + 1, n - s - 1) + ">";
    size_t e = xml.find(close, s);
    if (e == std::string::npos) break;
    e += close.size();
    std::string shape = xml.substr(s, e - s);
    int r = MoveVmlShape(shape, edited, index, c);
    out.append(xml, pos, s - pos);
    if (r >= 0) out += shape;
    changed = changed || r != 0;
    pos = e;
  }
  if (!changed) return false;
  out.append(xml, pos, std::string::npos);
  xml = std::move(out);
  return true;
}

bool MoveTable(std::string& xml, const Table& before, const Table& after, int index, const Context& c) {
  pugi::xml_document d;
  if (!LoadDoc(d, xml)) return false;
  pugi::xml_node root = d.document_element();
  const Mover& m = c.move;
  bool changed = false;
  if (!(before.ref == after.ref)) {
    SetAttr(root, "ref", RangeName(after.ref));
    changed = true;
  }
  if (before.totalsRows && !after.totalsRows) {  // its totals row deleted (as Excel writes it)
    root.remove_attribute("totalsRowCount");
    if (!after.totalsRowShown) SetAttr(root, "totalsRowShown", "0");
    changed = true;
  }
  for (pugi::xml_node part : {ChildNamed(root, "autoFilter"), ChildNamed(root, "sortState")}) {
    if (!part) continue;
    Range r;
    if (ParseRange(xlsx::Attr(part, "ref"), r) && m.Area(r) && RangeName(r) != xlsx::Attr(part, "ref")) {
      SetAttr(part, "ref", RangeName(r));
      changed = true;
    }
    Each(part, "sortCondition", [&](pugi::xml_node sc) {
      Range s;
      if (ParseRange(xlsx::Attr(sc, "ref"), s) && m.Area(s) && RangeName(s) != xlsx::Attr(sc, "ref")) {
        SetAttr(sc, "ref", RangeName(s));
        changed = true;
      }
    });
  }
  // Columns inserted or deleted inside the table: its tableColumn list follows the model's.
  pugi::xml_node list = ChildNamed(root, "tableColumns");
  bool same = before.columns.size() == after.columns.size();
  for (size_t i = 0; same && i < after.columns.size(); ++i) same = before.columns[i].id == after.columns[i].id;
  if (list && !same) {
    std::string p(list.name());
    p = p.find(':') == std::string::npos ? std::string() : p.substr(0, p.find(':') + 1);
    std::vector<std::pair<uint32_t, pugi::xml_node>> have;
    for (pugi::xml_node x = list.first_child(); x; x = x.next_sibling())
      if (Local(x) == "tableColumn") have.push_back({ToUInt(xlsx::Attr(x, "id"), 0), x});
    pugi::xml_document kept;
    for (const TableColumn& col : after.columns) {
      auto it = std::find_if(have.begin(), have.end(), [&](const auto& h) { return h.first == col.id; });
      if (it != have.end()) {
        kept.append_copy(it->second);
      } else {
        pugi::xml_node x = kept.append_child((p + "tableColumn").c_str());
        x.append_attribute("id").set_value(std::to_string(col.id).c_str());
        x.append_attribute("name").set_value(col.name.c_str());
      }
    }
    for (const auto& h : have) list.remove_child(h.second);
    for (pugi::xml_node x = kept.first_child(); x; x = x.next_sibling()) list.append_copy(x);
    SetAttr(list, "count", std::to_string(after.columns.size()));
    changed = true;
  }
  // Columns renamed (a header typed over), their names escaped as ST_Xstring (_x000A_, _x005F_).
  if (list)
    for (pugi::xml_node x = list.first_child(); x; x = x.next_sibling()) {
      if (Local(x) != "tableColumn") continue;
      uint32_t id = ToUInt(xlsx::Attr(x, "id"), 0);
      for (const TableColumn& was : before.columns)
        for (const TableColumn& col : after.columns)
          if (was.id == id && col.id == id && was.name != col.name) {
            std::string name;
            for (size_t i = 0; i < col.name.size(); ++i) {
              unsigned char ch = static_cast<unsigned char>(col.name[i]);
              bool looks = ch == '_' && i + 6 < col.name.size() && col.name[i + 1] == 'x' && col.name[i + 6] == '_';
              if (ch < 0x20 || looks) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "_x%04X_", ch);
                name += buf;
              } else {
                name += static_cast<char>(ch);
              }
            }
            SetAttr(x, "name", name);
            changed = true;
          }
    }
  // Totals typed in the totals row: the column's function, label and formula as the model has them.
  if (list)
    for (pugi::xml_node x = list.first_child(); x; x = x.next_sibling()) {
      if (Local(x) != "tableColumn") continue;
      const uint32_t id = ToUInt(xlsx::Attr(x, "id"), 0);
      for (const TableColumn& was : before.columns)
        for (const TableColumn& col : after.columns) {
          if (was.id != id || col.id != id ||
              (was.totalsFunction == col.totalsFunction && was.totalsLabel == col.totalsLabel &&
               was.totalsFormula == col.totalsFormula))
            continue;
          for (const auto& [name, value] : {std::pair<const char*, const std::string*>{"totalsRowFunction", &col.totalsFunction},
                                            std::pair<const char*, const std::string*>{"totalsRowLabel", &col.totalsLabel}}) {
            if (value->empty()) x.remove_attribute(name);
            else SetAttr(x, name, *value);
          }
          pugi::xml_node f = ChildNamed(x, "totalsRowFormula");
          if (col.totalsFormula.empty()) {
            if (f) x.remove_child(f);
          } else {
            if (!f) {
              std::string p(x.name());
              p = p.find(':') == std::string::npos ? std::string() : p.substr(0, p.find(':') + 1);
              pugi::xml_node calc = ChildNamed(x, "calculatedColumnFormula");
              f = calc ? x.insert_child_after((p + "totalsRowFormula").c_str(), calc)
                       : x.prepend_child((p + "totalsRowFormula").c_str());
            }
            SetText(f, col.totalsFormula);
          }
          changed = true;
        }
    }
  // Column formulas (relative ones are written for the first data row).
  if (list)
    for (pugi::xml_node x = list.first_child(); x; x = x.next_sibling()) {
      if (Local(x) != "tableColumn") continue;
      uint32_t id = ToUInt(xlsx::Attr(x, "id"), 0);
      int k = -1;
      for (size_t i = 0; i < before.columns.size(); ++i)
        if (before.columns[i].id == id) k = static_cast<int>(i);
      CellAddr at{before.ref.firstRow + before.headerRows, before.ref.firstCol + static_cast<uint32_t>(std::max(k, 0))};
      for (std::string_view f : {"calculatedColumnFormula", "totalsRowFormula"})
        if (pugi::xml_node n = ChildNamed(x, f)) changed = RewriteText(n, index, f == "totalsRowFormula" ? CellAddr{before.ref.lastRow, at.col} : at, c) || changed;
    }
  if (changed) xml = SaveDoc(d);
  return changed;
}

std::string DropTableColumns(std::string_view f, std::string_view table, const std::vector<std::string>& columns) {
  auto up = [](char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 32) : c; };
  auto same = [&](std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
      if (up(a[i]) != up(b[i])) return false;
    return true;
  };
  auto namePart = [](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '\\' ||
           static_cast<unsigned char>(c) >= 0x80;
  };
  std::string out;
  for (size_t i = 0; i < f.size();) {
    char c = f[i];
    if (c == '"' || c == '\'') {  // a string, or a quoted sheet name: as it is
      size_t j = i + 1;
      while (j < f.size() && !(f[j] == c && (j + 1 >= f.size() || f[j + 1] != c))) j += f[j] == c ? 2 : 1;
      out.append(f, i, std::min(j + 1, f.size()) - i);
      i = j + 1;
      continue;
    }
    if ((i == 0 || !namePart(f[i - 1])) && i + table.size() < f.size() && f[i + table.size()] == '[' &&
        same(f.substr(i, table.size()), table)) {
      // The table's brackets, nested, with ' escaping the next character.
      size_t j = i + table.size(), depth = 0;
      for (; j < f.size(); ++j) {
        if (f[j] == '\'') {
          ++j;
          continue;
        }
        if (f[j] == '[') ++depth;
        else if (f[j] == ']' && --depth == 0) break;
      }
      if (j >= f.size()) break;
      std::string_view inner = f.substr(i + table.size() + 1, j - i - table.size() - 1);
      bool gone = columns.empty();  // the whole table went (its sheet was deleted)
      auto item = [&](std::string_view s) {
        std::string name;
        for (size_t k = 0; k < s.size(); ++k) name += s[k] == '\'' && k + 1 < s.size() ? s[++k] : s[k];
        if (!name.empty() && name[0] != '#')
          for (const std::string& col : columns) gone = gone || same(name, col);
      };
      if (inner.empty() || inner[0] != '[') {
        item(inner);
      } else {
        for (size_t k = 0; k < inner.size();) {
          if (inner[k] != '[') {
            ++k;
            continue;
          }
          size_t e = k + 1;
          while (e < inner.size() && inner[e] != ']') e += inner[e] == '\'' ? 2 : 1;
          item(inner.substr(k + 1, std::min(e, inner.size()) - k - 1));
          k = e + 1;
        }
      }
      if (gone) out += "#REF!";
      else out.append(f, i, j + 1 - i);
      i = j + 1;
      continue;
    }
    out += c;
    ++i;
  }
  return out;
}

std::string RenameTableColumn(std::string_view f, std::string_view table, std::string_view from, std::string_view to) {
  auto up = [](char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 32) : c; };
  auto same = [&](std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
      if (up(a[i]) != up(b[i])) return false;
    return true;
  };
  auto namePart = [](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '\\' ||
           static_cast<unsigned char>(c) >= 0x80;
  };
  std::string escaped;  // [ ] # and ' take a ' before them
  for (char c : to) {
    if (c == '[' || c == ']' || c == '#' || c == '\'') escaped += '\'';
    escaped += c;
  }
  // An item ("[name]" or a bare name) as it is, or the new name when it names the column.
  auto item = [&](std::string_view s) -> std::string {
    std::string name;
    for (size_t k = 0; k < s.size(); ++k) name += s[k] == '\'' && k + 1 < s.size() ? s[++k] : s[k];
    return !name.empty() && name[0] != '#' && same(name, from) ? escaped : std::string(s);
  };
  std::string out;
  for (size_t i = 0; i < f.size();) {
    char c = f[i];
    if (c == '"' || c == '\'') {  // a string, or a quoted sheet name: as it is
      size_t j = i + 1;
      while (j < f.size() && !(f[j] == c && (j + 1 >= f.size() || f[j + 1] != c))) j += f[j] == c ? 2 : 1;
      out.append(f, i, std::min(j + 1, f.size()) - i);
      i = j + 1;
      continue;
    }
    if ((i == 0 || !namePart(f[i - 1])) && i + table.size() < f.size() && f[i + table.size()] == '[' &&
        same(f.substr(i, table.size()), table)) {
      size_t j = i + table.size(), depth = 0;
      for (; j < f.size(); ++j) {
        if (f[j] == '\'') {
          ++j;
          continue;
        }
        if (f[j] == '[') ++depth;
        else if (f[j] == ']' && --depth == 0) break;
      }
      if (j >= f.size()) break;
      std::string_view inner = f.substr(i + table.size() + 1, j - i - table.size() - 1);
      out.append(f, i, table.size() + 1);
      if (inner.empty() || inner[0] != '[') {
        out += item(inner);
      } else {
        for (size_t k = 0; k < inner.size();) {
          if (inner[k] != '[') {
            out += inner[k++];
            continue;
          }
          size_t e = k + 1;
          while (e < inner.size() && inner[e] != ']') e += inner[e] == '\'' ? 2 : 1;
          out += '[' + item(inner.substr(k + 1, std::min(e, inner.size()) - k - 1)) + ']';
          k = e + 1;
        }
      }
      out += ']';
      i = j + 1;
      continue;
    }
    out += c;
    ++i;
  }
  return out;
}

std::string RenameTable(std::string_view f, std::string_view from, std::string_view to) {
  auto up = [](char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 32) : c; };
  auto same = [&](std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
      if (up(a[i]) != up(b[i])) return false;
    return true;
  };
  auto namePart = [](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '\\' ||
           static_cast<unsigned char>(c) >= 0x80;
  };
  if (from.empty()) return std::string(f);
  std::string out;
  for (size_t i = 0; i < f.size();) {
    const char c = f[i];
    if (c == '"' || c == '\'') {  // a string, or a quoted sheet name: as it is
      size_t j = i + 1;
      while (j < f.size() && !(f[j] == c && (j + 1 >= f.size() || f[j + 1] != c))) j += f[j] == c ? 2 : 1;
      out.append(f, i, std::min(j + 1, f.size()) - i);
      i = j + 1;
      continue;
    }
    if (c == '[') {  // a structured reference's items (nested, ' escaping), or an external book: as they are
      size_t j = i, depth = 0;
      for (; j < f.size(); ++j) {
        if (f[j] == '\'') {
          ++j;
          continue;
        }
        if (f[j] == '[') ++depth;
        else if (f[j] == ']' && --depth == 0) break;
      }
      out.append(f, i, std::min(j + 1, f.size()) - i);
      i = j + 1;
      continue;
    }
    // The table's name on its own (Table1[Qty], ROWS(Table1)); not a function's, a sheet's or a
    // longer name's.
    if ((i == 0 || !namePart(f[i - 1])) && i + from.size() <= f.size() && same(f.substr(i, from.size()), from)) {
      const size_t e = i + from.size();
      const char next = e < f.size() ? f[e] : '\0';
      if (!namePart(next) && next != '(' && next != '!') {
        out += to;
        i = e;
        continue;
      }
    }
    out += c;
    ++i;
  }
  return out;
}

bool MoveControl(std::string& xml, int index, const Context& c) {
  pugi::xml_document d;
  if (!LoadDoc(d, xml)) return false;
  bool changed = false;
  for (const char* a : {"fmlaLink", "fmlaRange", "fmlaGroup", "fmlaTxbx"})
    changed = RewriteAttr(d.document_element(), a, index, {}, c) || changed;
  if (changed) xml = SaveDoc(d);
  return changed;
}

bool MovePivotCache(std::string& xml, const Context& c) {
  pugi::xml_document d;
  if (!LoadDoc(d, xml)) return false;
  bool changed = false;
  for (std::string_view kind : {"worksheetSource", "rangeSet"})
    Each(d.document_element(), kind, [&](pugi::xml_node n) {
      Range r;
      if (!SameName(xlsx::Attr(n, "sheet"), c.sheetName) || !ParseRange(xlsx::Attr(n, "ref"), r)) return;
      if (c.move.Area(r) && RangeName(r) != xlsx::Attr(n, "ref")) {
        SetAttr(n, "ref", RangeName(r));
        changed = true;
      }
    });
  if (changed) xml = SaveDoc(d);
  return changed;
}

bool RenamePivotSource(std::string& xml, std::string_view from, std::string_view to) {
  pugi::xml_document d;
  if (!LoadDoc(d, xml)) return false;
  bool changed = false;
  for (std::string_view kind : {"worksheetSource", "rangeSet"})
    Each(d.document_element(), kind, [&](pugi::xml_node n) {
      if (!SameName(xlsx::Attr(n, "sheet"), from)) return;
      SetAttr(n, "sheet", std::string(to));
      changed = true;
    });
  if (changed) xml = SaveDoc(d);
  return changed;
}

bool MovePivotTable(std::string& xml, const Context& c) {
  pugi::xml_document d;
  if (!LoadDoc(d, xml)) return false;
  bool changed = false;
  Each(d.document_element(), "location", [&](pugi::xml_node n) {
    Range r;
    if (ParseRange(xlsx::Attr(n, "ref"), r) && c.move.Area(r) && RangeName(r) != xlsx::Attr(n, "ref")) {
      SetAttr(n, "ref", RangeName(r));
      changed = true;
    }
  });
  if (changed) xml = SaveDoc(d);
  return changed;
}

}  // namespace fw::sheet::edit::refs
