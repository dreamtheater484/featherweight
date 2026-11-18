#include "sheet/objects/SheetObjects.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <unordered_map>

#include "chart/Chart.h"
#include "core/Strings.h"
#include "draw/Drawing.h"
#include "draw/Shapes.h"
#include "draw/Xml.h"
#include "pkg/Package.h"
#include "pkg/XmlDom.h"
#include "sheet/model/Workbook.h"
#include "sheet/objects/ChartData.h"
#include "sheet/objects/RichText.h"
#include "sheet/xlsx/Internal.h"
#include "text/Fonts.h"

namespace fw::sheet {

namespace {

using namespace draw::xml;
using objects::RichParagraph;
using objects::RichSpan;

constexpr std::string_view kExcel = "urn:schemas-microsoft-com:office:excel";
constexpr std::string_view kThreads = "http://schemas.microsoft.com/office/spreadsheetml/2018/threadedcomments";
constexpr size_t kMaxObjects = 10000, kMaxNotes = 65536;
constexpr float kPx = 0.75f;  // a pixel at 96 DPI, in points

// ---- Anchors

struct Marker {
  uint32_t col = 0, row = 0;
  float colOff = 0, rowOff = 0;  // points
};

enum class AnchorKind : uint8_t { TwoCell, OneCell, Absolute };

struct Anchor {
  AnchorKind kind = AnchorKind::TwoCell;
  Marker from, to;
  draw::Point pos;  // absolute
  draw::Size ext;   // one-cell and absolute
};

uint32_t Index(pugi::xml_node n, uint32_t max) {
  return static_cast<uint32_t>(std::clamp(Number(n.text().get(), 0), 0.0, static_cast<double>(max - 1)));
}
float Emu(pugi::xml_node n, const char* attr) { return EmuToPt(std::clamp(Number(n, attr, 0), 0.0, 1e13)); }

Marker ReadMarker(pugi::xml_node m) {
  Marker k;
  k.col = Index(Child(m, kXdr, "col"), kMaxCols);
  k.row = Index(Child(m, kXdr, "row"), kMaxRows);
  k.colOff = EmuToPt(std::clamp(Number(Child(m, kXdr, "colOff").text().get(), 0), 0.0, 1e13));
  k.rowOff = EmuToPt(std::clamp(Number(Child(m, kXdr, "rowOff").text().get(), 0), 0.0, 1e13));
  return k;
}

// An offset never reaches past its cell: in a hidden column or row it's nothing.
float X(const Marker& m, const SheetLayout& l) {
  const double left = l.ColumnLeft(m.col), right = l.ColumnLeft(m.col + 1);
  return static_cast<float>(left + std::min<double>(m.colOff, std::max(right - left, 0.0)));
}
float Y(const Marker& m, const SheetLayout& l) {
  const double top = l.RowTop(m.row), bottom = l.RowTop(m.row + 1);
  return static_cast<float>(top + std::min<double>(m.rowOff, std::max(bottom - top, 0.0)));
}

draw::Rect Place(const Anchor& a, const SheetLayout& l) {
  draw::Rect r;
  switch (a.kind) {
    case AnchorKind::TwoCell: r = {X(a.from, l), Y(a.from, l), X(a.to, l), Y(a.to, l)}; break;
    case AnchorKind::OneCell: r = {X(a.from, l), Y(a.from, l), 0, 0}; break;
    case AnchorKind::Absolute: r = {a.pos.x, a.pos.y, 0, 0}; break;
  }
  if (a.kind != AnchorKind::TwoCell) r.right = r.left + a.ext.width, r.bottom = r.top + a.ext.height;
  if (r.right < r.left) std::swap(r.left, r.right);
  if (r.bottom < r.top) std::swap(r.top, r.bottom);
  return r;
}

bool Meets(const draw::Rect& a, const draw::Rect& b) {
  return a.left <= b.right && b.left <= a.right && a.top <= b.bottom && b.top <= a.bottom;
}
bool Empty(const draw::Rect& r) { return !(r.Width() > 0 && r.Height() > 0); }

// A right-to-left sheet's place for a rect (DrawOptions::mirrorX).
draw::Rect Mirror(const draw::Rect& r, float mirrorX) {
  return mirrorX > 0 ? draw::Rect{mirrorX - r.right, r.top, mirrorX - r.left, r.bottom} : r;
}

// ---- Placing a shape: its xfrm is set to where its anchor puts it, so it draws at its own size
// (files from other programs often leave it out; Excel's may differ by Excel's own rounding).

std::string Prefix(pugi::xml_node n) {
  std::string_view name = n.name();
  const size_t colon = name.find(':');
  return colon == std::string_view::npos ? std::string() : std::string(name.substr(0, colon + 1));
}

pugi::xml_node AChild(pugi::xml_node parent, const char* local) {
  if (pugi::xml_node c = A(parent, local)) return c;
  pugi::xml_node c = parent.append_child((std::string("a:") + local).c_str());
  c.append_attribute("xmlns:a") = std::string(kA).c_str();
  return c;
}

void SetAttr(pugi::xml_node n, const char* name, long long v) {
  pugi::xml_attribute a = n.attribute(name);
  if (!a) a = n.append_attribute(name);
  a.set_value(v);
}

void SetXfrm(pugi::xml_node shape, const draw::Rect& r) {
  const std::string_view k = LocalNameOf(shape);
  pugi::xml_node xfrm;
  if (k == "graphicFrame") {
    xfrm = Child(shape, kXdr, "xfrm");
    if (!xfrm) xfrm = shape.prepend_child((Prefix(shape) + "xfrm").c_str());
  } else {
    pugi::xml_node props = Child(shape, kXdr, k == "grpSp" ? "grpSpPr" : "spPr");
    if (!props) props = shape.append_child((Prefix(shape) + (k == "grpSp" ? "grpSpPr" : "spPr")).c_str());
    xfrm = A(props, "xfrm");
    if (!xfrm) {
      xfrm = props.prepend_child("a:xfrm");
      xfrm.append_attribute("xmlns:a") = std::string(kA).c_str();
    }
  }
  pugi::xml_node off = AChild(xfrm, "off"), ext = AChild(xfrm, "ext");
  SetAttr(off, "x", std::llround(r.left * kEmuPerPoint));
  SetAttr(off, "y", std::llround(r.top * kEmuPerPoint));
  SetAttr(ext, "cx", std::llround(r.Width() * kEmuPerPoint));
  SetAttr(ext, "cy", std::llround(r.Height() * kEmuPerPoint));
}

bool Near(const draw::Rect& a, const draw::Rect& b) {
  return std::fabs(a.left - b.left) < 0.01f && std::fabs(a.top - b.top) < 0.01f && std::fabs(a.right - b.right) < 0.01f &&
         std::fabs(a.bottom - b.bottom) < 0.01f;
}

draw::Color FromArgb(uint32_t argb) { return draw::Rgb(argb & 0xFFFFFF); }

// The first relationship of a type ("drawing", "comments"...) from a part, as a part name.
std::string Target(const pkg::Package& package, std::string_view source, std::string_view type) {
  const pkg::RelationshipSet* rels = package.RelationshipsOf(source);
  for (size_t i = 0; rels && i < rels->items.size(); ++i) {
    const pkg::Relationship& r = rels->items[i];
    const std::string_view t = r.type;
    if (!r.external && t.size() > type.size() && t.ends_with(type) && t[t.size() - type.size() - 1] == '/' &&
        package.FindPart(r.targetPart))
      return r.targetPart;
  }
  return {};
}

bool ParseInto(const pkg::Package& package, const std::string& part, pkg::XmlDom& dom) {
  std::vector<uint8_t> bytes;
  if (part.empty() || !package.ReadPart(part, bytes).Ok()) return false;
  return dom.Parse(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), package.GetLimits(),
                   part)
      .Ok();
}

}  // namespace

