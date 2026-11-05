#include "TestHarness.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "DocFixture.h"
#include "doc/edit/Clip.h"
#include "doc/edit/Export.h"
#include "doc/model/Utf.h"

// Save as RTF and as plain text (src/doc/edit/Export.cpp): sections, headers and footers, breaks
// and page number fields in RTF; Word's plain text, in UTF-8 and UTF-16.

using namespace fw::doc;
using namespace docfix;

namespace {

namespace edit = fw::doc::edit;

bool Has(const std::string& s, const std::string& what) {
  const bool found = s.find(what) != std::string::npos;
  if (!found) std::printf("  missing: %s\n", what.c_str());
  return found;
}

size_t Count(const std::string& s, const std::string& what) {
  size_t n = 0;
  for (size_t at = s.find(what); at != std::string::npos; at = s.find(what, at + what.size())) ++n;
  return n;
}

const char* const kMargins =
    "<w:pgMar w:top=\"1440\" w:right=\"1440\" w:bottom=\"1440\" w:left=\"1440\" w:header=\"720\" w:footer=\"720\" w:gutter=\"0\"/>";

// Two sections: a portrait one with a page break, and a landscape one in two columns with a header,
// and a footer that says "Page {PAGE} of {NUMPAGES}".
Fixture TwoSections() {
  Fixture f;
  f.body = std::string("<w:p><w:r><w:rPr><w:b/></w:rPr><w:t>Intro</w:t></w:r></w:p>") +
           "<w:p><w:r><w:t>Before</w:t></w:r><w:r><w:br w:type=\"page\"/></w:r><w:r><w:t>After</w:t></w:r></w:p>"
           "<w:p><w:pPr><w:sectPr><w:pgSz w:w=\"12240\" w:h=\"15840\"/>" + kMargins +
           "</w:sectPr></w:pPr><w:r><w:t>End one</w:t></w:r></w:p>"
           "<w:p><w:pPr><w:pageBreakBefore/></w:pPr><w:r><w:t>Wide</w:t></w:r></w:p>"
           "<w:sectPr><w:headerReference w:type=\"default\" r:id=\"rIdHeader\"/>"
           "<w:footerReference w:type=\"default\" r:id=\"rIdFooter\"/>"
           "<w:pgSz w:w=\"15840\" w:h=\"12240\" w:orient=\"landscape\"/>" + kMargins +
           "<w:pgNumType w:fmt=\"lowerRoman\" w:start=\"3\"/><w:cols w:num=\"2\" w:space=\"708\"/></w:sectPr>";
  f.header = "<w:p><w:r><w:t>Head</w:t></w:r></w:p>";
  f.footer =
      "<w:p><w:r><w:t xml:space=\"preserve\">Page </w:t></w:r><w:r><w:fldChar w:fldCharType=\"begin\"/></w:r>"
      "<w:r><w:instrText xml:space=\"preserve\"> PAGE </w:instrText></w:r><w:r><w:fldChar w:fldCharType=\"separate\"/></w:r>"
      "<w:r><w:t>1</w:t></w:r><w:r><w:fldChar w:fldCharType=\"end\"/></w:r><w:r><w:t xml:space=\"preserve\"> of </w:t></w:r>"
      "<w:fldSimple w:instr=\" NUMPAGES \"><w:r><w:t>2</w:t></w:r></w:fldSimple></w:p>";
  return f;
}

}  // namespace

