#include "doc/model/Schema.h"

#include <algorithm>
#include <unordered_map>

#include "pkg/XmlPull.h"

namespace fw::doc {
namespace {

// The vocabulary, one type per line: "Type = tokens".
//   @attr         a known attribute (in w: unless prefixed: @r:id, @w14:paraId, @xml:space)
//   name:Type     a known child and its type (in w: unless prefixed: m:oMath:Math)
//   +Type         everything Type knows (groups such as InlineContent)
//   !role         para, run, text, object, marker, story, or char=XXXX (hex code unit)
// Children are listed in schema order (ECMA-376 Part 1, §17), which is where editing operations
// insert new elements; reading and writing keep the document's own order. Elements a type doesn't
// list are kept as they are, and so are the attributes it doesn't list. A new child goes after the
// kept elements that follow the one before it, so a type children are added to lists all of its
// schema's (Settings: all of CT_Settings but m:mathPr and sl:schemaLibrary, which nothing is
// inserted before; d1-corpus torture finding 27), with Empty for those the model doesn't read.
constexpr std::string_view kSchema = R"(
OnOff = @val
Val = @val
Empty =
RFonts = @ascii @hAnsi @eastAsia @cs @hint @asciiTheme @hAnsiTheme @eastAsiaTheme @cstheme
Color = @val @themeColor @themeTint @themeShade
Underline = @val @color @themeColor @themeTint @themeShade
Border = @val @color @themeColor @themeTint @themeShade @sz @space @shadow @frame
Shd = @val @color @themeColor @themeTint @themeShade @fill @themeFill @themeFillTint @themeFillShade
Lang = @val @eastAsia @bidi
FitText = @val @id
EastAsianLayout = @id @combine @combineBrackets @vert @vertCompress
Change = @id @author @date
Width = @w @type
NumFmt = @val @format
RelId = @r:id

RPrBase = rStyle:Val rFonts:RFonts b:OnOff bCs:OnOff i:OnOff iCs:OnOff caps:OnOff smallCaps:OnOff strike:OnOff dstrike:OnOff outline:OnOff shadow:OnOff emboss:OnOff imprint:OnOff noProof:OnOff snapToGrid:OnOff vanish:OnOff webHidden:OnOff color:Color spacing:Val w:Val kern:Val position:Val sz:Val szCs:Val highlight:Val u:Underline effect:Val bdr:Border shd:Shd fitText:FitText vertAlign:Val rtl:OnOff cs:OnOff em:Val lang:Lang eastAsianLayout:EastAsianLayout specVanish:OnOff oMath:OnOff
RPrChange = @id @author @date rPr:RPrBase
RPr = +RPrBase rPrChange:RPrChange
ParaRPr = ins:Change del:Change moveFrom:Change moveTo:Change +RPrBase rPrChange:RPrChange

Tab = @val @leader @pos
Tabs = tab:Tab
NumPr = ilvl:Val numId:Val ins:Change
PBdr = top:Border left:Border bottom:Border right:Border between:Border bar:Border
FramePr = @dropCap @lines @w @h @vSpace @hSpace @wrap @hAnchor @vAnchor @x @xAlign @y @yAlign @hRule @anchorLock
Spacing = @before @beforeLines @beforeAutospacing @after @afterLines @afterAutospacing @line @lineRule
Ind = @start @startChars @end @endChars @left @leftChars @right @rightChars @hanging @hangingChars @firstLine @firstLineChars
CnfStyle = @val @firstRow @lastRow @firstColumn @lastColumn @oddVBand @evenVBand @oddHBand @evenHBand @firstRowFirstColumn @firstRowLastColumn @lastRowFirstColumn @lastRowLastColumn
PPrBase = pStyle:Val keepNext:OnOff keepLines:OnOff pageBreakBefore:OnOff framePr:FramePr widowControl:OnOff numPr:NumPr suppressLineNumbers:OnOff pBdr:PBdr shd:Shd tabs:Tabs suppressAutoHyphens:OnOff kinsoku:OnOff wordWrap:OnOff overflowPunct:OnOff topLinePunct:OnOff autoSpaceDE:OnOff autoSpaceDN:OnOff bidi:OnOff adjustRightInd:OnOff snapToGrid:OnOff spacing:Spacing ind:Ind contextualSpacing:OnOff mirrorIndents:OnOff suppressOverlap:OnOff jc:Val textDirection:Val textAlignment:Val textboxTightWrap:Val outlineLvl:Val divId:Val cnfStyle:CnfStyle
PPrChange = @id @author @date pPr:PPrBase
PPr = +PPrBase rPr:ParaRPr sectPr:SectPr pPrChange:PPrChange
PPrGeneral = +PPrBase pPrChange:PPrChange

HdrFtrRef = @type @r:id
NotePr = pos:Val numFmt:NumFmt numStart:Val numRestart:Val
PgSz = @w @h @orient @code
PgMar = @top @right @bottom @left @header @footer @gutter
PaperSrc = @first @other
PgBorder = @val @color @themeColor @themeTint @themeShade @sz @space @shadow @frame @r:id
PgBorderTop = +PgBorder @r:topLeft @r:topRight
PgBorderBottom = +PgBorder @r:bottomLeft @r:bottomRight
PgBorders = @zOrder @display @offsetFrom top:PgBorderTop left:PgBorder bottom:PgBorderBottom right:PgBorder
LnNumType = @countBy @start @distance @restart
PgNumType = @fmt @start @chapStyle @chapSep
Col = @w @space
Cols = @equalWidth @space @num @sep col:Col
DocGrid = @type @linePitch @charSpace
SectPrBase = footnotePr:NotePr endnotePr:NotePr type:Val pgSz:PgSz pgMar:PgMar paperSrc:PaperSrc pgBorders:PgBorders lnNumType:LnNumType pgNumType:PgNumType cols:Cols formProt:OnOff vAlign:Val noEndnote:OnOff titlePg:OnOff textDirection:Val bidi:OnOff rtlGutter:OnOff docGrid:DocGrid printerSettings:RelId
SectPrChange = @id @author @date sectPr:SectPrBase
SectPr = @rsidRPr @rsidDel @rsidR @rsidSect headerReference:HdrFtrRef footerReference:HdrFtrRef +SectPrBase sectPrChange:SectPrChange

TblpPr = @leftFromText @rightFromText @topFromText @bottomFromText @vertAnchor @horzAnchor @tblpXSpec @tblpX @tblpYSpec @tblpY
TblBorders = top:Border start:Border left:Border bottom:Border end:Border right:Border insideH:Border insideV:Border
TcBorders = +TblBorders tl2br:Border tr2bl:Border
CellMar = top:Width start:Width left:Width bottom:Width end:Width right:Width
TblLayout = @type
TblLook = @val @firstRow @lastRow @firstColumn @lastColumn @noHBand @noVBand
TblPrBase = tblStyle:Val tblpPr:TblpPr tblOverlap:Val bidiVisual:OnOff tblStyleRowBandSize:Val tblStyleColBandSize:Val tblW:Width jc:Val tblCellSpacing:Width tblInd:Width tblBorders:TblBorders shd:Shd tblLayout:TblLayout tblCellMar:CellMar tblLook:TblLook tblCaption:Val tblDescription:Val
TblPrChange = @id @author @date tblPr:TblPrBase
TblPr = +TblPrBase tblPrChange:TblPrChange
TblPrExBase = tblW:Width jc:Val tblCellSpacing:Width tblInd:Width tblBorders:TblBorders shd:Shd tblLayout:TblLayout tblCellMar:CellMar tblLook:TblLook
TblPrExChange = @id @author @date tblPrEx:TblPrExBase
TblPrEx = +TblPrExBase tblPrExChange:TblPrExChange
GridCol = @w
TblGridBase = gridCol:GridCol
TblGridChange = @id tblGrid:TblGridBase
TblGrid = gridCol:GridCol tblGridChange:TblGridChange
TrHeight = @val @hRule
TrPrBase = cnfStyle:CnfStyle divId:Val gridBefore:Val gridAfter:Val wBefore:Width wAfter:Width cantSplit:OnOff trHeight:TrHeight tblHeader:OnOff tblCellSpacing:Width jc:Val hidden:OnOff
TrPrChange = @id @author @date trPr:TrPrBase
TrPr = +TrPrBase ins:Change del:Change trPrChange:TrPrChange
Headers = header:Val
CellMerge = @id @author @date @vMerge @vMergeOrig
TcPrBase = cnfStyle:CnfStyle tcW:Width gridSpan:Val hMerge:Val vMerge:Val tcBorders:TcBorders shd:Shd noWrap:OnOff tcMar:CellMar textDirection:Val tcFitText:OnOff vAlign:Val hideMark:OnOff headers:Headers cellIns:Change cellDel:Change cellMerge:CellMerge
TcPrChange = @id @author @date tcPr:TcPrBase
TcPr = +TcPrBase tcPrChange:TcPrChange

Bookmark = @id @name @colFirst @colLast @displacedByCustomXml
RangeEnd = @id @displacedByCustomXml
MoveRangeStart = @id @name @author @date @colFirst @colLast @displacedByCustomXml
PermStart = @id @edGrp @ed @colFirst @colLast @displacedByCustomXml
TrackStart = @id @author @date
ProofErr = @type
RangeMarkup = bookmarkStart:Bookmark bookmarkEnd:RangeEnd moveFromRangeStart:MoveRangeStart moveFromRangeEnd:RangeEnd moveToRangeStart:MoveRangeStart moveToRangeEnd:RangeEnd commentRangeStart:RangeEnd commentRangeEnd:RangeEnd customXmlInsRangeStart:TrackStart customXmlInsRangeEnd:RangeEnd customXmlDelRangeStart:TrackStart customXmlDelRangeEnd:RangeEnd customXmlMoveFromRangeStart:TrackStart customXmlMoveFromRangeEnd:RangeEnd customXmlMoveToRangeStart:TrackStart customXmlMoveToRangeEnd:RangeEnd permStart:PermStart permEnd:RangeEnd proofErr:ProofErr

Text = !text @xml:space
TabChar = !char=0009
Br = !char=000B @type @clear
Cr = !char=000B
NoBreakHyphen = !char=2011
SoftHyphen = !char=00AD
Sym = !char=FFFD @font @char
PTab = !char=0009 @relativeTo @alignment @leader
FFHelp = @type @val
FFCheckBox = size:Val sizeAuto:OnOff default:OnOff checked:OnOff
FFDDList = result:Val default:Val listEntry:Val
FFTextInput = type:Val default:Val maxLength:Val format:Val
FFData = name:Val label:Val tabIndex:Val enabled:OnOff calcOnExit:OnOff entryMacro:Val exitMacro:Val helpText:FFHelp statusText:FFHelp checkBox:FFCheckBox ddList:FFDDList textInput:FFTextInput
FldChar = !char=0013 @fldCharType @fldLock @dirty ffData:FFData
NoteRef = !char=0002 @customMarkFollows @id
CommentRef = !char=0005 @id
NoteMark = !char=0002
AnnotationRef = !char=0005
Separator = !char=0003
ContSeparator = !char=0004
PgNum = !char=FFFC
DateItem = !char=FFFC
RenderedPageBreak = !marker
Drawing = !object
Pict = !object
Object = !object @dxaOrig @dyaOrig
ContentPart = !object @r:id
Math = !object
R = !run @rsidRPr @rsidDel @rsidR rPr:RPr t:Text delText:Text instrText:Text delInstrText:Text tab:TabChar br:Br cr:Cr noBreakHyphen:NoBreakHyphen softHyphen:SoftHyphen sym:Sym ptab:PTab fldChar:FldChar footnoteReference:NoteRef endnoteReference:NoteRef commentReference:CommentRef footnoteRef:NoteMark endnoteRef:NoteMark annotationRef:AnnotationRef separator:Separator continuationSeparator:ContSeparator pgNum:PgNum dayShort:DateItem monthShort:DateItem yearShort:DateItem dayLong:DateItem monthLong:DateItem yearLong:DateItem lastRenderedPageBreak:RenderedPageBreak drawing:Drawing pict:Pict object:Object contentPart:ContentPart

SmartAttr = @uri @name @val
SmartTagPr = attr:SmartAttr
CustomXmlPr = placeholder:Val attr:SmartAttr
Placeholder = docPart:Val
DataBinding = @prefixMappings @xpath @storeItemID
DocPartObj = docPartGallery:Val docPartCategory:Val docPartUnique:OnOff
SdtDate = @fullDate dateFormat:Val lid:Val storeMappedDataAs:Val calendar:Val
ListItem = @displayText @value
SdtList = @lastValue listItem:ListItem
SdtText = @multiLine
W14OnOff = @w14:val
W14CheckState = @w14:val @w14:font
SdtCheckbox = w14:checked:W14OnOff w14:checkedState:W14CheckState w14:uncheckedState:W14CheckState
SdtPr = rPr:RPrBase alias:Val tag:Val id:Val lock:Val placeholder:Placeholder temporary:OnOff showingPlcHdr:OnOff dataBinding:DataBinding label:Val tabIndex:Val docPartObj:DocPartObj docPartList:DocPartObj date:SdtDate dropDownList:SdtList comboBox:SdtList text:SdtText richText:Empty equation:Empty group:Empty picture:Empty citation:Empty bibliography:Empty w14:checkbox:SdtCheckbox
SdtEndPr = rPr:RPrBase

InlineContent = +RangeMarkup r:R hyperlink:Hyperlink ins:InlineChange del:InlineChange moveFrom:InlineChange moveTo:InlineChange sdt:SdtRun smartTag:SmartTag customXml:CustomXmlRun fldSimple:FldSimple dir:Dir bdo:Dir m:oMath:Math m:oMathPara:Math
Hyperlink = @r:id @anchor @tooltip @tgtFrame @history @docLocation +InlineContent
InlineChange = @id @author @date +InlineContent
InlineGroup = +InlineContent
SdtRun = sdtPr:SdtPr sdtEndPr:SdtEndPr sdtContent:InlineGroup
SmartTag = @uri @element smartTagPr:SmartTagPr +InlineContent
CustomXmlRun = @uri @element customXmlPr:CustomXmlPr +InlineContent
FldSimple = @instr @fldLock @dirty +InlineContent
Dir = @val +InlineContent
P = !para @rsidRPr @rsidR @rsidDel @rsidP @rsidRDefault @w14:paraId @w14:textId pPr:PPr +InlineContent

BlockContent = +RangeMarkup p:P tbl:Tbl sdt:SdtBlock customXml:CustomXmlBlock
BlockGroup = +BlockContent
SdtBlock = sdtPr:SdtPr sdtEndPr:SdtEndPr sdtContent:BlockGroup
CustomXmlBlock = @uri @element customXmlPr:CustomXmlPr +BlockContent
RowContent = +RangeMarkup tr:Tr sdt:SdtRow customXml:CustomXmlRow
RowGroup = +RowContent
SdtRow = sdtPr:SdtPr sdtEndPr:SdtEndPr sdtContent:RowGroup
CustomXmlRow = @uri @element customXmlPr:CustomXmlPr +RowContent
CellContent = +RangeMarkup tc:Tc sdt:SdtCell customXml:CustomXmlCell
CellGroup = +CellContent
SdtCell = sdtPr:SdtPr sdtEndPr:SdtEndPr sdtContent:CellGroup
CustomXmlCell = @uri @element customXmlPr:CustomXmlPr +CellContent
Tbl = tblPr:TblPr tblGrid:TblGrid +RowContent
Tr = @rsidRPr @rsidR @rsidDel @rsidTr @w14:paraId @w14:textId tblPrEx:TblPrEx trPr:TrPr +CellContent
Tc = @id tcPr:TcPr +BlockContent

Background = @color @themeColor @themeTint @themeShade
Body = !story +BlockContent sectPr:SectPr
Document = background:Background body:Body
HdrFtr = !story +BlockContent
Note = !story @type @id +BlockContent
Footnotes = footnote:Note
Endnotes = endnote:Note
Comment = !story @id @author @date @initials +BlockContent
Comments = comment:Comment
TxbxContent = !story +BlockContent

RPrDefault = rPr:RPrBase
PPrDefault = pPr:PPrGeneral
DocDefaults = rPrDefault:RPrDefault pPrDefault:PPrDefault
LsdException = @name @locked @uiPriority @semiHidden @unhideWhenUsed @qFormat
LatentStyles = @defLockedState @defUIPriority @defSemiHidden @defUnhideWhenUsed @defQFormat @count lsdException:LsdException
TblStylePr = @type pPr:PPrGeneral rPr:RPr tblPr:TblPrBase trPr:TrPr tcPr:TcPr
Style = @type @styleId @default @customStyle name:Val aliases:Val basedOn:Val next:Val link:Val autoRedefine:OnOff hidden:OnOff uiPriority:Val semiHidden:OnOff unhideWhenUsed:OnOff qFormat:OnOff locked:OnOff personal:OnOff personalCompose:OnOff personalReply:OnOff rsid:Val pPr:PPrGeneral rPr:RPr tblPr:TblPrBase trPr:TrPr tcPr:TcPr tblStylePr:TblStylePr
Styles = docDefaults:DocDefaults latentStyles:LatentStyles style:Style

LvlText = @val @null
Legacy = @legacy @legacySpace @legacyIndent
Lvl = @ilvl @tplc @tentative start:Val numFmt:NumFmt lvlRestart:Val pStyle:Val isLgl:OnOff suff:Val lvlText:LvlText lvlPicBulletId:Val legacy:Legacy lvlJc:Val pPr:PPrGeneral rPr:RPr
AbstractNum = @abstractNumId nsid:Val multiLevelType:Val tmpl:Val name:Val styleLink:Val numStyleLink:Val lvl:Lvl
LvlOverride = @ilvl startOverride:Val lvl:Lvl
Num = @numId abstractNumId:Val lvlOverride:LvlOverride
NumPicBullet = @numPicBulletId
Numbering = numPicBullet:NumPicBullet abstractNum:AbstractNum num:Num numIdMacAtCleanup:Val
)"
// (Two literals: MSVC takes up to 16 KB in one.)
R"(
NoteIdRef = @id
NoteSettings = pos:Val numFmt:NumFmt numStart:Val numRestart:Val footnote:NoteIdRef endnote:NoteIdRef
CompatSetting = @name @uri @val
Compat = useSingleBorderforContiguousCells:OnOff wpJustification:OnOff noTabHangInd:OnOff noLeading:OnOff spaceForUL:OnOff noColumnBalance:OnOff balanceSingleByteDoubleByteWidth:OnOff noExtraLineSpacing:OnOff doNotLeaveBackslashAlone:OnOff ulTrailSpace:OnOff doNotExpandShiftReturn:OnOff spacingInWholePoints:OnOff lineWrapLikeWord6:OnOff printBodyTextBeforeHeader:OnOff printColBlack:OnOff wpSpaceWidth:OnOff showBreaksInFrames:OnOff subFontBySize:OnOff suppressBottomSpacing:OnOff suppressTopSpacing:OnOff suppressSpacingAtTopOfPage:OnOff suppressTopSpacingWP:OnOff suppressSpBfAfterPgBrk:OnOff swapBordersFacingPages:OnOff convMailMergeEsc:OnOff truncateFontHeightsLikeWP6:OnOff mwSmallCaps:OnOff usePrinterMetrics:OnOff doNotSuppressParagraphBorders:OnOff wrapTrailSpaces:OnOff footnoteLayoutLikeWW8:OnOff shapeLayoutLikeWW8:OnOff alignTablesRowByRow:OnOff forgetLastTabAlignment:OnOff adjustLineHeightInTable:OnOff autoSpaceLikeWord95:OnOff noSpaceRaiseLower:OnOff doNotUseHTMLParagraphAutoSpacing:OnOff layoutRawTableWidth:OnOff layoutTableRowsApart:OnOff useWord97LineBreakRules:OnOff doNotBreakWrappedTables:OnOff doNotSnapToGridInCell:OnOff selectFldWithFirstOrLastChar:OnOff applyBreakingRules:OnOff doNotWrapTextWithPunct:OnOff doNotUseEastAsianBreakRules:OnOff useWord2002TableStyleRules:OnOff growAutofit:OnOff useFELayout:OnOff useNormalStyleForList:OnOff doNotUseIndentAsNumberingTabStop:OnOff useAltKinsokuLineBreakRules:OnOff allowSpaceOfSameStyleInTable:OnOff doNotSuppressIndentation:OnOff doNotAutofitConstrainedTables:OnOff autofitToFirstFixedWidthCell:OnOff underlineTabInNumList:OnOff displayHangulFixedWidth:OnOff splitPgBreakAndParaMark:OnOff doNotVertAlignCellWithSp:OnOff doNotBreakConstrainedForcedTable:OnOff doNotVertAlignInTxbx:OnOff useAnsiKerningPairs:OnOff cachedColBalance:OnOff compatSetting:CompatSetting
ClrSchemeMapping = @bg1 @t1 @bg2 @t2 @accent1 @accent2 @accent3 @accent4 @accent5 @accent6 @hyperlink @followedHyperlink
DocVar = @name @val
DocVars = docVar:DocVar
Rsids = rsidRoot:Val rsid:Val
Settings = writeProtection:Empty view:Empty zoom:Empty removePersonalInformation:Empty removeDateAndTime:Empty doNotDisplayPageBoundaries:Empty displayBackgroundShape:OnOff printPostScriptOverText:Empty printFractionalCharacterWidth:Empty printFormsData:Empty embedTrueTypeFonts:Empty embedSystemFonts:Empty saveSubsetFonts:Empty saveFormsData:Empty mirrorMargins:OnOff alignBordersAndEdges:Empty bordersDoNotSurroundHeader:OnOff bordersDoNotSurroundFooter:OnOff gutterAtTop:OnOff hideSpellingErrors:Empty hideGrammaticalErrors:Empty activeWritingStyle:Empty proofState:Empty formsDesign:Empty attachedTemplate:Empty linkStyles:Empty stylePaneFormatFilter:Empty stylePaneSortMethod:Empty documentType:Empty mailMerge:Empty revisionView:Empty trackRevisions:OnOff doNotTrackMoves:Empty doNotTrackFormatting:Empty documentProtection:Empty autoFormatOverride:Empty styleLockTheme:Empty styleLockQFSet:Empty defaultTabStop:Val autoHyphenation:OnOff consecutiveHyphenLimit:Val hyphenationZone:Val doNotHyphenateCaps:OnOff showEnvelope:Empty summaryLength:Empty clickAndTypeStyle:Empty defaultTableStyle:Val evenAndOddHeaders:OnOff bookFoldRevPrinting:Empty bookFoldPrinting:OnOff bookFoldPrintingSheets:Empty drawingGridHorizontalSpacing:Empty drawingGridVerticalSpacing:Empty displayHorizontalDrawingGridEvery:Empty displayVerticalDrawingGridEvery:Empty doNotUseMarginsForDrawingGridOrigin:Empty drawingGridHorizontalOrigin:Empty drawingGridVerticalOrigin:Empty doNotShadeFormData:Empty noPunctuationKerning:OnOff characterSpacingControl:Val printTwoOnOne:OnOff strictFirstAndLastChars:Empty noLineBreaksAfter:Empty noLineBreaksBefore:Empty savePreviewPicture:Empty doNotValidateAgainstSchema:Empty saveInvalidXml:Empty ignoreMixedContent:Empty alwaysShowPlaceholderText:Empty doNotDemarcateInvalidXml:Empty saveXmlDataOnly:Empty useXSLTWhenSaving:Empty saveThroughXslt:Empty showXMLTags:Empty alwaysMergeEmptyNamespace:Empty updateFields:OnOff hdrShapeDefaults:Empty footnotePr:NoteSettings endnotePr:NoteSettings compat:Compat docVars:DocVars rsids:Rsids attachedSchema:Empty themeFontLang:Lang clrSchemeMapping:ClrSchemeMapping doNotIncludeSubdocsInStats:Empty doNotAutoCompressPictures:Empty forceUpgrade:Empty captions:Empty readModeInkLockDown:Empty smartTagType:Empty shapeDefaults:Empty doNotEmbedSmartTags:Empty decimalSymbol:Val listSeparator:Val

Charset = @val @characterSet
Sig = @usb0 @usb1 @usb2 @usb3 @csb0 @csb1
FontRel = @r:id @fontKey @subsetted
Font = @name altName:Val panose1:Val charset:Charset family:Val notTrueType:OnOff pitch:Val sig:Sig embedRegular:FontRel embedBold:FontRel embedItalic:FontRel embedBoldItalic:FontRel
FontTable = font:Font

Root = document:Document hdr:HdrFtr ftr:HdrFtr footnotes:Footnotes endnotes:Endnotes comments:Comments styles:Styles numbering:Numbering settings:Settings fonts:FontTable txbxContent:TxbxContent
)";