// ---- The objects

struct SheetObjects::Impl {
  struct Item {
    Anchor anchor;
    pugi::xml_node node;  // the shape element: xdr:sp, pic, grpSp, cxnSp, graphicFrame
    std::unique_ptr<draw::Drawing> drawing;
    std::unique_ptr<chart::Chart> chart;
    std::shared_ptr<const draw::Art> art;  // the chart laid out at `rect`'s size
    std::vector<std::vector<RichParagraph>> texts;  // the text boxes' text, read when first drawn
    draw::Rect rect;
    bool prints = true;
    std::string name;
  };
  struct Note {
    std::vector<RichParagraph> text;
    std::unique_ptr<draw::Drawing> box;  // the note's VML shape: fill, outline, shadow, insets
    draw::Rect styleRect;                // where its style puts it
    bool anchored = false;
    Anchor anchor;     // x:Anchor
    draw::Rect shown;  // a shown note's box, placed
    draw::Rect cell;   // its cell (merged area), placed
  };

  Workbook* book = nullptr;
  size_t sheet = 0;
  bool chartSheet = false;
  std::shared_ptr<const draw::Theme> theme;
  draw::ColorMap colorMap;
  std::shared_ptr<draw::ImageCache> images = std::make_shared<draw::ImageCache>();
  std::unique_ptr<objects::WorkbookData> data;
  std::wstring chartFont;

  pkg::XmlDom drawingDom;
  std::unique_ptr<draw::PackageResources> resources;
  std::vector<Item> items;

