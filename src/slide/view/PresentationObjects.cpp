// The presentation tab's objects (P2, p2-objects): the ObjectTool made for the tab, and how its
// edits reach what the tab has drawn. The tool itself is in ObjectTool.cpp.

#include "Commands.h"
#include "slide/view/ObjectTool.h"
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
    // An edit (or undo) changed a slide (SIZE_MAX: any): what was drawn of it goes.
    h.slideEdited = [self](size_t slide) {
      if (!self->deck_) return;
      const size_t n = self->deck_->SlideCount();
      if (self->thumbCache_.size() != n || slide == SIZE_MAX) {
        self->thumbCache_.assign(n, {});
        for (size_t i = 0; i < n; ++i) self->deck_->Invalidate(i);
      } else if (slide < n) {
        self->thumbCache_[slide].bitmap.Reset();
        self->deck_->Invalidate(slide);
      }
      self->slideBitmap_.Reset();
      self->links_.clear();
      self->linksOf_ = SIZE_MAX;
      self->host_.ViewStateChanged();
      self->host_.ViewInvalidate();
    };
    objects_ = std::make_unique<ObjectTool>(std::move(h));
  }
  return *objects_;
}

bool PresentationView::ObjectsTake(int cmd) const {
  if (!keysInThumbs_) return true;
  return cmd != CmdCut && cmd != CmdCopy && cmd != CmdPaste && cmd != CmdSelectAll && cmd != CmdDeleteObject &&
         cmd != CmdDuplicateObjects;
}

bool PresentationView::Dragging() const { return drag_ != Drag::None || (objects_ && objects_->Dragging()); }

bool PresentationView::TakesPictures(const std::vector<std::wstring>& files) const {
  return Ready() && SlideCount() > 0 && Objects().TakesPictures(files);
}

void PresentationView::DropPictures(const std::vector<std::wstring>& files, int x, int y) {
  if (Ready() && SlideCount() > 0) Objects().DropPictures(files, static_cast<float>(x), static_cast<float>(y));
}

}  // namespace fw::slide::view