TEST(Export_RtfKeepsSectionsHeadersAndBreaks) {
  auto doc = LoadOk(TwoSections());
  CHECK(doc);
  if (!doc) return;
  const std::string rtf = edit::RtfFile(*doc);
  CHECK(rtf.starts_with("{\\rtf1\\ansi") && rtf.ends_with("}"));
  // The document's page is the first section's; each section says its own.
  CHECK(Has(rtf, "\\paperw12240\\paperh15840\\margl1440\\margr1440\\margt1440\\margb1440"));
  CHECK(Count(rtf, "\\sectd") == 2 && Count(rtf, "\\sect\r\n") == 1);
  CHECK(Has(rtf, "\\sectd\\pgwsxn12240\\pghsxn15840"));
  CHECK(Has(rtf, "\\sectd\\pgwsxn15840\\pghsxn12240"));
  CHECK(Has(rtf, "\\lndscpsxn") && Has(rtf, "\\cols2\\colsx708") && Has(rtf, "\\pgnrestart\\pgnstarts3\\pgnlcrm"));
  // The page break where it was, the page break before "Wide", bold kept (runs say the document's
  // default font and size, Times New Roman 10 pt without styles).
  CHECK(Has(rtf, "{\\f0\\fs20 Before\\page After}\\par\r\n"));
  CHECK(Has(rtf, "\\pagebb {\\f0\\fs20 Wide}}") && Has(rtf, "{\\f0\\fs20\\b Intro}"));
  // The second section's header and footer, before its text; the first has none.
  const size_t second = rtf.rfind("\\sectd");
  CHECK(rtf.find("{\\header ") > second && rtf.find("{\\footer ") > second && rtf.find("{\\header ") < rtf.find("Wide"));
  CHECK(Count(rtf, "{\\header ") == 1 && Has(rtf, "Head"));
  CHECK(Has(rtf, "{\\f0\\fs20 Page }{\\field{\\*\\fldinst PAGE}{\\fldrslt {\\f0\\fs20 1}}}{\\f0\\fs20  of }"
                 "{\\field{\\*\\fldinst NUMPAGES}{\\fldrslt {\\f0\\fs20 2}}}\\par}"));
  // Seven-bit ASCII throughout, and the clipboard's reader reads the body back.
  bool ascii = true;
  for (char c : rtf) ascii = ascii && static_cast<unsigned char>(c) < 0x80;
  CHECK(ascii);
  auto back = edit::FromRtf(rtf);
  CHECK(back.has_value());
  if (back) {
    std::vector<std::u16string> texts;
    for (const auto& p : back->paras) {
      std::u16string t;
      for (const auto& r : p.runs) t += r.text;
      texts.push_back(t);
    }
    const bool same = texts.size() == 4 && texts[0] == u"Intro" && texts[1] == u"BeforeAfter" && texts[2] == u"End one" &&
                      texts[3] == u"Wide";
    CHECK(same);
    if (!same)
      for (const auto& t : texts) std::printf("  read back: \"%s\"\n", ToUtf8(t).c_str());
  }
}

TEST(Export_PlainTextAsWordWritesIt) {
  Fixture f = TwoSections();
  f.body = "<w:p><w:r><w:t>Caf&#233; &#x1F600;</w:t></w:r></w:p>"
           "<w:tbl><w:tblGrid><w:gridCol w:w=\"2000\"/><w:gridCol w:w=\"2000\"/></w:tblGrid><w:tr>"
           "<w:tc><w:p><w:r><w:t>A</w:t></w:r></w:p></w:tc><w:tc><w:p><w:r><w:t>B</w:t></w:r></w:p></w:tc></w:tr></w:tbl>" +
           f.body;
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  // UTF-8 with its BOM: CR LF after each paragraph, a tab between cells, a form feed for the page
  // break and for the second section's new page. Headers and footers aren't in it.
  const std::string utf8 = edit::TextFile(*doc, edit::TextEncoding::Utf8);
  const std::string want = "\xEF\xBB\xBF" "Caf\xC3\xA9 \xF0\x9F\x98\x80\r\nA\tB\r\nIntro\r\nBefore\x0C" "After\r\nEnd one\r\n"
                           "\x0CWide\r\n";
  CHECK(utf8 == want);
  if (utf8 != want) std::printf("  got: %s\n", utf8.c_str());
  CHECK(utf8.find("Head") == std::string::npos && utf8.find("Page") == std::string::npos);
  // UTF-16, little-endian, with its BOM: the same text.
  const std::string utf16 = edit::TextFile(*doc, edit::TextEncoding::Utf16);
  std::u16string text;
  for (size_t i = 2; i + 1 < utf16.size(); i += 2)
    text += static_cast<char16_t>(static_cast<unsigned char>(utf16[i]) | (static_cast<unsigned char>(utf16[i + 1]) << 8));
  CHECK(utf16.size() % 2 == 0 && utf16.substr(0, 2) == "\xFF\xFE");
  CHECK(ToUtf8(text) == want.substr(3));
}