  pkg::XmlDom vmlDom;
  std::unique_ptr<draw::PackageResources> vmlResources;
  std::vector<Note> notes;  // by SheetObjects::notes_
  std::unordered_map<uint64_t, size_t> noteAt;

  std::unique_ptr<text::Fonts> fonts;
  text::Fonts& Fonts() {
    if (!fonts) fonts = std::make_unique<text::Fonts>();
    return *fonts;
  }
  draw::ColorContext Colors() const { return {theme.get(), &colorMap, std::nullopt}; }

  void ReadDrawing(const pkg::Package& package, const std::string& part);
  void Fit(Item& it);
  void DrawChart(draw::Painter& painter, Item& it, const draw::Rect& rect);
  void DrawText(draw::Painter& painter, Item& it, const draw::Rect& at);
  void DrawTextIn(draw::Painter& painter, const std::vector<RichParagraph>& text, const draw::Rect& box,
                  draw::TextBox::Anchor anchor);
};

void SheetObjects::Impl::ReadDrawing(const pkg::Package& package, const std::string& part) {
  if (!ParseInto(package, part, drawingDom)) return;
  resources = std::make_unique<draw::PackageResources>(package, part, theme, colorMap, images);
  const draw::ColorContext colors = Colors();
  ForEachChild(drawingDom.Root(), [&](pugi::xml_node a) {
    if (NamespaceOf(a) != kXdr) return true;
    const std::string_view k = LocalNameOf(a);
    Item it;
    if (k == "twoCellAnchor") {
      it.anchor.kind = AnchorKind::TwoCell;
      it.anchor.from = ReadMarker(Child(a, kXdr, "from"));
      it.anchor.to = ReadMarker(Child(a, kXdr, "to"));
    } else if (k == "oneCellAnchor" || k == "absoluteAnchor") {
      it.anchor.kind = k == "oneCellAnchor" ? AnchorKind::OneCell : AnchorKind::Absolute;
      it.anchor.from = ReadMarker(Child(a, kXdr, "from"));
      pugi::xml_node pos = Child(a, kXdr, "pos"), ext = Child(a, kXdr, "ext");
      it.anchor.pos = {Emu(pos, "x"), Emu(pos, "y")};
      it.anchor.ext = {std::min(Emu(ext, "cx"), 1e6f), std::min(Emu(ext, "cy"), 1e6f)};
    } else {
      return true;
    }
    ForEachChild(a, [&](pugi::xml_node c) {
      const std::string_view s = LocalNameOf(c);
      if (NamespaceOf(c) != kXdr) return true;
      if (s == "sp" || s == "pic" || s == "grpSp" || s == "cxnSp" || s == "graphicFrame") it.node = c;
      else if (s == "clientData") it.prints = Bool(c, "fPrintsWithSheet", true);
      return true;
    });
    if (!it.node) return true;
    if (LocalNameOf(it.node) == "graphicFrame") {
      it.name = Child(Child(it.node, kXdr, "nvGraphicFramePr"), kXdr, "cNvPr").attribute("name").value();
      pugi::xml_node c = Child(A(A(it.node, "graphic"), "graphicData"), kChart, "chart");
      if (pugi::xml_node space = c ? resources->GetPartXml(RelId(c, "id")) : pugi::xml_node()) {
        auto ch = std::make_unique<chart::Chart>();
        if (chart::Read(space, colors, *ch, data.get())) it.chart = std::move(ch);
      }
    }
    if (!it.chart) {
      it.drawing = draw::Drawing::FromShapeTree(it.node, *resources);
      if (!it.drawing) return true;
      it.name = it.drawing->Name();
    }
    items.push_back(std::move(it));
    return items.size() < kMaxObjects;
  });
}

void SheetObjects::Impl::Fit(Item& it) {
  if (!it.drawing || Empty(it.rect) || Near(it.drawing->Bounds(), it.rect)) return;
  SetXfrm(it.node, it.rect);
  if (auto d = draw::Drawing::FromShapeTree(it.node, *resources)) it.drawing = std::move(d);
}

void SheetObjects::Impl::DrawChart(draw::Painter& painter, Item& it, const draw::Rect& rect) {
  const draw::Size size{rect.Width(), rect.Height()};
  if (!it.art || std::fabs(it.art->size.width - size.width) > 0.01f || std::fabs(it.art->size.height - size.height) > 0.01f)
    it.art = std::make_shared<draw::Art>(chart::Draw(*it.chart, size, Colors(), chartFont));
  draw::Shape s;
  s.bounds = rect;
  s.art = it.art;
  draw::DrawShape(painter, s);
}

