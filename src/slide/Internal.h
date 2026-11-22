#pragma once

#include <pugixml.hpp>

#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "draw/Xml.h"
#include "pkg/Package.h"
#include "pkg/XmlDom.h"
#include "slide/Presentation.h"

namespace fw::slide::edit {
struct Changes;
}

// What the reader's files share. Internal to fwslide.
namespace fw::slide::detail {

using namespace draw::xml;

inline constexpr std::string_view kP14 = "http://schemas.microsoft.com/office/powerpoint/2010/main";
inline constexpr std::string_view kTableUri = "http://schemas.openxmlformats.org/drawingml/2006/table";
inline constexpr std::string_view kChartUri = "http://schemas.openxmlformats.org/drawingml/2006/chart";
inline constexpr std::string_view kDiagramUri = "http://schemas.openxmlformats.org/drawingml/2006/diagram";
inline constexpr std::string_view kOleUri = "http://schemas.openxmlformats.org/presentationml/2006/ole";

// A child in the PresentationML namespace (through mc:AlternateContent, as fwdraw chooses).
inline pugi::xml_node P(pugi::xml_node parent, std::string_view local) { return Child(parent, kP, local); }

// The first internal target of a relationship whose type ends in "/kind" ("slideLayout", "theme").
std::string TargetOfType(const pkg::RelationshipSet* rels, std::string_view kind);
// The internal target of a relationship id, or empty.
std::string TargetOf(const pkg::RelationshipSet* rels, std::string_view id);
// A part parsed within the package's limits; null when it's missing or not well-formed (the reason
// in `problem`).
std::unique_ptr<pkg::XmlDom> ParsePart(const pkg::Package& package, std::string_view part, std::string* problem);
// The same for a part of the deck as its edits left it (src/slide/edit).
std::shared_ptr<const pkg::XmlDom> DomOf(const Presentation& deck, std::string_view part, std::string* problem);

// EMUs to points; integers as attributes give them, `def` when missing.
inline float Emu(pugi::xml_node n, const char* attr, double def = 0) { return EmuToPt(Number(n, attr, def)); }

// Shapes.cpp: a part's shape tree, placeholders matched to the parent part's, drawNodes built.
void ReadShapeTree(SlideBase& part, pugi::xml_node spTree, Presentation& deck);
// The background in effect for `part`, as a shape filling `size`.
void ResolveBackground(SlideBase& part, draw::Size size);
// Placeholder of a shape element (p:nvSpPr/p:nvPr/p:ph and the like).
Placeholder ReadPlaceholder(pugi::xml_node shape);
// A hyperlink element (a:hlinkClick, a:hlinkHover) resolved through the part's relationships.
Hyperlink ReadHyperlink(pugi::xml_node link, const SlideBase& owner, const Presentation& deck);

// TextStyles.cpp: a shape's text resolved for the part it is shown on.
// `workingCopy`: a copy of its text body being edited (P2), resolved in its place.
std::unique_ptr<TextBody> ResolveText(const Presentation& deck, const Shape& shape, const SlideBase& owner,
                                      const SlideBase& shownOn, pugi::xml_node workingCopy = {});
// A text body with no placeholder chain (table cells): the default text style, then the table
// style's text properties (a:tcTxStyle) in the order given, then the body's own.
std::unique_ptr<TextBody> ResolvePlainText(const Presentation& deck, pugi::xml_node txBody, const SlideBase& owner,
                                           const SlideBase& shownOn, const std::vector<pugi::xml_node>& tcTxStyles);

// The master a part's text styles come from.
const Master* MasterOf(const SlideBase& part);

// After an edit (src/slide/edit): the model reads again what the parts with these keys
// (pkg::PartKey; a part whose relationships changed counts) changed, or everything when `full`.
// What it read again goes into the store's graveyard; `out` gets what changed for the view.
void RefreshModel(Presentation& deck, const std::set<std::string>& parts, bool full, edit::Changes& out);

}  // namespace fw::slide::detail
