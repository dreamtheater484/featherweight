// A table's grid edited in place (P2): rows and columns (from p2-objects' ObjectXml, moved here so
// the edit API and the objects share them), merging and splitting cells, widths and heights.
#include "slide/edit/TableGrid.h"

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include "slide/edit/EditInternal.h"

namespace fw::slide::edit::table {

namespace {

std::vector<pugi::xml_node> Kids(pugi::xml_node parent, std::string_view local) {
  std::vector<pugi::xml_node> out;
  for (pugi::xml_node c = parent.first_child(); c; c = c.next_sibling())
    if (c.type() == pugi::node_element && Is(c, kA, local)) out.push_back(c);
  return out;
}

pugi::xml_node Kid(pugi::xml_node parent, std::string_view local) { return DirectChild(parent, kA, local); }

std::string NameIn(pugi::xml_node scope, std::string_view local) {
  const std::string p = PrefixFor(scope, kA, "a");
  return p.empty() ? std::string(local) : p + ":" + std::string(local);
}

int64_t Int(pugi::xml_node n, const char* attr, int64_t def = 0) {
  pugi::xml_attribute a = n.attribute(attr);
  return a ? a.as_llong(def) : def;
}

void SetAttr(pugi::xml_node n, const char* attr, const char* v) {
  (n.attribute(attr) ? n.attribute(attr) : n.append_attribute(attr)).set_value(v);
}

void SetInt(pugi::xml_node n, const char* attr, int64_t v) { SetAttr(n, attr, std::to_string(v).c_str()); }

bool Flag(pugi::xml_node n, const char* attr) {
  const std::string_view v = n.attribute(attr).value();
  return v == "1" || v == "true" || v == "on";
}

// Cells of a table as a grid: tcs[row][column] (short where a row is short).
std::vector<std::vector<pugi::xml_node>> Grid(pugi::xml_node tbl) {
  std::vector<std::vector<pugi::xml_node>> out;
  for (pugi::xml_node tr : Kids(tbl, "tr")) out.push_back(Kids(tr, "tc"));
  return out;
}

int Span(pugi::xml_node tc, const char* attr) { return static_cast<int>(std::clamp<int64_t>(Int(tc, attr, 1), 1, 1000)); }

void SetSpan(pugi::xml_node tc, const char* attr, int v) {
  if (v <= 1) tc.remove_attribute(attr);
  else SetInt(tc, attr, v);
}

// A cell like it was but empty: its cell properties, the first paragraph's properties, and the look
// of its first run (else its end-of-paragraph properties) for the text typed next.
void ClearCell(pugi::xml_node tc) {
  pugi::xml_node body = Kid(tc, "txBody");
  if (!body) return;
  pugi::xml_node first = Kid(body, "p");
  pugi::xml_node pPr = first ? Kid(first, "pPr") : pugi::xml_node();
  pugi::xml_node endPr = first ? Kid(first, "endParaRPr") : pugi::xml_node();
  pugi::xml_node runPr;  // the text's own look wins over the empty paragraph end's
  if (first)
    for (pugi::xml_node r = first.first_child(); r && !runPr; r = r.next_sibling())
      if (LocalNameOf(r) == "r") runPr = Kid(r, "rPr");
  pugi::xml_document keep;
  pugi::xml_node kp = keep.append_child("keep");
  pugi::xml_node pPrCopy = pPr ? kp.append_copy(pPr) : pugi::xml_node();
  pugi::xml_node endCopy = runPr ? kp.append_copy(runPr) : endPr ? kp.append_copy(endPr) : pugi::xml_node();
  for (pugi::xml_node p : Kids(body, "p")) body.remove_child(p);
  pugi::xml_node np = body.append_child(NameIn(body, "p").c_str());
  if (pPrCopy) np.append_copy(pPrCopy);
  if (endCopy) {
    pugi::xml_node e = np.append_copy(endCopy);
    e.set_name(NameIn(body, "endParaRPr").c_str());
    e.remove_attribute("dirty");
    e.remove_attribute("err");
  }
  // An a:extLst after the paragraphs stays after them.
  if (pugi::xml_node ext = Kid(body, "extLst")) body.insert_move_after(ext, np);
}

// Office's row and column ids (a16:rowId, a16:colId in an a:extLst): a copy gets a new one.
void RenumberIds(pugi::xml_node copy, std::set<uint32_t>& used) {
  std::vector<pugi::xml_node> stack{copy};
  while (!stack.empty()) {
    pugi::xml_node n = stack.back();
    stack.pop_back();
    for (pugi::xml_node c = n.first_child(); c; c = c.next_sibling())
      if (c.type() == pugi::node_element) stack.push_back(c);
    const std::string_view local = LocalNameOf(n);
    if ((local == "rowId" || local == "colId") && n.attribute("val")) {
      uint32_t v = static_cast<uint32_t>(Int(n, "val", 0)) + 1;
      while (used.count(v) || v == 0) ++v;
      used.insert(v);
      SetInt(n, "val", v);
    }
  }
}

std::set<uint32_t> UsedIds(pugi::xml_node tbl) {
  std::set<uint32_t> out;
  std::vector<pugi::xml_node> stack{tbl};
  while (!stack.empty()) {
    pugi::xml_node n = stack.back();
    stack.pop_back();
    for (pugi::xml_node c = n.first_child(); c; c = c.next_sibling())
      if (c.type() == pugi::node_element) stack.push_back(c);
    const std::string_view local = LocalNameOf(n);
    if (local == "rowId" || local == "colId") out.insert(static_cast<uint32_t>(Int(n, "val", 0)));
  }
  return out;
}

// The frame's size from the grid: the columns' widths and the rows' heights.
void FitFrame(pugi::xml_node frame, pugi::xml_node tbl) {
  int64_t cx = 0, cy = 0;
  for (pugi::xml_node c : Kids(Kid(tbl, "tblGrid"), "gridCol")) cx += Int(c, "w");
  for (pugi::xml_node r : Kids(tbl, "tr")) cy += Int(r, "h");
  if (pugi::xml_node ext = Kid(DirectChild(frame, kP, "xfrm"), "ext")) {
    SetInt(ext, "cx", cx);
    SetInt(ext, "cy", cy);
  }
}

bool HasText(pugi::xml_node p) {
  std::vector<pugi::xml_node> stack{p};
  while (!stack.empty()) {
    pugi::xml_node n = stack.back();
    stack.pop_back();
    if (Is(n, kA, "t") && *n.text().get()) return true;
    for (pugi::xml_node c = n.first_child(); c; c = c.next_sibling())
      if (c.type() == pugi::node_element) stack.push_back(c);
  }
  return false;
}

}  // namespace

pugi::xml_node Of(pugi::xml_node frame) {
  if (!Is(frame, kP, "graphicFrame")) return {};
  return Kid(Kid(Kid(frame, "graphic"), "graphicData"), "tbl");
}

int Rows(pugi::xml_node frame) { return static_cast<int>(Kids(Of(frame), "tr").size()); }
int Columns(pugi::xml_node frame) { return static_cast<int>(Kids(Kid(Of(frame), "tblGrid"), "gridCol").size()); }

bool InsertRow(pugi::xml_node frame, int at) {
  pugi::xml_node tbl = Of(frame);
  std::vector<pugi::xml_node> rows = Kids(tbl, "tr");
  if (!tbl || rows.empty() || at < 0 || at > static_cast<int>(rows.size()) || rows.size() >= 1000) return false;
  const auto grid = Grid(tbl);
  const int n = static_cast<int>(rows.size());
  pugi::xml_node like = rows[at > 0 ? at - 1 : 0];
  pugi::xml_node row = at < n ? tbl.insert_copy_before(like, rows[at]) : tbl.insert_copy_after(like, rows[n - 1]);
  std::set<uint32_t> used = UsedIds(tbl);
  RenumberIds(row, used);
  std::vector<pugi::xml_node> cells = Kids(row, "tc");
  std::set<pugi::xml_node_struct*> grown;
  for (size_t c = 0; c < cells.size(); ++c) {
    pugi::xml_node tc = cells[c];
    ClearCell(tc);
    for (const char* a : {"gridSpan", "rowSpan", "hMerge", "vMerge"}) tc.remove_attribute(a);
    // A vertical merge over the place: the row below is covered by one above.
    if (at <= 0 || at >= n || c >= grid[at].size() || !Flag(grid[at][c], "vMerge")) continue;
    for (const char* a : {"gridSpan", "hMerge", "vMerge"})
      if (pugi::xml_attribute v = grid[at][c].attribute(a)) SetAttr(tc, a, v.value());
    int r = at - 1;
    while (r > 0 && c < grid[r].size() && Flag(grid[r][c], "vMerge")) --r;
    size_t oc = c;
    while (oc > 0 && oc < grid[r].size() && Flag(grid[r][oc], "hMerge")) --oc;
    if (oc < grid[r].size()) {
      pugi::xml_node origin = grid[r][oc];
      if (grown.insert(origin.internal_object()).second && Span(origin, "rowSpan") > 1)
        SetSpan(origin, "rowSpan", Span(origin, "rowSpan") + 1);
    }
  }
  FitFrame(frame, tbl);
  return true;
}

bool InsertColumn(pugi::xml_node frame, int at) {
  pugi::xml_node tbl = Of(frame);
  pugi::xml_node tblGrid = Kid(tbl, "tblGrid");
  std::vector<pugi::xml_node> cols = Kids(tblGrid, "gridCol");
  const int n = static_cast<int>(cols.size());
  if (!tbl || n == 0 || at < 0 || at > n || n >= 1000) return false;
  const auto grid = Grid(tbl);
  std::set<uint32_t> used = UsedIds(tbl);
  const int likeCol = at > 0 ? at - 1 : 0;
  pugi::xml_node col = at < n ? tblGrid.insert_copy_before(cols[likeCol], cols[at]) : tblGrid.insert_copy_after(cols[likeCol], cols[n - 1]);
  RenumberIds(col, used);
  std::vector<pugi::xml_node> rows = Kids(tbl, "tr");
  for (size_t r = 0; r < rows.size(); ++r) {
    const auto& cells = grid[r];
    if (cells.empty()) continue;
    const size_t likeIdx = std::min<size_t>(likeCol, cells.size() - 1);
    pugi::xml_node tc = at < static_cast<int>(cells.size()) ? rows[r].insert_copy_before(cells[likeIdx], cells[at])
                                                           : rows[r].insert_copy_after(cells[likeIdx], cells.back());
    ClearCell(tc);
    for (const char* a : {"gridSpan", "rowSpan", "hMerge", "vMerge"}) tc.remove_attribute(a);
    if (at <= 0 || at >= static_cast<int>(cells.size()) || !Flag(cells[at], "hMerge")) continue;
    for (const char* a : {"rowSpan", "hMerge", "vMerge"})
      if (pugi::xml_attribute v = cells[at].attribute(a)) SetAttr(tc, a, v.value());
    int c = at - 1;
    while (c > 0 && Flag(cells[c], "hMerge")) --c;
    if (Span(cells[c], "gridSpan") > 1) SetSpan(cells[c], "gridSpan", Span(cells[c], "gridSpan") + 1);
  }
  FitFrame(frame, tbl);
  return true;
}

bool DeleteRow(pugi::xml_node frame, int row) {
  pugi::xml_node tbl = Of(frame);
  std::vector<pugi::xml_node> rows = Kids(tbl, "tr");
  const int n = static_cast<int>(rows.size());
  if (!tbl || n <= 1 || row < 0 || row >= n) return false;
  const auto grid = Grid(tbl);
  std::set<pugi::xml_node_struct*> shrunk;
  for (size_t c = 0; c < grid[row].size(); ++c) {
    pugi::xml_node tc = grid[row][c];
    if (Flag(tc, "vMerge")) {  // covered from above: the merge shrinks
      int r = row - 1;
      while (r > 0 && c < grid[r].size() && Flag(grid[r][c], "vMerge")) --r;
      if (r >= 0 && c < grid[r].size()) {
        pugi::xml_node origin = grid[r][c];
        if (Span(origin, "rowSpan") > 1 && shrunk.insert(origin.internal_object()).second)
          SetSpan(origin, "rowSpan", Span(origin, "rowSpan") - 1);
      }
      continue;
    }
    // The top of a merge: the cell below takes over.
    if (row + 1 < n && c < grid[row + 1].size() && Flag(grid[row + 1][c], "vMerge")) {
      pugi::xml_node below = grid[row + 1][c];
      below.remove_attribute("vMerge");
      SetSpan(below, "rowSpan", Span(tc, "rowSpan") - 1);
      if (!Flag(tc, "hMerge")) {
        if (pugi::xml_attribute g = tc.attribute("gridSpan")) SetAttr(below, "gridSpan", g.value());
        pugi::xml_node body = Kid(tc, "txBody"), old = Kid(below, "txBody");
        if (body) {
          old ? below.insert_copy_before(body, old) : below.prepend_copy(body);
          if (old) below.remove_child(old);
        }
      }
    }
  }
  tbl.remove_child(rows[row]);
  FitFrame(frame, tbl);
  return true;
}

bool DeleteColumn(pugi::xml_node frame, int column) {
  pugi::xml_node tbl = Of(frame);
  pugi::xml_node tblGrid = Kid(tbl, "tblGrid");
  std::vector<pugi::xml_node> cols = Kids(tblGrid, "gridCol");
  const int n = static_cast<int>(cols.size());
  if (!tbl || n <= 1 || column < 0 || column >= n) return false;
  const auto grid = Grid(tbl);
  std::vector<pugi::xml_node> rows = Kids(tbl, "tr");
  for (size_t r = 0; r < rows.size(); ++r) {
    const auto& cells = grid[r];
    if (column >= static_cast<int>(cells.size())) continue;
    pugi::xml_node tc = cells[column];
    if (Flag(tc, "hMerge")) {
      int c = column - 1;
      while (c > 0 && Flag(cells[c], "hMerge")) --c;
      if (c >= 0 && Span(cells[c], "gridSpan") > 1) SetSpan(cells[c], "gridSpan", Span(cells[c], "gridSpan") - 1);
    } else if (column + 1 < static_cast<int>(cells.size()) && Flag(cells[column + 1], "hMerge")) {
      pugi::xml_node next = cells[column + 1];
      next.remove_attribute("hMerge");
      SetSpan(next, "gridSpan", Span(tc, "gridSpan") - 1);
      if (!Flag(tc, "vMerge")) {
        if (pugi::xml_attribute rs = tc.attribute("rowSpan")) SetAttr(next, "rowSpan", rs.value());
        pugi::xml_node body = Kid(tc, "txBody"), old = Kid(next, "txBody");
        if (body) {
          if (old) {
            next.insert_copy_before(body, old);
            next.remove_child(old);
          } else {
            next.prepend_copy(body);
          }
        }
      }
    }
    rows[r].remove_child(tc);
  }
  tblGrid.remove_child(cols[column]);
  FitFrame(frame, tbl);
  return true;
}

bool Merge(pugi::xml_node frame, int row0, int col0, int row1, int col1) {
  pugi::xml_node tbl = Of(frame);
  if (!tbl) return false;
  if (row0 > row1) std::swap(row0, row1);
  if (col0 > col1) std::swap(col0, col1);
  const auto grid = Grid(tbl);
  const int rows = static_cast<int>(grid.size());
  if (row0 < 0 || col0 < 0 || row1 >= rows || col1 >= Columns(frame) || (row0 == row1 && col0 == col1)) return false;
  for (int r = row0; r <= row1; ++r)
    if (static_cast<int>(grid[r].size()) <= col1) return false;  // a short row: nothing to merge with
  // Merges already there lie inside the range or outside it.
  for (int r = 0; r < rows; ++r) {
    for (int c = 0; c < static_cast<int>(grid[r].size()); ++c) {
      pugi::xml_node tc = grid[r][c];
      if (Flag(tc, "hMerge") || Flag(tc, "vMerge")) continue;
      const int r1 = r + Span(tc, "rowSpan") - 1, c1 = c + Span(tc, "gridSpan") - 1;
      const bool meets = !(r1 < row0 || r > row1 || c1 < col0 || c > col1);
      const bool inside = r >= row0 && r1 <= row1 && c >= col0 && c1 <= col1;
      if (meets && !inside) return false;
    }
  }
  pugi::xml_node origin = grid[row0][col0];
  pugi::xml_node body = Kid(origin, "txBody");
  bool ownText = false;
  for (pugi::xml_node p : Kids(body, "p")) ownText = ownText || HasText(p);
  for (int r = row0; r <= row1; ++r) {
    for (int c = col0; c <= col1; ++c) {
      pugi::xml_node tc = grid[r][c];
      if (tc == origin) continue;
      // The text of a cell shown until now joins the merged cell's, as PowerPoint does.
      if (body && !Flag(tc, "hMerge") && !Flag(tc, "vMerge")) {
        for (pugi::xml_node p : Kids(Kid(tc, "txBody"), "p")) {
          if (!HasText(p)) continue;
          if (!ownText) {  // the merged cell's own empty paragraph gives way
            for (pugi::xml_node q : Kids(body, "p")) body.remove_child(q);
            ownText = true;
          }
          pugi::xml_node ext = Kid(body, "extLst");
          ext ? body.insert_copy_before(p, ext) : body.append_copy(p);
        }
      }
      ClearCell(tc);
      for (const char* a : {"gridSpan", "rowSpan", "hMerge", "vMerge"}) tc.remove_attribute(a);
      if (r == row0) {
        SetAttr(tc, "hMerge", "1");
      } else {
        SetAttr(tc, "vMerge", "1");
        if (c == col0) SetSpan(tc, "gridSpan", col1 - col0 + 1);
        else SetAttr(tc, "hMerge", "1");
      }
    }
  }
  origin.remove_attribute("hMerge");
  origin.remove_attribute("vMerge");
  SetSpan(origin, "gridSpan", col1 - col0 + 1);
  SetSpan(origin, "rowSpan", row1 - row0 + 1);
  return true;
}

bool Split(pugi::xml_node frame, int row, int col) {
  pugi::xml_node tbl = Of(frame);
  if (!tbl) return false;
  auto grid = Grid(tbl);
  const int rows = static_cast<int>(grid.size());
  if (row < 0 || row >= rows || col < 0 || col >= static_cast<int>(grid[row].size())) return false;
  int r = row, c = col;
  while (r > 0 && c < static_cast<int>(grid[r].size()) && Flag(grid[r][c], "vMerge")) --r;
  if (c >= static_cast<int>(grid[r].size())) return false;
  while (c > 0 && Flag(grid[r][c], "hMerge")) --c;
  pugi::xml_node origin = grid[r][c];
  const int rs = Span(origin, "rowSpan"), gs = Span(origin, "gridSpan");
  if (rs == 1 && gs == 1) return false;
  for (int rr = r; rr < std::min(rows, r + rs); ++rr)
    for (int cc = c; cc < std::min(static_cast<int>(grid[rr].size()), c + gs); ++cc)
      for (const char* a : {"gridSpan", "rowSpan", "hMerge", "vMerge"}) grid[rr][cc].remove_attribute(a);
  return true;
}

bool SetColumnWidth(pugi::xml_node frame, int column, int64_t emu) {
  pugi::xml_node tbl = Of(frame);
  std::vector<pugi::xml_node> cols = Kids(Kid(tbl, "tblGrid"), "gridCol");
  if (!tbl || column < 0 || column >= static_cast<int>(cols.size()) || emu <= 0) return false;
  SetInt(cols[column], "w", emu);
  FitFrame(frame, tbl);
  return true;
}

bool SetRowHeight(pugi::xml_node frame, int row, int64_t emu) {
  pugi::xml_node tbl = Of(frame);
  std::vector<pugi::xml_node> rows = Kids(tbl, "tr");
  if (!tbl || row < 0 || row >= static_cast<int>(rows.size()) || emu <= 0) return false;
  SetInt(rows[row], "h", emu);
  FitFrame(frame, tbl);
  return true;
}

}  // namespace fw::slide::edit::table
