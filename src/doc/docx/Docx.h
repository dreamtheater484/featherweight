#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "doc/model/Document.h"
#include "pkg/Package.h"
#include "pkg/References.h"
#include "pkg/Status.h"

namespace fw::doc::docx {

struct ReadOptions {
  // The registry whose rules find pairs and whose renames reach the model. The default registry
  // when null; a given one must outlive the document.
  const pkg::ReferenceRegistry* registry = nullptr;
  // Namespaces understood when choosing an mc:AlternateContent branch, besides the model's own.
  // Empty means DefaultUnderstood().
  std::vector<std::string> understood;
};

// The namespaces whose content Featherweight shows: DrawingML (pictures, shapes, groups, canvases),
// VML and equations. An mc:Choice that needs only these is the one shown; otherwise the Fallback.
const std::vector<std::string_view>& DefaultUnderstood();

// Reads a Word document (.docx, .docm, .dotx, .dotm) from an opened package. The document keeps
// the package (drawings read their pictures from it). Fails with a plain message for packages that
// aren't Word documents and for parts that aren't well-formed or exceed the package's limits.
std::unique_ptr<Document> Read(std::unique_ptr<pkg::Package> package, pkg::Status& status,
                               const ReadOptions& options = {});

// A DrawingML drawing's wp:inline or wp:anchor, read into the Drawing's placement: anchored, extent,
// effect extent, wrap, distances, position, z-order and wp:docPr. Fields the frame doesn't mention
// keep their values. The reader uses it, and the editor after changing a picture.
void ReadDrawingFrame(pugi::xml_node frame, Drawing& d);
// A w:pict's or w:object's content (its VML shape, after any v:shapetype), read into the Drawing's
// placement from the shape's style: size, floating or not, position, z-order, wrap, id and alt text.
void ReadVmlFrame(pugi::xml_node content, Drawing& d);

// The w:txbxContent elements inside `from` (not `from` itself), in document order and not inside
// one another: those of what is shown (none inside an mc:Fallback), or with `fallbacks` every one.
// The reader makes a text-box story of each one shown; the writer puts the stories back in the
// same places, in each branch of the drawing's mc:AlternateContent block.
std::vector<pugi::xml_node> TextBoxesIn(pugi::xml_node from, bool fallbacks = false);

// The highest bookmark id (w:id of a w:bookmarkStart or w:bookmarkEnd) in the document, -1 when
// there's none: the model's, and those in content kept as XML that the model doesn't read, such as
// the mc:Fallback copy of a text box's text. A new bookmark takes an id above it.
int64_t TopBookmarkId(const Document& doc);

// Writes one of the document's parts from the model and the round-trip store (plan §4.3 rule 1).
// In D1 this is used by the round-trip tests; saving comes in D2.
pkg::Status WritePart(const Document& doc, uint32_t part, std::string& out);

// Writes every part the model owns into the package ("re-serialize all", plan §10). Parts the
// writer can't reproduce (Part::writable) are left as they are and counted in `skipped`.
pkg::Status WriteAllParts(Document& doc, size_t* written = nullptr, size_t* skipped = nullptr);

}  // namespace fw::doc::docx