constexpr std::string_view kW = "http://schemas.openxmlformats.org/wordprocessingml/2006/main";
constexpr std::string_view kWStrict = "http://purl.oclc.org/ooxml/wordprocessingml/main";
constexpr std::string_view kR = "http://schemas.openxmlformats.org/officeDocument/2006/relationships";
constexpr std::string_view kRStrict = "http://purl.oclc.org/ooxml/officeDocument/relationships";
constexpr std::string_view kW14 = "http://schemas.microsoft.com/office/word/2010/wordml";
constexpr std::string_view kM = "http://schemas.openxmlformats.org/officeDocument/2006/math";
constexpr std::string_view kMStrict = "http://purl.oclc.org/ooxml/officeDocument/math";
constexpr std::string_view kMc = "http://schemas.openxmlformats.org/markup-compatibility/2006";

std::vector<std::string_view> Tokens(std::string_view line) {
  std::vector<std::string_view> out;
  size_t i = 0;
  while (i < line.size()) {
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    size_t start = i;
    while (i < line.size() && line[i] != ' ' && line[i] != '\t') ++i;
    if (i > start) out.push_back(line.substr(start, i - start));
  }
  return out;
}

// "r:id" -> (R, "id"); "val" -> (W, "val").
std::pair<Ns, std::string_view> SplitName(std::string_view name) {
  size_t colon = name.find(':');
  if (colon == std::string_view::npos) return {Ns::W, name};
  std::string_view prefix = name.substr(0, colon);
  for (Ns ns : {Ns::R, Ns::W14, Ns::M, Ns::Xml})
    if (prefix == NsPrefix(ns)) return {ns, name.substr(colon + 1)};
  return {Ns::Other, name.substr(colon + 1)};
}

}  // namespace

