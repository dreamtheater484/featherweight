#pragma once

// The reference side of the fidelity measurement (plan §10): the manifest that
// scripts/reference-renders.ps1 writes, and Word's lines, read from its PDFs with PDFium.
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/LayoutDump.h"

namespace fwtool {

struct ReferenceEntry {
  std::string source;              // as the manifest names it: "layout/report.docx"
  std::filesystem::path document;  // the document itself
  std::filesystem::path pdf;       // Word's PDF; empty when Word couldn't export it
  int pages = 0;                   // Word's own page count, 0 if unknown
  std::string error;               // why there is no PDF
  std::string copyOf;              // a format copy: the source of the document it copies
  std::string password;            // a password-protected copy's (test) password
};

struct ReferenceSet {
  std::filesystem::path folder;  // the folder with manifest.json
  std::filesystem::path corpus;  // the documents: the manifest's "corpus", else the folder's parent
  std::vector<ReferenceEntry> entries;  // Word documents only, in the manifest's order
};

bool IsWordDocument(std::string_view name);
// Reads <folder>/manifest.json (a path to the manifest itself works too).
std::optional<ReferenceSet> ReadReferences(const std::filesystem::path& folder, std::string& error);
// Where a layout dump for `source` lives under a dumps folder: <dumps>/<source>.layout.json.
std::filesystem::path DumpPathFor(const std::filesystem::path& dumps, std::string_view source);

// Word's lines in a PDF it exported, as a layout dump: each page's size, and its lines in the
// order Word drew them, with their story (docs/layout-dump.md says how they're found).
std::optional<fw::LayoutDump> ReadPdfLines(const std::filesystem::path& pdf, std::string& error);

// Loads pdfium.dll from next to fwtool.exe and initializes it, once; false with `error` if it can't.
bool InitPdfium(std::string& error);

}  // namespace fwtool