void SheetObjects::Impl::DrawTextIn(draw::Painter& painter, const std::vector<RichParagraph>& text,
                                    const draw::Rect& box, draw::TextBox::Anchor anchor) {
  if (text.empty() || Empty(box)) return;
  float y = box.top;
  if (anchor == draw::TextBox::Anchor::Middle || anchor == draw::TextBox::Anchor::Bottom) {
    const float h = objects::LayoutText(Fonts(), text, box.Width(), nullptr, {});
    y += anchor == draw::TextBox::Anchor::Middle ? (box.Height() - h) / 2 : box.Height() - h;
  }
  objects::LayoutText(Fonts(), text, box.Width(), &painter, {box.left, y});
}

void SheetObjects::Impl::DrawText(draw::Painter& painter, Item& it, const draw::Rect& at) {
  const std::vector<draw::TextBox>& boxes = it.drawing->TextBoxes();
  if (boxes.empty()) return;
  const draw::ColorContext colors = Colors();
  if (it.texts.size() != boxes.size()) {
    it.texts.clear();
    for (const draw::TextBox& b : boxes) {
      // The text's color and font: the shape's style (xdr:style/a:fontRef), else black.
      pugi::xml_node fontRef = A(Child(b.content.parent(), kXdr, "style"), "fontRef");
      const draw::Color color = draw::ReadColor(fontRef, colors).value_or(draw::Color{0, 0, 0, 1});
      it.texts.push_back(objects::ReadShapeText(b.content, colors, color, 11));
    }
  }
  for (size_t i = 0; i < boxes.size(); ++i) {
    // The boxes are relative to the drawing's top left; Fit made the drawing the anchor's size.
    painter.Save();
    painter.Transform(boxes[i].transform * draw::Matrix::Translate(at.left, at.top));
    DrawTextIn(painter, it.texts[i], boxes[i].rect, boxes[i].anchor);
    painter.Restore();
  }
}

SheetObjects::SheetObjects() : impl_(std::make_unique<Impl>()) {}
SheetObjects::~SheetObjects() = default;

std::unique_ptr<SheetObjects> SheetObjects::Load(Workbook& book, size_t index) {
  std::unique_ptr<SheetObjects> o(new SheetObjects());
  Impl& m = *o->impl_;
  m.book = &book;
  m.sheet = index;
  m.theme = book.Theme() ? book.Theme() : draw::Theme::Default();
  m.chartFont = Utf8ToWide(m.theme->MinorFont().latin);
  if (m.chartFont.empty()) m.chartFont = L"Calibri";
  m.data = std::make_unique<objects::WorkbookData>(book, static_cast<int>(index));
  const pkg::Package* package = book.GetPackage();
  if (index >= book.SheetCount() || !package) return o;
  book.LoadSheet(index);
  const Sheet& sheet = book.GetSheet(index);
  if (sheet.PartName().empty()) return o;
  m.chartSheet = sheet.Kind() == SheetKind::Chartsheet;
  m.ReadDrawing(*package, Target(*package, sheet.PartName(), "drawing"));
  if (!m.chartSheet) o->ReadNotes();
  return o;
}

void SheetObjects::Place(const SheetLayout& layout) {
  Impl& m = *impl_;
  for (Impl::Item& it : m.items) {
    it.rect = sheet::Place(it.anchor, layout);
    m.Fit(it);
  }
  PlaceNotes(layout);
}

void SheetObjects::Draw(draw::Painter& painter, const draw::Rect& visible, const DrawOptions& options) {
  Impl& m = *impl_;
  if (m.chartSheet) return;
  for (Impl::Item& it : m.items) {
    if ((options.printing && !it.prints) || Empty(it.rect)) continue;
    const draw::Rect rect = Mirror(it.rect, options.mirrorX);
    // Room for outlines and shadows past the box.
    const draw::Rect reach{rect.left - 20, rect.top - 20, rect.right + 20, rect.bottom + 20};
    if (!Meets(reach, visible)) continue;
    if (it.chart) {
      m.DrawChart(painter, it, rect);
    } else if (it.drawing && !it.drawing->IsHidden()) {
      it.drawing->Draw(painter, rect);
      m.DrawText(painter, it, rect);
    }
  }
  DrawNotes(painter, visible, options);
}

std::vector<SheetObjects::Object> SheetObjects::Objects() const {
  std::vector<Object> out;
  for (const Impl::Item& it : impl_->items)
    if (!Empty(it.rect) && !(it.drawing && it.drawing->IsHidden())) out.push_back({it.name, it.rect, it.chart != nullptr, it.prints});
  return out;
}

bool SheetObjects::IsChartSheet() const { return impl_->chartSheet; }

draw::Size SheetObjects::ChartSheetSize() const {
  for (const Impl::Item& it : impl_->items)
    if (it.chart || it.drawing) return it.anchor.kind == AnchorKind::Absolute ? it.anchor.ext : draw::Size{};
  return {};
}

