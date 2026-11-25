#include "core/FileKinds.h"

#include "core/Lang.h"
#include "core/Strings.h"

namespace fw {
namespace {

constexpr const wchar_t* kPdf[] = {L".pdf"};
constexpr const wchar_t* kDocument[] = {L".docx", L".docm", L".dotx", L".dotm", L".doc", L".odt", L".ott"};
constexpr const wchar_t* kDocumentLater[] = {L".rtf", L".txt"};  // .rtf and .txt come in D2
constexpr const wchar_t* kWorkbook[] = {L".xlsx", L".xlsm", L".xltx", L".xltm", L".csv", L".tsv", L".xls", L".ods", L".ots"};
constexpr const wchar_t* kPresentation[] = {L".pptx", L".pptm", L".potx", L".potm", L".ppsx", L".ppt", L".pps",
                                              L".pot",  L".odp",  L".otp"};

// Ships a kind: set `viewer` when its viewer is released, `editor` when its editor is.
const FileKindInfo kKinds[] = {
    {FileKind::Pdf, L"PDF", L"PDF", L"PDFs", L"Featherweight.PDF", N_(L"PDF Document"), N_(L"PDF documents"), kPdf, {}, 0,
     true, false},
    {FileKind::Document, L"Document", L"document", L"documents", L"Featherweight.Document", N_(L"Document"), N_(L"Documents"),  // lang: no (the name: a key)
     kDocument, kDocumentLater, 1, true, true},
    {FileKind::Workbook, L"Workbook", L"spreadsheet", L"spreadsheets", L"Featherweight.Workbook", N_(L"Spreadsheet"),  // lang: no
     N_(L"Spreadsheets"), kWorkbook, {}, 2, true, true},  // the editor ships in 3.0 (S4, W13)
    {FileKind::Presentation, L"Presentation", L"presentation", L"presentations", L"Featherweight.Presentation",  // lang: no
     N_(L"Presentation"), N_(L"Presentations"), kPresentation, {}, 3, true, true},  // the viewer in 3.1 (P1), the editor in 4.0 (P2, P3)
};
static_assert(sizeof(kKinds) / sizeof(kKinds[0]) == kFileKindCount);

// By kind, then KindPhrase. PDFs are never made new.
constexpr UiText kPhrases[kFileKindCount][4] = {
    {L"", L"", N_(L"Open PDFs with Featherweight by default?"), N_(L"Featherweight couldn’t register itself as a PDF app.")},
    {N_(L"New document"), N_(L"Blank document"), N_(L"Open documents with Featherweight by default?"),
     N_(L"Featherweight couldn’t register itself as a document app.")},
    {N_(L"New spreadsheet"), N_(L"Blank spreadsheet"), N_(L"Open spreadsheets with Featherweight by default?"),
     N_(L"Featherweight couldn’t register itself as a spreadsheet app.")},
    {N_(L"New presentation"), N_(L"Blank presentation"), N_(L"Open presentations with Featherweight by default?"),
     N_(L"Featherweight couldn’t register itself as a presentation app.")},
};

bool g_editorForTesting[kFileKindCount] = {};

bool Valid(FileKind kind) { return static_cast<int>(kind) >= 0 && static_cast<int>(kind) < kFileKindCount; }

}  // namespace

std::span<const FileKindInfo> FileKinds() { return kKinds; }

const FileKindInfo& Info(FileKind kind) { return kKinds[Valid(kind) ? static_cast<int>(kind) : 0]; }

const wchar_t* Phrase(FileKind kind, KindPhrase phrase) {
  return kPhrases[Valid(kind) ? static_cast<int>(kind) : 0][static_cast<int>(phrase)];
}

FileKind KindOfPath(std::wstring_view path) {
  for (const FileKindInfo& k : kKinds) {
    for (const wchar_t* ext : k.extensions)
      if (EndsWithNoCase(path, ext)) return k.kind;
    for (const wchar_t* ext : k.later)
      if (EndsWithNoCase(path, ext)) return k.kind;
  }
  return FileKind::Unknown;
}

bool ViewerShipped(FileKind kind) { return Valid(kind) && Info(kind).viewer; }

bool Opens(std::wstring_view path) {
  for (const FileKindInfo& k : kKinds) {
    if (!k.viewer) continue;
    for (const wchar_t* ext : k.extensions)
      if (EndsWithNoCase(path, ext)) return true;
  }
  return false;
}

bool EditorShipped(FileKind kind) {
  return Valid(kind) && (Info(kind).editor || g_editorForTesting[static_cast<int>(kind)]);
}

std::vector<FileKind> ShippedViewers() {
  std::vector<FileKind> kinds;
  for (const FileKindInfo& k : kKinds)
    if (k.viewer) kinds.push_back(k.kind);
  return kinds;
}

bool AnyEditorShipped() {
  for (const FileKindInfo& k : kKinds)
    if (EditorShipped(k.kind)) return true;
  return false;
}

void SetEditorShippedForTesting(FileKind kind, bool shipped) {
  if (Valid(kind) && kind != FileKind::Pdf) g_editorForTesting[static_cast<int>(kind)] = shipped;
}

}  // namespace fw
