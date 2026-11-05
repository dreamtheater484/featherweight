// The hook for D1's layout: fwtool opens a Word document as the app does and lays out every page
// with fwdoclayout (src/doc/layout/Layout.h), marking each stage in OpenTimings.
#include "LayoutHook.h"

#include <memory>

#include "core/Strings.h"
#include "doc/docx/Docx.h"
#include "doc/layout/Layout.h"
#include "pkg/Package.h"
#include "pkg/Snapshot.h"
#include "text/Fonts.h"

namespace fwtool {

namespace {

// The document being laid out, for font substitution from its font table (w:altName).
const fw::doc::Document* g_document = nullptr;

std::wstring Substitute(std::wstring_view family) {
  if (!g_document) return {};
  std::string name = fw::WideToUtf8(family);
  for (const fw::doc::FontInfo& f : g_document->Fonts())
    if (f.name == name && !f.altName.empty()) return fw::Utf8ToWide(f.altName);
  return {};
}

// One Fonts for the process: faces stay cached from one document to the next, as in the app.
fw::text::Fonts& SharedFonts() {
  static fw::text::Fonts fonts(Substitute);
  return fonts;
}

}  // namespace

bool HaveLayout() { return true; }

bool LayOutDocument(const std::filesystem::path& document, fw::LayoutDump* dump, fw::OpenTimings* timings,
                    std::string& error) {
  fw::pkg::Status status;
  fw::pkg::Snapshot::Options options;
  std::error_code ec;
  options.recoveryDir = (std::filesystem::temp_directory_path(ec) / L"fwtool-recovery").wstring();
  auto snapshot = fw::pkg::Snapshot::Open(document.wstring(), options, status);
  if (!snapshot) {
    error = status.message.empty() ? "can't read the file" : status.message;
    return false;
  }
  if (timings) timings->Mark(fw::OpenTimings::Read);
  auto package = fw::pkg::Package::Open(std::move(snapshot), fw::pkg::Limits::Default(), status);
  if (!package) {
    error = status.message.empty() ? "can't open the package" : status.message;
    return false;
  }
  if (timings) timings->Mark(fw::OpenTimings::Package);
  auto doc = fw::doc::docx::Read(std::move(package), status);
  if (!doc) {
    error = status.message.empty() ? "can't read the document" : status.message;
    return false;
  }
  if (timings) timings->Mark(fw::OpenTimings::Model);
  g_document = doc.get();
  fw::text::Fonts& fonts = SharedFonts();
  fonts.ClearSubstitutions();
  {
    fw::doc::layout::Layout layout(*doc, fonts, {}, timings);
    layout.LayOutAll();
    if (dump) *dump = layout.Dump(fw::WideToUtf8(document.filename().wstring()));
  }
  g_document = nullptr;
  return true;
}

}  // namespace fwtool
