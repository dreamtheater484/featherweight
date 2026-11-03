// The reference registry's rules (plan §4.3 rule 2): every place each kind of reference can live,
// and the extensions Office writes, classified.
//
// Word's rules are complete for what D1 reads. Left out on purpose: revision ids (w:ins, w:del,
// ...), which Word doesn't keep unique and renumbers itself; rsids; comment authors (people.xml);
// placeholders naming glossary building blocks (w:placeholder), which cross into the glossary
// document. Excel's and PowerPoint's rules cover the public corpus; these are still to come:
// TODO(S1): table ids and names, pivot cache ids, shared string and style indices (s, dxfId),
//           definedName localSheetId, slicer and timeline caches, external link indices ([1]),
//           threaded comment ids, x14:cfRule ids.
// TODO(P1): comment authors (p:cmAuthor id, p:cm authorId), modern comments, custom show ids,
//           tags (p:custData), guide ids.
#include "pkg/References.h"

#include <tuple>

namespace fw::pkg {
namespace {

constexpr std::string_view R = kOfficeRelationshipsNs;
constexpr std::string_view W = "http://schemas.openxmlformats.org/wordprocessingml/2006/main";
constexpr std::string_view W14 = "http://schemas.microsoft.com/office/word/2010/wordml";
constexpr std::string_view W15 = "http://schemas.microsoft.com/office/word/2012/wordml";
constexpr std::string_view W16Cid = "http://schemas.microsoft.com/office/word/2016/wordml/cid";
constexpr std::string_view W16Cex = "http://schemas.microsoft.com/office/word/2018/wordml/cex";
constexpr std::string_view Wp = "http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing";
constexpr std::string_view A = "http://schemas.openxmlformats.org/drawingml/2006/main";
constexpr std::string_view A14 = "http://schemas.microsoft.com/office/drawing/2010/main";
constexpr std::string_view V = "urn:schemas-microsoft-com:vml";
constexpr std::string_view O = "urn:schemas-microsoft-com:office:office";
constexpr std::string_view Xv = "urn:schemas-microsoft-com:office:excel";  // Excel's VML vocabulary
constexpr std::string_view X = "http://schemas.openxmlformats.org/spreadsheetml/2006/main";
constexpr std::string_view Xm = "http://schemas.microsoft.com/office/excel/2006/main";
constexpr std::string_view Xdr = "http://schemas.openxmlformats.org/drawingml/2006/spreadsheetDrawing";
constexpr std::string_view C = "http://schemas.openxmlformats.org/drawingml/2006/chart";
constexpr std::string_view C15 = "http://schemas.microsoft.com/office/drawing/2012/chart";
constexpr std::string_view P = "http://schemas.openxmlformats.org/presentationml/2006/main";
constexpr std::string_view P14 = "http://schemas.microsoft.com/office/powerpoint/2010/main";
constexpr std::string_view Ds = "http://schemas.openxmlformats.org/officeDocument/2006/customXml";

RefSite Attr(std::string_view ens, std::string_view element, std::string_view ans, std::string_view attr,
             uint8_t role = kRefers, RefSyntax syntax = RefSyntax::Value) {
  RefSite s;
  s.elementNs = ens;
  s.elementLocal = element;
  s.attrNs = ans;
  s.attrLocal = attr;
  s.role = role;
  s.syntax = syntax;
  return s;
}
// An unprefixed attribute (no namespace), as SpreadsheetML, PresentationML and DrawingML use.
RefSite Plain(std::string_view ens, std::string_view element, std::string_view attr, uint8_t role = kRefers,
              RefSyntax syntax = RefSyntax::Value) {
  return Attr(ens, element, {}, attr, role, syntax);
}
RefSite Text(std::string_view ens, std::string_view element, uint8_t role = kRefers,
             RefSyntax syntax = RefSyntax::Value) {
  return Attr(ens, element, {}, {}, role, syntax);
}
RefSite Lenient(RefSite s) {
  s.lenient = true;
  return s;
}
RefSite When(RefSite s, std::string_view value) {
  s.whenValue = value;
  return s;
}
RefSite Under(RefSite s, std::string_view parent) {
  s.parentLocal = parent;
  return s;
}
RefSite Skip(RefSite s, std::string_view value) {
  s.skipValue = value;
  return s;
}

RefRule Rule(RefKind kind, std::string_view name, DocFormat format, RefScope scope, std::vector<RefSite> sites) {
  RefRule r;
  r.kind = kind;
  r.name = name;
  r.format = format;
  r.scope = scope;
  r.sites = std::move(sites);
  return r;
}

// Excel's rules for sheet names: 1 to 31 characters, none of \ / ? * [ ] :.
bool IsSheetName(std::string_view v) {
  if (v.empty() || v.size() > 31 * 4) return false;  // 31 characters of up to 4 UTF-8 bytes
  size_t chars = 0;
  for (char c : v) {
    if (c == '\\' || c == '/' || c == '?' || c == '*' || c == '[' || c == ']' || c == ':') return false;
    if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++chars;
  }
  return chars <= 31 && v.front() != '\'' && v.back() != '\'';
}

void AddWordRules(ReferenceRegistry& r) {
  using enum RefKind;
  constexpr DocFormat Word = DocFormat::Word;

  RefRule para = Rule(ParagraphId, "paragraph id", Word, RefScope::Document,
                      {Attr(W, "p", W14, "paraId", kDefines), Attr(W, "tr", W14, "paraId", kDefines),
                       Attr(W15, "commentEx", W15, "paraId"), Attr(W15, "commentEx", W15, "paraIdParent"),
                       Attr(W16Cid, "commentId", W16Cid, "paraId")});
  para.unique = para.ignoreCase = true;
  para.valid = IsLongHexId;
  r.AddRule(std::move(para));

  RefRule text = Rule(TextId, "text id", Word, RefScope::Document,
                      {Attr(W, "p", W14, "textId", kDefines), Attr(W, "tr", W14, "textId", kDefines)});
  text.ignoreCase = text.lenient = true;
  text.valid = IsLongHexId;
  r.AddRule(std::move(text));

  // Pairs: a start and an end with the same id, in the same part.
  struct Pair {
    RefKind kind;
    std::string_view name, start, end;
  };
  for (const Pair& p : {Pair{BookmarkId, "bookmark", "bookmarkStart", "bookmarkEnd"},
                        Pair{PermissionId, "permission range", "permStart", "permEnd"},
                        Pair{MoveRangeId, "move source range", "moveFromRangeStart", "moveFromRangeEnd"},
                        Pair{MoveRangeId, "move destination range", "moveToRangeStart", "moveToRangeEnd"},
                        Pair{CustomXmlRangeId, "custom XML insertion", "customXmlInsRangeStart", "customXmlInsRangeEnd"},
                        Pair{CustomXmlRangeId, "custom XML deletion", "customXmlDelRangeStart", "customXmlDelRangeEnd"},
                        Pair{CustomXmlRangeId, "custom XML move source", "customXmlMoveFromRangeStart",
                             "customXmlMoveFromRangeEnd"},
                        Pair{CustomXmlRangeId, "custom XML move destination", "customXmlMoveToRangeStart",
                             "customXmlMoveToRangeEnd"}}) {
    RefRule rule = Rule(p.kind, p.name, Word, RefScope::Part,
                        {Attr(W, p.start, W, "id", kOpens), Attr(W, p.end, W, "id", kCloses)});
    rule.unique = true;
    r.AddRule(std::move(rule));
  }

  RefRule bookmarkName = Rule(BookmarkName, "bookmark", Word, RefScope::Document,
                              {Attr(W, "bookmarkStart", W, "name", kDefines),
                               Lenient(Attr(W, "hyperlink", W, "anchor")),
                               Lenient(Text(W, "instrText", kRefers, RefSyntax::FieldCode)),
                               Lenient(Attr(W, "fldSimple", W, "instr", kRefers, RefSyntax::FieldCode))});
  bookmarkName.unique = bookmarkName.ignoreCase = true;
  r.AddRule(std::move(bookmarkName));

  RefRule comment = Rule(CommentId, "comment", Word, RefScope::Document,
                         {Attr(W, "comment", W, "id", kDefines), Attr(W, "commentRangeStart", W, "id", kOpens | kRefers),
                          Attr(W, "commentRangeEnd", W, "id", kCloses | kRefers),
                          Attr(W, "commentReference", W, "id", kRefers)});
  comment.unique = true;
  r.AddRule(std::move(comment));

  RefRule durable = Rule(CommentDurableId, "comment durable id", Word, RefScope::Document,
                         {Attr(W16Cid, "commentId", W16Cid, "durableId", kDefines),
                          Lenient(Attr(W16Cex, "commentExtensible", W16Cex, "durableId"))});
  durable.unique = durable.ignoreCase = true;
  r.AddRule(std::move(durable));

  RefRule field = Rule(Field, "field", Word, RefScope::Part,
                       {When(Attr(W, "fldChar", W, "fldCharType", kOpens, RefSyntax::None), "begin"),
                        When(Attr(W, "fldChar", W, "fldCharType", kInside, RefSyntax::None), "separate"),
                        When(Attr(W, "fldChar", W, "fldCharType", kCloses, RefSyntax::None), "end")});
  field.nested = true;
  r.AddRule(std::move(field));

  RefRule moveName = Rule(MoveRangeName, "move name", Word, RefScope::Document,
                          {Attr(W, "moveFromRangeStart", W, "name", kDefines), Attr(W, "moveToRangeStart", W, "name")});
  moveName.unique = moveName.lenient = true;
  r.AddRule(std::move(moveName));

  // Notes: the separator notes (-1, 0) are named by settings.xml too.
  for (auto [kind, name, note, list, ref, props] :
       {std::tuple{FootnoteId, "footnote", "footnote", "footnotes", "footnoteReference", "footnotePr"},
        std::tuple{EndnoteId, "endnote", "endnote", "endnotes", "endnoteReference", "endnotePr"}}) {
    RefRule rule = Rule(kind, name, Word, RefScope::Document,
                        {Under(Attr(W, note, W, "id", kDefines), list), Attr(W, ref, W, "id"),
                         Under(Attr(W, note, W, "id"), props)});
    rule.unique = true;
    r.AddRule(std::move(rule));
  }

  // Word falls back to defaults for a missing style or list, so these are warnings.
  RefRule style = Rule(StyleId, "style", Word, RefScope::Document, {Attr(W, "style", W, "styleId", kDefines)});
  for (std::string_view el : {"pStyle", "rStyle", "tblStyle", "basedOn", "next", "link", "numStyleLink", "styleLink"})
    style.sites.push_back(Attr(W, el, W, "val"));
  style.lenient = true;  // not unique: stylesWithEffects.xml repeats styles.xml
  r.AddRule(std::move(style));

  RefRule num = Rule(NumberingId, "numbering", Word, RefScope::Document,
                     {Attr(W, "num", W, "numId", kDefines), Skip(Attr(W, "numId", W, "val"), "0")});
  num.unique = num.lenient = true;
  r.AddRule(std::move(num));

  RefRule abstractNum = Rule(AbstractNumberingId, "abstract numbering", Word, RefScope::Document,
                             {Attr(W, "abstractNum", W, "abstractNumId", kDefines), Attr(W, "abstractNumId", W, "val")});
  abstractNum.unique = abstractNum.lenient = true;
  r.AddRule(std::move(abstractNum));

  RefRule bullet = Rule(PictureBulletId, "picture bullet", Word, RefScope::Document,
                        {Attr(W, "numPicBullet", W, "numPicBulletId", kDefines), Attr(W, "lvlPicBulletId", W, "val")});
  bullet.unique = bullet.lenient = true;
  r.AddRule(std::move(bullet));

  // Word gives every content control an id of its own, and renumbers duplicates.
  RefRule control = Rule(ContentControlId, "content control id", Word, RefScope::Document,
                         {Under(Attr(W, "id", W, "val", kDefines), "sdtPr")});
  control.unique = control.lenient = true;
  r.AddRule(std::move(control));

  RefRule drawing = Rule(DrawingId, "drawing object id", Word, RefScope::Document, {Plain(Wp, "docPr", "id", kDefines)});
  drawing.unique = true;
  drawing.valid = IsUnsignedId;
  r.AddRule(std::move(drawing));

  RefRule spid = Rule(VmlShapeId, "VML shape id", Word, RefScope::Document, {Attr(V, {}, O, "spid", kDefines)});
  spid.unique = true;
  r.AddRule(std::move(spid));
}

void AddExcelRules(ReferenceRegistry& r) {
  using enum RefKind;
  constexpr DocFormat Excel = DocFormat::Excel;

  RefRule sheet = Rule(SheetName, "sheet", Excel, RefScope::Document, {Plain(X, "sheet", "name", kDefines)});
  RefRule cells = Rule(CellRange, "cell reference", Excel, RefScope::Document, {});
  // Formulas name sheets and cells.
  for (const RefSite& site :
       {Text(X, "f"), Text(X, "definedName"), Text(X, "formula"), Text(X, "formula1"), Text(X, "formula2"), Text(Xm, "f"),
        Text(C, "f"), Text(C15, "f"), Plain(X, "hyperlink", "location")}) {
    RefSite s = site;
    s.syntax = RefSyntax::Formula;
    sheet.sites.push_back(s);
    s.role = kAnchors;
    cells.sites.push_back(s);
  }
  sheet.sites.push_back(Lenient(Plain(X, "worksheetSource", "sheet")));  // a pivot cache's source
  sheet.unique = sheet.ignoreCase = true;
  sheet.valid = IsSheetName;
  r.AddRule(std::move(sheet));

  for (const RefSite& site :
       {Plain(X, "c", "r", kAnchors), Plain(X, "dimension", "ref", kAnchors), Plain(X, "mergeCell", "ref", kAnchors),
        Plain(X, "selection", "activeCell", kAnchors), Plain(X, "selection", "sqref", kAnchors, RefSyntax::Ranges),
        Plain(X, "pane", "topLeftCell", kAnchors), Plain(X, "sheetView", "topLeftCell", kAnchors),
        Plain(X, "conditionalFormatting", "sqref", kAnchors, RefSyntax::Ranges),
        Plain(X, "dataValidation", "sqref", kAnchors, RefSyntax::Ranges), Plain(X, "hyperlink", "ref", kAnchors),
        Plain(X, "autoFilter", "ref", kAnchors), Plain(X, "sortState", "ref", kAnchors),
        Plain(X, "sortCondition", "ref", kAnchors), Plain(X, "table", "ref", kAnchors), Plain(X, "comment", "ref", kAnchors),
        Plain(X, "f", "ref", kAnchors), Plain(X, "protectedRange", "sqref", kAnchors, RefSyntax::Ranges),
        Plain(X, "ignoredError", "sqref", kAnchors, RefSyntax::Ranges), Text(Xm, "sqref", kAnchors, RefSyntax::Ranges)})
    cells.sites.push_back(site);
  r.AddRule(std::move(cells));

  RefRule sheetId = Rule(SheetId, "sheet id", Excel, RefScope::Document, {Plain(X, "sheet", "sheetId", kDefines)});
  sheetId.unique = true;
  sheetId.valid = IsUnsignedId;
  r.AddRule(std::move(sheetId));

  // Notes and form controls in VML name their cell by row and column (0-based); fwtool check
  // matches notes with the comments they show.
  r.AddRule(Rule(VmlAnchor, "VML anchor", Excel, RefScope::Part,
                 {Text(Xv, "Row", kAnchors), Text(Xv, "Column", kAnchors), Text(Xv, "Anchor", kAnchors, RefSyntax::List)}));

  RefRule drawing = Rule(DrawingId, "drawing object id", Excel, RefScope::Part,
                         {Plain(Xdr, "cNvPr", "id", kDefines), Plain(A, "stCxn", "id"), Plain(A, "endCxn", "id")});
  drawing.unique = true;
  drawing.valid = IsUnsignedId;
  r.AddRule(std::move(drawing));

  // Form controls drawn in DrawingML keep a VML twin, which a14:compatExt names.
  RefRule vml = Rule(VmlShapeId, "VML shape id", Excel, RefScope::Document,
                     {Plain(V, "shape", "id", kDefines), Lenient(Plain(A14, "compatExt", "spid"))});
  r.AddRule(std::move(vml));
}

void AddPowerPointRules(ReferenceRegistry& r) {
  using enum RefKind;
  constexpr DocFormat PowerPoint = DocFormat::PowerPoint;

  RefRule slide = Rule(SlideId, "slide id", PowerPoint, RefScope::Document,
                       {Plain(P, "sldId", "id", kDefines), Plain(P14, "sldId", "id")});  // p14: sections
  slide.unique = true;
  slide.valid = IsSlideId;
  r.AddRule(std::move(slide));

  RefRule master = Rule(SlideMasterId, "slide master or layout id", PowerPoint, RefScope::Document,
                        {Plain(P, "sldMasterId", "id", kDefines), Plain(P, "sldLayoutId", "id", kDefines)});
  master.unique = true;
  master.valid = IsSlideMasterId;
  r.AddRule(std::move(master));

  // Shape ids are unique on their slide; animations and connectors name them.
  RefRule shape = Rule(DrawingId, "shape id", PowerPoint, RefScope::Part, {Plain(P, "cNvPr", "id", kDefines)});
  for (std::string_view el : {"spTgt", "bldP", "bldGraphic", "bldOleChart", "bldDgm", "inkTgt"})
    shape.sites.push_back(Plain(P, el, "spid"));
  shape.sites.push_back(Plain(A, "stCxn", "id"));
  shape.sites.push_back(Plain(A, "endCxn", "id"));
  shape.unique = true;
  shape.valid = IsUnsignedId;
  r.AddRule(std::move(shape));
}

}  // namespace

const std::vector<KnownExtension>& OfficeExtensions() {
  using enum ExtensionClass;
  // Only URIs Office is known to write. Anything else is Unknown, so edits near it are blocked.
  static const std::vector<KnownExtension> kExtensions = {
      // [MS-XLSX]
      {"{78C0D931-6437-407d-A8EE-F0AAD7539E65}", "x14:conditionalFormattings", Registry},  // xm:sqref, xm:f
      {"{CCE6A557-97BC-4b89-ADB6-D9C93CAAB3DF}", "x14:dataValidations", Registry},         // xm:sqref, xm:f
      {"{05C60535-1F16-4fd2-B633-F4F36F0B64E0}", "x14:sparklineGroups", Registry},         // xm:f, xm:sqref
      {"{B025F937-C7B1-47D3-B67F-A62EFF666E3E}", "x14:id", ReferenceFree},  // a GUID no structural edit changes
      {"{A8765BA9-456A-4dab-B4F3-ACF838C121DE}", "x14:slicerList", Registry},       // r:id
      {"{3A4CF648-6AED-40f4-86FF-DC5316D8AED3}", "x15:slicerList", Registry},       // r:id
      {"{876F7934-8845-4945-9796-88D515C7AA90}", "x14:pivotCaches", Registry},      // r:id
      {"{BBE1A952-AA13-448e-AADC-164F8A28A991}", "x14:slicerCaches", Registry},     // r:id
      {"{46BE6895-7355-4a93-B00E-2C351335B9C9}", "x15:slicerCaches", Registry},     // r:id
      {"{7E03D99C-DC04-49d9-9315-930204A7B6E9}", "x15:timelineRefs", Registry},     // r:id
      {"{D0CA8CA8-9F24-4464-BF8E-62219DCF47F9}", "x15:timelineCacheRefs", Registry},  // r:id
      {"{79F54976-1DA5-4618-B147-4CDE4B953A38}", "x14:workbookPr", ReferenceFree},
      {"{140A7094-0E35-4892-8432-C4D2E57EDEB5}", "x15:workbookPr", ReferenceFree},
      {"{B58B0392-4F1F-4190-BB64-5DF3571DCE5F}", "xcalcf:calcFeatures", ReferenceFree},
      {"{504A1905-F514-4f6f-8877-14C23A59335A}", "x14:table", ReferenceFree},  // alternative text
      {"{962EF5D1-5CA2-4c93-8EF4-DBF5C05439D2}", "x14:pivotTableDefinition", ReferenceFree},
      // Table names, pivot cache ids and style indices aren't in the registry yet (TODO(S1)).
      {"{FCE2AD5D-F65C-4FA6-A056-5C36A1767C68}", "x15:dataModel", Unknown},
      {"{725AE2AE-9491-48be-B2B4-4EB974FC3084}", "x14:pivotCacheDefinition", Unknown},
      {"{EB79DEF2-80B8-43e5-95BD-54CBDDF9020C}", "x14:slicerStyles", Unknown},
      {"{9260A510-F301-46a8-8635-F512D64BE5F5}", "x15:timelineStyles", Unknown},
      // [MS-ODRAWXML]: charts
      {"{C3380CC4-5D6E-409C-BE32-E72D297353CC}", "c16:uniqueId", ReferenceFree},
      {"{02D57815-91ED-43cb-92C2-25804820EDAC}", "c15 series (filteredSeriesTitle, datalabelsRange)", Registry},  // c15:f
      {"{CE6537A1-D6FC-4f65-9D91-7224C49458BB}", "c15 data labels (dlblFieldTable)", Registry},  // c15:f
      {"{56B9EC1D-385E-4148-901F-78D8002777C0}", "c16r3:dataDisplayOptions16", ReferenceFree},
      // [MS-ODRAWXML]: DrawingML
      {"{28A0092B-C50C-407E-A947-70E740481C1C}", "a14:useLocalDpi", ReferenceFree},
      {"{BEBA8EAE-BF5A-486C-A8C5-ECC9F3942E4B}", "a14:imgProps", Registry},   // r:embed
      {"{96DAC541-7B7A-43D3-8B79-37D633B846F1}", "asvg:svgBlip", Registry},   // r:embed
      {"{FF2B5EF4-FFF2-40B4-BE49-F238E27FC236}", "a16:creationId", ReferenceFree},
      {"{C183D7F6-B498-43B3-948B-1728B52AA6E4}", "adec:decorative", ReferenceFree},
      {"{53640926-AAD7-44D8-BBD7-CCE9431645EC}", "a14:shadowObscured", ReferenceFree},
      {"{909E8E84-426E-40DD-AFC4-6F175D3DCCD1}", "a14:hiddenFill", Registry},  // a picture fill: r:embed
      {"{91240B29-F687-4F45-9708-019B960494DF}", "a14:hiddenLine", ReferenceFree},
      {"{AF507438-7753-43E0-B8FC-AC1667EBCBE1}", "a14:hiddenEffects", ReferenceFree},
      {"{63B3BB69-23CF-44E3-9099-C40C66FF867C}", "a14:compatExt", Registry},  // spid: the VML twin
      {"{05A4C25C-085E-4340-85A3-A5531E510DB2}", "thm15:themeFamily", ReferenceFree},
      // [MS-PPTX]
      {"{521415D9-36F7-43E2-AB2F-B90AF26B5E84}", "p14:sectionLst", Registry},  // slide ids
      {"{DAA4B4D4-6D2D-4DE8-9B6F-3A2D2ED3ED7B}", "p14:media", Registry},       // r:embed, r:link
      {"{D42A27DB-BD31-4B8C-83A1-F6EECF244321}", "p14:modId", ReferenceFree},
      {"{BB962C8B-B14F-4D97-AF65-F5344CB8AC3E}", "p14:creationId", ReferenceFree},
      {"{EFAFB233-063F-42B5-8137-9DF3F51BA10A}", "p15:sldGuideLst", ReferenceFree},
      {"{DCECCB84-F9BA-43D5-87BE-67443E8EF086}", "p15:sldGuideLst", ReferenceFree},
      {"{2D200454-40CA-4A62-9FC3-DE9A4176ACB9}", "p15:notesGuideLst", ReferenceFree},
      {"{E76CE94A-603C-4142-B9EB-6D1370010A27}", "p14:discardImageEditData", ReferenceFree},
      {"{D31A062A-798A-4329-ABDD-BBA856620510}", "p14:defaultImageDpi", ReferenceFree},
      {"{FD5EFAAD-0ECE-453E-9831-46B23BE46B34}", "p15:chartTrackingRefBased", ReferenceFree},
      {"{3A86A75C-4F4B-4683-9AE1-C65F6400EC91}", "p14:laserClr", ReferenceFree},
      {"{2FDB2607-1784-4EEB-B798-7EB5836EED8A}", "p14:showMediaCtrls", ReferenceFree},
      // [MS-DOCX]: Word extends WordprocessingML with w14, w15 and w16 markup rather than extLst;
      // its extensions are DrawingML's, above.
  };
  return kExtensions;
}

ReferenceRegistry ReferenceRegistry::Default() {
  ReferenceRegistry r;
  // Every attribute in the relationships namespace holds a relationship id (ST_RelationshipId):
  // r:id, r:embed, r:link, r:pict, r:dm, r:lo, r:qs, r:cs, ... VML also uses o:relid.
  r.AddRule(Rule(RefKind::RelationshipId, "relationship id", DocFormat::Any, RefScope::Part,
                 {Attr({}, {}, R, {}), Attr({}, {}, O, "relid")}));
  // Shape types are defined and used within a VML part (or a Word story); Office knows the
  // built-in ones without a definition, hence lenient.
  RefRule shapeType = Rule(RefKind::VmlShapeType, "VML shape type", DocFormat::Any, RefScope::Part,
                           {Plain(V, "shapetype", "id", kDefines), Plain(V, "shape", "type", kRefers, RefSyntax::Fragment)});
  shapeType.lenient = true;
  r.AddRule(std::move(shapeType));
  // Content controls bind to custom XML parts by their item id.
  RefRule store = Rule(RefKind::DataStoreItemId, "custom XML item", DocFormat::Any, RefScope::Document,
                       {Attr(Ds, "datastoreItem", Ds, "itemID", kDefines), Attr(W, "dataBinding", W, "storeItemID")});
  store.unique = store.ignoreCase = store.lenient = true;
  r.AddRule(std::move(store));

  AddWordRules(r);
  AddExcelRules(r);
  AddPowerPointRules(r);
  for (const auto& e : OfficeExtensions()) r.AddExtension(std::string(e.uri), e.cls);
  return r;
}

}  // namespace fw::pkg
