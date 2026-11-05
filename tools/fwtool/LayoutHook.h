#pragma once

// Where fwtool meets Featherweight's Word layout (milestone D1, src/doc/layout). `fwtool compare`
// and `fwtool bench` call LayOutDocument for every document; `fwtool compare --dumps` measures
// layout dumps from a folder instead.
#include <filesystem>
#include <string>
#include <vector>

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

// fwtool bench --typing: keystrokes at a place in a document, each timed from the edit to the
// page on screen painted again (plan §11), in ms.
struct TypingPlace {
  std::string where;  // "start", "middle", "end", "table" (a cell from the middle on)
  std::vector<double> typing, backspace, enter;  // enter: Enter, and Backspace joining again
  std::vector<double> format;  // bold, centered, Heading 1 and bullets, each undone
};
// Opens and lays out the document, then types at its start, middle and end, and in a table, through
// the editor, and formats there as Word's keys and buttons do.
bool BenchTyping(const std::filesystem::path& document, std::vector<TypingPlace>& places, size_t& pages,
                 std::string& error);

// fwtool edit-check: random edits through the editor, laid out again a few pages at a time after
// each (Layout::Edited), and every ten the whole layout compared with one made from scratch.
struct EditCheck {
  int edits = 0, checks = 0;
  bool skipped = false;  // the document can't be edited
  std::string mismatch;  // where the layouts first differ; empty when they never do
};
bool CheckEdits(const std::filesystem::path& document, int edits, unsigned seed, EditCheck& result, std::string& error);

}  // namespace fwtool