void SheetObjects::DrawChartSheet(draw::Painter& painter, const draw::Rect& rect) {
  Impl& m = *impl_;
  for (Impl::Item& it : m.items) {
    if (it.chart) {
      m.DrawChart(painter, it, rect);
      return;
    }
    if (it.drawing) {
      it.drawing->Draw(painter, rect);
      return;
    }
  }
}

// ---- Notes and threaded comments

namespace {

uint64_t Key(uint32_t row, uint32_t col) { return (static_cast<uint64_t>(row) << 32) | col; }

constexpr size_t kMaxNoteChars = 32767;

// Text split into paragraphs at its line breaks, each part in `look`.
void AddText(std::vector<RichParagraph>& out, std::wstring_view s, const RichSpan& look, size_t& chars) {
  if (out.empty()) out.emplace_back().mark = look.style;
  for (size_t start = 0;;) {
    const size_t nl = s.find(L'\n', start);
    std::wstring_view part = s.substr(start, nl == std::wstring_view::npos ? std::wstring_view::npos : nl - start);
    if (!part.empty() && part.back() == L'\r') part.remove_suffix(1);
    if (!part.empty() && chars < kMaxNoteChars) {
      RichSpan span = look;
      span.text = part.substr(0, kMaxNoteChars - chars);
      chars += span.text.size();
      out.back().spans.push_back(std::move(span));
    }
    if (nl == std::wstring_view::npos) break;
    out.emplace_back().mark = look.style;
    start = nl + 1;
  }
}

RichSpan NoteLook() {
  RichSpan s;
  s.style.family = L"Tahoma";  // Excel's notes
  s.style.size = 9;
  s.color = draw::Color{0, 0, 0, 1};
  return s;
}

// A comment's <text>: plain, or runs with their fonts.
std::vector<RichParagraph> NoteText(pugi::xml_node text, const Styles& styles, std::wstring& plain) {
  std::vector<RichParagraph> out;
  size_t chars = 0;
  const RichSpan base = NoteLook();
  for (pugi::xml_node c = text.first_child(); c; c = c.next_sibling()) {
    const std::string_view k = xlsx::Local(c);
    RichSpan s = base;
    if (k == "r") {
      if (pugi::xml_node rPr = xlsx::Child(c, "rPr")) {
        const Font f = xlsx::ReadFont(rPr);
        if (!f.name.empty()) s.style.family = Utf8ToWide(f.name);
        if (f.size > 0) s.style.size = static_cast<float>(std::clamp(f.size, 1.0, 409.0));
        s.style.bold = f.bold;
        s.style.italic = f.italic;
        s.underline = f.underline != Underline::None;
        if (f.color.IsSet()) s.color = FromArgb(styles.Resolve(f.color, 0xFF000000u));
      }
    } else if (k != "t") {
      continue;
    }
    const std::wstring t = Utf8ToWide(k == "t" ? c.text().get() : xlsx::Child(c, "t").text().get());
    plain += t;
    AddText(out, t, s, chars);
  }
  return out;
}

// A note's VML shape: its row and column, whether it's shown, and where (x:Anchor: columns and
// rows with offsets in pixels).
struct VmlNote {
  pugi::xml_node shape;
  uint32_t row = 0, col = 0;
  bool shown = false, anchored = false;
  Anchor anchor;
};

bool ReadVmlNote(pugi::xml_node shape, VmlNote& out) {
  pugi::xml_node data = Child(shape, kExcel, "ClientData");
  if (std::string_view(data.attribute("ObjectType").value()) != "Note") return false;
  out.shape = shape;
  out.row = Index(Child(data, kExcel, "Row"), kMaxRows);
  out.col = Index(Child(data, kExcel, "Column"), kMaxCols);
  out.shown = static_cast<bool>(Child(data, kExcel, "Visible"));
  std::string_view a = Child(data, kExcel, "Anchor").text().get();
  double v[8];
  int n = 0;
  while (n < 8 && !a.empty()) {
    const size_t comma = a.find(',');
    std::string_view part = a.substr(0, comma);
    while (!part.empty() && std::isspace(static_cast<unsigned char>(part.front()))) part.remove_prefix(1);
    while (!part.empty() && std::isspace(static_cast<unsigned char>(part.back()))) part.remove_suffix(1);
    v[n++] = std::clamp(Number(part, 0), 0.0, 1e7);
    a = comma == std::string_view::npos ? std::string_view() : a.substr(comma + 1);
  }
  if (n == 8) {
    out.anchored = true;
    out.anchor.kind = AnchorKind::TwoCell;
    out.anchor.from = {static_cast<uint32_t>(std::min(v[0], double(kMaxCols - 1))), static_cast<uint32_t>(std::min(v[2], double(kMaxRows - 1))),
                       static_cast<float>(v[1]) * kPx, static_cast<float>(v[3]) * kPx};
    out.anchor.to = {static_cast<uint32_t>(std::min(v[4], double(kMaxCols - 1))), static_cast<uint32_t>(std::min(v[6], double(kMaxRows - 1))),
                     static_cast<float>(v[5]) * kPx, static_cast<float>(v[7]) * kPx};
  }
  return true;
}

// Excel's note colors name its palette's tooltip entries: "infoBackground [80]" for the fill,
// "none [81]" or "infoText [81]" for the outline and shadow. As plain colors for fwdraw.
void PlainColor(pugi::xml_attribute a) {
  const std::string_view v = a.value();
  if (v.ends_with("[80]")) a.set_value("#ffffe1");
  else if (v.ends_with("[81]")) a.set_value("black");
}

// Shown or not, a note draws: its style's visibility:hidden goes (in this copy of the part), and
// its colors are made plain.
void Prepare(pugi::xml_node shape) {
  pugi::xml_attribute style = shape.attribute("style");
  std::string s = style.value();
  for (size_t at; (at = s.find("visibility:hidden")) != std::string::npos;) s.replace(at, 17, "visibility:visible");
  style.set_value(s.c_str());
  PlainColor(shape.attribute("fillcolor"));
  PlainColor(shape.attribute("strokecolor"));
  for (pugi::xml_node c = shape.first_child(); c; c = c.next_sibling())
    if (NamespaceOf(c) == kV) {
      PlainColor(c.attribute("color"));
      PlainColor(c.attribute("color2"));
    }
}

// A note's corner mark: the cell's top right corner, or top left on a right-to-left sheet.
void TriangleAt(draw::Painter& painter, const draw::Rect& cell, draw::Color color, bool rtl) {
  const float s = std::min({4.5f, cell.Width(), cell.Height()});
  if (!(s > 0)) return;
  const float x = rtl ? cell.left : cell.right, in = rtl ? s : -s;
  draw::Path p;
  p.MoveTo({x + in, cell.top});
  p.LineTo({x, cell.top});
  p.LineTo({x, cell.top + s});
  p.Close();
  painter.FillPath(p, draw::Brush::Solid(color));
}

const draw::Color kNoteRed{1, 0, 0, 1};
const draw::Color kThreadPurple{0.44f, 0.19f, 0.63f, 1};

}  // namespace

