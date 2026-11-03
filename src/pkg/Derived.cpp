#include "pkg/Derived.h"

namespace fw::pkg {

const std::vector<DerivedData>& DerivedDataList() {
  using enum OnSave;
  constexpr DocFormat Any = DocFormat::Any, Word = DocFormat::Word, Excel = DocFormat::Excel,
                      PowerPoint = DocFormat::PowerPoint;
  static const std::vector<DerivedData> kList = {
      // Every format
      {Any, "AlternateContent branches (mc:Fallback, other mc:Choice)", "the branch Featherweight edits", Regenerate,
       "after an edit inside the block: regenerated where Featherweight can write it (a VML text box), dropped "
       "otherwise; a stale branch is never kept next to an edited one (rule 1)",
       false},
      {Any, "thumbnail (docProps/thumbnail.*)", "the first page, slide or sheet", Drop,
       "when the content changes, with its package relationship and content type", false},
      {Any, "chart caches (c:numCache, c:strCache) and the chart's embedded workbook", "the cells the series name (c:f)",
       Regenerate,
       "together, whenever the data changes: the cache is what Office draws, and the workbook is what Edit Data "
       "opens, so they must never disagree",
       true},
      // Word
      {Word, "field results (between w:fldChar separate and end, w:fldSimple content)", "the field code and its sources",
       MarkDirty, "when a source changes: w:dirty=\"true\" on the field, or w:updateFields in settings", false},
      {Word, "table of contents and indexes", "headings and index entries (they are field results)", MarkDirty,
       "as fields", false},
      {Word, "rendered page breaks (w:lastRenderedPageBreak)", "the layout of the application that saved the file",
       Drop, "in a paragraph whose text changes; Word ignores them when it opens a file", false},
      {Word, "comment companions (commentsExtended, commentsIds, commentsExtensible)", "the comments", Regenerate,
       "with the comments: entries of deleted comments go, and paragraph and durable ids follow (references)", true},
      {Word, "content controls bound to custom XML (w:dataBinding)", "the bound custom XML part", Regenerate,
       "the custom XML part is updated when the control's content is edited", false},
      {Word, "document statistics (docProps/app.xml: Pages, Words, Characters, Lines, Paragraphs)",
       "the content and its layout", Regenerate, "on every save, from Featherweight's layout", false},
      // Excel
      {Excel, "calculation chain (xl/calcChain.xml)", "the formulas of every sheet", Drop,
       "when any formula changes or a sheet is added, removed or moved: the part, its relationship and its "
       "content type go together, and Excel rebuilds it",
       true},
      {Excel, "cached formula results (<v> of a formula cell)", "the formulas and their inputs", Regenerate,
       "by recalculation (S2); until then they are kept and fullCalcOnLoad is set", false},
      {Excel, "shared string counts (sst count, uniqueCount)", "the shared strings and the cells that use them",
       Regenerate, "whenever the shared strings part is written", false},
      {Excel, "sheet dimension (<dimension ref>)", "the cells of the sheet", Regenerate,
       "when cells are added or removed", false},
      {Excel, "table column names (tableColumn name)", "the table's header row", Regenerate,
       "when a header cell changes: Excel repairs tables whose names differ", false},
      {Excel, "filter database names (_xlnm._FilterDatabase)", "the sheet's autoFilter range", Regenerate,
       "with the filter", false},
      {Excel, "pivot cache records", "the pivot table's source range", Keep,
       "and refreshOnLoad is set when the source changes", false},
      {Excel, "sheet list in docProps/app.xml (TitlesOfParts, HeadingPairs)", "the workbook's sheets", Regenerate,
       "when sheets are added, removed or renamed", false},
      // PowerPoint
      {PowerPoint, "slide counts in docProps/app.xml (Slides, Notes, HiddenSlides, TitlesOfParts)", "the slides",
       Regenerate, "when slides are added, removed or hidden", false},
      {PowerPoint, "sections (p14:sectionLst)", "the slide list", Regenerate,
       "with the slide list: slide ids follow (references)", true},
      {PowerPoint, "embedded objects' preview images", "the embedded object", Keep,
       "Featherweight doesn't edit embedded objects", false},
  };
  return kList;
}

std::vector<const DerivedData*> DerivedDataFor(DocFormat format) {
  std::vector<const DerivedData*> out;
  for (const auto& d : DerivedDataList())
    if (d.format == DocFormat::Any || d.format == format) out.push_back(&d);
  return out;
}

}  // namespace fw::pkg