std::string_view NsUri(Ns ns) {
  switch (ns) {
    case Ns::W: return kW;
    case Ns::R: return kR;
    case Ns::W14: return kW14;
    case Ns::M: return kM;
    case Ns::Xml: return pkg::kXmlNamespace;
    case Ns::Mc: return kMc;
    default: return {};
  }
}

std::string_view NsPrefix(Ns ns) {
  switch (ns) {
    case Ns::W: return "w";
    case Ns::R: return "r";
    case Ns::W14: return "w14";
    case Ns::M: return "m";
    case Ns::Xml: return "xml";
    case Ns::Mc: return "mc";
    default: return {};
  }
}

Ns NsFromUri(std::string_view uri) {
  if (uri == kW || uri == kWStrict) return Ns::W;
  if (uri == kR || uri == kRStrict) return Ns::R;
  if (uri == kW14) return Ns::W14;
  if (uri == kM || uri == kMStrict) return Ns::M;
  if (uri == pkg::kXmlNamespace) return Ns::Xml;
  if (uri == kMc) return Ns::Mc;
  if (uri.empty()) return Ns::None;
  return Ns::Other;
}

bool IsStrictUri(std::string_view uri) { return uri == kWStrict || uri == kRStrict || uri == kMStrict; }

uint32_t NameOf(Ns ns, std::string_view local) {
  uint32_t h = 2166136261u;
  auto add = [&](std::string_view s) {
    for (char c : s) {
      h ^= static_cast<uint8_t>(c);
      h *= 16777619u;
    }
  };
  if (ns != Ns::W && ns != Ns::None) {
    // Names outside the model's namespaces get a prefix no known name has.
    add(ns == Ns::Other ? std::string_view("?") : NsPrefix(ns));
    add(":");
  }
  add(local);
  return h;
}

