// Shared by the clipboard fuzz targets (fuzz_rtf.cpp, fuzz_html.cpp, fuzz_clip.cpp): pasting a
// clip through the editor (src/doc/edit/Clip.h) must leave a document that is well-formed, passes
// the package checker when saved, opens again with the same paragraphs, and comes back exactly
// with one undo. A refused paste changes nothing.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "doc/docx/Docx.h"
#include "doc/edit/Clip.h"
#include "doc/edit/Editor.h"
#include "doc/model/Utf.h"
#include "pkg/Canonical.h"
#include "pkg/Check.h"
#include "pkg/Package.h"
#include "pkg/Snapshot.h"
#include "pkg/Zip.h"

namespace fuzz_paste {

using namespace fw::doc;
using fw::pkg::Status;

inline void Check(bool ok, const char* what) {
  if (ok) return;
  std::fprintf(stderr, "Broken: %s\n", what);
  std::abort();
}

inline fw::pkg::Limits FuzzLimits() {
  fw::pkg::Limits limits;
  limits.maxEntrySize = 16u << 20;
  limits.maxTotalSize = 64u << 20;
  limits.maxEntries = 4096;
  limits.maxStringLength = 1u << 20;
  return limits;
}

// Parts as text (as fuzz_check.cpp and fuzz_docx.cpp read them), stored in a zip: each part starts
// on a line "@<part name>", and the rest up to the next such line is its content.
inline std::vector<uint8_t> ZipFromText(std::string_view text) {
  fw::pkg::MemorySink sink;
  fw::pkg::ZipWriter writer(sink);
  size_t pos = text.starts_with('@') ? 0 : text.find("\n@");
  while (pos != std::string_view::npos) {
    if (text[pos] == '\n') ++pos;
    size_t eol = text.find('\n', pos);
    if (eol == std::string_view::npos) break;
    std::string_view name = text.substr(pos + 1, eol - pos - 1);
    if (!name.empty() && name.back() == '\r') name.remove_suffix(1);
    size_t next = text.find("\n@", eol);
    std::string_view body = text.substr(eol + 1, (next == std::string_view::npos ? text.size() : next) - eol - 1);
    auto bytes = reinterpret_cast<const uint8_t*>(body.data());
    writer.Add(name, std::span<const uint8_t>(bytes, body.size()), nullptr, fw::pkg::ZipWriter::Compression::Store);
    pos = next;
  }
  writer.Finish();
  return std::move(sink.bytes);
}

inline std::unique_ptr<Document> Open(std::vector<uint8_t> bytes) {
  Status st;
  auto pkg = fw::pkg::Package::Open(fw::pkg::Snapshot::FromBytes(std::move(bytes)), FuzzLimits(), st);
  return pkg ? docx::Read(std::move(pkg), st) : nullptr;
}

// Where pasting goes: "ab" and a numbered item, with the styles pasting looks for (headings, a
// character style) and a list, so Featherweight's own copies (data-fw-*) find what they name.
inline std::unique_ptr<Document> Target() {
  static const char kParts[] =
      "@[Content_Types].xml\n"
      "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
      "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
      "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
      "<Override PartName=\"/word/document.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml\"/>"
      "<Override PartName=\"/word/styles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.styles+xml\"/>"
      "<Override PartName=\"/word/numbering.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.numbering+xml\"/>"
      "</Types>\n"
      "@_rels/.rels\n"
      "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
      "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"word/document.xml\"/>"
      "</Relationships>\n"
      "@word/_rels/document.xml.rels\n"
      "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
      "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles\" Target=\"styles.xml\"/>"
      "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/numbering\" Target=\"numbering.xml\"/>"
      "</Relationships>\n"
      "@word/document.xml\n"
      "<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\" "
      "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\"><w:body>"
      "<w:p><w:r><w:t>ab</w:t></w:r></w:p>"
      "<w:p><w:pPr><w:pStyle w:val=\"ListParagraph\"/><w:numPr><w:ilvl w:val=\"0\"/><w:numId w:val=\"1\"/></w:numPr></w:pPr>"
      "<w:r><w:rPr><w:b/></w:rPr><w:t>item</w:t></w:r></w:p>"
      "<w:sectPr><w:pgSz w:w=\"12240\" w:h=\"15840\"/></w:sectPr></w:body></w:document>\n"
      "@word/styles.xml\n"
      "<w:styles xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
      "<w:docDefaults><w:rPrDefault><w:rPr><w:rFonts w:ascii=\"Calibri\" w:hAnsi=\"Calibri\"/><w:sz w:val=\"22\"/></w:rPr></w:rPrDefault></w:docDefaults>"
      "<w:style w:type=\"paragraph\" w:default=\"1\" w:styleId=\"Normal\"><w:name w:val=\"Normal\"/></w:style>"
      "<w:style w:type=\"paragraph\" w:styleId=\"Heading1\"><w:name w:val=\"heading 1\"/><w:basedOn w:val=\"Normal\"/>"
      "<w:pPr><w:outlineLvl w:val=\"0\"/></w:pPr><w:rPr><w:b/><w:sz w:val=\"32\"/></w:rPr></w:style>"
      "<w:style w:type=\"paragraph\" w:styleId=\"Heading2\"><w:name w:val=\"heading 2\"/><w:basedOn w:val=\"Normal\"/>"
      "<w:pPr><w:outlineLvl w:val=\"1\"/></w:pPr></w:style>"
      "<w:style w:type=\"paragraph\" w:styleId=\"ListParagraph\"><w:name w:val=\"List Paragraph\"/><w:basedOn w:val=\"Normal\"/>"
      "<w:pPr><w:ind w:left=\"720\"/></w:pPr></w:style>"
      "<w:style w:type=\"character\" w:default=\"1\" w:styleId=\"DefaultParagraphFont\"><w:name w:val=\"Default Paragraph Font\"/></w:style>"
      "<w:style w:type=\"character\" w:styleId=\"Strong\"><w:name w:val=\"Strong\"/><w:rPr><w:b/></w:rPr></w:style>"
      "</w:styles>\n"
      "@word/numbering.xml\n"
      "<w:numbering xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
      "<w:abstractNum w:abstractNumId=\"0\"><w:lvl w:ilvl=\"0\"><w:start w:val=\"1\"/><w:numFmt w:val=\"decimal\"/>"
      "<w:lvlText w:val=\"%1.\"/></w:lvl><w:lvl w:ilvl=\"1\"><w:start w:val=\"1\"/><w:numFmt w:val=\"bullet\"/>"
      "<w:lvlText w:val=\"o\"/></w:lvl></w:abstractNum><w:num w:numId=\"1\"><w:abstractNumId w:val=\"0\"/></w:num>"
      "</w:numbering>\n";
  return Open(ZipFromText(kParts));
}

// The parts edits changed, written from the model and saved, as the Word tab saves.
inline std::vector<uint8_t> Save(Document& doc, const edit::Editor& ed) {
  fw::pkg::Package& pkg = doc.Package();
  for (uint32_t part : ed.DirtyParts()) {
    std::string xml;
    Check(docx::WritePart(doc, part, xml).Ok(), "a changed part couldn't be written");
    pkg.SetPart(doc.Parts()[part].name, std::vector<uint8_t>(xml.begin(), xml.end()));
  }
  fw::pkg::MemorySink sink;
  Check(pkg.Save(sink).Ok(), "the package couldn't be saved");
  return std::move(sink.bytes);
}

inline std::string MainXml(const Document& doc) {
  const Part* main = doc.FindPart(PartKind::Document);
  std::string out;
  if (main) Check(docx::WritePart(doc, static_cast<uint32_t>(main - doc.Parts().data()), out).Ok(), "WritePart");
  return out;
}

inline bool PackageChecks(const std::vector<uint8_t>& bytes, bool print) {
  Status st;
  auto pkg = fw::pkg::Package::Open(fw::pkg::Snapshot::FromBytes(bytes), FuzzLimits(), st);
  if (!pkg) {
    if (print) std::fprintf(stderr, "The saved package doesn't open: %s\n", st.message.c_str());
    return false;
  }
  const fw::pkg::CheckReport rep = fw::pkg::CheckPackage(*pkg);
  if (print)
    for (const auto& e : rep.errors) std::fprintf(stderr, "check: %s\n", e.c_str());
  return rep.Ok();
}

// Pastes over [from, to) and checks the result. `checked`: the document passed the package checker
// before, so it must after. Returns whether the paste was taken.
inline bool PasteAndCheck(Document& doc, Pos from, Pos to, const edit::Clip& clip, uint64_t target, bool checked) {
  edit::Editor ed(doc);
  const std::string before = MainXml(doc);
  const edit::Result r = edit::PasteClip(ed, from, to, clip, target);
  if (!r.ok) {
    Check(MainXml(doc) == before, "a refused paste changed the document");
    return false;
  }
  const std::string pasted = MainXml(doc);
  std::string canonical;
  if (const Status x = fw::pkg::CanonicalizeXml(pasted, canonical, FuzzLimits(), "word/document.xml"); !x.Ok()) {
    std::fprintf(stderr, "The pasted document isn't well-formed: %s\n", x.message.c_str());
    std::abort();
  }

  // Saved, it passes the package checker and opens with the same paragraphs.
  const std::vector<uint8_t> saved = Save(doc, ed);
  if (checked && !PackageChecks(saved, true)) Check(false, "the pasted document fails the package checker");
  auto back = Open(saved);
  Check(back != nullptr, "the pasted document doesn't open again");
  const auto a = doc.StoryParagraphs(doc.MainStory()), b = back->StoryParagraphs(back->MainStory());
  Check(a.size() == b.size(), "the paragraphs read back aren't the ones pasted");
  for (size_t i = 0; i < a.size(); ++i) Check(doc.Para(a[i]).text == back->Para(b[i]).text, "a paragraph reads back differently");

  // One undo puts back exactly what was there.
  if (pasted != before) {
    Check(ed.Undo().ok, "the paste can't be undone");
    Check(MainXml(doc) == before, "undo didn't restore the document");
  }
  return true;
}

// What the writers make of a clip reads again.
inline void ReadsBack(const edit::Clip& clip) {
  clip.PlainText();
  Check(edit::FromRtf(edit::ToRtf(clip)).has_value(), "ToRtf's RTF doesn't read");
  Check(edit::FromHtml(edit::ToHtml(clip)).has_value(), "ToHtml's HTML doesn't read");
}

// A clip from the clipboard, pasted into the middle of "ab" (and, for Featherweight's own copies,
// into the document they name).
inline void PasteIntoTarget(const edit::Clip& clip) {
  static const std::vector<uint8_t> target = [] {
    auto doc = Target();
    Check(doc != nullptr, "the target document doesn't open");
    edit::Editor ed(*doc);
    return Save(*doc, ed);
  }();
  auto doc = Open(target);
  const uint32_t first = doc->StoryParagraphs(doc->MainStory()).front();
  if (!PasteAndCheck(*doc, {first, 1}, {first, 1}, clip, clip.source, true)) {
    std::fprintf(stderr, "The paste into a plain paragraph was refused\n");
    std::abort();
  }
}

}  // namespace fuzz_paste