void SheetObjects::ReadNotes() {
  Impl& m = *impl_;
  const pkg::Package& package = *m.book->GetPackage();
  const Sheet& sheet = m.book->GetSheet(m.sheet);
  const Styles& styles = m.book->GetStyles();
  auto add = [&](CellNote n, Impl::Note d) {
    if (notes_.size() >= kMaxNotes || !m.noteAt.emplace(Key(n.row, n.col), notes_.size()).second) return false;
    notes_.push_back(std::move(n));
    m.notes.push_back(std::move(d));
    return true;
  };

  // Threaded comments first: a cell's thread wins over the note Excel writes for older versions.
  std::unordered_map<std::string, std::wstring> people;
  pkg::XmlDom dom;
  if (ParseInto(package, Target(package, m.book->WorkbookPart(), "person"), dom))
    for (pugi::xml_node p = dom.Root().first_child(); p && people.size() < kMaxNotes; p = p.next_sibling())
      if (xlsx::Local(p) == "person") people[p.attribute("id").value()] = Utf8ToWide(p.attribute("displayName").value());
  if (ParseInto(package, Target(package, sheet.PartName(), "threadedComment"), dom)) {
    std::unordered_map<std::string, size_t> threads;  // a thread's first comment's id -> note
    RichSpan name = NoteLook(), body = NoteLook();
    name.style.family = body.style.family = L"Segoe UI";
    name.style.bold = true;
    for (pugi::xml_node c = dom.Root().first_child(); c; c = c.next_sibling()) {
      if (xlsx::Local(c) != "threadedComment" || NamespaceOf(c) != kThreads) continue;
      auto who = people.find(c.attribute("personId").value());
      const std::wstring person = who == people.end() ? std::wstring() : who->second;
      const std::wstring text = Utf8ToWide(xlsx::Child(c, "text").text().get());
      const std::string_view parent = c.attribute("parentId").value();
      size_t at;
      if (parent.empty()) {
        CellAddr a;
        if (!ParseA1(c.attribute("ref").value(), a)) continue;
        CellNote n;
        n.row = a.row, n.col = a.col, n.threaded = true, n.author = person;
        n.resolved = Bool(c, "done", false);
        if (!add(std::move(n), Impl::Note{})) continue;
        at = notes_.size() - 1;
        threads[c.attribute("id").value()] = at;
      } else if (auto t = threads.find(std::string(parent)); t != threads.end()) {
        at = t->second;
      } else {
        continue;
      }
      CellNote& n = notes_[at];
      std::vector<RichParagraph>& p = m.notes[at].text;
      size_t chars = 0;
      for (const RichParagraph& q : p)
        for (const RichSpan& s : q.spans) chars += s.text.size();
      if (!n.text.empty()) n.text += L"\n";
      n.text += (person.empty() ? text : person + L": " + text);
      if (!p.empty()) p.emplace_back().mark = body.style;  // a blank line between comments
      p.emplace_back().mark = name.style;
      AddText(p, person, name, chars);
      p.emplace_back().mark = body.style;
      AddText(p, text, body, chars);
    }
  }

  // Notes, and their boxes from the VML drawing.
  std::unordered_map<uint64_t, VmlNote> boxes;
  std::string vml = sheet.LegacyDrawingRelId().empty() ? Target(package, sheet.PartName(), "vmlDrawing")
                                                        : m.book->ResolveRel(sheet.PartName(), sheet.LegacyDrawingRelId());
  if (ParseInto(package, vml, m.vmlDom)) {
    m.vmlResources = std::make_unique<draw::PackageResources>(package, vml, m.theme, m.colorMap, m.images);
    for (pugi::xml_node s = m.vmlDom.Root().first_child(); s; s = s.next_sibling()) {
      VmlNote v;
      if (NamespaceOf(s) == kV && ReadVmlNote(s, v)) boxes.emplace(Key(v.row, v.col), v);
    }
  }
  if (ParseInto(package, Target(package, sheet.PartName(), "comments"), dom)) {
    std::vector<std::wstring> authors;
    for (pugi::xml_node a = xlsx::Child(dom.Root(), "authors").first_child(); a && authors.size() < kMaxNotes;
         a = a.next_sibling())
      if (xlsx::Local(a) == "author") authors.push_back(Utf8ToWide(a.text().get()));
    for (pugi::xml_node c = xlsx::Child(dom.Root(), "commentList").first_child(); c; c = c.next_sibling()) {
      CellAddr a;
      if (xlsx::Local(c) != "comment" || !ParseA1(c.attribute("ref").value(), a)) continue;
      CellNote n;
      n.row = a.row, n.col = a.col;
      const size_t author = static_cast<size_t>(Number(c.attribute("authorId").value(), -1));
      if (author < authors.size()) n.author = authors[author];
      Impl::Note d;
      d.text = NoteText(xlsx::Child(c, "text"), styles, n.text);
      // A cell with a thread: this is the note Excel writes for older versions, kept for printing.
      if (auto t = m.noteAt.find(Key(a.row, a.col)); t != m.noteAt.end()) {
        if (notes_[t->second].threaded) notes_[t->second].legacyText = n.text;
        continue;
      }
      if (auto b = boxes.find(Key(a.row, a.col)); b != boxes.end()) {
        n.shown = b->second.shown;
        d.anchored = b->second.anchored;
        d.anchor = b->second.anchor;
        Prepare(b->second.shape);
        d.box = draw::Drawing::FromVml(b->second.shape, *m.vmlResources);
        if (d.box) d.styleRect = d.box->Bounds();
        if (d.box) n.width = d.styleRect.Width(), n.height = d.styleRect.Height();
      }
      if (!add(std::move(n), std::move(d)) && notes_.size() >= kMaxNotes) break;
    }
  }
}

