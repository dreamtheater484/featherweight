// Tables in slides (a:tbl): the grid, merged cells, and the table style (ppt/tableStyles.xml, or
// one of PowerPoint's built-in styles a deck names without defining) applied to every cell.
#include <algorithm>
#include <cctype>
#include <string>

#include "slide/Internal.h"

namespace fw::slide {

using namespace detail;

namespace {

constexpr int kMaxRows = 4096, kMaxColumns = 1024;
constexpr size_t kMaxCells = 200000;

// The built-in styles decks most often name without defining them (python-pptx and other
// generators write only the id): Medium Style 2 in its seven colors, and the two plain styles.
// "@" stands for the style's color.
constexpr const char* kMedium2 =
    "<a:wholeTbl><a:tcTxStyle><a:fontRef idx=\"minor\"><a:prstClr val=\"black\"/></a:fontRef><a:schemeClr "
    "val=\"dk1\"/></a:tcTxStyle><a:tcStyle><a:tcBdr>"
    "<a:left><a:ln w=\"12700\" cmpd=\"sng\"><a:solidFill><a:schemeClr val=\"lt1\"/></a:solidFill></a:ln></a:left>"
    "<a:right><a:ln w=\"12700\" cmpd=\"sng\"><a:solidFill><a:schemeClr val=\"lt1\"/></a:solidFill></a:ln></a:right>"
    "<a:top><a:ln w=\"12700\" cmpd=\"sng\"><a:solidFill><a:schemeClr val=\"lt1\"/></a:solidFill></a:ln></a:top>"
    "<a:bottom><a:ln w=\"12700\" cmpd=\"sng\"><a:solidFill><a:schemeClr val=\"lt1\"/></a:solidFill></a:ln></a:bottom>"
    "<a:insideH><a:ln w=\"12700\" cmpd=\"sng\"><a:solidFill><a:schemeClr val=\"lt1\"/></a:solidFill></a:ln></a:insideH>"
    "<a:insideV><a:ln w=\"12700\" cmpd=\"sng\"><a:solidFill><a:schemeClr val=\"lt1\"/></a:solidFill></a:ln></a:insideV>"
    "</a:tcBdr><a:fill><a:solidFill><a:schemeClr val=\"@\"><a:tint val=\"20000\"/></a:schemeClr></a:solidFill></a:fill>"
    "</a:tcStyle></a:wholeTbl>"
    "<a:band1H><a:tcStyle><a:tcBdr/><a:fill><a:solidFill><a:schemeClr val=\"@\"><a:tint val=\"40000\"/></a:schemeClr>"
    "</a:solidFill></a:fill></a:tcStyle></a:band1H><a:band2H><a:tcStyle><a:tcBdr/></a:tcStyle></a:band2H>"
    "<a:band1V><a:tcStyle><a:tcBdr/><a:fill><a:solidFill><a:schemeClr val=\"@\"><a:tint val=\"40000\"/></a:schemeClr>"
    "</a:solidFill></a:fill></a:tcStyle></a:band1V><a:band2V><a:tcStyle><a:tcBdr/></a:tcStyle></a:band2V>"
    "<a:lastCol><a:tcTxStyle b=\"on\"><a:fontRef idx=\"minor\"><a:prstClr val=\"black\"/></a:fontRef><a:schemeClr "
    "val=\"lt1\"/></a:tcTxStyle><a:tcStyle><a:tcBdr/><a:fill><a:solidFill><a:schemeClr val=\"@\"/></a:solidFill></a:fill>"
    "</a:tcStyle></a:lastCol>"
    "<a:firstCol><a:tcTxStyle b=\"on\"><a:fontRef idx=\"minor\"><a:prstClr val=\"black\"/></a:fontRef><a:schemeClr "
    "val=\"lt1\"/></a:tcTxStyle><a:tcStyle><a:tcBdr/><a:fill><a:solidFill><a:schemeClr val=\"@\"/></a:solidFill></a:fill>"
    "</a:tcStyle></a:firstCol>"
    "<a:lastRow><a:tcTxStyle b=\"on\"><a:fontRef idx=\"minor\"><a:prstClr val=\"black\"/></a:fontRef><a:schemeClr "
    "val=\"lt1\"/></a:tcTxStyle><a:tcStyle><a:tcBdr><a:top><a:ln w=\"38100\" cmpd=\"sng\"><a:solidFill><a:schemeClr "
    "val=\"lt1\"/></a:solidFill></a:ln></a:top></a:tcBdr><a:fill><a:solidFill><a:schemeClr val=\"@\"/></a:solidFill>"
    "</a:fill></a:tcStyle></a:lastRow>"
    "<a:firstRow><a:tcTxStyle b=\"on\"><a:fontRef idx=\"minor\"><a:prstClr val=\"black\"/></a:fontRef><a:schemeClr "
    "val=\"lt1\"/></a:tcTxStyle><a:tcStyle><a:tcBdr><a:bottom><a:ln w=\"38100\" cmpd=\"sng\"><a:solidFill><a:schemeClr "
    "val=\"lt1\"/></a:solidFill></a:ln></a:bottom></a:tcBdr><a:fill><a:solidFill><a:schemeClr val=\"@\"/></a:solidFill>"
    "</a:fill></a:tcStyle></a:firstRow>";

constexpr const char* kTableGrid =
    "<a:wholeTbl><a:tcTxStyle><a:fontRef idx=\"minor\"><a:scrgbClr r=\"0\" g=\"0\" b=\"0\"/></a:fontRef><a:schemeClr "
    "val=\"tx1\"/></a:tcTxStyle><a:tcStyle><a:tcBdr>"
    "<a:left><a:ln w=\"12700\" cmpd=\"sng\"><a:solidFill><a:schemeClr val=\"tx1\"/></a:solidFill></a:ln></a:left>"
    "<a:right><a:ln w=\"12700\" cmpd=\"sng\"><a:solidFill><a:schemeClr val=\"tx1\"/></a:solidFill></a:ln></a:right>"
    "<a:top><a:ln w=\"12700\" cmpd=\"sng\"><a:solidFill><a:schemeClr val=\"tx1\"/></a:solidFill></a:ln></a:top>"
    "<a:bottom><a:ln w=\"12700\" cmpd=\"sng\"><a:solidFill><a:schemeClr val=\"tx1\"/></a:solidFill></a:ln></a:bottom>"
    "<a:insideH><a:ln w=\"12700\" cmpd=\"sng\"><a:solidFill><a:schemeClr val=\"tx1\"/></a:solidFill></a:ln></a:insideH>"
    "<a:insideV><a:ln w=\"12700\" cmpd=\"sng\"><a:solidFill><a:schemeClr val=\"tx1\"/></a:solidFill></a:ln></a:insideV>"
    "</a:tcBdr><a:fill><a:noFill/></a:fill></a:tcStyle></a:wholeTbl>";

constexpr const char* kNoGrid =
    "<a:wholeTbl><a:tcTxStyle><a:fontRef idx=\"minor\"><a:scrgbClr r=\"0\" g=\"0\" b=\"0\"/></a:fontRef><a:schemeClr "
    "val=\"tx1\"/></a:tcTxStyle><a:tcStyle><a:tcBdr/><a:fill><a:noFill/></a:fill></a:tcStyle></a:wholeTbl>";

struct BuiltIn {
  const char* id;
  const char* body;
  const char* color;
};
constexpr BuiltIn kBuiltIns[] = {
    {"{073A0DAA-6AF3-43AB-8588-CEC1D06C72B9}", kMedium2, "dk1"},
    {"{5C22544A-7EE6-4342-B048-85BDC9FD1C3A}", kMedium2, "accent1"},
    {"{F5AB1C69-6EDB-4FF4-983F-18BD219EF322}", kMedium2, "accent3"},
    {"{00A15C55-8517-42AA-B614-E9B94910E393}", kMedium2, "accent4"},
    {"{7DF18680-E054-41AD-8BC1-D1AEF772440D}", kMedium2, "accent5"},
    {"{93296810-A885-4BE3-A3E7-6D5BEEA58F35}", kMedium2, "accent6"},
    {"{5940675A-B579-460E-94D1-54222C63F5DA}", kTableGrid, ""},
    {"{2D5ABB26-0587-4C30-8999-92F81FD0307C}", kNoGrid, ""},
};

bool SameId(std::string_view a, std::string_view b) {
  auto trim = [](std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
  };
  a = trim(a);
  b = trim(b);
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
  }
  return true;
}

// A fill element (or a:fillRef through the theme) resolved.
CellFill ReadCellFill(pugi::xml_node el, const draw::ColorContext& colors, const draw::Theme& theme) {
  CellFill f;
  if (!el) return f;
  draw::ColorContext ctx = colors;
  if (LocalNameOf(el) == "fillRef") {
    ctx.placeholder = draw::ReadColor(el, colors);
    el = theme.Fill(static_cast<int>(Number(el, "idx", 0)));
    if (!el) return f;
  }
  std::string_view k = LocalNameOf(el);
  if (k == "solidFill") {
    if (std::optional<draw::Color> c = draw::ReadColor(el, ctx)) {
      f.kind = CellFill::Kind::Solid;
      f.color = *c;
    }
  } else if (k == "gradFill" || k == "pattFill" || k == "blipFill") {
    f.kind = CellFill::Kind::Other;
    f.node = el;
    std::optional<draw::Color> c = k == "gradFill"   ? draw::ReadColor(A(A(el, "gsLst"), "gs"), ctx)
                                   : k == "pattFill" ? draw::ReadColor(A(el, "fgClr"), ctx)
                                                     : std::nullopt;
    if (c) f.color = *c;
  }
  return f;
}

// The fill element among `parent`'s children, or null.
pugi::xml_node FillChild(pugi::xml_node parent) {
  pugi::xml_node found;
  ForEachChild(parent, [&](pugi::xml_node c) {
    std::string_view k = LocalNameOf(c);
    if (k == "noFill" || k == "solidFill" || k == "gradFill" || k == "blipFill" || k == "pattFill" || k == "fillRef") {
      found = c;
    }
    return !found;
  });
  return found;
}

// An a:ln, or a:lnRef through the theme, resolved.
CellBorder ReadBorder(pugi::xml_node el, const draw::ColorContext& colors, const draw::Theme& theme) {
  CellBorder b;
  if (!el) return b;
  draw::ColorContext ctx = colors;
  if (LocalNameOf(el) == "lnRef") {
    ctx.placeholder = draw::ReadColor(el, colors);
    el = theme.Line(static_cast<int>(Number(el, "idx", 0)));
    if (!el) return b;
  } else {
    b.line = el;
  }
  b.width = std::clamp(Emu(el, "w", 12700), 0.f, 1584.f);
  b.dash = A(el, "prstDash").attribute("val").value();
  if (b.dash == "solid") b.dash.clear();
  pugi::xml_node fill = FillChild(el);
  CellFill f = ReadCellFill(fill, ctx, theme);
  b.visible = f.kind != CellFill::Kind::None;
  b.color = f.color;
  return b;
}

}  // namespace

