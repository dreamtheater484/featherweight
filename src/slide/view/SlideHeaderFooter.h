#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "slide/view/SlideDesign.h"

namespace fw::slide {
struct Slide;
struct Layout;
}

// Slide numbers, footer and date (P2, p2-slides; plan §8.2): what Insert > Header & footer sets,
// the date formats it offers, and the placeholders it puts on a slide (copies of the layout's date,
// footer and slide number placeholders, holding a field or the text). Headless.
namespace fw::slide::view {

struct HeaderFooterSettings {
  bool date = false;
  bool dateAuto = true;      // updates when shown (a datetimeN field); else `dateFixed`
  int dateFormat = 1;        // the field: datetime1..datetime13
  std::wstring dateFixed;
  bool slideNumber = false;
  bool footer = false;
  std::wstring footerText;
  bool notOnTitle = false;   // left off slides with a title slide layout
  bool operator==(const HeaderFooterSettings&) const = default;
};

enum class HfKind : uint8_t { Date, Footer, SlideNumber };
constexpr HfKind kHfKinds[] = {HfKind::Date, HfKind::Footer, HfKind::SlideNumber};
// p:ph type: "dt", "ftr", "sldNum".
const char* HfTypeName(HfKind kind);

// The date formats the dialog lists, as each shows now in `lang` (empty: the user's), datetime1
// first.
std::vector<std::wstring> DateFormatSamples(const std::string& lang);

// Whether `kind` shows on a slide with this layout under these settings.
bool HfWanted(const HeaderFooterSettings& s, HfKind kind, bool titleLayout);
// A title slide layout ("Don't show on title slide"): type "title", or a layout holding a centered
// title placeholder.
bool IsTitleLayout(const fw::slide::Layout* layout);

// The placeholder's p:txBody (an empty a:bodyPr and a:lstStyle, one paragraph): the date's field
// or text, the footer's text, or the slide number field. `fieldId` is the a:fld id (PowerPoint
// copies the layout's); `lang` the run's language.
std::string HfTextBodyXml(HfKind kind, const HeaderFooterSettings& s, std::string_view fieldId, std::string_view lang,
                          const Prefixes& ns = {});
// The whole p:sp: the placeholder (with `idx` and `size`, "half", "quarter" or empty, as the
// layout's) named as PowerPoint names it, with no shape properties of its own (it inherits the
// layout's position) and that text body.
std::string HfShapeXml(HfKind kind, uint32_t shapeId, uint32_t idx, std::string_view size, const HeaderFooterSettings& s,
                       std::string_view fieldId, std::string_view lang, const Prefixes& ns = {});
// A new field id: "{XXXXXXXX-XXXX-4XXX-8XXX-XXXXXXXXXXXX}".
std::string NewFieldId();

// What a slide shows now: its date, footer and slide number placeholders, as the dialog starts
// from (the first selected slide's).
HeaderFooterSettings ReadHeaderFooter(const fw::slide::Slide& slide);

}  // namespace fw::slide::view
