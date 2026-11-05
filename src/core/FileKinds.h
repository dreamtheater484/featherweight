#pragma once

#include <span>
#include <string_view>
#include <vector>

namespace fw {

// The kinds of file Featherweight opens (plan §5.1, §12). Each kind gets its ProgID, file icon
// and extensions registered once its viewer ships, and its "New" choice once its editor ships.
enum class FileKind { Pdf, Document, Workbook, Presentation, Unknown };
constexpr int kFileKindCount = 4;

struct FileKindInfo {
  FileKind kind;
  const wchar_t* name;        // "Document": settings keys and test scripts
  const wchar_t* noun;        // "document": "a document app", "New document"
  const wchar_t* plural;      // "documents": "Open documents with Featherweight by default?"
  const wchar_t* progId;      // "Featherweight.Document"
  const wchar_t* typeName;    // what Explorer calls these files
  const wchar_t* filterName;  // their name in the Open dialog
  std::span<const wchar_t* const> extensions;  // opened by the viewer; the first is the main one
  // Formats of this kind the viewer doesn't open yet: .rtf and .txt (D2), and the older and other
  // formats imported in the app's last milestone (decision 10).
  std::span<const wchar_t* const> later;
  int iconIndex;              // the file icon: its index among the exe's icons (0 is the app icon)
  bool viewer;                // the viewer has shipped
  bool editor;                // the editor has shipped (PDFs are read, not created)
};

std::span<const FileKindInfo> FileKinds();
const FileKindInfo& Info(FileKind kind);
// By extension, including the formats that come later; Unknown for anything else.
FileKind KindOfPath(std::wstring_view path);

bool ViewerShipped(FileKind kind);
// A shipped viewer opens files with this path's extension.
bool Opens(std::wstring_view path);
bool EditorShipped(FileKind kind);
// The kinds Featherweight opens today, in table order.
std::vector<FileKind> ShippedViewers();
bool AnyEditorShipped();
// Snapshots and UI checks show the New choices as they'll be once an editor ships. Only the
// start screen and the ＋ menu look at this; file associations follow the viewers.
void SetEditorShippedForTesting(FileKind kind, bool shipped);

}  // namespace fw
