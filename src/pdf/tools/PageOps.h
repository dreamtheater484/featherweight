#pragma once

// The PDF tab's page tools (F1, f1-pdf): rotating, deleting, moving and inserting pages, and new
// files from some of them (extract, split). All through PDFium's own functions (fpdf_edit,
// fpdf_ppo, fpdf_save), on a document whose pages the caller has closed. No UI: the tab, fwtool
// and the unit tests use them.

#include <fpdfview.h>

#include <string>
#include <vector>

namespace pdftools {

// Why the page tools can't change a document safely.
enum class Block {
  None,
  Xfa,        // an XFA form: its pages come from the form, not the page tree
  Protected,  // encrypted, and its permissions don't allow changing or assembling pages
};
Block PageToolsBlock(FPDF_DOCUMENT doc, bool isXfa);

// Each of `pages` turned by `quarterTurns` quarter turns clockwise (negative: counterclockwise).
bool Rotate(FPDF_DOCUMENT doc, const std::vector<int>& pages, int quarterTurns);
// Removes `pages`. Refuses to remove every page: a PDF needs at least one.
bool Delete(FPDF_DOCUMENT doc, std::vector<int> pages);
// Moves `pages` (in page order) to the gap before page `gap` of the document as it is now
// (0..count: count is after the last page). The pages stay in their order.
bool Move(FPDF_DOCUMENT doc, std::vector<int> pages, int gap);
// Where the first moved page ends up: what FPDF_MovePages calls the destination.
int MoveDestination(const std::vector<int>& sortedPages, int gap);

// Reading another PDF to insert or merge.
enum class SourceError { None, Read, NotPdf, Password, NotAllowed, Empty };
struct Source {
  std::string bytes;  // PDFium reads from these while the document is open
  FPDF_DOCUMENT doc = nullptr;
  Source() = default;
  Source(const Source&) = delete;
  Source& operator=(const Source&) = delete;
  ~Source();
};
// Opens `bytes` (moved in) as a source; a protected file needs `password`. A file whose
// permissions don't allow copying its content is refused (NotAllowed).
SourceError OpenSource(std::string bytes, const std::string& password, Source& out);
// Inserts every page of `src` before page `at` (count: at the end). Returns the pages inserted.
int Insert(FPDF_DOCUMENT doc, const Source& src, int at);

// A new PDF with `pages` of `doc`, in that order, as its bytes.
bool Extract(FPDF_DOCUMENT doc, const std::vector<int>& pages, std::string& out);
// The parts a split makes: every `every` pages, or a new part at each of `starts`.
std::vector<std::vector<int>> SplitEvery(int pageCount, int every);
std::vector<std::vector<int>> SplitAt(int pageCount, std::vector<int> starts);
// "1-3", "4", "2, 5-7": pages (0-based) as people number them, for file names and messages.
std::wstring PageList(const std::vector<int>& pages);

// The document as a save writes it: PDFium's incremental update after the file (or the whole
// file written again, when `incremental` is false or the update can't be made).
bool SaveBytes(FPDF_DOCUMENT doc, bool incremental, std::string& out);

}  // namespace pdftools
