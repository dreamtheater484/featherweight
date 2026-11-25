#include "slide/view/TextModelEdit.h"

#include <sstream>

#include "core/Lang.h"
#include "core/Strings.h"
#include "draw/Xml.h"
#include "slide/Presentation.h"

namespace fw::slide::view {

using namespace draw::xml;

namespace {

edit::TextTarget Their(const TextTarget& t) {
  edit::TextTarget e;
  e.slide = t.slide;
  e.shape = t.shape;
  e.row = t.row;
  e.column = t.column;
  e.notes = t.notes;
  return e;
}

}  // namespace

const SlideBase* EditTextModel::Part(const TextTarget& t) {
  if (t.slide >= deck_.SlideCount()) return nullptr;
  const Slide* s = deck_.GetSlide(t.slide);
  if (!s) return nullptr;
  if (t.notes) return s->notes;
  return s;
}

const Shape* EditTextModel::ShapeOf(const TextTarget& t) {
  const SlideBase* part = Part(t);
  return part ? part->FindShape(t.shape) : nullptr;
}

pugi::xml_node EditTextModel::Body(const TextTarget& t) {
  const Shape* sh = ShapeOf(t);
  if (!sh) return {};
  if (t.Cell()) {
    const Table* table = deck_.GetTable(*sh, *Part(t));
    const TableCell* c = table ? table->Cell(t.row, t.column) : nullptr;
    return c ? A(c->node, "txBody") : pugi::xml_node();
  }
  return Child(sh->node, kP, "txBody");
}

std::unique_ptr<TextBody> EditTextModel::Resolve(const TextTarget& t, pugi::xml_node txBody) {
  const Shape* sh = ShapeOf(t);
  if (!sh) return nullptr;
  if (t.Cell()) {
    const Table* table = deck_.GetTable(*sh, *Part(t));
    const TableCell* c = table ? table->Cell(t.row, t.column) : nullptr;
    return c ? deck_.ResolveCellText(*sh, *c, txBody, *Part(t)) : nullptr;
  }
  return deck_.ResolveTextBody(*sh, txBody, *Part(t));
}

bool EditTextModel::SetText(const TextTarget& t, pugi::xml_node txBody, std::u16string_view step, bool coalesce,
                            std::wstring& error) {
  // Text holding an equation (a14:m in an mc:Choice) isn't edited: the app draws the Fallback while
  // an edit would land on the Choice, so p2-model refuses its own text operations there too.
  const pugi::xml_node now = Body(t);
  if (now && now.find_node([](pugi::xml_node n) {
        const std::string_view name = n.name();
        return name == "a14:m" || name.ends_with(":oMath") || name.ends_with(":oMathPara");
      })) {
    error = T(L"This text holds an equation, which Featherweight can't edit yet.");
    return false;
  }
  std::ostringstream os;
  txBody.print(os, "", pugi::format_raw);
  // Typing is marked as typing from its first keystroke, so the ones after join its step; a
  // keystroke the session doesn't coalesce (the caret moved) starts a new one.
  const bool typing = step == T(u"Typing");
  if (typing && !coalesce) editor_.EndTyping();
  const edit::Result r = editor_.SetTextBodyXml(Their(t), os.str(), std::u16string(step), typing);
  if (!r) error.assign(r.message.begin(), r.message.end());
  return r.ok;
}

bool EditTextModel::SetShapeHeight(const TextTarget& t, float height) {
  const Shape* sh = ShapeOf(t);
  if (!sh || t.notes || t.Cell()) return false;
  edit::Transform tr;
  tr.bounds = sh->bounds;
  tr.bounds.bottom = tr.bounds.top + height;
  tr.rotation = sh->rotation;
  tr.flipH = sh->flipH;
  tr.flipV = sh->flipV;
  return editor_.SetTransform(t.slide, t.shape, tr).ok;
}

std::string EditTextModel::AddLink(const TextTarget& t, const std::wstring& url, int slide) {
  const SlideBase* part = Part(t);
  if (!part) return {};
  constexpr std::string_view kRel = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/";
  if (slide >= 0) {
    const std::string target = editor_.SlidePart(static_cast<size_t>(slide));
    if (target.empty()) return {};
    return editor_.AddRelationship(part->part, std::string(kRel) + "slide", target, false);
  }
  if (url.empty()) return {};
  return editor_.AddRelationship(part->part, std::string(kRel) + "hyperlink", WideToUtf8(url), true);
}

}  // namespace fw::slide::view
