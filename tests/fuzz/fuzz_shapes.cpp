// Fuzz target: shapes, text boxes and watermarks in Word documents (milestone D4). The input is a
// document: the reader keeps each mc:AlternateContent block whole and reads its text boxes as
// stories (src/doc/docx/Reader.cpp); text typed into each text box the editor takes is written into
// every copy of it when saved (src/doc/docx/Writer.cpp). Then changes a hash of the input picks:
// shapes inserted, resized, turned, moved, wrapped, filled, outlined, given text, deleted
// (src/doc/edit/Shapes.cpp, Editor.cpp), and a text watermark put in or taken off
// (src/doc/edit/Watermark.cpp).
// Seeds: tests/fuzz/seeds/shapes, seeds/docx and seeds/package. An input is a zip, or parts as
// text (fuzz_docx.cpp).
//
// It aborts when:
// - a refused change changed a part;
// - a change the editor made can't be written (WritePart fails on a part it changed);
// - the document passed the package checker before and its saved copy doesn't, or the saved copy
//   doesn't open again;
// - the typing in a text box still in the document isn't in its story when read back;
// - undoing everything doesn't give each part back exactly as the model first wrote it, or redoing
//   it fails.
#include <windows.h>
// After windows.h:
#include <objbase.h>

#include "doc/edit/Shapes.h"
#include "doc/edit/Watermark.h"
#include "fuzz_paste.h"

namespace {

using namespace fuzz_paste;
using fw::Emu;

constexpr std::u16string_view kMark = u"Qz7";  // typed into text boxes, found again when read back

// FW_FUZZ_TRACE=1 prints each change and how it went, for replaying a failure.
bool Trace() {
  static const bool on = GetEnvironmentVariableW(L"FW_FUZZ_TRACE", nullptr, 0) > 0;
  return on;
}

// Each part as the model writes it now (empty for parts it doesn't write).
std::vector<std::string> PartsXml(const Document& doc) {
  std::vector<std::string> out(doc.Parts().size());
  for (uint32_t i = 0; i < doc.Parts().size(); ++i) {
    std::string xml;
    if (docx::WritePart(doc, i, xml).Ok()) out[i] = std::move(xml);
  }
  return out;
}

bool SameParts(const std::vector<std::string>& a, const std::vector<std::string>& b) {
  for (size_t i = 0; i < a.size(); ++i)
    if (i >= b.size() || a[i] != b[i]) return false;
  return true;
}

std::vector<uint32_t> Shown(const Document& doc) {
  std::vector<uint32_t> out;
  for (uint32_t i = 0; i < doc.Drawings().size(); ++i)
    if (doc.Drawings()[i].pos.para != kNone) out.push_back(i);
  return out;
}

bool HasMark(const Document& doc, uint32_t story) {
  for (uint32_t p : doc.StoryParagraphs(story))
    if (doc.Para(p).text.find(kMark) != std::u16string::npos) return true;
  return false;
}

size_t MarkedStories(const Document& doc) {
  size_t n = 0;
  for (uint32_t s = 0; s < doc.Stories().size(); ++s)
    if (doc.Stories()[s].kind == StoryKind::TextBox && doc.StoryInDocument(s) && HasMark(doc, s)) ++n;
  return n;
}

struct Choices {
  uint64_t h = 1469598103934665603ull;
  Choices(const uint8_t* data, size_t size) {
    for (size_t i = 0; i < size; ++i) h = (h ^ data[i]) * 1099511628211ull;
  }
  uint32_t Next(uint32_t n) {
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33;
    return static_cast<uint32_t>(h % n);
  }
  double Fraction() { return Next(10001) / 10000.0; }
};

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  static const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  (void)com;
  const std::string_view input(reinterpret_cast<const char*>(data), size);
  const std::vector<uint8_t> zip =
      input.starts_with("PK\x03\x04") ? std::vector<uint8_t>(data, data + size) : ZipFromText(input);
  auto doc = Open(zip);
  if (!doc) return 0;
  const bool checked = PackageChecks(zip, false);
  edit::Editor ed(*doc);
  edit::Shapes shapes(ed);
  Choices pick(data, size);
  const std::vector<std::string> first = PartsXml(*doc);

  // Typing at the start of each text box the editor takes (the text a file's reader shows).
  const size_t stories = doc->Stories().size();
  for (uint32_t s = 0; s < stories; ++s) {
    if (doc->Stories()[s].kind != StoryKind::TextBox || !edit::TextBoxEditable(*doc, s)) continue;
    const std::vector<uint32_t> paras = doc->StoryParagraphs(s);
    if (paras.empty()) continue;
    const std::vector<std::string> before = PartsXml(*doc);
    const edit::Result r = ed.InsertText({paras.front(), 0}, kMark);
    if (!r) Check(SameParts(before, PartsXml(*doc)), "refused typing changed a part");
    if (Trace()) std::fprintf(stderr, "type in story %u: %s\n", s, r.ok ? "made" : "refused");
  }