TEST(Export_ClipboardRtfIsUnchanged) {
  // Copying still makes the RTF it made: no breaks, no fields, no section words.
  Fixture f = TwoSections();
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  const auto paras = doc->StoryParagraphs(doc->MainStory());
  const edit::Clip clip = edit::CopyClip(*doc, {paras.front(), 0}, {paras.back(), 4});
  const std::string rtf = edit::ToRtf(clip);
  CHECK(rtf.find("\\page") == std::string::npos && rtf.find("\\sect") == std::string::npos &&
        rtf.find("\\field") == std::string::npos);
  CHECK(Has(rtf, "{\\f0\\fs20\\b Intro}\\par\r\n") && Has(rtf, "{\\f0\\fs20 BeforeAfter}"));
}

TEST(Export_EveryGeneratedDocument) {
  // Every test document exports; the RTF reads back with the body's words in order.
  const auto office = std::filesystem::path(__FILE__).parent_path().parent_path() / "office";
  int files = 0;
  for (const char* folder : {"generated", "layout", "edge"})
  for (const auto& entry : std::filesystem::directory_iterator(office / folder)) {
    if (entry.path().extension() != ".docx") continue;
    std::ifstream in(entry.path(), std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    Status st;
    auto pkg = test::OpenBytes(std::move(bytes), st);
    auto doc = pkg ? docx::Read(std::move(pkg), st) : nullptr;
    if (!doc) continue;
    ++files;
    const std::string rtf = edit::RtfFile(*doc);
    const std::string txt = edit::TextFile(*doc, edit::TextEncoding::Utf8);
    auto back = edit::FromRtf(rtf);
    bool ok = back.has_value() && txt.starts_with("\xEF\xBB\xBF") && txt.ends_with("\r\n");
    // List labels are text, which Word shows ("{\listtext}" it leaves out without list tables):
    // simple.docx's bullets (Symbol's, written as U+2022) and numbers.
    ok = ok && rtf.find("{\\listtext") == std::string::npos;
    if (entry.path().filename() == "simple.docx") ok = ok && Has(rtf, "?\\tab First item") && Has(rtf, "1.\\tab One");
    if (back) {
      // The text file's words, breaks aside, are the RTF's.
      // (Breaks are left out: the clipboard's reader doesn't read them.)
      std::u16string a, b = back->PlainText();
      AppendUtf16(a, txt.substr(3));
      std::erase(a, char16_t{0x0C});
      auto words = [](const std::u16string& s) {
        std::vector<std::u16string> out(1);
        for (char16_t c : s) {
          if (c <= u' ' || c == 0xA0) {
            if (!out.back().empty()) out.emplace_back();
          } else {
            out.back() += c;
          }
        }
        if (out.back().empty()) out.pop_back();
        return out;
      };
      const auto wa = words(a), wb = words(b);
      ok = ok && wa == wb;
      if (wa != wb) {
        size_t k = 0;
        while (k < wa.size() && k < wb.size() && wa[k] == wb[k]) ++k;
        std::printf("  word %zu of %zu/%zu: text \"%s\", RTF \"%s\"\n", k, wa.size(), wb.size(),
                    k < wa.size() ? ToUtf8(wa[k]).c_str() : "", k < wb.size() ? ToUtf8(wb[k]).c_str() : "");
      }
    }
    if (!ok) std::printf("  %s doesn't export as it should\n", entry.path().filename().string().c_str());
    CHECK(ok);
  }
  CHECK(files > 0);
}
