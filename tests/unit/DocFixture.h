#pragma once

// A Word package built in memory from parts given as XML, for the document model's tests.
// (The same fixture as test_doc.cpp's, shared by the editing tests.)

#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "PkgTestUtil.h"
#include "doc/docx/Docx.h"
#include "pkg/Canonical.h"

namespace docfix {

using namespace fw::doc;
using fw::pkg::Status;

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

// A Word package built from parts given as XML.
struct Fixture {
  std::string body;         // inside w:body
  std::string documentXml;  // the whole main part, instead of body
  std::string styles, numbering, settings, theme, comments, commentsEx, footnotes, endnotes, header, footer;

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
                            : "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n<" + root + " " + kNs + ">" +
                                  inner + "</" + root + ">";
      parts.push_back({name, type, relType, xml});
    };
    add("word/styles.xml", wml + "styles+xml", rel + "styles", "w:styles", styles);
    add("word/numbering.xml", wml + "numbering+xml", rel + "numbering", "w:numbering", numbering);
    add("word/settings.xml", wml + "settings+xml", rel + "settings", "w:settings", settings);
    add("word/comments.xml", wml + "comments+xml", rel + "comments", "w:comments", comments);
    add("word/footnotes.xml", wml + "footnotes+xml", rel + "footnotes", "w:footnotes", footnotes);
    add("word/endnotes.xml", wml + "endnotes+xml", rel + "endnotes", "w:endnotes", endnotes);
    add("word/header1.xml", wml + "header+xml", rel + "header", "w:hdr", header);
    add("word/footer1.xml", wml + "footer+xml", rel + "footer", "w:ftr", footer);
    if (!commentsEx.empty())
      parts.push_back({"word/commentsExtended.xml", wml + "commentsExtended+xml",
                       "http://schemas.microsoft.com/office/2011/relationships/commentsExtended",
                       "<w15:commentsEx xmlns:w15=\"http://schemas.microsoft.com/office/word/2012/wordml\">" + commentsEx +
                           "</w15:commentsEx>"});
    if (!theme.empty())
      parts.push_back({"word/theme/theme1.xml", "application/vnd.openxmlformats-officedocument.theme+xml", rel + "theme",
                       "<a:theme xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" name=\"T\">"
                       "<a:themeElements>" + theme + "</a:themeElements></a:theme>"});
    std::string types =
        "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
        "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
        "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
        "<Override PartName=\"/word/document.xml\" ContentType=\"" + wml + "document.main+xml\"/>";
    std::string rels = "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">";
    int id = 1;
    for (const auto& p : parts) {
      types += "<Override PartName=\"/" + p.name + "\" ContentType=\"" + p.type + "\"/>";
      std::string relId = p.name == "word/header1.xml" ? "rIdHeader" : p.name == "word/footer1.xml" ? "rIdFooter"
                                                                                               : "rId" + std::to_string(id++);
      rels += "<Relationship Id=\"" + relId + "\" Type=\"" + p.rel + "\" Target=\"" +
              p.name.substr(5) + "\"/>";
    }
    rels += "<Relationship Id=\"rIdLink\" Type=\"" + rel +
            "hyperlink\" Target=\"https://example.com/\" TargetMode=\"External\"/></Relationships>";
    types += "</Types>";
    std::string document = documentXml.empty()
                               ? "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n<w:document " +
                                     std::string(kNs) + "><w:body>" + body + "</w:body></w:document>"
                               : documentXml;
    std::vector<std::pair<std::string, std::string>> entries = {
        {"[Content_Types].xml", types},
        {"_rels/.rels", "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
                        "<Relationship Id=\"rId1\" Type=\"" + rel + "officeDocument\" Target=\"word/document.xml\"/>"
                        "</Relationships>"},
        {"word/document.xml", document},
        {"word/_rels/document.xml.rels", rels},
    };
    for (const auto& p : parts) entries.emplace_back(p.name, p.xml);
    return test::MakeZip(entries);
  }

  std::unique_ptr<Document> Load(Status& st) const {
    auto pkg = test::OpenBytes(Bytes(), st);
    if (!pkg) return nullptr;
    return docx::Read(std::move(pkg), st);
  }
};

inline std::unique_ptr<Document> LoadOk(const Fixture& f) {
  Status st;
  auto doc = f.Load(st);
  if (!doc) std::printf("  load failed: %s\n", st.message.c_str());
  return doc;
}

inline std::string Canon(std::string_view xml) {
  std::string out;
  Status st = fw::pkg::CanonicalizeXml(xml, out);
  return st.Ok() ? out : "ERROR: " + st.message;
}

// Every part the model owns, written from the model, equals the original after canonicalization.
inline bool RoundTrips(const Document& doc) {
  bool ok = true;
  for (uint32_t i = 0; i < doc.Parts().size(); ++i) {
    std::vector<uint8_t> bytes;
    if (!doc.Package().ReadPart(doc.Parts()[i].name, bytes).Ok()) return false;
    std::string out;
    if (!docx::WritePart(doc, i, out).Ok()) return false;
    std::string a = Canon(test::Text(bytes)), b = Canon(out);
    if (a != b) {
      size_t at = 0;
      while (at < a.size() && at < b.size() && a[at] == b[at]) ++at;
      std::printf("  %s differs at %zu:\n    original: %s\n    written:  %s\n", doc.Parts()[i].name.c_str(), at,
                  a.substr(at > 60 ? at - 60 : 0, 200).c_str(), b.substr(at > 60 ? at - 60 : 0, 200).c_str());
      ok = false;
    }
  }
  return ok;
}

}  // namespace docfix
