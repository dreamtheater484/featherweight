#include "core/FileKinds.h"

#include "core/Strings.h"

namespace fw {
namespace {

constexpr const wchar_t* kPdf[] = {L".pdf"};
constexpr const wchar_t* kDocument[] = {L".docx", L".docm", L".dotx", L".dotm", L".doc", L".odt", L".ott"};
constexpr const wchar_t* kDocumentLater[] = {L".rtf", L".txt"};  // .rtf and .txt come in D2
constexpr const wchar_t* kWorkbook[] = {L".xlsx", L".xlsm", L".xltx", L".xltm", L".csv", L".tsv"};
constexpr const wchar_t* kWorkbookLater[] = {L".xls", L".ods"};
constexpr const wchar_t* kPresentation[] = {L".pptx", L".pptm", L".potx", L".potm", L".ppsx"};
constexpr const wchar_t* kPresentationLater[] = {L".ppt", L".odp"};

// Ships a kind: set `viewer` when its viewer is released, `editor` when its editor is.
const FileKindInfo kKinds[] = {
    {FileKind::Pdf, L"PDF", L"PDF", L"PDFs", L"Featherweight.PDF", L"PDF Document", L"PDF documents", kPdf, {}, 0,
     true, false},
    {FileKind::Document, L"Document", L"document", L"documents", L"Featherweight.Document", L"Document", L"Documents",
     kDocument, kDocumentLater, 1, true, true},
    {FileKind::Workbook, L"Workbook", L"spreadsheet", L"spreadsheets", L"Featherweight.Workbook", L"Spreadsheet",
     L"Spreadsheets", kWorkbook, kWorkbookLater, 2, false, false},
    {FileKind::Presentation, L"Presentation", L"presentation", L"presentations", L"Featherweight.Presentation",
     L"Presentation", L"Presentations", kPresentation, kPresentationLater, 3, false, false},
};
static_assert(sizeof(kKinds) / sizeof(kKinds[0]) == kFileKindCount);

bool g_editorForTesting[kFileKindCount] = {};

bool Valid(FileKind kind) { return static_cast<int>(kind) >= 0 && static_cast<int>(kind) < kFileKindCount; }

}  // namespace

std::span<const FileKindInfo> FileKinds() { return kKinds; }

const FileKindInfo& Info(FileKind kind) { return kKinds[Valid(kind) ? static_cast<int>(kind) : 0]; }

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
