#pragma once

#include <pugixml.hpp>

#include <charconv>
#include <cstdint>
#include <string_view>

#include "pkg/Mce.h"
#include "pkg/XmlDom.h"

// Small helpers the DrawingML, VML and theme readers share. Internal to fwdraw.
namespace draw::xml {

inline constexpr std::string_view kA = "http://schemas.openxmlformats.org/drawingml/2006/main";
inline constexpr std::string_view kPic = "http://schemas.openxmlformats.org/drawingml/2006/picture";
inline constexpr std::string_view kWp = "http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing";
inline constexpr std::string_view kWps = "http://schemas.microsoft.com/office/word/2010/wordprocessingShape";
inline constexpr std::string_view kWpg = "http://schemas.microsoft.com/office/word/2010/wordprocessingGroup";
inline constexpr std::string_view kWpc = "http://schemas.microsoft.com/office/word/2010/wordprocessingCanvas";
inline constexpr std::string_view kWp14 = "http://schemas.microsoft.com/office/word/2010/wordprocessingDrawing";
inline constexpr std::string_view kP = "http://schemas.openxmlformats.org/presentationml/2006/main";
inline constexpr std::string_view kXdr = "http://schemas.openxmlformats.org/drawingml/2006/spreadsheetDrawing";
inline constexpr std::string_view kDsp = "http://schemas.microsoft.com/office/drawing/2008/diagram";
inline constexpr std::string_view kDgm = "http://schemas.openxmlformats.org/drawingml/2006/diagram";
inline constexpr std::string_view kChart = "http://schemas.openxmlformats.org/drawingml/2006/chart";
inline constexpr std::string_view kR = "http://schemas.openxmlformats.org/officeDocument/2006/relationships";
inline constexpr std::string_view kW = "http://schemas.openxmlformats.org/wordprocessingml/2006/main";
inline constexpr std::string_view kV = "urn:schemas-microsoft-com:vml";
inline constexpr std::string_view kO = "urn:schemas-microsoft-com:office:office";
inline constexpr std::string_view kW10 = "urn:schemas-microsoft-com:office:word";
inline constexpr std::string_view kA14 = "http://schemas.microsoft.com/office/drawing/2010/main";
inline constexpr std::string_view kAsvg = "http://schemas.microsoft.com/office/drawing/2016/SVG/main";

using fw::pkg::FindAttribute;
using fw::pkg::IsElement;
using fw::pkg::LocalNameOf;
using fw::pkg::NamespaceOf;

// The namespaces drawings may require in mc:AlternateContent that fwdraw reads.
inline const fw::pkg::mce::Understood& Understood() {
  static const fw::pkg::mce::Understood u{kA, kPic, kWp, kWps, kWpg, kWpc, kWp14, kV, kO, kW10, kA14, kW};
  return u;
}

// Child elements in order, with mc:AlternateContent replaced by the children of its chosen
// branch. `f` returns false to stop.
template <class F>
bool ForEachChild(pugi::xml_node parent, F&& f, int depth = 0) {
  for (pugi::xml_node c = parent.first_child(); c; c = c.next_sibling()) {
    if (c.type() != pugi::node_element) continue;
    if (IsElement(c, fw::pkg::mce::kNamespace, "AlternateContent")) {
      if (depth < 8) {
        pugi::xml_node branch = fw::pkg::mce::SelectBranch(c, Understood());
        if (branch && !ForEachChild(branch, f, depth + 1)) return false;
      }
      continue;
    }
    if (!f(c)) return false;
  }
  return true;
}

// The first child element (ns, local), looking inside mc:AlternateContent; null when there's none.
inline pugi::xml_node Child(pugi::xml_node parent, std::string_view ns, std::string_view local) {
  pugi::xml_node found;
  ForEachChild(parent, [&](pugi::xml_node c) {
    if (LocalNameOf(c) == local && NamespaceOf(c) == ns) found = c;
    return !found;
  });
  return found;
}

// A child in the DrawingML namespace.
inline pugi::xml_node A(pugi::xml_node parent, std::string_view local) { return Child(parent, kA, local); }

inline bool Is(pugi::xml_node n, std::string_view ns, std::string_view local) { return IsElement(n, ns, local); }

// Numbers as OOXML writes them; `def` when missing or not a number.
inline double Number(std::string_view v, double def) {
  while (!v.empty() && (v.front() == ' ' || v.front() == '+')) v.remove_prefix(1);
  double d = 0;
  auto [end, ec] = std::from_chars(v.data(), v.data() + v.size(), d);
  if (ec != std::errc() || !(d == d) || d > 1e15 || d < -1e15) return def;
  return d;
}
inline double Number(pugi::xml_node n, const char* attr, double def) {
  pugi::xml_attribute a = n.attribute(attr);
  return a ? Number(a.value(), def) : def;
}
// xsd:boolean, and VML's t/f, true/false and on/off.
inline bool Bool(std::string_view v, bool def) {
  if (v == "1" || v == "true" || v == "t" || v == "on") return true;
  if (v == "0" || v == "false" || v == "f" || v == "off") return false;
  return def;
}
inline bool Bool(pugi::xml_node n, const char* attr, bool def) {
  pugi::xml_attribute a = n.attribute(attr);
  return a ? Bool(a.value(), def) : def;
}

// An r:id-style attribute (relationships namespace).
inline std::string_view RelId(pugi::xml_node n, std::string_view local) {
  return FindAttribute(n, kR, local).value();
}

constexpr float kEmuPerPoint = 12700.f;
inline float EmuToPt(double emu) { return static_cast<float>(emu / kEmuPerPoint); }

}  // namespace draw::xml
