#include "doc/docx/Blank.h"

#include <cstring>
#include <string_view>

#include "doc/docx/Save.h"
#include "pkg/Snapshot.h"
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

std::string Styles(const std::string& lang, bool placeholders) {
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
  if (placeholders)  // Word's own, for the templates' placeholders
    s += "<w:style w:type=\"character\" w:styleId=\"PlaceholderText\"><w:name w:val=\"Placeholder Text\"/><w:basedOn "
         "w:val=\"DefaultParagraphFont\"/><w:uiPriority w:val=\"99\"/><w:semiHidden/><w:rPr><w:color "
         "w:val=\"808080\"/></w:rPr></w:style>";
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

// ---- The templates' text. Placeholders are in brackets: select one and type.

constexpr std::string_view kTight = "<w:spacing w:after=\"0\"/>";
constexpr std::string_view kGrey = "<w:color w:val=\"595959\"/>";
#define FW_DOT " \xC2\xB7 "  // a middle dot between spaces
#define FW_DASH " \xE2\x80\x93 "  // an en dash between spaces

// A paragraph with a style (or none), more of w:pPr (after the style, in schema order) and one run.
// The content controls' ids, from 1 in each document (Body starts them).
thread_local int t_nextId = 1;

// Text as runs. Each placeholder in brackets is a content control showing its placeholder, as in
// Word's templates: one click selects it and typing replaces it (the friction review's F16).
std::string Runs(std::string_view text, std::string_view rpr) {
  std::string s;
  const std::string props = rpr.empty() ? "" : "<w:rPr>" + std::string(rpr) + "</w:rPr>";
  auto run = [&](std::string_view t, std::string_view style) {
    s += "<w:r>";
    if (!style.empty()) s += "<w:rPr><w:rStyle w:val=\"" + std::string(style) + "\"/>" + std::string(rpr) + "</w:rPr>";
    else s += props;
    s += "<w:t xml:space=\"preserve\">" + Escape(t) + "</w:t></w:r>";
  };
  size_t i = 0;
  while (i < text.size()) {
    const size_t open = text.find('[', i), close = open == std::string_view::npos ? open : text.find(']', open);
    if (close == std::string_view::npos) {
      run(text.substr(i), {});
      break;
    }
    if (open > i) run(text.substr(i, open - i), {});
    s += "<w:sdt><w:sdtPr>" + props + "<w:id w:val=\"" + std::to_string(t_nextId++) +
         "\"/><w:showingPlcHdr/></w:sdtPr><w:sdtContent>";
    run(text.substr(open, close + 1 - open), "PlaceholderText");
    s += "</w:sdtContent></w:sdt>";
    i = close + 1;
  }
  return s;
}

// A paragraph with a style (or none), more of w:pPr (after the style, in schema order) and its text.
std::string P(std::string_view style, std::string_view text, std::string_view ppr = {}, std::string_view rpr = {}) {
  std::string s = "<w:p>";
  if (!style.empty() || !ppr.empty()) {
    s += "<w:pPr>";
    if (!style.empty()) s += "<w:pStyle w:val=\"" + std::string(style) + "\"/>";
    s += std::string(ppr) + "</w:pPr>";
  }
  return s + Runs(text, rpr) + "</w:p>";
}

std::string Bullet(std::string_view text) {
  return P("ListParagraph", text, "<w:numPr><w:ilvl w:val=\"0\"/><w:numId w:val=\"1\"/></w:numPr>");
}

std::string Letter() {
  std::string s;
  for (const char* line : {"[Your name]", "[Street address]", "[Postcode and city]", "[Email or phone]"})
    s += P("", line, kTight);
  s += P("", "") + P("", "[Date]");
  for (const char* line : {"[Recipient's name]", "[Company]", "[Street address]", "[Postcode and city]"})
    s += P("", line, kTight);
  s += P("", "") + P("", "[Subject]", "", "<w:b/><w:bCs/>") + P("", "Dear [Recipient's name],") +
       P("", "[Start your letter here. To replace text in brackets, click it and type.]") +
       P("", "[Add another paragraph, or delete this one.]") + P("", "Kind regards,", kTight) + P("", "", kTight) +
       P("", "", kTight) + P("", "[Your name]");
  return s;
}

std::string Cv() {
  auto job = [](const char* title, const char* when) { return P("Heading2", title) + P("", when, "", kGrey); };
  return P("Title", "[Your name]") + P("Subtitle", "[Email]" FW_DOT "[Phone]" FW_DOT "[City]" FW_DOT "[Website]") +
         P("Heading1", "Profile") +
         P("", "[Two or three sentences about who you are, what you do best and what you are looking for.]") +
         P("Heading1", "Experience") + job("[Job title], [Company]", "[Start date]" FW_DASH "[End date]") +
         Bullet("[What you were responsible for]") + Bullet("[Something you achieved, with a number if you can]") +
         job("[Job title], [Company]", "[Start date]" FW_DASH "[End date]") + Bullet("[What you were responsible for]") +
         P("Heading1", "Education") + job("[Degree or diploma], [School]", "[Start year]" FW_DASH "[End year]") +
         P("Heading1", "Skills") + Bullet("[A skill, or a tool you know well]") +
         Bullet("[Languages, and how well you speak them]");
}

std::string Report(int width) {
  // A three-column table with a heading row that repeats on each page.
  const std::string w = std::to_string(width / 3), line = "w:val=\"single\" w:sz=\"4\" w:space=\"0\" w:color=\"A6A6A6\"/>";
  std::string table = "<w:tbl><w:tblPr><w:tblW w:w=\"" + std::to_string(width / 3 * 3) + "\" w:type=\"dxa\"/><w:tblBorders>";
  for (const char* side : {"top", "left", "bottom", "right", "insideH", "insideV"})
    table += "<w:" + std::string(side) + " " + line;
  table += "</w:tblBorders></w:tblPr><w:tblGrid>";
  for (int i = 0; i < 3; ++i) table += "<w:gridCol w:w=\"" + w + "\"/>";
  table += "</w:tblGrid>";
  for (int row = 0; row < 3; ++row) {
    table += row == 0 ? "<w:tr><w:trPr><w:tblHeader/></w:trPr>" : "<w:tr>";
    for (const char* head : {"[Item]", "[Value]", "[Notes]"})
      table += "<w:tc><w:tcPr><w:tcW w:w=\"" + w + "\" w:type=\"dxa\"/>" +
               (row == 0 ? "<w:shd w:val=\"clear\" w:color=\"auto\" w:fill=\"D9E2F3\"/>" : "") + "</w:tcPr>" +
               P("", row == 0 ? head : "", kTight, row == 0 ? "<w:b/><w:bCs/>" : "") + "</w:tc>";
    table += "</w:tr>";
  }
  table += "</w:tbl>";
  return P("Title", "[Report title]") + P("Subtitle", "[A one-line summary of the report]") +
         P("", "[Author]" FW_DOT "[Date]", "", kGrey) + P("Heading1", "Summary") +
         P("", "[The main findings and what you recommend, in a few sentences, for readers who read nothing else.]") +
         P("Heading1", "Introduction") + P("", "[Why this report was written, what it covers and how it is organised.]") +
         P("Heading1", "[Findings]") + P("Heading2", "[A topic]") +
         P("", "[What you found. Start new sections with the styles Heading 1 and Heading 2.]") +
         Bullet("[A point]") + Bullet("[Another point]") + table + P("", "") + P("Heading1", "Conclusion") +
         P("", "[What the findings mean, and what should happen next.]");
}

#undef FW_DOT
#undef FW_DASH

// Bullets for the templates' lists: three levels, as Word indents them.
std::string Numbering() {
  std::string s = std::string(kDecl) + "<w:numbering " + std::string(kW) +
                  "><w:abstractNum w:abstractNumId=\"0\"><w:multiLevelType w:val=\"hybridMultilevel\"/>";
  const char* marks[] = {"\xE2\x80\xA2", "\xE2\x80\x93", "\xE2\x80\xA2"};  // bullet, en dash, bullet
  for (int i = 0; i < 3; ++i)
    s += "<w:lvl w:ilvl=\"" + std::to_string(i) + "\"><w:start w:val=\"1\"/><w:numFmt w:val=\"bullet\"/><w:lvlText w:val=\"" +
         marks[i] + "\"/><w:lvlJc w:val=\"left\"/><w:pPr><w:ind w:left=\"" + std::to_string(720 * (i + 1)) +
         "\" w:hanging=\"360\"/></w:pPr></w:lvl>";
  return s + "</w:abstractNum><w:num w:numId=\"1\"><w:abstractNumId w:val=\"0\"/></w:num></w:numbering>";
}

std::string Body(bool a4, Template kind) {
  t_nextId = 1;
  // Word's own: Letter with 1-inch margins; A4 with 2.5 cm (as Word's European templates).
  const std::string page = a4 ? "<w:pgSz w:w=\"11906\" w:h=\"16838\"/><w:pgMar w:top=\"1417\" w:right=\"1417\" "
                                "w:bottom=\"1417\" w:left=\"1417\" w:header=\"708\" w:footer=\"708\" w:gutter=\"0\"/>"
                                "<w:cols w:space=\"708\"/>"
                              : "<w:pgSz w:w=\"12240\" w:h=\"15840\"/><w:pgMar w:top=\"1440\" w:right=\"1440\" "
                                "w:bottom=\"1440\" w:left=\"1440\" w:header=\"720\" w:footer=\"720\" w:gutter=\"0\"/>"
                                "<w:cols w:space=\"720\"/>";
  const std::string text = kind == Template::Letter   ? Letter()
                           : kind == Template::Cv     ? Cv()
                           : kind == Template::Report ? Report(a4 ? 11906 - 2 * 1417 : 12240 - 2 * 1440)
                                                      : "<w:p/>";
  const std::string footer = kind == Template::Report ? "<w:footerReference w:type=\"default\" r:id=\"rId4\"/>" : "";
  return std::string(kDecl) + "<w:document " + std::string(kW) +
         " xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\"><w:body>" + text +
         "<w:sectPr>" + footer + page + "<w:docGrid w:linePitch=\"360\"/></w:sectPr></w:body></w:document>";
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
  const std::string wml = "application/vnd.openxmlformats-officedocument.wordprocessingml.";
  const std::string rel = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/";
  const bool lists = options.kind == Template::Cv || options.kind == Template::Report;
  const bool footer = options.kind == Template::Report;
  std::string types = std::string(kDecl) +
                      "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
                      "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
                      "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
                      "<Override PartName=\"/word/document.xml\" ContentType=\"" + wml + "document.main+xml\"/>"
                      "<Override PartName=\"/word/styles.xml\" ContentType=\"" + wml + "styles+xml\"/>"
                      "<Override PartName=\"/word/settings.xml\" ContentType=\"" + wml + "settings+xml\"/>";
  std::string rels = std::string(kDecl) + "<Relationships " + std::string(kRelsNs) + "><Relationship Id=\"rId1\" Type=\"" +
                     rel + "styles\" Target=\"styles.xml\"/><Relationship Id=\"rId2\" Type=\"" + rel +
                     "settings\" Target=\"settings.xml\"/>";
  if (lists) {
    types += "<Override PartName=\"/word/numbering.xml\" ContentType=\"" + wml + "numbering+xml\"/>";
    rels += "<Relationship Id=\"rId3\" Type=\"" + rel + "numbering\" Target=\"numbering.xml\"/>";
  }
  if (footer) {
    types += "<Override PartName=\"/word/footer1.xml\" ContentType=\"" + wml + "footer+xml\"/>";
    rels += "<Relationship Id=\"rId4\" Type=\"" + rel + "footer\" Target=\"footer1.xml\"/>";
  }
  types += "<Override PartName=\"/docProps/core.xml\" "
           "ContentType=\"application/vnd.openxmlformats-package.core-properties+xml\"/></Types>";
  std::vector<std::pair<std::string, std::string>> parts = {
      {"[Content_Types].xml", types},
      {"_rels/.rels",
       std::string(kDecl) + "<Relationships " + std::string(kRelsNs) + "><Relationship Id=\"rId1\" Type=\"" + rel +
           "officeDocument\" Target=\"word/document.xml\"/>"
           "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/package/2006/relationships/metadata/"
           "core-properties\" Target=\"docProps/core.xml\"/></Relationships>"},
      {"word/document.xml", Body(options.a4, options.kind)},
      {"word/_rels/document.xml.rels", rels + "</Relationships>"},
      {"word/styles.xml", Styles(lang, options.kind != Template::None)},
      {"word/settings.xml", Settings(lang)},
  };
  if (lists) parts.push_back({"word/numbering.xml", Numbering()});
  if (footer)  // the page number, centred
    parts.push_back({"word/footer1.xml", std::string(kDecl) + "<w:ftr " + std::string(kW) +
                                             "><w:p><w:pPr><w:jc w:val=\"center\"/></w:pPr><w:fldSimple w:instr=\" PAGE \">"
                                             "<w:r><w:t>1</w:t></w:r></w:fldSimple></w:p></w:ftr>"});
  parts.push_back({"docProps/core.xml", Core(options.author, options.now)});
  pkg::MemorySink sink;
  pkg::ZipWriter zip(sink);
  for (const auto& [name, text] : parts)
    zip.Add(name, std::span(reinterpret_cast<const uint8_t*>(text.data()), text.size()));
  zip.Finish();
  return std::move(sink.bytes);
}

pkg::Status DocumentFromTemplate(std::span<const uint8_t> dotx, std::vector<uint8_t>& docx, const pkg::Limits& limits) {
  auto snapshot = pkg::Snapshot::FromBytes(std::vector<uint8_t>(dotx.begin(), dotx.end()));
  pkg::ZipReader zip;
  if (pkg::Status st = zip.Open(*snapshot, limits); !st.Ok()) return st;
  pkg::MemorySink sink;
  pkg::ZipWriter out(sink);
  for (const pkg::ZipEntry& e : zip.Entries()) {
    pkg::Status st;
    if (_stricmp(e.name.c_str(), "[Content_Types].xml") == 0) {
      std::vector<uint8_t> bytes;
      if (st = zip.Read(e, bytes); !st.Ok()) return st;
      std::string types(bytes.begin(), bytes.end());
      for (auto [from, to] : {std::pair<std::string_view, std::string_view>{"wordprocessingml.template.main+xml",
                                                                             "wordprocessingml.document.main+xml"},
                              {"ms-word.template.macroEnabledTemplate.main+xml", "ms-word.document.macroEnabled.main+xml"}})
        for (size_t at; (at = types.find(from)) != std::string::npos;) types.replace(at, from.size(), to);
      st = out.Add(e.name, std::span(reinterpret_cast<const uint8_t*>(types.data()), types.size()), &e);
    } else {
      st = out.CopyRaw(zip, e);
    }
    if (!st.Ok()) return st;
  }
  if (pkg::Status st = out.Finish(); !st.Ok()) return st;
  docx = std::move(sink.bytes);
  return {};
}

}  // namespace fw::doc::docx
