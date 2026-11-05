// File associations as dry runs: what Register would write and Unregister would remove. These
// tests never touch the registry.

#include "FileAssoc.h"
#include "TestHarness.h"

using fileassoc::Removal;
using fileassoc::Value;
using fw::FileKind;

namespace {

const std::wstring kExe = L"C:\\Program Files\\Featherweight\\Featherweight.exe";

size_t Count(const std::vector<Value>& plan, const std::wstring& key) {
  size_t n = 0;
  for (const Value& v : plan) n += v.key == key;
  return n;
}

bool Has(const std::vector<Value>& plan, const Value& value) {
  for (const Value& v : plan)
    if (v == value) return true;
  return false;
}

}  // namespace

// Exactly what 0.9.4 writes for PDFs, value by value and in the same order.
TEST(FileAssoc_PdfRegistrationUnchanged) {
  const std::wstring open = L"\"" + kExe + L"\" \"%1\"";
  const std::wstring icon = L"\"" + kExe + L"\",0";
  const std::vector<Value> expected = {
      {L"Software\\Classes\\Featherweight.PDF", L"", L"PDF Document"},
      {L"Software\\Classes\\Featherweight.PDF\\DefaultIcon", L"", icon},
      {L"Software\\Classes\\Featherweight.PDF\\shell\\open\\command", L"", open},
      {L"Software\\Classes\\Featherweight.PDF\\Application", L"ApplicationName", L"Featherweight"},
      {L"Software\\Classes\\.pdf\\OpenWithProgids", L"Featherweight.PDF", L""},
      {L"Software\\Classes\\Applications\\Featherweight.exe", L"FriendlyAppName", L"Featherweight"},
      {L"Software\\Classes\\Applications\\Featherweight.exe\\shell\\open\\command", L"", open},
      {L"Software\\Classes\\Applications\\Featherweight.exe\\SupportedTypes", L".pdf", L""},
      {L"Software\\Featherweight\\Capabilities", L"ApplicationName", L"Featherweight"},
      {L"Software\\Featherweight\\Capabilities", L"ApplicationDescription", L"A light, fast PDF reader"},
      {L"Software\\Featherweight\\Capabilities", L"ApplicationIcon", icon},
      {L"Software\\Featherweight\\Capabilities\\FileAssociations", L".pdf", L"Featherweight.PDF"},
      {L"Software\\RegisteredApplications", L"Featherweight", L"Software\\Featherweight\\Capabilities"},
  };
  CHECK(fileassoc::Plan(kExe, {FileKind::Pdf}) == expected);
  // The PDF reader and the document viewer have shipped, so Register writes both, PDF first.
  const auto shipped = fileassoc::Plan(kExe, fw::ShippedViewers());
  CHECK(shipped == fileassoc::Plan(kExe, {FileKind::Pdf, FileKind::Document}));
  CHECK(std::vector<Value>(shipped.begin(), shipped.begin() + 5) == std::vector<Value>(expected.begin(), expected.begin() + 5));
}

TEST(FileAssoc_EveryKindOnceShipped) {
  const std::vector<FileKind> all = {FileKind::Pdf, FileKind::Document, FileKind::Workbook, FileKind::Presentation};
  const auto plan = fileassoc::Plan(kExe, all);
  const auto pdf = fileassoc::Plan(kExe, {FileKind::Pdf});
  // PDF's own values come first and don't change.
  CHECK(std::vector<Value>(plan.begin(), plan.begin() + 5) == std::vector<Value>(pdf.begin(), pdf.begin() + 5));

  size_t extensions = 0;
  for (FileKind kind : all) {
    const fw::FileKindInfo& k = fw::Info(kind);
    const std::wstring progId = L"Software\\Classes\\" + std::wstring(k.progId);
    CHECK(Has(plan, {progId, L"", k.typeName}));
    CHECK(Has(plan, {progId + L"\\DefaultIcon", L"", L"\"" + kExe + L"\"," + std::to_wstring(k.iconIndex)}));
    CHECK(Has(plan, {progId + L"\\shell\\open\\command", L"", L"\"" + kExe + L"\" \"%1\""}));
    for (const wchar_t* ext : k.extensions) {
      CHECK(Has(plan, {L"Software\\Classes\\" + std::wstring(ext) + L"\\OpenWithProgids", k.progId, L""}));
      CHECK(Has(plan, {L"Software\\Featherweight\\Capabilities\\FileAssociations", ext, k.progId}));
      ++extensions;
    }
    // Formats that come later aren't registered until their importers ship.
    for (const wchar_t* ext : k.later)
      CHECK(Count(plan, L"Software\\Classes\\" + std::wstring(ext) + L"\\OpenWithProgids") == 0);
  }
  CHECK_EQ(Count(plan, L"Software\\Classes\\Applications\\Featherweight.exe\\SupportedTypes"), extensions);
  CHECK(Has(plan, {L"Software\\Featherweight\\Capabilities", L"ApplicationDescription",
                   L"A light, fast PDF reader that also opens documents, spreadsheets and presentations"}));
  CHECK(Has(fileassoc::Plan(kExe, {FileKind::Pdf, FileKind::Document}),
            {L"Software\\Featherweight\\Capabilities", L"ApplicationDescription",
             L"A light, fast PDF reader that also opens documents"}));
  CHECK(plan.back() == pdf.back());  // RegisteredApplications, last as before
}

TEST(FileAssoc_UnregisterRemovesEveryKind) {
  const auto r = fileassoc::RemovalPlan();
  // What 0.9.4 removes, in its order: the PDF ProgID, its .pdf entry, then the application keys.
  CHECK(r[0] == (Removal{L"Software\\Classes\\Featherweight.PDF", L"", true}));
  CHECK(r[1] == (Removal{L"Software\\Classes\\.pdf\\OpenWithProgids", L"Featherweight.PDF", false}));
  const size_t n = r.size();
  CHECK(r[n - 3] == (Removal{L"Software\\Classes\\Applications\\Featherweight.exe", L"", true}));
  CHECK(r[n - 2] == (Removal{L"Software\\Featherweight\\Capabilities", L"", true}));
  CHECK(r[n - 1] == (Removal{L"Software\\RegisteredApplications", L"Featherweight", false}));
  // And every other kind, shipped or not, with the formats that come later.
  auto has = [&](const Removal& x) {
    for (const Removal& y : r)
      if (x == y) return true;
    return false;
  };
  CHECK(has({L"Software\\Classes\\Featherweight.Presentation", L"", true}));
  CHECK(has({L"Software\\Classes\\.txt\\OpenWithProgids", L"Featherweight.Document", false}));
  CHECK(has({L"Software\\Classes\\.ods\\OpenWithProgids", L"Featherweight.Workbook", false}));
}