void SheetObjects::PlaceNotes(const SheetLayout& layout) {
  Impl& m = *impl_;
  const Sheet& sheet = m.book->GetSheet(m.sheet);
  for (size_t i = 0; i < notes_.size(); ++i) {
    const CellNote& n = notes_[i];
    Impl::Note& d = m.notes[i];
    Range r = Range::Cell(n.row, n.col);
    if (const Range* merged = sheet.MergeAt(n.row, n.col)) r = *merged;
    d.cell = {static_cast<float>(layout.ColumnLeft(r.firstCol)), static_cast<float>(layout.RowTop(r.firstRow)),
              static_cast<float>(layout.ColumnLeft(r.lastCol + 1)), static_cast<float>(layout.RowTop(r.lastRow + 1))};
    d.shown = {};
    if (n.shown) d.shown = d.anchored ? sheet::Place(d.anchor, layout) : d.styleRect;
  }
}

void SheetObjects::DrawNotes(draw::Painter& painter, const draw::Rect& visible, const DrawOptions& options) {
  Impl& m = *impl_;
  // Excel prints notes as displayed only when the page setup says so.
  const bool shownNotes =
      !options.printing || m.book->GetSheet(m.sheet).Page().cellComments == CellComments::AsDisplayed;
  for (size_t i = 0; i < notes_.size() && shownNotes; ++i) {
    const Impl::Note& d = m.notes[i];
    const bool rtl = options.mirrorX > 0;
    const draw::Rect shown = Mirror(d.shown, options.mirrorX), cell = Mirror(d.cell, options.mirrorX);
    if (Empty(shown) || !Meets(shown, visible)) continue;
    // A shown note: a line from its cell's corner to the box, then the box.
    if (!Empty(cell)) {
      draw::Path line;
      line.MoveTo({rtl ? cell.left : cell.right, cell.top});
      line.LineTo({rtl ? shown.right : shown.left, shown.top});
      draw::Pen pen;
      pen.brush = draw::Brush::Solid({0, 0, 0, 1});
      pen.width = 0.75f;
      painter.StrokePath(line, pen);
    }
    DrawNote(painter, notes_[i], shown);
  }
  if (options.printing || !options.indicators) return;
  for (size_t i = 0; i < notes_.size(); ++i) {
    const draw::Rect cell = Mirror(m.notes[i].cell, options.mirrorX);
    if (!Empty(cell) && Meets(cell, visible))
      TriangleAt(painter, cell, notes_[i].threaded ? kThreadPurple : kNoteRed, options.mirrorX > 0);
  }
}