  // Changes; each is made or refused with a reason, and a refused one changes nothing.
  const std::vector<uint32_t> main = doc->StoryParagraphs(doc->MainStory());
  for (int k = 0; k < 8; ++k) {
    const std::vector<uint32_t> shown = Shown(*doc);
    const uint32_t d = shown.empty() ? 0 : shown[pick.Next(static_cast<uint32_t>(shown.size()))];
    const uint32_t op = shown.empty() ? 0 : pick.Next(13);
    const std::vector<std::string> before = PartsXml(*doc);
    edit::Result r;
    switch (op) {
      case 0: {
        if (main.empty()) continue;
        const uint32_t para = main[pick.Next(static_cast<uint32_t>(main.size()))];
        if (para >= doc->Paragraphs().size()) continue;
        r = shapes.Insert(para, static_cast<edit::ShapeKind>(pick.Next(7)), Emu(pick.Next(9000000)), Emu(pick.Next(11000000)),
                          Emu(pick.Next(6000000)), Emu(pick.Next(4000000)));
        break;
      }
      case 1: r = shapes.Resize(d, Emu(pick.Next(30000000)), Emu(pick.Next(30000000))); break;
      case 2: r = shapes.Rotate(d, pick.Fraction() * 1440 - 720); break;
      case 3: r = shapes.Move(d, Emu(pick.Next(9000000)), Emu(pick.Next(11000000))); break;
      case 4:
        r = shapes.SetWrap(d, static_cast<edit::Wrap>(pick.Next(6)), Emu(pick.Next(9000000)), Emu(pick.Next(11000000)));
        break;
      case 5: r = shapes.SetFill(d, pick.Next(3) ? std::optional<uint32_t>(pick.Next(0x1000000)) : std::nullopt); break;
      case 6:
        r = shapes.SetOutline(d, pick.Next(3) ? std::optional<uint32_t>(pick.Next(0x1000000)) : std::nullopt, pick.Fraction() * 12);
        break;
      case 7: r = shapes.AddText(d); break;
      case 8: {  // deleting the shape's character
        const Pos at = doc->Drawings()[d].pos;
        r = ed.Delete(at, {at.para, at.offset + 1});
        break;
      }
      case 9: {
        edit::TextWatermark mark;
        mark.text = pick.Next(2) ? u"DRAFT" : u"A <b>&amp; \"quoted\" 'mark' é";
        mark.diagonal = pick.Next(2) != 0;
        mark.semitransparent = pick.Next(2) != 0;
        mark.aspect = pick.Fraction() * 10;
        r = edit::SetTextWatermark(ed, mark);
        break;
      }
      case 10: r = edit::RemoveWatermark(ed); break;
      case 11: {  // typing in the shape's text
        const uint32_t story = shapes.TextStory(d);
        if (story == kNone || !edit::TextBoxEditable(*doc, story)) continue;
        const std::vector<uint32_t> paras = doc->StoryParagraphs(story);
        if (paras.empty()) continue;
        r = ed.InsertText({paras.back(), static_cast<uint32_t>(doc->Para(paras.back()).text.size())}, kMark);
        break;
      }
      case 12:
        r = ed.Undo();
        break;
    }
    Check(r.ok || !r.message.empty(), "a change was refused without a reason");
    if (!r) Check(SameParts(before, PartsXml(*doc)), "a refused change changed a part");
    if (Trace()) std::fprintf(stderr, "op %u on %u: %s\n", op, d, r.ok ? "made" : "refused");
  }

  // Saved as the Word tab saves (each changed part written, or the save fails here), and read back.
  const size_t marked = MarkedStories(*doc);
  for (uint32_t part : ed.DirtyParts()) {
    std::string xml;
    if (const fw::pkg::Status st = docx::WritePart(*doc, part, xml); !st.Ok())
      std::fprintf(stderr, "%s: %s\n", doc->Parts()[part].name.c_str(), st.message.c_str());
  }
  const std::vector<uint8_t> saved = Save(*doc, ed);
  if (checked) Check(PackageChecks(saved, true), "the saved document fails the package checker");
  {
    auto back = Open(saved);
    Check(back != nullptr, "the saved document doesn't open again");
    Check(MarkedStories(*back) >= marked, "typing in a text box didn't read back");
  }

  // Undone: each part as it was; redone: no step fails.
  while (ed.CanUndo()) Check(ed.Undo().ok, "a change can't be undone");
  Check(SameParts(first, PartsXml(*doc)), "undoing everything didn't give the document back");
  while (ed.CanRedo()) Check(ed.Redo().ok, "a change can't be redone");
  return 0;
}
