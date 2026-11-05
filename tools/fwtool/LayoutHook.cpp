// The hook for D1's layout: fwtool opens a Word document as the app does and lays out every page
// with fwdoclayout (src/doc/layout/Layout.h), marking each stage in OpenTimings.
#include "LayoutHook.h"

#include <cstdio>
#include <functional>
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
  return g_document ? fw::doc::layout::SubstituteFamily(*g_document, family) : std::wstring();
}

// One Fonts for the process: faces stay cached from one document to the next, as in the app.
fw::text::Fonts& SharedFonts() {
  static fw::text::Fonts fonts(Substitute);
  return fonts;
}

// Opens a document as the app does and lays out every page, then hands the layout to `use`.
bool OpenAndLayOut(const std::filesystem::path& document, fw::OpenTimings* timings, std::string& error,
                   const std::function<void(const fw::doc::layout::Layout&)>& use) {
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
    use(layout);
  }
  g_document = nullptr;
  return true;
}

}  // namespace

bool HaveLayout() { return true; }

bool LayOutDocument(const std::filesystem::path& document, fw::LayoutDump* dump, fw::OpenTimings* timings,
                    std::string& error) {
  return OpenAndLayOut(document, timings, error, [&](const fw::doc::layout::Layout& layout) {
    if (dump) *dump = layout.Dump(fw::WideToUtf8(document.filename().wstring()));
  });
}

void DumpLayout(const std::filesystem::path& document, const std::filesystem::path& out) {
  std::string error;
  bool ok = OpenAndLayOut(document, nullptr, error, [&](const fw::doc::layout::Layout& layout) {
    if (!out.empty()) {
      std::error_code ec;
      std::filesystem::create_directories(out, ec);
      std::filesystem::path file = out / (document.filename().wstring() + L".layout.json");
      std::string json = fw::WriteLayoutDump(layout.Dump(fw::WideToUtf8(document.filename().wstring())));
      FILE* f = nullptr;
      if (_wfopen_s(&f, file.c_str(), L"wb") == 0 && f) {
        std::fwrite(json.data(), 1, json.size(), f);
        std::fclose(f);
      }
      std::printf("%s: %zu pages\n", fw::WideToUtf8(file.wstring()).c_str(), layout.PageCount());
      return;
    }
    std::printf("%s: %zu pages, %zu KB of layout\n", fw::WideToUtf8(document.wstring()).c_str(), layout.PageCount(),
                layout.MemoryBytes() / 1024);
    std::printf("  story      top  height baseline     x  asc/desc/gap  para | text\n");
    for (size_t p = 0; p < layout.PageCount(); ++p) {
      const fw::doc::layout::Page& page = layout.GetPage(p);
      std::printf("page %zu (%.0f x %.0f), number %d, text area %.1f-%.1f\n", p + 1, page.width, page.height, page.number,
                  page.body.top, page.body.bottom);
      for (const fw::doc::layout::LineBox& l : page.lines)
        std::printf("  %-7s %7.2f %6.2f %7.2f %6.2f %5.2f/%4.2f/%4.2f %5u | %s\n", std::string(fw::StoryName(l.story)).c_str(),
                    l.top, l.height, l.baseline, l.x + l.line.contentLeft, l.line.ascent, l.line.descent, l.line.lineGap,
                    l.para, fw::WideToUtf8(std::wstring(l.text.begin(), l.text.end())).c_str());
    }
  });
  if (!ok) std::fprintf(stderr, "%s: %s\n", fw::WideToUtf8(document.wstring()).c_str(), error.c_str());
}

}  // namespace fwtool
