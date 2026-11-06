#pragma once

// Where fwtool meets Featherweight's Word layout (milestone D1, src/doc/layout). `fwtool compare`
// and `fwtool bench` call LayOutDocument for every document; `fwtool compare --dumps` measures
// layout dumps from a folder instead.
#include <filesystem>
#include <string>

#include "core/LayoutDump.h"
#include "core/OpenTimings.h"

namespace fwtool {

// Whether this build can lay out Word documents.
bool HaveLayout();

// Opens a Word document and lays out every page, as the app does: read the file, open the
// package, build the model, lay out the first page, then the rest. Marks each of those stages in
// `timings` (if given; the caller has called Start()) and fills `dump` (if given) with every
// line drawn (docs/layout-dump.md). Returns false with a plain message in `error` if it can't.
bool LayOutDocument(const std::filesystem::path& document, fw::LayoutDump* dump, fw::OpenTimings* timings,
                    std::string& error);

// fwtool dump --layout: prints each page's lines, or with `out`, writes <out>/<name>.layout.json.
void DumpLayout(const std::filesystem::path& document, const std::filesystem::path& out);

}  // namespace fwtool
