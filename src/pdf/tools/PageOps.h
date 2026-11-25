#pragma once

// The PDF tab's page tools (F1, f1-pdf): rotating, deleting, moving and inserting pages, and new
// files from some of them (extract, split). All through PDFium's own functions (fpdf_edit,
// fpdf_ppo, fpdf_save), on a document whose pages the caller has closed. No UI: the tab, fwtool
// and the unit tests use them.

#include <fpdfview.h>

#include <string>
#include <string_view>
#include <vector>

namespace pdftools {

// Why the page tools can't change a document safely.
enum class Block {
  None,
  Xfa,        // an XFA form: its pages come from the form, not the page tree
  Protected,  // encrypted, and its permissions don't allow changing or assembling pages
};
// `isXfa`: PDFium loaded the document as an XFA form; one whose catalog says it is (FPDF_GetFormType)
// is refused either way, since its pages are the form's and the page tree only a stand-in.
Block PageToolsBlock(FPDF_DOCUMENT doc, bool isXfa = false);
// The document is an XFA form (full or foreground), whether PDFium can run XFA or not.
bool IsXfaForm(FPDF_DOCUMENT doc);

// Each of `pages` turned by `quarterTurns` quarter turns clockwise (negative: counterclockwise).
bool Rotate(FPDF_DOCUMENT doc, const std::vector<int>& pages, int quarterTurns);
// Removes `pages`. Refuses to remove every page: a PDF needs at least one.
bool Delete(FPDF_DOCUMENT doc, std::vector<int> pages);
// Moves `pages` (in page order) to the gap before page `gap` of the document as it is now
// (0..count: count is after the last page). The pages stay in their order. FPDF_MovePages gives a
// moved page its new parent's inherited attributes, so the page's rotation and visible area are
// written on it first; for everything a page inherits (its resources too), call PinInherited on
// the document's bytes before loading it.
bool Move(FPDF_DOCUMENT doc, std::vector<int> pages, int gap);
// The attributes `pages` inherit from the page tree (/Resources, /MediaBox, /CropBox, /Rotate),
// written on the pages themselves as an incremental update after `bytes`, so a move to another
// branch of the tree can't change them. True with `out` when that was needed and done (load `out`
// instead); false when not needed (one branch, or nothing inherited), or when the file can't be read
// strictly or is encrypted (then Move's own pinning is what there is). `pageCount` is PDFium's.
bool PinInherited(std::string_view bytes, const std::vector<int>& pages, int pageCount, std::string& out);
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
// Inserts every page of `src` before page `at` (count: at the end), each with the rotation and
// visible area it had there. Returns the pages inserted. PDFium's imported pages go on reading
// from `src` (their streams) until `doc` is written and loaded again: keep `src` open until then
// (Editor::Insert and the tab do it at once).
int Insert(FPDF_DOCUMENT doc, const Source& src, int at);

// A new PDF with `pages` of `doc`, in that order, as its bytes.
bool Extract(FPDF_DOCUMENT doc, const std::vector<int>& pages, std::string& out);
// The parts a split makes: every `every` pages, or a new part at each of `starts`.
std::vector<std::vector<int>> SplitEvery(int pageCount, int every);
std::vector<std::vector<int>> SplitAt(int pageCount, std::vector<int> starts);
// "1-3", "4", "2, 5-7": pages (0-based) as people number them, for file names and messages.
std::wstring PageList(const std::vector<int>& pages);

// The document's bytes through PDFium: its incremental update after the file (or the whole file
// written again, when `incremental` is false or the update can't be made).
bool SaveBytes(FPDF_DOCUMENT doc, bool incremental, std::string& out);
// The document as Featherweight saves an edited PDF: written whole, except a signed PDF, which gets
// an incremental update so its signatures keep framing what they signed (a change after signing
// shows as one). PDFium's update after a file with a cross-reference stream leaves /Type /XRef out
// of its own stream (ISO 32000-1 table 17 requires it): it's added. `loadedSize` is the size of the
// bytes the document was loaded from (nothing before it is ever touched).
bool SaveDocument(FPDF_DOCUMENT doc, size_t loadedSize, std::string& out);
// Adds /Type /XRef to the cross-reference stream `bytes` end with, when it starts at or after
// `from` and lacks it. Exposed for the tests.
bool TypeLastXrefStream(std::string& bytes, size_t from);

}  // namespace pdftools
