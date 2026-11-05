// Fuzz target: copy and paste within Featherweight (src/doc/edit/Clip.cpp). A document's main story
// is copied (CopyClip), written as RTF and as HTML, and read back: the same paragraphs, list labels,
// text and character formatting. Then each is pasted, the HTML (Featherweight's own, with its styles
// and lists) over the whole story and the RTF at its end, and the document must stay valid
// (fuzz_paste.h).
// Seeds: tests/fuzz/seeds/docx and seeds/package. An input is a zip, or parts as text (fuzz_docx.cpp).
#include "fuzz_paste.h"

namespace {

using namespace fuzz_paste;

// What a reader keeps of a paragraph: its label (as Clip's readers tidy one), its text, and each
// character's bold, italic, underline, strikethrough and raised or lowered position.
struct Shown {
  std::u16string label, text;
  std::vector<uint8_t> look;
  bool operator==(const Shown&) const = default;
};

std::vector<Shown> Show(const edit::Clip& clip) {
  std::vector<Shown> out;
  for (const edit::ClipPara& p : clip.paras) {
    Shown s;
    s.label = p.label;
    while (!s.label.empty() && (s.label.back() == u' ' || s.label.back() == u'\t' || s.label.back() == 0xA0))
      s.label.pop_back();
    if (s.label == u"\u00B7" || s.label == u"\uF0B7") s.label = u"\u2022";
    for (const edit::ClipRun& r : p.runs) {
      s.text += r.text;
      const uint8_t look = (r.bold ? 1 : 0) | (r.italic ? 2 : 0) | (r.underline ? 4 : 0) | (r.strike ? 8 : 0) |
                           static_cast<uint8_t>(static_cast<int>(r.vertAlign) << 4);
      s.look.insert(s.look.end(), r.text.size(), look);
    }
    out.push_back(std::move(s));
  }
  return out;
}

void Same(const std::vector<Shown>& want, const std::vector<Shown>& got, const char* format) {
  if (want == got) return;
  std::fprintf(stderr, "Copied as %s and read back: %zu paragraphs, %zu read\n", format, want.size(), got.size());
  for (size_t i = 0; i < want.size() && i < got.size(); ++i) {
    if (want[i] == got[i]) continue;
    std::fprintf(stderr, "paragraph %zu:\n  copied [%s] \"%s\"\n  read   [%s] \"%s\"%s\n", i, ToUtf8(want[i].label).c_str(),
                 ToUtf8(want[i].text).c_str(), ToUtf8(got[i].label).c_str(), ToUtf8(got[i].text).c_str(),
                 want[i].text == got[i].text && want[i].label == got[i].label ? " (the formatting differs)" : "");
    break;
  }
  std::abort();
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const std::string_view input(reinterpret_cast<const char*>(data), size);
  const std::vector<uint8_t> zip =
      input.starts_with("PK\x03\x04") ? std::vector<uint8_t>(data, data + size) : ZipFromText(input);
  auto doc = Open(zip);
  if (!doc) return 0;
  const std::vector<uint32_t> paras = doc->StoryParagraphs(doc->MainStory());
  if (paras.empty()) return 0;
  const Pos from{paras.front(), 0}, to{paras.back(), static_cast<uint32_t>(doc->Para(paras.back()).text.size())};

  const edit::Clip clip = edit::CopyClip(*doc, from, to, 1);
  const std::vector<Shown> shown = Show(clip);
  clip.PlainText();
  const auto rtf = edit::FromRtf(edit::ToRtf(clip));
  Check(rtf.has_value(), "ToRtf's RTF doesn't read");
  Same(shown, Show(*rtf), "RTF");
  const auto html = edit::FromHtml(edit::ToHtml(clip));
  Check(html.has_value() && html->source == 1, "ToHtml's HTML doesn't read as Featherweight's");
  Same(shown, Show(*html), "HTML");

  // Pasted back: the document checked out before, so it must after.
  bool checked = false;
  {
    Status st;
    auto pkg = fw::pkg::Package::Open(fw::pkg::Snapshot::FromBytes(zip), FuzzLimits(), st);
    checked = pkg && fw::pkg::CheckPackage(*pkg).Ok();
  }
  PasteAndCheck(*doc, from, to, *html, 1, checked);
  auto again = Open(zip);
  const Pos end{to.para, static_cast<uint32_t>(again->Para(to.para).text.size())};
  PasteAndCheck(*again, end, end, *rtf, 0, checked);
  return 0;
}
