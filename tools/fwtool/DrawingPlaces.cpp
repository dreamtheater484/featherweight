#include "DrawingPlaces.h"

#include <utility>
#include <vector>

#include "pkg/Mce.h"
#include "pkg/XmlDom.h"

namespace fwtool::torture {

namespace {

constexpr std::string_view kW = "http://schemas.openxmlformats.org/wordprocessingml/2006/main";
constexpr std::string_view kWStrict = "http://purl.oclc.org/ooxml/wordprocessingml/main";
constexpr std::string_view kV = "urn:schemas-microsoft-com:vml";
constexpr std::string_view kWps = "http://schemas.microsoft.com/office/word/2010/wordprocessingShape";
constexpr std::string_view kWpg = "http://schemas.microsoft.com/office/word/2010/wordprocessingGroup";
constexpr std::string_view kWpc = "http://schemas.microsoft.com/office/word/2010/wordprocessingCanvas";

bool IsW(pugi::xml_node n, std::string_view local) {
  if (n.type() != pugi::node_element || fw::pkg::LocalNameOf(n) != local) return false;
  const std::string_view ns = fw::pkg::NamespaceOf(n);
  return ns == kW || ns == kWStrict;
}

pugi::xml_node ChildNamed(pugi::xml_node n, std::string_view local) {
  for (pugi::xml_node c = n.first_child(); c; c = c.next_sibling())
    if (c.type() == pugi::node_element && fw::pkg::LocalNameOf(c) == local) return c;
  return {};
}

// A w:drawing that Word counts as a drawing object: floating, or a shape, a group or a canvas.
bool DrawingObject(pugi::xml_node drawing) {
  for (pugi::xml_node frame = drawing.first_child(); frame; frame = frame.next_sibling()) {
    if (frame.type() != pugi::node_element) continue;
    if (fw::pkg::LocalNameOf(frame) == "anchor") return true;
    const std::string_view uri = ChildNamed(ChildNamed(frame, "graphic"), "graphicData").attribute("uri").value();
    if (uri == kWps || uri == kWpg || uri == kWpc) return true;
  }
  return false;
}

// "position:absolute" in a VML style, spaces allowed around the colon.
bool Absolute(std::string_view style) {
  for (size_t i = style.find("position"); i != std::string_view::npos; i = style.find("position", i + 1)) {
    size_t j = style.find_first_not_of(' ', i + 8);
    if (j == std::string_view::npos || style[j] != ':') continue;
    j = style.find_first_not_of(' ', j + 1);
    if (j != std::string_view::npos && style.substr(j).starts_with("absolute")) return true;
  }
  return false;
}

// A w:pict that Word counts as a drawing object: VML with a text box, or positioned absolutely.
// The text box's own text isn't looked into.
bool PictObject(pugi::xml_node pict) {
  std::vector<pugi::xml_node> stack(pict.begin(), pict.end());
  while (!stack.empty()) {
    const pugi::xml_node n = stack.back();
    stack.pop_back();
    if (n.type() != pugi::node_element) continue;
    if (fw::pkg::IsElement(n, kV, "textbox") || Absolute(n.attribute("style").value())) return true;
    if (IsW(n, "txbxContent")) continue;
    stack.insert(stack.end(), n.begin(), n.end());
  }
  return false;
}

std::map<std::string, int> Count(std::string_view part, std::string_view xml, const fw::pkg::Limits& limits) {
  std::map<std::string, int> out;
  // Only a part with a drawing and a place for one is read.
  auto has = [&](std::string_view s) { return xml.find(s) != std::string_view::npos; };
  if (!has("drawing") && !has("pict")) return out;
  if (!has("txbxContent") && !has("textbox") && !has("footnote") && !has("endnote") && !has("comment")) return out;
  fw::pkg::XmlDom dom;
  if (!dom.Parse(xml, limits, part).Ok()) return out;
  static const fw::pkg::mce::Understood understood{
      kW, kV, kWps, kWpg, kWpc, "http://schemas.openxmlformats.org/drawingml/2006/main",
      "http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing",
      "http://schemas.microsoft.com/office/word/2010/wordml", "http://schemas.microsoft.com/office/word/2010/wordprocessingDrawing",
      "http://schemas.microsoft.com/office/drawing/2010/main"};
  std::vector<std::pair<pugi::xml_node, const char*>> stack{{dom.Root(), nullptr}};
  while (!stack.empty()) {
    auto [n, place] = stack.back();
    stack.pop_back();
    if (n.type() != pugi::node_element) continue;
    pugi::xml_node children = n;
    if (fw::pkg::IsElement(n, fw::pkg::mce::kNamespace, "AlternateContent")) {
      children = fw::pkg::mce::SelectBranch(n, understood);  // the branch a reader takes
      if (!children) continue;
    } else if (IsW(n, "footnote")) {
      place = "a footnote";
    } else if (IsW(n, "endnote")) {
      place = "an endnote";
    } else if (IsW(n, "comment")) {
      place = "a comment";
    } else if (IsW(n, "txbxContent") || fw::pkg::IsElement(n, kV, "textbox")) {
      place = "a text box";
    } else if (place && ((IsW(n, "drawing") && DrawingObject(n)) || (IsW(n, "pict") && PictObject(n)))) {
      ++out["a shape or floating drawing in " + std::string(place) + " (" + std::string(part) + ")"];
    }
    for (pugi::xml_node c = children.first_child(); c; c = c.next_sibling()) stack.push_back({c, place});
  }
  return out;
}

}  // namespace

std::map<std::string, int> MisplacedDrawings(std::string_view part, std::string_view xml) {
  return Count(part, xml, fw::pkg::Limits::Default());
}

std::map<std::string, int> MisplacedDrawings(const fw::pkg::Package& pkg) {
  std::map<std::string, int> out;
  std::vector<uint8_t> bytes;
  for (const fw::pkg::Part& p : pkg.Parts()) {
    if (p.deleted || !p.name.starts_with("/word/") || !p.name.ends_with(".xml") || !pkg.ReadPart(p.name, bytes).Ok()) continue;
    const std::string_view xml(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    for (const auto& [problem, count] : Count(p.name, xml, pkg.GetLimits())) out[problem] += count;
  }
  return out;
}

}  // namespace fwtool::torture
