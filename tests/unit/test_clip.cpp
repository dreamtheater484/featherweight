#include "TestHarness.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "DocTestUtil.h"
#include "doc/edit/Clip.h"
#include "doc/edit/Editor.h"
#include "doc/model/Utf.h"
#include "pkg/Check.h"

// Pasting what other programs put on the clipboard (src/doc/edit/Clip.cpp), which is untrusted
// input: real copies from Word, LibreOffice and Chromium (tests/clipboard, made by the capture
// scripts there), and hostile ones. Whatever is pasted, the document stays valid.

using namespace fw::doc;
using namespace doc_test;

namespace {

namespace edit = fw::doc::edit;

std::string Sample(const std::string& name) {
  const auto path = std::filesystem::path(__FILE__).parent_path().parent_path() / "clipboard" / name;
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), {});
}

// Each sample's name; word-* and libreoffice-* come with RTF and HTML, chromium-* with HTML only.
const char* const kSamples[] = {"word-all", "word-partial", "word-paragraphs", "word-lists", "word-table",
                                "libreoffice-all", "libreoffice-partial", "libreoffice-table",
                                "chromium-page", "chromium-partial"};

std::vector<std::u16string> Words(std::u16string_view s) {
  std::vector<std::u16string> out;
  std::u16string w;
  for (char16_t c : s) {
    if (c == u' ' || c == u'\t' || c == u'\r' || c == u'\n' || c == 0x0B || c == 0xA0) {
      if (!w.empty()) out.push_back(std::move(w));
      w.clear();
    } else {
      w += c;
    }
  }
  if (!w.empty()) out.push_back(std::move(w));
  return out;
}

// Every word of `want`, in order, is in `got`.
bool InOrder(const std::vector<std::u16string>& want, const std::vector<std::u16string>& got) {
  size_t k = 0;
  for (const auto& w : got)
    if (k < want.size() && w == want[k]) ++k;
  if (k < want.size()) std::printf("  missing \"%s\" (word %zu)\n", ToUtf8(want[k]).c_str(), k);
  return k == want.size();
}

// What a reader keeps of a paragraph: its label, its text, and each character's look.
std::vector<std::u16string> Shown(const edit::Clip& clip) {
  std::vector<std::u16string> out;
  for (const edit::ClipPara& p : clip.paras) {
    std::u16string s = p.label + u"|";
    for (const edit::ClipRun& r : p.runs)
      for (char16_t c : r.text) {
        s += c;
        s += static_cast<char16_t>(u'A' + (r.bold ? 1 : 0) + (r.italic ? 2 : 0) + (r.underline ? 4 : 0) +
                                   (r.strike ? 8 : 0) + 16 * static_cast<int>(r.vertAlign));
      }
    out.push_back(std::move(s));
  }
  return out;
}

Fixture Target() {
  Fixture f;
  f.styles =
      "<w:style w:type=\"paragraph\" w:default=\"1\" w:styleId=\"Normal\"><w:name w:val=\"Normal\"/></w:style>"
      "<w:style w:type=\"paragraph\" w:styleId=\"Heading1\"><w:name w:val=\"heading 1\"/><w:basedOn w:val=\"Normal\"/>"
      "<w:pPr><w:outlineLvl w:val=\"0\"/></w:pPr><w:rPr><w:b/><w:sz w:val=\"32\"/></w:rPr></w:style>"
      "<w:style w:type=\"character\" w:default=\"1\" w:styleId=\"DefaultParagraphFont\"><w:name w:val=\"Default Paragraph Font\"/></w:style>";
  f.body = "<w:p><w:r><w:t xml:space=\"preserve\">a  b</w:t></w:r></w:p><w:sectPr/>";
  return f;
}

