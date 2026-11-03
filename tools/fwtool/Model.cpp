// fwtool dump --model: what the document model reads from a Word document, for checking the
// reader and for the layout work that builds on it.
#include <cstdio>

#include "Common.h"
#include "doc/docx/Docx.h"
#include "doc/model/Utf.h"
#include "pkg/Check.h"

namespace fwtool {
namespace {

using namespace fw::doc;

// Paragraph text with the characters that stand for something shown as {names}.
std::string Shown(std::u16string_view text) {
  std::string out;
  for (char16_t c : text) {
    switch (c) {
      case u'\t': out += "{tab}"; break;
      case u'\x0B': out += "{br}"; break;
      case u'\x0C': out += "{page}"; break;
      case u'\x0E': out += "{column}"; break;
      case u'\x13': out += "{field "; break;
      case u'\x14': out += "|"; break;
      case u'\x15': out += "}"; break;
      case u'\x02': out += "{note}"; break;
      case u'\x05': out += "{comment}"; break;
      case u'\x03': case u'\x04': out += "{separator}"; break;
      case u'￼': out += "{object}"; break;
      case u'­': out += "{shy}"; break;
      case u'‑': out += "{nbhy}"; break;
      default: AppendUtf8(out, std::u16string_view(&c, 1));
    }
  }
  return out;
}

const char* StoryName(StoryKind k) {
  switch (k) {
    case StoryKind::Main: return "main";
    case StoryKind::Header: return "header";
    case StoryKind::Footer: return "footer";
    case StoryKind::Footnote: return "footnote";
    case StoryKind::Endnote: return "endnote";
    case StoryKind::Comment: return "comment";
    case StoryKind::TextBox: return "text box";
  }
  return "?";
}

}  // namespace

void DumpModel(const std::filesystem::path& file) {
  fw::pkg::Status st;
  auto pkg = OpenPackage(file, st);
  std::printf("%s\n", Display(file).c_str());
  std::unique_ptr<Document> doc = pkg ? docx::Read(std::move(pkg), st) : nullptr;
  if (!doc) {
    std::printf("  refused: %s\n\n", st.message.c_str());
    return;
  }
  const DocSettings& set = doc->Settings();
  std::printf("  compatibility mode %d, default tab %d twips, auto hyphenation %s, even/odd headers %s\n",
              set.compatibilityMode, set.defaultTabStop.Value(), set.autoHyphenation ? "on" : "off",
              set.evenAndOddHeaders ? "on" : "off");
  std::printf("  %zu parts, %zu styles, %zu fonts, %zu drawings, %zu pairs; model %zu KB, store %zu KB\n",
              doc->Parts().size(), doc->Styles().size(), doc->Fonts().size(), doc->Drawings().size(),
              doc->Pairs().size(), doc->ModelBytes() / 1024, doc->Store().BytesUsed() / 1024);
  for (size_t i = 0; i < doc->Sections().size(); ++i) {
    const SectionProps& s = doc->Sections()[i].props;
    std::printf("  section %zu: %d x %d twips, margins %d %d %d %d, %d column(s)%s\n", i + 1, s.pageWidth.Value(),
                s.pageHeight.Value(), s.marginTop.Value(), s.marginEnd.Value(), s.marginBottom.Value(),
                s.marginStart.Value(), s.columns.count, s.titlePage ? ", title page" : "");
  }
  for (uint32_t story = 0; story < doc->Stories().size(); ++story) {
    const Story& s = doc->Stories()[story];
    std::printf("\n  [%s%s%s]\n", StoryName(s.kind), s.id.empty() ? "" : " ", s.id.c_str());
    for (uint32_t para : doc->StoryParagraphs(story)) {
      const ParaProps& pp = doc->ResolvedPara(para);
      auto label = doc->Label(para);
      const RunProps& mark = doc->ResolvedMark(para);
      std::printf("    %-14.*s %4.1fpt %s| %s%s\n", static_cast<int>(pp.style.size()), pp.style.data(),
                  mark.size.Value() / 2.0, label ? (ToUtf8(label->text) + " ").c_str() : "",
                  Shown(doc->Para(para).text).c_str(), doc->ParaNode(para).DisplayOnly() ? "  (display only)" : "");
    }
    for (const Field& f : doc->Fields(story))
      std::printf("    field %s: \"%s\" -> \"%s\"\n", ToUtf8(f.Type()).c_str(), ToUtf8(f.code).c_str(),
                  Shown(f.result).c_str());
  }
  for (const CommentInfo& c : doc->Comments())
    std::printf("\n  comment %.*s by %.*s%s\n", static_cast<int>(c.id.size()), c.id.data(), static_cast<int>(c.author.size()),
                c.author.data(), c.done ? " (resolved)" : "");
  std::printf("\n");
}

}  // namespace fwtool
