// Table operations (P2, p2-model): rows and columns, merging and splitting cells, widths and
// heights, cell properties, on a table frame by id. The grid work is TableGrid's (shared with
// p2-objects' ObjectXml); each call is one undo step, and a refused one changes nothing.
#include <algorithm>
#include <cmath>
#include <functional>
#include <sstream>
#include <utility>

#include "core/Lang.h"
#include "slide/edit/EditInternal.h"
#include "slide/edit/ShapeXml.h"
#include "slide/edit/TableGrid.h"

namespace fw::slide::edit {

namespace {

constexpr double kEmu = 12700.0;

// The table frame `id` on slide part `part`, in the copy being edited; null when it isn't a table.
pugi::xml_node TableFrame(Editor::Impl& m, const std::string& part, uint32_t id) {
  pugi::xml_node frame = Editor::Impl::FindShape(m.Edit(part), id);
  if (!frame || !table::Of(frame)) return {};
  return Editor::Impl::Unwrap(frame);
}

// One table operation: the slide's table `id` changed by `change`, or the call refused with `why`.
template <typename F>
Result OnTable(Editor& ed, std::u16string name, size_t slide, uint32_t id, const char16_t* why, F change) {
  Call call(ed, std::move(name));
  Editor::Impl& m = ed.Internals();
  std::string part = m.SlidePart(slide);
  if (part.empty()) return call.End(Result::Fail(T(u"That slide isn't in the presentation.")));
  pugi::xml_node frame = TableFrame(m, part, id);
  if (!frame) return call.End(Result::Fail(T(u"That isn't a table.")));
  if (!change(frame)) return call.End(Result::Fail(why));
  m.Touched(part, id);
  return call.End({});
}

bool Repeat(int count, const std::function<bool(int)>& each) {
  if (count < 1 || count > 1000) return false;
  for (int i = 0; i < count; ++i)
    if (!each(i)) return false;
  return true;
}

}  // namespace

Result Editor::InsertTableRows(size_t slide, uint32_t id, int at, int count) {
  return OnTable(*this, T(u"Insert Rows"), slide, id, T(u"Rows can't be added there."), [&](pugi::xml_node f) {
    return Repeat(count, [&](int i) { return table::InsertRow(f, at + i); });
  });
}

Result Editor::DeleteTableRows(size_t slide, uint32_t id, int first, int count) {
  return OnTable(*this, T(u"Delete Rows"), slide, id, T(u"Those rows can't be deleted (a table keeps one row)."),
                 [&](pugi::xml_node f) { return Repeat(count, [&](int) { return table::DeleteRow(f, first); }); });
}

Result Editor::InsertTableColumns(size_t slide, uint32_t id, int at, int count) {
  return OnTable(*this, T(u"Insert Columns"), slide, id, T(u"Columns can't be added there."), [&](pugi::xml_node f) {
    return Repeat(count, [&](int i) { return table::InsertColumn(f, at + i); });
  });
}

Result Editor::DeleteTableColumns(size_t slide, uint32_t id, int first, int count) {
  return OnTable(*this, T(u"Delete Columns"), slide, id, T(u"Those columns can't be deleted (a table keeps one column)."),
                 [&](pugi::xml_node f) { return Repeat(count, [&](int) { return table::DeleteColumn(f, first); }); });
}

Result Editor::MergeTableCells(size_t slide, uint32_t id, int row0, int col0, int row1, int col1) {
  return OnTable(*this, T(u"Merge Cells"), slide, id, T(u"Those cells can't be merged."),
                 [&](pugi::xml_node f) { return table::Merge(f, row0, col0, row1, col1); });
}

Result Editor::SplitTableCell(size_t slide, uint32_t id, int row, int col) {
  return OnTable(*this, T(u"Split Cells"), slide, id, T(u"That cell isn't merged."),
                 [&](pugi::xml_node f) { return table::Split(f, row, col); });
}

Result Editor::SetTableColumnWidth(size_t slide, uint32_t id, int column, float points) {
  return OnTable(*this, T(u"Column Width"), slide, id, T(u"That width can't be set."), [&](pugi::xml_node f) {
    return points >= 1 && points <= 4000 && table::SetColumnWidth(f, column, std::llround(points * kEmu));
  });
}

Result Editor::SetTableRowHeight(size_t slide, uint32_t id, int row, float points) {
  return OnTable(*this, T(u"Row Height"), slide, id, T(u"That height can't be set."), [&](pugi::xml_node f) {
    return points >= 1 && points <= 4000 && table::SetRowHeight(f, row, std::llround(points * kEmu));
  });
}

Result Editor::SetCellProps(size_t slide, uint32_t id, int row0, int col0, int row1, int col1, const CellPatch& patch) {
  return OnTable(*this, T(u"Format Cells"), slide, id, T(u"Those cells can't be formatted that way."), [&](pugi::xml_node f) {
    if (row0 > row1) std::swap(row0, row1);
    if (col0 > col1) std::swap(col0, col1);
    if (row0 < 0 || col0 < 0 || row1 >= table::Rows(f) || col1 >= table::Columns(f)) return false;
    // a:tcPr's children in schema order: the borders, cell3D, the fill, headers, extLst.
    static const char* const kBorders[] = {"lnL", "lnR", "lnT", "lnB", "lnTlToBr", "lnBlToTr"};
    auto groupOf = [](std::string_view k) {
      for (int i = 0; i < 6; ++i)
        if (k == kBorders[i]) return i;
      if (k == "cell3D") return 6;
      if (k == "noFill" || k == "solidFill" || k == "gradFill" || k == "blipFill" || k == "pattFill" || k == "grpFill") return 7;
      if (k == "headers") return 8;
      return 9;
    };
    int r = 0;
    for (pugi::xml_node tr = table::Of(f).first_child(); tr; tr = tr.next_sibling()) {
      if (!Is(tr, kA, "tr")) continue;
      int c = 0;
      for (pugi::xml_node tc = tr.first_child(); tc; tc = tc.next_sibling()) {
        if (!Is(tc, kA, "tc")) continue;
        if (r >= row0 && r <= row1 && c >= col0 && c <= col1) {
          pugi::xml_node pr = DirectChild(tc, kA, "tcPr");
          if (!pr) pr = AddChildOrdered(tc, PrefixFor(tc, kA, "a") + ":tcPr", {"extLst"});
          if (patch.fill && !ReplaceGroup(pr, *patch.fill, groupOf, 7)) return false;
          for (int b = 0; b < 6; ++b) {
            if (!patch.borders[b]) continue;
            std::string xml = *patch.borders[b];
            if (!xml.empty()) {
              // Given as a:ln would hold it: the same content, named for its edge.
              pugi::xml_document d;
              if (!ParseFragment(xml, d)) return false;
              pugi::xml_node e = d.document_element();
              if (LocalNameOf(e) == "ln") {
                const std::string_view qname = e.name();
                e.set_name((std::string(qname.substr(0, qname.size() - 2)) + kBorders[b]).c_str());
              }
              std::ostringstream out;
              e.print(out, "", pugi::format_raw);
              xml = out.str();
            }
            if (!ReplaceGroup(pr, xml, groupOf, b)) return false;
          }
          if (patch.margins) {
            static const char* const kMargins[] = {"marL", "marT", "marR", "marB"};
            for (int i = 0; i < 4; ++i) {
              const float v = (*patch.margins)[i];
              if (!(v >= 0 && v <= 1000)) return false;
              const std::string emu = std::to_string(std::llround(v * kEmu));
              (pr.attribute(kMargins[i]) ? pr.attribute(kMargins[i]) : pr.append_attribute(kMargins[i])).set_value(emu.c_str());
            }
          }
          if (patch.anchor) {
            static const char* const kAnchor[] = {"t", "ctr", "b", "just", "dist"};
            (pr.attribute("anchor") ? pr.attribute("anchor") : pr.append_attribute("anchor"))
                .set_value(kAnchor[static_cast<size_t>(*patch.anchor) % 5]);
          }
        }
        ++c;
      }
      ++r;
    }
    return true;
  });
}

}  // namespace fw::slide::edit
