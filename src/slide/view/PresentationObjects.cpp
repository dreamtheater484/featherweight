// The presentation tab's objects (P2, p2-objects): the ObjectTool made for the tab, and how its
// edits reach what the tab has drawn. The tool itself is in ObjectTool.cpp.

#include "Commands.h"
#include "slide/view/ObjectTool.h"
#include "slide/view/PresentationComments.h"
#include "slide/view/PresentationText.h"
#include "slide/view/PresentationView.h"
#include "ui/Banner.h"

namespace fw::slide::view {

ObjectTool& PresentationView::Objects() const {
  if (!objects_) {
    PresentationView* self = const_cast<PresentationView*>(this);
    ObjectTool::Host h;
    h.deck = [self] { return self->deck_.get(); };
    h.current = [self] { return self->current_; };
    h.slideRect = [self] { return self->SlideRect(); };
    h.ui = [self] { return self->ui_; };
    h.invalidate = [self] { self->host_.ViewInvalidate(); };
    h.refused = [self](const std::wstring& why) { self->host_.ViewMessage(BannerKind::Warning, why); };
    h.window = [self] { return self->host_.ViewHwnd(); };
    // p2-text: after inserting a text box (or a table), the caret goes into it.
    h.beginText = [self](uint32_t id) {
      if (self->text_->BeginTextEdit(self->current_, id)) self->objects_->SetTextEditing(true);
    };
    // p2-text: the cells chosen while a table cell is edited (a block dragged or Shift+clicked, else
    // the edited cell) are where row and column commands, merge and split work.
    h.tableCells = [self](uint32_t table, int& row0, int& col0, int& row1, int& col1) {
      TextSession* s = self->text_->Session();
      if (!s) return false;
      const TextTarget& t = s->Target();
      if (t.slide != self->current_ || t.notes || t.shape != table) return false;
      const std::optional<std::array<int, 4>> r = self->text_->CellRange();  // a block, else the edited cell
      if (!r) return false;
      row0 = (*r)[0], col0 = (*r)[1], row1 = (*r)[2], col1 = (*r)[3];
      return true;
    };
    // h.slideEdited stays unset: what was drawn of edited slides (thumbnails, the slide's bitmap, the
    // Deck's caches) is the tab's own Editor listener's job (OnSlideEdits, p2-slides), which follows
    // slides by id and redraws only the touched ones (agreed with p2-objects, 2026-11-22).
    objects_ = std::make_unique<ObjectTool>(std::move(h));
  }
  return *objects_;
}

bool PresentationView::ObjectsTake(int cmd) const {
  if (cmd == CmdPaste && SlidesOnClipboard()) return false;  // p2-slides: copied slides paste as slides, as in PowerPoint
  if (!keysInThumbs_) return true;
  return cmd != CmdCut && cmd != CmdCopy && cmd != CmdPaste && cmd != CmdSelectAll && cmd != CmdDeleteObject &&
         cmd != CmdDuplicateObjects;
}

bool PresentationView::Dragging() const {
  return drag_ != Drag::None || (objects_ && objects_->Dragging()) || (commentsPanel_ && commentsPanel_->Dragging());
}

bool PresentationView::TakesPictures(const std::vector<std::wstring>& files) const {
  return Ready() && SlideCount() > 0 && Objects().TakesPictures(files);
}

void PresentationView::DropPictures(const std::vector<std::wstring>& files, int x, int y) {
  if (Ready() && SlideCount() > 0) Objects().DropPictures(files, static_cast<float>(x), static_cast<float>(y));
}

}  // namespace fw::slide::view
