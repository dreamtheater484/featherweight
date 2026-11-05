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

// Writes one of the document's parts from the model and the round-trip store (plan §4.3 rule 1).
// In D1 this is used by the round-trip tests; saving comes in D2.
pkg::Status WritePart(const Document& doc, uint32_t part, std::string& out);

// Writes every part the model owns into the package ("re-serialize all", plan §10). Parts the
// writer can't reproduce (Part::writable) are left as they are and counted in `skipped`.
pkg::Status WriteAllParts(Document& doc, size_t* written = nullptr, size_t* skipped = nullptr);

}  // namespace fw::doc::docx
