// Design operations of the presentation tab (P2, p2-slides): see SlideDesignOps.h.

#include "slide/edit/SlideDesignOps.h"

#include "core/Lang.h"
#include "slide/edit/EditInternal.h"

namespace fw::slide::edit {

namespace {

// Replaces `old` (an element) by a copy of the fragment's element, in place.
bool ReplaceWith(pugi::xml_node old, std::string_view xml) {
  pugi::xml_document doc;
  if (!old || !ParseFragment(xml, doc)) return false;
  InsertCopy(old.parent(), doc.document_element(), old);
  old.parent().remove_child(old);
  return true;
}

void SetLatin(pugi::xml_node font, const std::string& face) {
  if (!font || face.empty()) return;
  pugi::xml_node latin = A(font, "latin");
  if (!latin) return;
  latin.attribute("typeface").set_value(face.c_str());
  latin.remove_attribute("panose");
  latin.remove_attribute("pitchFamily");
  latin.remove_attribute("charset");
}

}  // namespace

Result ApplyThemeDesign(Editor& ed, size_t master, const ThemeDesign& d) {
  Call call(ed, d.step);
  Editor::Impl& m = ed.Internals();
  const Presentation& deck = ed.Deck();
  if (master >= deck.Masters().size()) return call.End(Result::Fail(T(u"That slide master isn't in the presentation.")));
  const std::string& masterPart = deck.Masters()[master]->part;
  const std::string themePart = TargetOfType(m.Rels(masterPart), "theme");
  if (!d.name.empty() || !d.colorScheme.empty() || !d.major.empty() || !d.minor.empty()) {
    pugi::xml_node theme = themePart.empty() ? pugi::xml_node() : m.Edit(themePart);
    if (!theme || LocalNameOf(theme) != "theme") return call.End(Result::Fail(T(u"This slide master has no theme to change.")));
    pugi::xml_node elements = A(theme, "themeElements");
    if (!d.name.empty()) (theme.attribute("name") ? theme.attribute("name") : theme.append_attribute("name")).set_value(d.name.c_str());
    if (!d.colorScheme.empty() && !ReplaceWith(A(elements, "clrScheme"), d.colorScheme))
      return call.End(Result::Fail(T(u"Those theme colors can't be written.")));
    pugi::xml_node fonts = A(elements, "fontScheme");
    SetLatin(A(fonts, "majorFont"), d.major);
    SetLatin(A(fonts, "minorFont"), d.minor);
  }
  if (d.dark >= 0) {
    pugi::xml_node root = m.Edit(masterPart);
    if (!root) return call.End(Result::Fail(T(u"That slide master can't be changed.")));
    if (pugi::xml_node map = DirectChild(root, kP, "clrMap")) {
      const bool dark = d.dark == 1;
      const char* pairs[4][2] = {{"bg1", dark ? "dk1" : "lt1"}, {"tx1", dark ? "lt1" : "dk1"},
                                 {"bg2", dark ? "dk2" : "lt2"}, {"tx2", dark ? "lt2" : "dk2"}};
      for (auto& [k, v] : pairs) (map.attribute(k) ? map.attribute(k) : map.append_attribute(k)).set_value(v);
    }
  }
  // Inside this call, so part of its step.
  if (!d.background.empty())
    if (Result r = ed.SetMasterBackground(master, d.background); !r) return call.End(r);
  m.Full();
  return call.End({});
}

Result SetShowMasterShapes(Editor& ed, const std::vector<size_t>& slides, bool show) {
  Call call(ed, T(u"Format Background"));
  Editor::Impl& m = ed.Internals();
  const std::vector<Editor::Impl::SlideEntry> list = m.SlideList();
  if (Result r = m.CheckSlides(slides, list.size()); !r) return call.End(r);
  for (size_t i : slides) {
    pugi::xml_node root = m.Edit(list[i].part);
    if (!root) return call.End(Result::Fail(T(u"That slide can't be changed.")));
    if (show) root.remove_attribute("showMasterSp");
    else (root.attribute("showMasterSp") ? root.attribute("showMasterSp") : root.append_attribute("showMasterSp")).set_value("0");
  }
  return call.End({});
}

}  // namespace fw::slide::edit