const Schema& Schema::Get() {
  static const Schema schema;
  return schema;
}

Schema::Schema() { Build(kSchema); }

TypeId Schema::FindType(std::string_view name) const {
  for (size_t i = 0; i < types_.size(); ++i)
    if (types_[i].name == name) return static_cast<TypeId>(i);
  return kNoDef;
}

ElemDefId Schema::Child(TypeId parent, uint32_t name) const {
  uint64_t key = (uint64_t{parent} << 32) | name;
  auto it = std::lower_bound(childIndex_.begin(), childIndex_.end(), std::pair<uint64_t, ElemDefId>{key, 0},
                             [](const auto& a, const auto& b) { return a.first < b.first; });
  return it != childIndex_.end() && it->first == key ? it->second : kNoDef;
}

AttrDefId Schema::AttrOf(TypeId type, uint32_t name) const {
  uint64_t key = (uint64_t{type} << 32) | name;
  auto it = std::lower_bound(attrIndex_.begin(), attrIndex_.end(), std::pair<uint64_t, AttrDefId>{key, 0},
                             [](const auto& a, const auto& b) { return a.first < b.first; });
  return it != attrIndex_.end() && it->first == key ? it->second : kNoDef;
}

void Schema::Build(std::string_view text) {
  // Pass 1: every type's name and tokens, so types can refer to ones defined later.
  std::vector<std::vector<std::string_view>> raw;
  std::unordered_map<std::string_view, TypeId> byName;
  for (size_t pos = 0; pos < text.size();) {
    size_t end = text.find('\n', pos);
    if (end == std::string_view::npos) end = text.size();
    std::string_view line = text.substr(pos, end - pos);
    pos = end + 1;
    auto tokens = Tokens(line);
    if (tokens.empty()) continue;
    if (tokens.size() < 2 || tokens[1] != "=") {
      errors_.push_back("malformed line: " + std::string(line));
      continue;
    }
    if (byName.contains(tokens[0])) errors_.push_back("type defined twice: " + std::string(tokens[0]));
    byName[tokens[0]] = static_cast<TypeId>(types_.size());
    types_.push_back(TypeDef{tokens[0]});
    raw.emplace_back(tokens.begin() + 2, tokens.end());
  }

  std::unordered_map<uint32_t, std::string> spelled;  // name -> spelling, to find collisions
  auto checkName = [&](uint32_t name, std::string_view spelling) {
    auto [it, added] = spelled.emplace(name, std::string(spelling));
    if (!added && it->second != spelling) errors_.push_back("names collide: " + it->second + ", " + std::string(spelling));
  };
  auto attrId = [&](Ns ns, std::string_view local) {
    for (size_t i = 0; i < attrs_.size(); ++i)
      if (attrs_[i].ns == ns && attrs_[i].local == local) return static_cast<AttrDefId>(i);
    attrs_.push_back(AttrDef{ns, local, NameOf(ns, local)});
    return static_cast<AttrDefId>(attrs_.size() - 1);
  };
  auto elemId = [&](Ns ns, std::string_view local, TypeId type) {
    for (size_t i = 0; i < elems_.size(); ++i)
      if (elems_[i].ns == ns && elems_[i].local == local && elems_[i].type == type) return static_cast<ElemDefId>(i);
    elems_.push_back(ElemDef{ns, local, NameOf(ns, local), type});
    return static_cast<ElemDefId>(elems_.size() - 1);
  };

  // Pass 2: expand each type, groups included (iteratively, with a guard against cycles).
  for (TypeId t = 0; t < types_.size(); ++t) {
    TypeDef& def = types_[t];
    std::vector<std::pair<TypeId, size_t>> work{{t, 0}};  // (type whose tokens are read, next token)
    std::vector<TypeId> open{t};
    while (!work.empty()) {
      auto& [src, next] = work.back();
      if (next == raw[src].size()) {
        work.pop_back();
        open.pop_back();
        continue;
      }
      std::string_view tok = raw[src][next++];
      if (tok.starts_with('!')) {
        if (src != t) continue;  // a role belongs to its own type, not to those that include it
        std::string_view role = tok.substr(1);
        if (role == "para") def.role = Role::Paragraph;
        else if (role == "run") def.role = Role::Run;
        else if (role == "text") def.role = Role::Text;
        else if (role == "object") def.role = Role::Object;
        else if (role == "marker") def.role = Role::Marker;
        else if (role == "story") def.role = Role::Story;
        else if (role.starts_with("char=")) {
          def.role = Role::Char;
          unsigned v = 0;
          for (char c : role.substr(5)) v = v * 16 + static_cast<unsigned>(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
          def.ch = static_cast<char16_t>(v);
        } else {
          errors_.push_back("unknown role: " + std::string(tok));
        }
      } else if (tok.starts_with('@')) {
        auto [ns, local] = SplitName(tok.substr(1));
        AttrDefId a = attrId(ns, local);
        checkName(attrs_[a].name, tok.substr(1));
        if (std::find(def.attrs.begin(), def.attrs.end(), a) == def.attrs.end()) def.attrs.push_back(a);
      } else if (tok.starts_with('+')) {
        auto it = byName.find(tok.substr(1));
        if (it == byName.end()) {
          errors_.push_back("unknown group: " + std::string(tok));
        } else if (std::find(open.begin(), open.end(), it->second) != open.end()) {
          errors_.push_back("group includes itself: " + std::string(tok));
        } else {
          open.push_back(it->second);
          work.emplace_back(it->second, 0);
        }
      } else {
        size_t colon = tok.rfind(':');
        if (colon == std::string_view::npos) {
          errors_.push_back("child without a type: " + std::string(tok));
          continue;
        }
        auto type = byName.find(tok.substr(colon + 1));
        if (type == byName.end()) {
          errors_.push_back("unknown type: " + std::string(tok));
          continue;
        }
        auto [ns, local] = SplitName(tok.substr(0, colon));
        ElemDefId e = elemId(ns, local, type->second);
        checkName(elems_[e].name, tok.substr(0, colon));
        bool clash = false;
        for (ElemDefId c : def.children)
          if (elems_[c].name == elems_[e].name) clash = true;
        if (clash) errors_.push_back("child listed twice in " + std::string(def.name) + ": " + std::string(tok));
        else def.children.push_back(e);
      }
    }
  }

  for (TypeId t = 0; t < types_.size(); ++t) {
    for (ElemDefId c : types_[t].children) childIndex_.emplace_back((uint64_t{t} << 32) | elems_[c].name, c);
    for (AttrDefId a : types_[t].attrs) attrIndex_.emplace_back((uint64_t{t} << 32) | attrs_[a].name, a);
  }
  std::sort(childIndex_.begin(), childIndex_.end());
  std::sort(attrIndex_.begin(), attrIndex_.end());
  root_ = byName.contains("Root") ? byName["Root"] : 0;
  if (!byName.contains("Root")) errors_.push_back("no Root type");
}

}  // namespace fw::doc
