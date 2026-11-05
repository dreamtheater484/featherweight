#include "doc/docx/Blank.h"

#include <string_view>

#include "doc/docx/Save.h"
#include "pkg/Zip.h"

namespace fw::doc::docx {
namespace {

constexpr std::string_view kDecl = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n";
constexpr std::string_view kW = "xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\"";
constexpr std::string_view kRelsNs = "xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\"";

std::string Escape(std::string_view s) {
  std::string out;
  for (char c : s) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      default: out += c;
    }
  }
  return out;
}

std::string Fonts(std::string_view face) {
  std::string f(face);
  return "<w:rFonts w:ascii=\"" + f + "\" w:eastAsia=\"" + f + "\" w:hAnsi=\"" + f + "\" w:cs=\"Times New Roman\"/>";
}

// A paragraph style. `ppr` and `rpr` are the contents of w:pPr and w:rPr.
std::string ParaStyle(std::string_view id, std::string_view name, int priority, std::string_view ppr,
                      std::string_view rpr, bool next = false) {
  std::string s = "<w:style w:type=\"paragraph\" w:styleId=\"" + std::string(id) + "\"><w:name w:val=\"" +
                  std::string(name) + "\"/><w:basedOn w:val=\"Normal\"/>";
  if (next) s += "<w:next w:val=\"Normal\"/>";
  s += "<w:uiPriority w:val=\"" + std::to_string(priority) + "\"/><w:qFormat/>";
  if (!ppr.empty()) s += "<w:pPr>" + std::string(ppr) + "</w:pPr>";
  if (!rpr.empty()) s += "<w:rPr>" + std::string(rpr) + "</w:rPr>";
  return s + "</w:style>";
}

std::string Heading(int level, int size, std::string_view color, int before) {
  const std::string sz = std::to_string(size);
  return ParaStyle("Heading" + std::to_string(level), "heading " + std::to_string(level), 9,
                   "<w:keepNext/><w:keepLines/><w:spacing w:before=\"" + std::to_string(before) +
                       "\" w:after=\"0\"/><w:outlineLvl w:val=\"" + std::to_string(level - 1) + "\"/>",
                   Fonts("Calibri Light") + "<w:color w:val=\"" + std::string(color) + "\"/><w:sz w:val=\"" + sz +
                       "\"/><w:szCs w:val=\"" + sz + "\"/>",
                   true);
}

std::string Styles(const std::string& lang) {
  std::string s(kDecl);
  s += "<w:styles " + std::string(kW) + ">";
  s += "<w:docDefaults><w:rPrDefault><w:rPr>" + Fonts("Calibri") +
       "<w:sz w:val=\"22\"/><w:szCs w:val=\"22\"/><w:lang w:val=\"" + lang +
       "\" w:eastAsia=\"en-US\" w:bidi=\"ar-SA\"/></w:rPr></w:rPrDefault>"
       "<w:pPrDefault><w:pPr><w:spacing w:after=\"160\" w:line=\"259\" w:lineRule=\"auto\"/></w:pPr></w:pPrDefault>"
       "</w:docDefaults>";
  s += "<w:style w:type=\"paragraph\" w:default=\"1\" w:styleId=\"Normal\"><w:name w:val=\"Normal\"/><w:qFormat/></w:style>";
  s += "<w:style w:type=\"character\" w:default=\"1\" w:styleId=\"DefaultParagraphFont\"><w:name w:val=\"Default "
       "Paragraph Font\"/><w:uiPriority w:val=\"1\"/><w:semiHidden/><w:unhideWhenUsed/></w:style>";
  s += "<w:style w:type=\"table\" w:default=\"1\" w:styleId=\"TableNormal\"><w:name w:val=\"Normal Table\"/>"
       "<w:uiPriority w:val=\"99\"/><w:semiHidden/><w:unhideWhenUsed/><w:tblPr><w:tblInd w:w=\"0\" w:type=\"dxa\"/>"
       "<w:tblCellMar><w:top w:w=\"0\" w:type=\"dxa\"/><w:left w:w=\"108\" w:type=\"dxa\"/><w:bottom w:w=\"0\" "
       "w:type=\"dxa\"/><w:right w:w=\"108\" w:type=\"dxa\"/></w:tblCellMar></w:tblPr></w:style>";
  s += "<w:style w:type=\"numbering\" w:default=\"1\" w:styleId=\"NoList\"><w:name w:val=\"No List\"/>"
       "<w:uiPriority w:val=\"99\"/><w:semiHidden/><w:unhideWhenUsed/></w:style>";
  s += Heading(1, 32, "2F5496", 240);
  s += Heading(2, 26, "2F5496", 40);
  s += Heading(3, 24, "1F3763", 40);
  s += ParaStyle("Title", "Title", 10, "<w:spacing w:after=\"0\" w:line=\"240\" w:lineRule=\"auto\"/><w:contextualSpacing/>",
                 Fonts("Calibri Light") + "<w:spacing w:val=\"-10\"/><w:kern w:val=\"28\"/><w:sz w:val=\"56\"/>"
                                          "<w:szCs w:val=\"56\"/>",
                 true);
  s += ParaStyle("Subtitle", "Subtitle", 11, "<w:spacing w:after=\"160\"/>",
                 "<w:color w:val=\"5A5A5A\"/><w:spacing w:val=\"15\"/>", true);
  s += ParaStyle("ListParagraph", "List Paragraph", 34, "<w:ind w:left=\"720\"/><w:contextualSpacing/>", "");
  s += "<w:style w:type=\"character\" w:styleId=\"Hyperlink\"><w:name w:val=\"Hyperlink\"/><w:basedOn "
       "w:val=\"DefaultParagraphFont\"/><w:uiPriority w:val=\"99\"/><w:unhideWhenUsed/><w:rPr><w:color "
       "w:val=\"0563C1\"/><w:u w:val=\"single\"/></w:rPr></w:style>";
  return s + "</w:styles>";
}