// Pastes the clip between "a " and " b": the document saves, passes the package checker, reads
// back with the same paragraphs, and one undo restores it.
bool PastesCleanly(const edit::Clip& clip, std::u16string* text = nullptr) {
  Fixture f = Target();
  auto doc = LoadOk(f);
  if (!doc) return false;
  edit::Editor ed(*doc);
  const uint32_t p = doc->StoryParagraphs(doc->MainStory()).front();
  const std::string before = MainXml(*doc);
  const edit::Result r = edit::PasteClip(ed, Pos{p, 2}, Pos{p, 2}, clip);
  if (!r) {
    std::printf("  refused: %s\n", ToUtf8(r.message).c_str());
    return false;
  }
  if (text)
    for (uint32_t q : doc->StoryParagraphs(doc->MainStory())) *text += doc->Para(q).text + u"\n";
  const std::string after = MainXml(*doc);
  Fixture g = f;
  g.body.clear();
  g.documentXml = after;
  auto back = LoadOk(g);
  if (!back) return false;
  const auto a = doc->StoryParagraphs(doc->MainStory()), b = back->StoryParagraphs(back->MainStory());
  bool ok = a.size() == b.size();
  for (size_t i = 0; ok && i < a.size(); ++i) ok = doc->Para(a[i]).text == back->Para(b[i]).text;
  if (!ok) std::printf("  the pasted paragraphs don't read back the same\n");
  // Saved and opened again, the package checks out.
  CHECK(docx::WriteAllParts(*doc).Ok());
  fw::pkg::MemorySink sink;
  CHECK(doc->Package().Save(sink).Ok());
  fw::pkg::Status st;
  auto saved = test::OpenBytes(std::move(sink.bytes), st);
  if (!saved) return false;
  const auto report = fw::pkg::CheckPackage(*saved);
  for (const auto& e : report.errors) std::printf("  check: %s\n", e.c_str());
  ok = ok && report.Ok();
  if (!(ed.Undo() && MainXml(*doc) == before)) {
    std::printf("  undo didn't restore the document\n");
    ok = false;
  }
  return ok;
}

std::string Repeat(std::string_view s, size_t n) {
  std::string out;
  out.reserve(s.size() * n);
  for (size_t i = 0; i < n; ++i) out += s;
  return out;
}

}  // namespace

TEST(Clip_RealCopiesReadAsTheirTextSays) {
  // Word's and LibreOffice's RTF (which pasting prefers) has exactly the words of their plain text,
  // list labels included; HTML, from them and from Chromium, has at least those words, in order.
  for (const char* name : kSamples) {
    const auto want = Words(ToUtf16(Sample(std::string(name) + ".txt")));
    CHECK(!want.empty());
    for (const char* ext : {".rtf", ".html"}) {
      const std::string bytes = Sample(name + std::string(ext));
      if (bytes.empty()) continue;
      const bool rtf = std::string_view(ext) == ".rtf";
      const auto clip = rtf ? edit::FromRtf(bytes) : edit::FromHtml(bytes);
      CHECK(clip.has_value());
      if (!clip) continue;
      const auto got = Words(clip->PlainText());
      const bool same = rtf ? got == want : InOrder(want, got);
      if (!same) std::printf("  %s%s: \"%s\"\n", name, ext, ToUtf8(clip->PlainText()).c_str());
      CHECK(same);
    }
  }
  // LibreOffice's bullets are a symbol font's character after a space: they read as bullets.
  const auto lists = edit::FromRtf(Sample("libreoffice-all.rtf"));
  CHECK(lists && lists->paras.size() > 6 && lists->paras[5].label == u"\u2022");
}

TEST(Clip_RealCopiesPasteAndRoundTrip) {
  for (const char* name : kSamples)
    for (const char* ext : {".rtf", ".html"}) {
      const std::string bytes = Sample(name + std::string(ext));
      if (bytes.empty()) continue;
      const bool rtf = std::string_view(ext) == ".rtf";
      const auto clip = rtf ? edit::FromRtf(bytes) : edit::FromHtml(bytes);
      if (!clip) continue;
      // Pasted into a document, every word arrives and the document stays valid.
      std::u16string text;
      const bool pasted = PastesCleanly(*clip, &text);
      if (!pasted) std::printf("  pasting %s%s\n", name, ext);
      CHECK(pasted);
      CHECK(InOrder(Words(clip->PlainText()), Words(text)));
      // Copied on from Featherweight, as RTF or HTML, it reads back the same.
      for (const bool asRtf : {true, false}) {
        const auto via = asRtf ? edit::FromRtf(edit::ToRtf(*clip)) : edit::FromHtml(edit::ToHtml(*clip));
        CHECK(via.has_value());
        if (!via) continue;
        const auto a = Shown(*clip), b = Shown(*via);
        CHECK(a == b);
        if (a == b) continue;
        std::printf("  %s%s copied as %s: %zu paragraphs, %zu read\n", name, ext, asRtf ? "RTF" : "HTML", a.size(), b.size());
        for (size_t i = 0; i < a.size() && i < b.size(); ++i)
          if (a[i] != b[i]) {
            std::printf("    [%s]\n    [%s]\n", ToUtf8(a[i]).c_str(), ToUtf8(b[i]).c_str());
            break;
          }
      }
    }
}