const CellNote* SheetObjects::NoteAt(uint32_t row, uint32_t col) const {
  const Impl& m = *impl_;
  auto it = m.noteAt.find(Key(row, col));
  if (it == m.noteAt.end())
    if (const Range* merged = m.book->GetSheet(m.sheet).MergeAt(row, col)) it = m.noteAt.find(Key(merged->firstRow, merged->firstCol));
  return it == m.noteAt.end() ? nullptr : &notes_[it->second];
}

namespace {
constexpr float kCardWidth = 216, kCardPadding = 6;
}

draw::Size SheetObjects::NoteSize(const CellNote& note) {
  Impl& m = *impl_;
  const size_t i = static_cast<size_t>(&note - notes_.data());
  if (i >= notes_.size()) return {};
  if (note.threaded) {
    const float h = objects::LayoutText(m.Fonts(), m.notes[i].text, kCardWidth - 2 * kCardPadding, nullptr, {});
    return {kCardWidth, std::min(h + 2 * kCardPadding, 600.f)};
  }
  const draw::Rect& r = m.notes[i].styleRect;
  return Empty(r) ? draw::Size{108, 59.25f} : draw::Size{r.Width(), r.Height()};  // Excel's default: 144 x 79 px
}

void SheetObjects::DrawNote(draw::Painter& painter, const CellNote& note, const draw::Rect& box) {
  Impl& m = *impl_;
  const size_t i = static_cast<size_t>(&note - notes_.data());
  if (i >= notes_.size() || Empty(box)) return;
  const Impl::Note& d = m.notes[i];
  draw::Rect text = box;
  if (note.threaded) {
    painter.FillRect(box, draw::Brush::Solid({1, 1, 1, 1}));
    draw::Pen pen;
    pen.brush = draw::Brush::Solid(draw::Rgb(0xC8C6C4));
    pen.width = 0.75f;
    painter.StrokePath(draw::Path::FromRect(box), pen);
    text = {box.left + kCardPadding, box.top + kCardPadding, box.right - kCardPadding, box.bottom - kCardPadding};
  } else if (d.box) {
    d.box->Draw(painter, box);
    // Excel keeps a note's text at least 1.5 points from its edges (o:insetmode auto).
    draw::Rect in{1.5f, 1.5f, 1.5f, 1.5f};
    if (!d.box->TextBoxes().empty()) {
      const draw::Rect& own = d.box->TextBoxes().front().insets;
      in = {std::max(own.left, in.left), std::max(own.top, in.top), std::max(own.right, in.right),
            std::max(own.bottom, in.bottom)};
    }
    text = {box.left + in.left, box.top + in.top, box.right - in.right, box.bottom - in.bottom};
  } else {
    // Excel's look for a note without a shape of its own.
    painter.FillRect(box, draw::Brush::Solid(draw::Rgb(0xFFFFE1)));
    draw::Pen pen;
    pen.brush = draw::Brush::Solid({0, 0, 0, 1});
    pen.width = 0.75f;
    painter.StrokePath(draw::Path::FromRect(box), pen);
    text = {box.left + 1.5f, box.top + 1.5f, box.right - 1.5f, box.bottom - 1.5f};
  }
  painter.Save();
  painter.ClipRect(box);
  m.DrawTextIn(painter, d.text, text, draw::TextBox::Anchor::Top);
  painter.Restore();
}

}  // namespace fw::sheet