std::string Settings(const std::string& lang) {
  constexpr std::string_view kWord = "\" w:uri=\"http://schemas.microsoft.com/office/word\" w:val=\"";
  std::string compat;
  for (auto [name, val] : {std::pair{"compatibilityMode", "15"}, {"overrideTableStyleFontSizeAndJustification", "1"},
                           {"enableOpenTypeFeatures", "1"}, {"doNotFlipMirrorIndents", "1"},
                           {"differentiateMultirowTableHeaders", "1"}})
    compat += "<w:compatSetting w:name=\"" + std::string(name) + std::string(kWord) + val + "\"/>";
  return std::string(kDecl) + "<w:settings " + std::string(kW) +
         "><w:zoom w:percent=\"100\"/><w:defaultTabStop w:val=\"720\"/>"
         "<w:characterSpacingControl w:val=\"doNotCompress\"/><w:compat>" +
         compat + "</w:compat><w:themeFontLang w:val=\"" + lang + "\"/></w:settings>";
}

std::string Body(bool a4) {
  // Word's own: Letter with 1-inch margins; A4 with 2.5 cm (as Word's European templates).
  const std::string page = a4 ? "<w:pgSz w:w=\"11906\" w:h=\"16838\"/><w:pgMar w:top=\"1417\" w:right=\"1417\" "
                                "w:bottom=\"1417\" w:left=\"1417\" w:header=\"708\" w:footer=\"708\" w:gutter=\"0\"/>"
                                "<w:cols w:space=\"708\"/>"
                              : "<w:pgSz w:w=\"12240\" w:h=\"15840\"/><w:pgMar w:top=\"1440\" w:right=\"1440\" "
                                "w:bottom=\"1440\" w:left=\"1440\" w:header=\"720\" w:footer=\"720\" w:gutter=\"0\"/>"
                                "<w:cols w:space=\"720\"/>";
  return std::string(kDecl) + "<w:document " + std::string(kW) +
         " xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\"><w:body><w:p/><w:sectPr>" +
         page + "<w:docGrid w:linePitch=\"360\"/></w:sectPr></w:body></w:document>";
}

std::string Core(const std::string& author, int64_t now) {
  const std::string when = W3cTime(now), who = Escape(author);
  return std::string(kDecl) +
         "<cp:coreProperties xmlns:cp=\"http://schemas.openxmlformats.org/package/2006/metadata/core-properties\" "
         "xmlns:dc=\"http://purl.org/dc/elements/1.1/\" xmlns:dcterms=\"http://purl.org/dc/terms/\" "
         "xmlns:dcmitype=\"http://purl.org/dc/dcmitype/\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\">"
         "<dc:title></dc:title><dc:creator>" +
         who + "</dc:creator><cp:lastModifiedBy>" + who +
         "</cp:lastModifiedBy><cp:revision>1</cp:revision><dcterms:created xsi:type=\"dcterms:W3CDTF\">" + when +
         "</dcterms:created><dcterms:modified xsi:type=\"dcterms:W3CDTF\">" + when +
         "</dcterms:modified></cp:coreProperties>";
}

}  // namespace

std::vector<uint8_t> BlankDocx(const BlankOptions& options) {
  const std::string lang = Escape(options.language.empty() ? "en-US" : options.language);
  constexpr std::string_view kWml = "application/vnd.openxmlformats-officedocument.wordprocessingml.";
  constexpr std::string_view kRel = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/";
  const std::pair<std::string, std::string> parts[] = {
      {"[Content_Types].xml",
       std::string(kDecl) +
           "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
           "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
           "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
           "<Override PartName=\"/word/document.xml\" ContentType=\"" + std::string(kWml) + "document.main+xml\"/>"
           "<Override PartName=\"/word/styles.xml\" ContentType=\"" + std::string(kWml) + "styles+xml\"/>"
           "<Override PartName=\"/word/settings.xml\" ContentType=\"" + std::string(kWml) + "settings+xml\"/>"
           "<Override PartName=\"/docProps/core.xml\" "
           "ContentType=\"application/vnd.openxmlformats-package.core-properties+xml\"/></Types>"},
      {"_rels/.rels",
       std::string(kDecl) + "<Relationships " + std::string(kRelsNs) +
           "><Relationship Id=\"rId1\" Type=\"" + std::string(kRel) + "officeDocument\" Target=\"word/document.xml\"/>"
           "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/package/2006/relationships/metadata/"
           "core-properties\" Target=\"docProps/core.xml\"/></Relationships>"},
      {"word/document.xml", Body(options.a4)},
      {"word/_rels/document.xml.rels",
       std::string(kDecl) + "<Relationships " + std::string(kRelsNs) + "><Relationship Id=\"rId1\" Type=\"" +
           std::string(kRel) + "styles\" Target=\"styles.xml\"/><Relationship Id=\"rId2\" Type=\"" +
           std::string(kRel) + "settings\" Target=\"settings.xml\"/></Relationships>"},
      {"word/styles.xml", Styles(lang)},
      {"word/settings.xml", Settings(lang)},
      {"docProps/core.xml", Core(options.author, options.now)},
  };
  pkg::MemorySink sink;
  pkg::ZipWriter zip(sink);
  for (const auto& [name, text] : parts)
    zip.Add(name, std::span(reinterpret_cast<const uint8_t*>(text.data()), text.size()));
  zip.Finish();
  return std::move(sink.bytes);
}

}  // namespace fw::doc::docx
