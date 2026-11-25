// The blank presentation's source (P2, p2-slides): the generator of the bytes NewDeck.cpp ships
// (NewDeckData.inc). Compiled into the tests only, which check the two agree.

#include "slide/view/NewDeck.h"

#include <span>
#include <string_view>
#include <utility>

#include "core/Strings.h"
#include "draw/Theme.h"
#include "pkg/Zip.h"
#include "slide/Text.h"

namespace fw::slide::view {

namespace {

constexpr std::string_view kDecl = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n";
constexpr std::string_view kNs =
    " xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\""
    " xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\""
    " xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\"";
constexpr std::string_view kRel = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/";
constexpr std::string_view kCt = "application/vnd.openxmlformats-officedocument.presentationml.";
constexpr std::string_view kRelsOpen = "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">";
constexpr std::string_view kDateField = "{B7F3B5D6-2B7A-4E43-9B44-6A0C3C1E8F21}";
constexpr std::string_view kNumberField = "{4C1F2E64-8D3A-4F0B-A1B6-2E5F0C9D7A13}";
constexpr int kLayouts = 11;

std::string Rel(int id, std::string_view type, std::string_view target) {
  return "<Relationship Id=\"rId" + std::to_string(id) + "\" Type=\"" + std::string(kRel) + std::string(type) + "\" Target=\"" +
         std::string(target) + "\"/>";
}

std::string Xfrm(long long x, long long y, long long cx, long long cy) {
  return "<a:xfrm><a:off x=\"" + std::to_string(x) + "\" y=\"" + std::to_string(y) + "\"/><a:ext cx=\"" + std::to_string(cx) +
         "\" cy=\"" + std::to_string(cy) + "\"/></a:xfrm>";
}

std::string Run(std::string_view text) {
  return "<a:r><a:rPr lang=\"en-US\"/><a:t>" + std::string(text) + "</a:t></a:r>";
}

std::string Para(std::string_view content, int level = 0) {
  std::string p = "<a:p>";
  if (level > 0) p += "<a:pPr lvl=\"" + std::to_string(level) + "\"/>";
  return p + std::string(content) + "<a:endParaRPr lang=\"en-US\"/></a:p>";
}

// The prompts PowerPoint's master and layouts hold.
std::string Prompt(char kind) {
  switch (kind) {
    case 't': return Para(Run("Click to edit Master title style"));
    case 's': return Para(Run("Click to edit Master subtitle style"));
    case '1': return Para(Run("Click to edit Master text styles"));
    case 'b': {
      const char* levels[] = {"Click to edit Master text styles", "Second level", "Third level", "Fourth level", "Fifth level"};
      std::string out;
      for (int i = 0; i < 5; ++i) out += Para(Run(levels[i]), i);
      return out;
    }
    case 'd':
      return Para("<a:fld id=\"" + std::string(kDateField) + "\" type=\"datetimeFigureOut\"><a:rPr lang=\"en-US\"/><a:t>" +
                  "11/22/2026</a:t></a:fld>");  // the text when generated: shown text is computed when drawn
    case 'n':
      return Para("<a:fld id=\"" + std::string(kNumberField) +
                  "\" type=\"slidenum\"><a:rPr lang=\"en-US\"/><a:t>\xE2\x80\xB9#\xE2\x80\xBA</a:t></a:fld>");
    default: return Para("");
  }
}

// One placeholder: `ph` is p:ph's attributes; no position (x < 0) inherits the master's.
struct Ph {
  const char* name;
  const char* ph;
  long long x, y, cx, cy;
  const char* bodyPr;   // a:bodyPr's attributes
  const char* lstStyle; // a:lstStyle's content
  char prompt;
};

std::string Shape(int id, const Ph& p, bool master) {
  std::string spPr = "<p:spPr/>";
  if (p.x >= 0) spPr = "<p:spPr>" + Xfrm(p.x, p.y, p.cx, p.cy) + (master ? "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom>" : "") + "</p:spPr>";
  return "<p:sp><p:nvSpPr><p:cNvPr id=\"" + std::to_string(id) + "\" name=\"" + p.name + " " + std::to_string(id - 1) +
         "\"/><p:cNvSpPr><a:spLocks noGrp=\"1\"/></p:cNvSpPr><p:nvPr><p:ph " + p.ph + "/></p:nvPr></p:nvSpPr>" + spPr +
         "<p:txBody><a:bodyPr" + (*p.bodyPr ? " " : "") + p.bodyPr + "/><a:lstStyle>" + p.lstStyle + "</a:lstStyle>" + Prompt(p.prompt) +
         "</p:txBody></p:sp>";
}

std::string Tree(const std::string& shapes) {
  return "<p:spTree><p:nvGrpSpPr><p:cNvPr id=\"1\" name=\"\"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr><p:grpSpPr><a:xfrm>"
         "<a:off x=\"0\" y=\"0\"/><a:ext cx=\"0\" cy=\"0\"/><a:chOff x=\"0\" y=\"0\"/><a:chExt cx=\"0\" cy=\"0\"/></a:xfrm>"
         "</p:grpSpPr>" + shapes + "</p:spTree>";
}

// A text style level: indent and size; `bullet` a bullet, `spcBef` points before (x100).
std::string Level(const char* tag, int n, long long marL, long long indent, int size, bool bullet, int spcBef, bool title) {
  std::string x = "<a:" + std::string(tag) + " marL=\"" + std::to_string(marL) + "\" indent=\"" + std::to_string(indent) +
                  "\" algn=\"l\" defTabSz=\"914400\" rtl=\"0\" eaLnBrk=\"1\" latinLnBrk=\"0\" hangingPunct=\"1\">";
  (void)n;
  if (title || bullet) x += "<a:lnSpc><a:spcPct val=\"90000\"/></a:lnSpc>";
  if (title) x += "<a:spcBef><a:spcPct val=\"0\"/></a:spcBef><a:buNone/>";
  if (bullet)
    x += "<a:spcBef><a:spcPts val=\"" + std::to_string(spcBef) + "\"/></a:spcBef><a:buFont typeface=\"Arial\" "
         "panose=\"020B0604020202020204\" pitchFamily=\"34\" charset=\"0\"/><a:buChar char=\"\xE2\x80\xA2\"/>";
  const char* face = title ? "mj" : "mn";
  x += "<a:defRPr sz=\"" + std::to_string(size) + "\" kern=\"1200\"><a:solidFill><a:schemeClr val=\"tx1\"/></a:solidFill>"
       "<a:latin typeface=\"+" + face + "-lt\"/><a:ea typeface=\"+" + face + "-ea\"/><a:cs typeface=\"+" + face +
       "-cs\"/></a:defRPr></a:" + tag + ">";
  return x;
}

// Nine plain levels (other text, the presentation's default text style).
std::string PlainLevels() {
  std::string x = "<a:defPPr><a:defRPr lang=\"en-US\"/></a:defPPr>";
  for (int i = 1; i <= 9; ++i) {
    const std::string tag = "lvl" + std::to_string(i) + "pPr";
    x += Level(tag.c_str(), i, 457200LL * (i - 1), 0, 1800, false, 0, false);
  }
  return x;
}

std::string Master() {
  const char* body5 = "vert=\"horz\" lIns=\"91440\" tIns=\"45720\" rIns=\"91440\" bIns=\"45720\" rtlCol=\"0\"";
  const std::string footStyle = "<a:defRPr sz=\"1200\"><a:solidFill><a:schemeClr val=\"tx1\"><a:tint val=\"75000\"/></a:schemeClr></a:solidFill></a:defRPr>";
  const std::string dtStyle = "<a:lvl1pPr algn=\"l\">" + footStyle + "</a:lvl1pPr>";
  const std::string ftrStyle = "<a:lvl1pPr algn=\"ctr\">" + footStyle + "</a:lvl1pPr>";
  const std::string numStyle = "<a:lvl1pPr algn=\"r\">" + footStyle + "</a:lvl1pPr>";
  const std::string anchorCtr = std::string(body5) + " anchor=\"ctr\"";
  const Ph shapes[] = {
      {"Title Placeholder", "type=\"title\"", 838200, 365125, 10515600, 1325563, "", "", 't'},
      {"Text Placeholder", "type=\"body\" idx=\"1\"", 838200, 1825625, 10515600, 4351338, "", "", 'b'},
      {"Date Placeholder", "type=\"dt\" sz=\"half\" idx=\"2\"", 838200, 6356350, 2743200, 365125, "", "", 'd'},
      {"Footer Placeholder", "type=\"ftr\" sz=\"quarter\" idx=\"3\"", 4038600, 6356350, 4114800, 365125, "", "", ' '},
      {"Slide Number Placeholder", "type=\"sldNum\" sz=\"quarter\" idx=\"4\"", 8610600, 6356350, 2743200, 365125, "", "", 'n'},
  };
  std::string tree;
  for (int i = 0; i < 5; ++i) {
    Ph p = shapes[i];
    std::string s = Shape(i + 2, p, true);
    // The master's bodies: insets, autofit for the title and text, centered for the small ones.
    const std::string empty = "<a:bodyPr/>";
    const std::string with = i == 0   ? "<a:bodyPr " + anchorCtr + "><a:normAutofit/></a:bodyPr>"
                             : i == 1 ? "<a:bodyPr " + std::string(body5) + "><a:normAutofit/></a:bodyPr>"
                                      : "<a:bodyPr " + anchorCtr + "/>";
    s.replace(s.find(empty), empty.size(), with);
    if (i >= 2) {
      const std::string& style = i == 2 ? dtStyle : i == 3 ? ftrStyle : numStyle;
      const std::string lst = "<a:lstStyle></a:lstStyle>";
      s.replace(s.find(lst), lst.size(), "<a:lstStyle>" + style + "</a:lstStyle>");
    }
    tree += s;
  }
  std::string layouts;
  for (int i = 0; i < kLayouts; ++i)
    layouts += "<p:sldLayoutId id=\"" + std::to_string(2147483649LL + i) + "\" r:id=\"rId" + std::to_string(i + 1) + "\"/>";
  std::string title = "<p:titleStyle>" + Level("lvl1pPr", 1, 0, 0, 4400, false, 0, true) + "</p:titleStyle>";
  std::string body = "<p:bodyStyle>";
  const int sizes[9] = {2800, 2400, 2000, 1800, 1800, 1800, 1800, 1800, 1800};
  for (int i = 1; i <= 9; ++i) {
    const std::string tag = "lvl" + std::to_string(i) + "pPr";
    body += Level(tag.c_str(), i, 228600LL + 457200LL * (i - 1), -228600, sizes[i - 1], true, i == 1 ? 1000 : 500, false);
  }
  body += "</p:bodyStyle>";
  return std::string(kDecl) + "<p:sldMaster" + std::string(kNs) + "><p:cSld><p:bg><p:bgRef idx=\"1001\"><a:schemeClr val=\"bg1\"/>"
         "</p:bgRef></p:bg>" + Tree(tree) + "</p:cSld><p:clrMap bg1=\"lt1\" tx1=\"dk1\" bg2=\"lt2\" tx2=\"dk2\" accent1=\"accent1\" "
         "accent2=\"accent2\" accent3=\"accent3\" accent4=\"accent4\" accent5=\"accent5\" accent6=\"accent6\" hlink=\"hlink\" "
         "folHlink=\"folHlink\"/><p:sldLayoutIdLst>" + layouts + "</p:sldLayoutIdLst><p:txStyles>" + title + body +
         "<p:otherStyle>" + PlainLevels() + "</p:otherStyle></p:txStyles></p:sldMaster>";
}

struct LayoutDef {
  const char* name;
  const char* type;
  std::vector<Ph> shapes;
};

std::vector<LayoutDef> LayoutDefs() {
  const char* b = "anchor=\"b\"";
  const char* vert = "vert=\"eaVert\"";
  const char* big = "<a:lvl1pPr algn=\"ctr\"><a:defRPr sz=\"6000\"/></a:lvl1pPr>";
  const char* sub = "<a:lvl1pPr marL=\"0\" indent=\"0\" algn=\"ctr\"><a:buNone/><a:defRPr sz=\"2400\"/></a:lvl1pPr>";
  const char* sec = "<a:lvl1pPr><a:defRPr sz=\"6000\"/></a:lvl1pPr>";
  const char* secBody = "<a:lvl1pPr marL=\"0\" indent=\"0\"><a:buNone/><a:defRPr sz=\"2400\"><a:solidFill><a:schemeClr val=\"tx1\">"
                        "<a:tint val=\"75000\"/></a:schemeClr></a:solidFill></a:defRPr></a:lvl1pPr>";
  const char* head = "<a:lvl1pPr marL=\"0\" indent=\"0\"><a:buNone/><a:defRPr sz=\"2400\" b=\"1\"/></a:lvl1pPr>";
  const char* capTitle = "<a:lvl1pPr><a:defRPr sz=\"3200\"/></a:lvl1pPr>";
  const char* capObj = "<a:lvl1pPr><a:defRPr sz=\"3200\"/></a:lvl1pPr><a:lvl2pPr><a:defRPr sz=\"2800\"/></a:lvl2pPr>"
                       "<a:lvl3pPr><a:defRPr sz=\"2400\"/></a:lvl3pPr><a:lvl4pPr><a:defRPr sz=\"2000\"/></a:lvl4pPr>"
                       "<a:lvl5pPr><a:defRPr sz=\"2000\"/></a:lvl5pPr>";
  const char* capBody = "<a:lvl1pPr marL=\"0\" indent=\"0\"><a:buNone/><a:defRPr sz=\"1600\"/></a:lvl1pPr>";
  const char* pic = "<a:lvl1pPr marL=\"0\" indent=\"0\"><a:buNone/><a:defRPr sz=\"3200\"/></a:lvl1pPr>";
  const Ph title{"Title", "type=\"title\"", -1, 0, 0, 0, "", "", 't'};
  const Ph dt{"Date Placeholder", "type=\"dt\" sz=\"half\" idx=\"10\"", -1, 0, 0, 0, "", "", 'd'};
  const Ph ftr{"Footer Placeholder", "type=\"ftr\" sz=\"quarter\" idx=\"11\"", -1, 0, 0, 0, "", "", ' '};
  const Ph num{"Slide Number Placeholder", "type=\"sldNum\" sz=\"quarter\" idx=\"12\"", -1, 0, 0, 0, "", "", 'n'};
  const Ph content{"Content Placeholder", "idx=\"1\"", -1, 0, 0, 0, "", "", 'b'};
  const Ph capT{"Title", "type=\"title\"", 839788, 457200, 3932237, 1600200, b, capTitle, 't'};
  const Ph capB{"Text Placeholder", "type=\"body\" sz=\"half\" idx=\"2\"", 839788, 2057400, 3932237, 3811588, "", capBody, '1'};
  return {
      {"Title Slide", "title",
       {{"Title", "type=\"ctrTitle\"", 1524000, 1122363, 9144000, 2387600, b, big, 't'},
        {"Subtitle", "type=\"subTitle\" idx=\"1\"", 1524000, 3602038, 9144000, 1655762, "", sub, 's'}, dt, ftr, num}},
      {"Title and Content", "obj", {title, content, dt, ftr, num}},
      {"Section Header", "secHead",
       {{"Title", "type=\"title\"", 831850, 1709738, 10515600, 2852737, b, sec, 't'},
        {"Text Placeholder", "type=\"body\" idx=\"1\"", 831850, 4589463, 10515600, 1500187, "", secBody, '1'}, dt, ftr, num}},
      {"Two Content", "twoObj",
       {title, {"Content Placeholder", "sz=\"half\" idx=\"1\"", 838200, 1825625, 5181600, 4351338, "", "", 'b'},
        {"Content Placeholder", "sz=\"half\" idx=\"2\"", 6172200, 1825625, 5181600, 4351338, "", "", 'b'}, dt, ftr, num}},
      {"Comparison", "twoTxTwoObj",
       {{"Title", "type=\"title\"", 839788, 365125, 10515600, 1325563, "", "", 't'},
        {"Text Placeholder", "type=\"body\" idx=\"1\"", 839788, 1681163, 5157787, 823912, b, head, '1'},
        {"Content Placeholder", "sz=\"half\" idx=\"2\"", 839788, 2505075, 5157787, 3684588, "", "", 'b'},
        {"Text Placeholder", "type=\"body\" sz=\"quarter\" idx=\"3\"", 6172200, 1681163, 5183188, 823912, b, head, '1'},
        {"Content Placeholder", "sz=\"quarter\" idx=\"4\"", 6172200, 2505075, 5183188, 3684588, "", "", 'b'}, dt, ftr, num}},
      {"Title Only", "titleOnly", {title, dt, ftr, num}},
      {"Blank", "blank", {dt, ftr, num}},
      {"Content with Caption", "objTx",
       {capT, {"Content Placeholder", "idx=\"1\"", 5183188, 987425, 6172200, 4873625, "", capObj, 'b'}, capB, dt, ftr, num}},
      {"Picture with Caption", "picTx",
       {capT, {"Picture Placeholder", "type=\"pic\" idx=\"1\"", 5183188, 987425, 6172200, 4873625, "", pic, ' '}, capB, dt, ftr, num}},
      {"Title and Vertical Text", "vertTx",
       {title, {"Vertical Text Placeholder", "type=\"body\" orient=\"vert\" idx=\"1\"", -1, 0, 0, 0, vert, "", 'b'}, dt, ftr, num}},
      {"Vertical Title and Text", "vertTitleAndTx",
       {{"Vertical Title", "type=\"title\" orient=\"vert\"", 8724900, 365125, 2628900, 5811838, vert, "", 't'},
        {"Vertical Text Placeholder", "type=\"body\" orient=\"vert\" idx=\"1\"", 838200, 365125, 7734300, 5811838, vert, "", 'b'}, dt,
        ftr, num}},
  };
}

std::string LayoutXml(const LayoutDef& l) {
  std::string tree;
  int id = 2;
  for (const Ph& p : l.shapes) tree += Shape(id++, p, false);
  return std::string(kDecl) + "<p:sldLayout" + std::string(kNs) + " type=\"" + l.type + "\" preserve=\"1\"><p:cSld name=\"" + l.name +
         "\">" + Tree(tree) + "</p:cSld><p:clrMapOvr><a:masterClrMapping/></p:clrMapOvr></p:sldLayout>";
}

std::string SlideXml() {
  auto empty = [](int id, const char* name, const char* ph) {
    return "<p:sp><p:nvSpPr><p:cNvPr id=\"" + std::to_string(id) + "\" name=\"" + name + "\"/><p:cNvSpPr><a:spLocks noGrp=\"1\"/>"
           "</p:cNvSpPr><p:nvPr><p:ph " + ph + "/></p:nvPr></p:nvSpPr><p:spPr/><p:txBody><a:bodyPr/><a:lstStyle/>"
           "<a:p><a:endParaRPr lang=\"en-US\"/></a:p></p:txBody></p:sp>";
  };
  return std::string(kDecl) + "<p:sld" + std::string(kNs) + "><p:cSld>" +
         Tree(empty(2, "Title 1", "type=\"ctrTitle\"") + empty(3, "Subtitle 2", "type=\"subTitle\" idx=\"1\"")) +
         "</p:cSld><p:clrMapOvr><a:masterClrMapping/></p:clrMapOvr></p:sld>";
}

}  // namespace

std::vector<uint8_t> GenerateBlankPresentation() {
  const std::string ct(kCt);
  std::string types = std::string(kDecl) +
                      "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
                      "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
                      "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
                      "<Override PartName=\"/ppt/presentation.xml\" ContentType=\"" + ct + "presentation.main+xml\"/>"
                      "<Override PartName=\"/ppt/slideMasters/slideMaster1.xml\" ContentType=\"" + ct + "slideMaster+xml\"/>"
                      "<Override PartName=\"/ppt/slides/slide1.xml\" ContentType=\"" + ct + "slide+xml\"/>"
                      "<Override PartName=\"/ppt/presProps.xml\" ContentType=\"" + ct + "presProps+xml\"/>"
                      "<Override PartName=\"/ppt/viewProps.xml\" ContentType=\"" + ct + "viewProps+xml\"/>"
                      "<Override PartName=\"/ppt/theme/theme1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.theme+xml\"/>"
                      "<Override PartName=\"/ppt/tableStyles.xml\" ContentType=\"" + ct + "tableStyles+xml\"/>"
                      "<Override PartName=\"/docProps/app.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.extended-properties+xml\"/>";
  std::string masterRels = std::string(kDecl) + std::string(kRelsOpen);
  std::vector<std::pair<std::string, std::string>> parts;
  const std::vector<LayoutDef> layouts = LayoutDefs();
  for (int i = 0; i < kLayouts; ++i) {
    const std::string n = std::to_string(i + 1);
    types += "<Override PartName=\"/ppt/slideLayouts/slideLayout" + n + ".xml\" ContentType=\"" + ct + "slideLayout+xml\"/>";
    masterRels += Rel(i + 1, "slideLayout", "../slideLayouts/slideLayout" + n + ".xml");
    parts.push_back({"ppt/slideLayouts/slideLayout" + n + ".xml", LayoutXml(layouts[static_cast<size_t>(i)])});
    parts.push_back({"ppt/slideLayouts/_rels/slideLayout" + n + ".xml.rels",
                     std::string(kDecl) + std::string(kRelsOpen) + Rel(1, "slideMaster", "../slideMasters/slideMaster1.xml") + "</Relationships>"});
  }
  masterRels += Rel(kLayouts + 1, "theme", "../theme/theme1.xml") + "</Relationships>";
  types += "</Types>";
  const std::string presentation =
      std::string(kDecl) + "<p:presentation" + std::string(kNs) + " saveSubsetFonts=\"1\"><p:sldMasterIdLst><p:sldMasterId "
      "id=\"2147483648\" r:id=\"rId1\"/></p:sldMasterIdLst><p:sldIdLst><p:sldId id=\"256\" r:id=\"rId2\"/></p:sldIdLst>"
      "<p:sldSz cx=\"12192000\" cy=\"6858000\"/><p:notesSz cx=\"6858000\" cy=\"9144000\"/><p:defaultTextStyle>" + PlainLevels() +
      "</p:defaultTextStyle></p:presentation>";
  parts.insert(parts.begin(),
               {
                   {"[Content_Types].xml", types},
                   {"_rels/.rels", std::string(kDecl) + std::string(kRelsOpen) + Rel(1, "officeDocument", "ppt/presentation.xml") +
                                       Rel(2, "extended-properties", "docProps/app.xml") + "</Relationships>"},
                   {"docProps/app.xml", std::string(kDecl) + "<Properties xmlns=\"http://schemas.openxmlformats.org/officeDocument/2006/"
                                                             "extended-properties\"><Application>Featherweight</Application>"
                                                             "<PresentationFormat>Widescreen</PresentationFormat><Slides>1</Slides></Properties>"},
                   {"ppt/presentation.xml", presentation},
                   {"ppt/_rels/presentation.xml.rels",
                    std::string(kDecl) + std::string(kRelsOpen) + Rel(1, "slideMaster", "slideMasters/slideMaster1.xml") +
                        Rel(2, "slide", "slides/slide1.xml") + Rel(3, "presProps", "presProps.xml") + Rel(4, "viewProps", "viewProps.xml") +
                        Rel(5, "theme", "theme/theme1.xml") + Rel(6, "tableStyles", "tableStyles.xml") + "</Relationships>"},
                   {"ppt/slideMasters/slideMaster1.xml", Master()},
                   {"ppt/slideMasters/_rels/slideMaster1.xml.rels", masterRels},
                   {"ppt/slides/slide1.xml", SlideXml()},
                   {"ppt/slides/_rels/slide1.xml.rels", std::string(kDecl) + std::string(kRelsOpen) +
                                                            Rel(1, "slideLayout", "../slideLayouts/slideLayout1.xml") + "</Relationships>"},
                   {"ppt/theme/theme1.xml", std::string(kDecl) + std::string(draw::Theme::DefaultXml())},
                   {"ppt/presProps.xml", std::string(kDecl) + "<p:presentationPr" + std::string(kNs) + "/>"},
                   {"ppt/viewProps.xml", std::string(kDecl) + "<p:viewPr" + std::string(kNs) +
                                             "><p:gridSpacing cx=\"76200\" cy=\"76200\"/></p:viewPr>"},
                   {"ppt/tableStyles.xml", std::string(kDecl) + "<a:tblStyleLst xmlns:a=\"http://schemas.openxmlformats.org/"
                                                               "drawingml/2006/main\" def=\"{5C22544A-7EE6-4342-B048-85BDC9FD1C3A}\"/>"},
               });
  pkg::MemorySink sink;
  pkg::ZipWriter zip(sink);
  for (const auto& [name, xml] : parts)
    if (!zip.Add(name, std::span(reinterpret_cast<const uint8_t*>(xml.data()), xml.size())).Ok()) return {};
  if (!zip.Finish().Ok()) return {};
  return std::move(sink.bytes);
}

}  // namespace fw::slide::view
