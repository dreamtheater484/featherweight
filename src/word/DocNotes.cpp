// Footnotes, endnotes and tables of contents in the Word document (plan §6.2 "advanced", milestone
// D4): their commands, keys, Insert menu entries and context menu. The model's side is
// doc/edit/Notes.h and Toc.h; a note is edited in place where the layout shows it.

#include <algorithm>

#include "Commands.h"
#include "doc/edit/Notes.h"
#include "doc/edit/Toc.h"
#include "ui/Menu.h"
#include "word/DocView.h"

namespace layout = fw::doc::layout;
namespace edit = fw::doc::edit;

namespace {

fw::doc::Pos ToDoc(layout::TextPos p) { return {p.para, p.offset}; }

}  // namespace

int DocView::ShortcutNotes(UINT vk, UINT mods) {
  // Word's keys: Ctrl+Alt+F a footnote, Ctrl+Alt+D an endnote.
  if ((mods & kModCtrl) && (mods & kModAlt) && !(mods & kModShift)) {
    if (vk == 'F') return CmdInsertFootnote;
    if (vk == 'D') return CmdInsertEndnote;
  }
  return 0;
}

const std::vector<int>& DocView::CommandsNotes() {
  static const std::vector<int> ids = {CmdInsertFootnote, CmdInsertEndnote, CmdInsertToc, CmdUpdateTocPages, CmdUpdateToc};
  return ids;
}

edit::PageText DocView::PageTexts() {
  return [this](uint32_t para) -> std::u16string {
    if (!pagination_ || para >= doc_->Paragraphs().size()) return {};
    const std::optional<size_t> page = pagination_->PageOf({para, 0});
    return page ? pagination_->GetPage(*page).numberText : std::u16string();
  };
}

std::optional<edit::TocInfo> DocView::CurrentToc() const {
  if (!doc_) return std::nullopt;
  if (auto toc = edit::TocAt(*doc_, ToDoc(focus_))) return toc;
  std::vector<edit::TocInfo> all = edit::TablesOfContents(*doc_);
  if (all.empty()) return std::nullopt;
  return all.front();
}

std::optional<CommandState> DocView::QueryNotes(int cmd) {
  const bool text = Editing() && !hf_ && doc_ && focus_.para < doc_->Paragraphs().size() &&
                    doc_->Para(focus_.para).story == doc_->MainStory();
  CommandState s;
  switch (cmd) {
    case CmdInsertFootnote: s = {text, false}; s.label = L"Footnote"; s.shortcut = L"Ctrl+Alt+F"; break;
    case CmdInsertEndnote: s = {text, false}; s.label = L"Endnote"; s.shortcut = L"Ctrl+Alt+D"; break;
    case CmdInsertToc: s = {text, false}; s.label = L"Table of contents"; break;
    case CmdUpdateTocPages:
      s = {Editing() && CurrentToc().has_value(), false};
      s.label = L"Update table of contents: page numbers only";
      break;
    case CmdUpdateToc:
      s = {Editing() && CurrentToc().has_value(), false};
      s.label = L"Update table of contents: entire table";
      break;
    default: return std::nullopt;
  }
  return s;
}

bool DocView::ExecuteNotes(int cmd) {
  const std::optional<CommandState> q = QueryNotes(cmd);
  if (!q) return false;
  if (!q->enabled) return true;
  const layout::TextPos at = std::max(anchor_, focus_);
  switch (cmd) {
    case CmdInsertFootnote:
    case CmdInsertEndnote:
      // After the selection, which stays: the caret goes into the new note.
      Apply(edit::InsertNote(*editor_, ToDoc(at), cmd == CmdInsertEndnote));
      return true;
    case CmdInsertToc: Apply(edit::InsertToc(*editor_, ToDoc(std::min(anchor_, focus_)), PageTexts())); return true;
    case CmdUpdateTocPages: {
      edit::Result r = edit::UpdateTocPages(*editor_, *CurrentToc(), PageTexts());
      // The caret stays where it was, as it can: only numbers changed.
      if (r && focus_.para < doc_->Paragraphs().size())
        r.caret = {focus_.para, std::min<uint32_t>(focus_.offset, static_cast<uint32_t>(doc_->Para(focus_.para).text.size()))};
      Apply(r);
      return true;
    }
    case CmdUpdateToc: Apply(edit::UpdateToc(*editor_, *CurrentToc(), PageTexts())); return true;
  }
  return false;
}

void DocView::FillNotesMenu(HMENU m) {
  if (!Editing() || !doc_ || !edit::TocAt(*doc_, ToDoc(focus_))) return;
  menu::AppendItem(m, CmdUpdateTocPages, L"Update page numbers");
  menu::AppendItem(m, CmdUpdateToc, L"Update entire table");
  menu::AppendSeparator(m);
}