const Table* Presentation::GetTable(const Shape& shape, const SlideBase& shownOn) {
  if (shape.kind != Shape::Kind::Table || !shape.owner) return nullptr;
  std::lock_guard lock(mutex_);
  auto key = std::make_pair(static_cast<const void*>(&shape), static_cast<const void*>(&shownOn));
  if (auto it = tables_.find(key); it != tables_.end()) return it->second.get();
  std::unique_ptr<Table>& slot = tables_[key];
  pugi::xml_node tbl = A(A(A(shape.node, "graphic"), "graphicData"), "tbl");
  if (!tbl) return nullptr;
  auto t = std::make_unique<Table>();
  t->node = tbl;
  const draw::Theme& theme = shownOn.theme ? *shownOn.theme : *draw::Theme::Default();
  t->colors.theme = &theme;
  t->colors.colorMap = &shownOn.colorMap;

  // The grid.
  ForEachChild(A(tbl, "tblGrid"), [&](pugi::xml_node col) {
    if (LocalNameOf(col) == "gridCol") t->columns.push_back(std::clamp(Emu(col, "w"), 0.f, 4032.f));
    return t->columns.size() < kMaxColumns;
  });
  std::vector<pugi::xml_node> rows;
  ForEachChild(tbl, [&](pugi::xml_node tr) {
    if (LocalNameOf(tr) != "tr") return true;
    rows.push_back(tr);
    t->rows.push_back(std::clamp(Emu(tr, "h"), 0.f, 4032.f));
    return rows.size() < kMaxRows && rows.size() * t->columns.size() < kMaxCells;
  });
  const int R = static_cast<int>(t->rows.size()), C = static_cast<int>(t->columns.size());

  // The style: the deck's, a built-in one, or one written in the table.
  pugi::xml_node tblPr = A(tbl, "tblPr");
  t->rtl = Bool(tblPr, "rtl", false);
  bool firstRow = Bool(tblPr, "firstRow", false), lastRow = Bool(tblPr, "lastRow", false);
  bool firstCol = Bool(tblPr, "firstCol", false), lastCol = Bool(tblPr, "lastCol", false);
  bool bandRow = Bool(tblPr, "bandRow", false), bandCol = Bool(tblPr, "bandCol", false);
  pugi::xml_node style = A(tblPr, "tableStyle");
  if (!style) {
    std::string_view id = A(tblPr, "tableStyleId").text().get();
    if (!id.empty()) {
      if (!tableStylesRead_) {
        tableStylesRead_ = true;
        std::string part = TargetOfType(RelationshipsOf(presentationPart_), "tableStyles");
        tableStyles_ = DomOf(*this, part, nullptr);
      }
      if (tableStyles_) {
        ForEachChild(tableStyles_->Root(), [&](pugi::xml_node s) {
          if (LocalNameOf(s) == "tblStyle" && SameId(s.attribute("styleId").value(), id)) style = s;
          return !style;
        });
      }
    }
    if (!style) {
      // A built-in style the deck names without defining it. PowerPoint 2021 (probed 2026-11-20)
      // draws a table without a style, or with an id it doesn't know, as a black 1 pt grid with no
      // fills: Table Grid's look.
      const BuiltIn* b = &kBuiltIns[std::size(kBuiltIns) - 2];
      for (const BuiltIn& candidate : kBuiltIns) {
        if (!id.empty() && SameId(candidate.id, id)) b = &candidate;
      }
      if (!builtInStyles_) builtInStyles_ = std::make_unique<pugi::xml_document>();
      std::string body = b->body;
      for (size_t at = body.find('@'); at != std::string::npos; at = body.find('@', at)) body.replace(at, 1, b->color);
      std::string xml =
          std::string("<a:tblStyle xmlns:a=\"") + std::string(kA) + "\" styleId=\"" + b->id + "\">" + body + "</a:tblStyle>";
      pugi::xml_document doc;
      if (doc.load_buffer(xml.data(), xml.size())) style = builtInStyles_->append_copy(doc.document_element());
    }
  }
  t->background = ReadCellFill(FillChild(A(style, "tblBg")), t->colors, theme);

  // Every grid cell, then the spans.
  t->cells.resize(static_cast<size_t>(R) * static_cast<size_t>(C));
  for (int r = 0; r < R; ++r) {
    int c = 0;
    ForEachChild(rows[r], [&](pugi::xml_node tc) {
      if (LocalNameOf(tc) != "tc") return true;
      TableCell& cell = t->cells[static_cast<size_t>(r) * C + c];
      cell.node = tc;
      cell.row = r;
      cell.column = c;
      cell.columnSpan = std::clamp(static_cast<int>(Number(tc, "gridSpan", 1)), 1, C - c);
      cell.rowSpan = std::clamp(static_cast<int>(Number(tc, "rowSpan", 1)), 1, R - r);
      cell.covered = Bool(tc, "hMerge", false) || Bool(tc, "vMerge", false);
      return ++c < C;
    });
    for (; c < C; ++c) {
      TableCell& cell = t->cells[static_cast<size_t>(r) * C + c];
      cell.row = r;
      cell.column = c;
    }
  }
  for (TableCell& cell : t->cells) {
    if (cell.covered || (cell.rowSpan == 1 && cell.columnSpan == 1)) continue;
    for (int r = cell.row; r < cell.row + cell.rowSpan; ++r) {
      for (int c = cell.column; c < cell.column + cell.columnSpan; ++c) {
        if (r != cell.row || c != cell.column) t->cells[static_cast<size_t>(r) * C + c].covered = true;
      }
    }
  }

  // The style's parts for each cell, lowest first, each with the region its outer borders bound.
  struct Region {
    pugi::xml_node part;
    int r0, r1, c0, c1;
  };
  auto partOf = [&](const char* name) { return A(style, name); };
  for (TableCell& cell : t->cells) {
    const int r = cell.row, c = cell.column;
    const int rEnd = r + cell.rowSpan - 1, cEnd = c + cell.columnSpan - 1;
    std::vector<Region> regions;
    regions.push_back({partOf("wholeTbl"), 0, R - 1, 0, C - 1});
    if (bandRow && !(firstRow && r == 0) && !(lastRow && r == R - 1)) {
      int i = r - (firstRow ? 1 : 0);
      regions.push_back({partOf(i % 2 == 0 ? "band1H" : "band2H"), r, r, 0, C - 1});
    }
    if (bandCol && !(firstCol && c == 0) && !(lastCol && c == C - 1)) {
      int i = c - (firstCol ? 1 : 0);
      regions.push_back({partOf(i % 2 == 0 ? "band1V" : "band2V"), 0, R - 1, c, c});
    }
    if (lastCol && cEnd == C - 1) regions.push_back({partOf("lastCol"), 0, R - 1, C - 1, C - 1});
    if (firstCol && c == 0) regions.push_back({partOf("firstCol"), 0, R - 1, 0, 0});
    if (lastRow && rEnd == R - 1) regions.push_back({partOf("lastRow"), R - 1, R - 1, 0, C - 1});
    if (firstRow && r == 0) regions.push_back({partOf("firstRow"), 0, 0, 0, C - 1});
    if (lastRow && lastCol && rEnd == R - 1 && cEnd == C - 1) regions.push_back({partOf("seCell"), R - 1, R - 1, C - 1, C - 1});
    if (lastRow && firstCol && rEnd == R - 1 && c == 0) regions.push_back({partOf("swCell"), R - 1, R - 1, 0, 0});
    if (firstRow && lastCol && r == 0 && cEnd == C - 1) regions.push_back({partOf("neCell"), 0, 0, C - 1, C - 1});
    if (firstRow && firstCol && r == 0 && c == 0) regions.push_back({partOf("nwCell"), 0, 0, 0, 0});

    std::vector<pugi::xml_node> textStyles;
    for (const Region& g : regions) {
      if (!g.part) continue;
      if (pugi::xml_node tx = A(g.part, "tcTxStyle")) textStyles.push_back(tx);
      pugi::xml_node tcStyle = A(g.part, "tcStyle");
      if (pugi::xml_node fill = A(tcStyle, "fill")) cell.fill = ReadCellFill(FillChild(fill), t->colors, theme);
      if (pugi::xml_node ref = A(tcStyle, "fillRef")) cell.fill = ReadCellFill(ref, t->colors, theme);
      pugi::xml_node bdr = A(tcStyle, "tcBdr");
      auto edge = [&](int which, const char* name) {
        pugi::xml_node e = A(bdr, name);
        if (!e) return;
        pugi::xml_node line = A(e, "ln");
        if (!line) line = A(e, "lnRef");
        cell.borders[which] = ReadBorder(line, t->colors, theme);
      };
      edge(TableCell::Left, c == g.c0 ? "left" : "insideV");
      edge(TableCell::Right, cEnd == g.c1 ? "right" : "insideV");
      edge(TableCell::Top, r == g.r0 ? "top" : "insideH");
      edge(TableCell::Bottom, rEnd == g.r1 ? "bottom" : "insideH");
      edge(TableCell::TopLeftToBottomRight, "tl2br");
      edge(TableCell::BottomLeftToTopRight, "tr2bl");
    }

    // The cell's own properties.
    pugi::xml_node tcPr = A(cell.node, "tcPr");
    cell.marginLeft = std::clamp(Emu(tcPr, "marL", 91440), 0.f, 4032.f);
    cell.marginRight = std::clamp(Emu(tcPr, "marR", 91440), 0.f, 4032.f);
    cell.marginTop = std::clamp(Emu(tcPr, "marT", 45720), 0.f, 4032.f);
    cell.marginBottom = std::clamp(Emu(tcPr, "marB", 45720), 0.f, 4032.f);
    std::string_view anchor = tcPr.attribute("anchor").value();
    cell.anchor = anchor == "ctr"    ? BodyProps::Anchor::Middle
                  : anchor == "b"    ? BodyProps::Anchor::Bottom
                  : anchor == "just" ? BodyProps::Anchor::Justified
                  : anchor == "dist" ? BodyProps::Anchor::Distributed
                                     : BodyProps::Anchor::Top;
    std::string_view vert = tcPr.attribute("vert").value();
    cell.vertical = vert == "vert"           ? BodyProps::Vertical::Vertical
                    : vert == "vert270"      ? BodyProps::Vertical::Vertical270
                    : vert == "wordArtVert"  ? BodyProps::Vertical::WordArtVertical
                    : vert == "eaVert"       ? BodyProps::Vertical::EastAsian
                    : vert == "mongolianVert" ? BodyProps::Vertical::Mongolian
                                              : BodyProps::Vertical::Horizontal;
    const char* lines[] = {"lnL", "lnR", "lnT", "lnB", "lnTlToBr", "lnBlToTr"};
    for (int i = 0; i < 6; ++i) {
      if (pugi::xml_node ln = A(tcPr, lines[i])) cell.borders[i] = ReadBorder(ln, t->colors, theme);
    }
    if (pugi::xml_node fill = FillChild(tcPr)) cell.fill = ReadCellFill(fill, t->colors, theme);

    cell.textStyles = textStyles;
    if (pugi::xml_node txBody = A(cell.node, "txBody")) {
      cellTexts_.push_back(ResolvePlainText(*this, txBody, *shape.owner, shownOn, textStyles));
      cell.text = cellTexts_.back().get();
    }
  }
  slot = std::move(t);
  return slot.get();
}

}  // namespace fw::slide
