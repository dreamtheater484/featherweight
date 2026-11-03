#pragma once

#include <string>
#include <vector>

#include "pkg/Package.h"
#include "pkg/References.h"

namespace fw::pkg {

struct CheckReport {
  std::vector<std::string> errors, warnings;  // "part: what is wrong"
  std::vector<std::string> unknownExtensions;  // "part: uri": structural edits near them are blocked
  size_t relationships = 0;  // relationships in the package
  size_t references = 0;     // references that resolved
  size_t ids = 0;            // values defined (checked for uniqueness)
  size_t pairs = 0;          // pairs that closed, fields included
  size_t derived = 0;        // derived values compared with their sources
  size_t embedded = 0;       // embedded packages checked

  bool Ok() const { return errors.empty(); }
};

// The consistency checker (plan §10): written from the reference registry's rules, independently
// of the models, so it sees damage to content they don't understand. It checks that
//  - every reference resolves, ids are unique and pairs are complete, by the registry's rules;
//  - relationships are unique and their targets exist, and every part has a content type;
//  - the derived data it can see matches its sources: the calculation chain, chart caches and
//    embedded workbooks, VML notes and the comments they show.
// Embedded packages (a chart's workbook) are checked too. Errors are what Office would repair or
// reject; warnings are what it tolerates.
CheckReport CheckPackage(const Package& pkg, const ReferenceRegistry& registry = ReferenceRegistry::Default());

// The kind of document a package holds, from its main part's content type.
DocFormat FormatOf(const Package& pkg);

}  // namespace fw::pkg