TEST(Clip_HostileCopiesAreCapped) {
  // Too big, or holding more than a real document: no clip (pasting takes the plain text instead).
  CHECK(!edit::FromRtf("{\\rtf1 " + std::string(64u << 20, 'x') + "}"));
  CHECK(!edit::FromHtml("<p>" + std::string(64u << 20, 'x')));
  CHECK(!edit::FromRtf("{\\rtf1 " + std::string((8u << 20) + 1, 'x') + "}"));
  CHECK(!edit::FromRtf("{\\rtf1 " + Repeat("\\par ", 10001) + "}"));
  CHECK(!edit::FromRtf("{\\rtf1 " + Repeat("{\\b x}{x}", 20001) + "}"));
  CHECK(!edit::FromRtf("{\\rtf1 " + Repeat("{}", (4u << 20) + 1) + "}"));
  auto many = edit::FromRtf("{\\rtf1 " + Repeat("\\par ", 1000) + "}");
  CHECK(many && many->paras.size() == 1001);
  // Linear time: an & with no ; after it isn't searched for to the end each time.
  auto amps = edit::FromHtml("<p>" + std::string(4u << 20, '&') + "</p>");
  CHECK(amps && amps->paras.size() == 1 && amps->paras[0].runs.size() == 1 &&
        amps->paras[0].runs[0].text.size() == (4u << 20));

  // Addresses and font names longer than any real one are left out or cut, between characters.
  auto web = edit::FromHtml("<p><a href=\"https://example.com/" + std::string(3000, 'x') +
                            "\">long</a> <span style=\"font-family:'" + Repeat("\xC3\xA9", 100) + "'\">font</span></p>");
  CHECK(web && web->paras[0].runs.size() == 2 && web->paras[0].runs[0].text == u"long " && web->paras[0].runs[0].link.empty());
  CHECK(web && web->paras[0].runs.back().font.size() == 62 && ToUtf16(web->paras[0].runs.back().font).size() == 31);

  // Sizes, indents and spacing beyond Word's are pasted at its limits: 1638 pt, 22 inches, no
  // spacing below 0 (the validator's ranges).
  auto big = edit::FromRtf("{\\rtf1\\ansi\\pard\\sb-400\\sa99999999\\li-99999999\\fi-2147483648 {\\fs9999999 big}\\par}");
  CHECK(big.has_value());
  Fixture f = Target();
  auto doc = LoadOk(f);
  CHECK(doc && big);
  if (!doc || !big) return;
  edit::Editor ed(*doc);
  const uint32_t p = doc->StoryParagraphs(doc->MainStory()).front();
  CHECK(edit::PasteClip(ed, Pos{p, 0}, Pos{p, 0}, *big));
  const std::string xml = MainXml(*doc);
  CHECK(xml.find("w:val=\"3276\"") != std::string::npos);
  CHECK(xml.find("w:left=\"-31680\"") != std::string::npos && xml.find("w:hanging=\"31680\"") != std::string::npos);
  CHECK(xml.find("w:after=\"31680\"") != std::string::npos && xml.find("w:before=\"-") == std::string::npos);

  // Characters XML can't hold, from the reader's escapes and from bytes that aren't UTF-8, and a
  // font and a link with control characters in their names: the pasted document is valid.
  for (const char* hostile :
       {"<p>a&#xD800;b&#xFFFE;c&#xFFFF;d\xED\xA0\x80" "e\xEF\xBF\xBE" "f</p>",
        "<p><a href=\"https://example.com/a\x80\x01\">link</a> <span style=\"font-family:'Bad\x01\x80'\">x</span></p>",
        "{\\rtf1\\ansi a\\u-10240?b\\u-2?c\\u-1?d}",
        "{\\rtf1\\ansi{\\fonttbl{\\f0\\fnil Bad\\'01\\'80Name;}}\\f0 {\\field{\\*\\fldinst HYPERLINK \"https://e.com/\\'01\"}{\\fldrslt x}}}"}) {
    const std::string_view h = hostile;
    const auto clip = h.starts_with("{\\rtf") ? edit::FromRtf(h) : edit::FromHtml(h);
    CHECK(clip.has_value());
    if (!clip) continue;
    const bool ok = PastesCleanly(*clip);
    if (!ok) std::printf("  pasting %s\n", hostile);
    CHECK(ok);
  }
}
