#pragma once

// Word packages built from XML for the document model's tests (test_edit.cpp).

#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "PkgTestUtil.h"
#include "doc/docx/Docx.h"
#include "pkg/Canonical.h"

namespace doc_test {

inline constexpr const char* kNs =
    "xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\" "
    "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\" "
    "xmlns:w14=\"http://schemas.microsoft.com/office/word/2010/wordml\" "
    "xmlns:mc=\"http://schemas.openxmlformats.org/markup-compatibility/2006\" "
    "xmlns:wp=\"http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing\" "
    "xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" "
    "xmlns:wps=\"http://schemas.microsoft.com/office/word/2010/wordprocessingShape\" "
    "xmlns:v=\"urn:schemas-microsoft-com:vml\" xmlns:o=\"urn:schemas-microsoft-com:office:office\" "
    "xmlns:w10=\"urn:schemas-microsoft-com:office:word\" "
    "xmlns:m=\"http://schemas.openxmlformats.org/officeDocument/2006/math\" "
    "xmlns:x=\"urn:unknown\" mc:Ignorable=\"w14 x\"";

// The main part for a body.
inline std::string DocumentXml(std::string_view body) {
  return "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n<w:document " + std::string(kNs) +
         "><w:body>" + std::string(body) + "</w:body></w:document>";
}

// A Word package built from parts given as XML.
struct Fixture {
  std::string body;         // inside w:body
  std::string documentXml;  // the whole main part, instead of body
  std::string styles, numbering, settings, comments, footnotes, commentsExtended;

  std::vector<uint8_t> Bytes() const {
    struct Part {
      std::string name, type, rel, xml;
    };
    std::vector<Part> parts;
    const std::string wml = "application/vnd.openxmlformats-officedocument.wordprocessingml.";
    const std::string rel = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/";
    auto add = [&](const std::string& name, const std::string& type, const std::string& relType, const std::string& root,
                   const std::string& inner) {
      if (inner.empty()) return;
      std::string xml = inner.starts_with("<?xml") || inner.starts_with("<" + root)
                            ? inner
                            : "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n<" + root + " " + kNs +
                                  ">" + inner + "</" + root + ">";
      parts.push_back({name, type, relType, xml});
    };
    add("word/styles.xml", wml + "styles+xml", rel + "styles", "w:styles", styles);
    add("word/numbering.xml", wml + "numbering+xml", rel + "numbering", "w:numbering", numbering);
    add("word/settings.xml", wml + "settings+xml", rel + "settings", "w:settings", settings);
    add("word/comments.xml", wml + "comments+xml", rel + "comments", "w:comments", comments);
    add("word/footnotes.xml", wml + "footnotes+xml", rel + "footnotes", "w:footnotes", footnotes);
    add("word/commentsExtended.xml", wml + "commentsExtended+xml",
        "http://schemas.microsoft.com/office/2011/relationships/commentsExtended", "w15:commentsEx", commentsExtended);
    std::string types =
        "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
        "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
        "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
        "<Override PartName=\"/word/document.xml\" ContentType=\"" + wml + "document.main+xml\"/>";
    std::string rels = "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">";
    int id = 1;
    for (const auto& p : parts) {
      types += "<Override PartName=\"/" + p.name + "\" ContentType=\"" + p.type + "\"/>";
      rels += "<Relationship Id=\"rId" + std::to_string(id++) + "\" Type=\"" + p.rel + "\" Target=\"" +
              p.name.substr(5) + "\"/>";
    }
    rels += "<Relationship Id=\"rIdLink\" Type=\"" + rel +
            "hyperlink\" Target=\"https://example.com/\" TargetMode=\"External\"/></Relationships>";
    types += "</Types>";
    std::vector<std::pair<std::string, std::string>> entries = {
        {"[Content_Types].xml", types},
        {"_rels/.rels", "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
                        "<Relationship Id=\"rId1\" Type=\"" + rel + "officeDocument\" Target=\"word/document.xml\"/>"
                        "</Relationships>"},
        {"word/document.xml", documentXml.empty() ? DocumentXml(body) : documentXml},
        {"word/_rels/document.xml.rels", rels},
    };
    for (const auto& p : parts) entries.emplace_back(p.name, p.xml);
    return test::MakeZip(entries);
  }

  std::unique_ptr<fw::doc::Document> Load(fw::pkg::Status& st) const {
    auto pkg = test::OpenBytes(Bytes(), st);
    if (!pkg) return nullptr;
    return fw::doc::docx::Read(std::move(pkg), st);
  }
};

inline std::unique_ptr<fw::doc::Document> LoadOk(const Fixture& f) {
  fw::pkg::Status st;
  auto doc = f.Load(st);
  if (!doc) std::printf("  load failed: %s\n", st.message.c_str());
  return doc;
}

inline std::string Canon(std::string_view xml) {
  std::string out;
  fw::pkg::Status st = fw::pkg::CanonicalizeXml(xml, out);
  return st.Ok() ? out : "ERROR: " + st.message;
}

// Prints where two canonical forms part.
inline bool SameXml(const std::string& want, const std::string& got, const char* what) {
  if (want == got) return true;
  size_t at = 0;
  while (at < want.size() && at < got.size() && want[at] == got[at]) ++at;
  size_t from = at > 80 ? at - 80 : 0;
  std::printf("  %s differs at %zu:\n    want: %s\n    got:  %s\n", what, at, want.substr(from, 240).c_str(),
              got.substr(from, 240).c_str());
  return false;
}

// Every part the model owns, written from the model, equals the original after canonicalization.
inline bool RoundTrips(const fw::doc::Document& doc) {
  bool ok = true;
  for (uint32_t i = 0; i < doc.Parts().size(); ++i) {
    std::vector<uint8_t> bytes;
    if (!doc.Package().ReadPart(doc.Parts()[i].name, bytes).Ok()) return false;
    std::string out;
    if (!fw::doc::docx::WritePart(doc, i, out).Ok()) return false;
    ok = SameXml(Canon(test::Text(bytes)), Canon(out), doc.Parts()[i].name.c_str()) && ok;
  }
  return ok;
}

// The main part as the model writes it now.
inline std::string MainXml(const fw::doc::Document& doc) {
  const fw::doc::Part* main = doc.FindPart(fw::doc::PartKind::Document);
  std::string out;
  if (!main) return out;
  auto index = static_cast<uint32_t>(main - doc.Parts().data());
  if (!fw::doc::docx::WritePart(doc, index, out).Ok()) out.clear();
  return out;
}

}  // namespace doc_test
