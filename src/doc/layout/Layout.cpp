// Pagination (Layout.h). The interface first: this lays out nothing yet, so a view built against it
// shows an empty document until pagination lands in the next commits.
#include "doc/layout/Layout.h"

#include "core/OpenTimings.h"

namespace fw::doc::layout {

struct Layout::Impl {
  const Document& doc;
  text::Fonts& fonts;
  Options options;
  OpenTimings* timings;
  std::vector<Page> pages;
  uint32_t revision = 0;
};

Layout::Layout(const Document& doc, text::Fonts& fonts, Options options, OpenTimings* timings)
    : impl_(new Impl{doc, fonts, options, timings}) {}
Layout::~Layout() = default;

void Layout::LayOutPages(size_t) {}
bool Layout::Continue(double) { return false; }
bool Layout::Done() const { return true; }
size_t Layout::PageCount() const { return impl_->pages.size(); }
const Page& Layout::GetPage(size_t index) const { return impl_->pages[index]; }
uint32_t Layout::Revision() const { return impl_->revision; }

std::optional<TextPos> Layout::HitTest(size_t, float, float) const { return std::nullopt; }
std::optional<Layout::Caret> Layout::CaretAt(TextPos) { return std::nullopt; }
std::vector<std::pair<size_t, Rect>> Layout::Ranges(TextPos, TextPos) { return {}; }
std::optional<size_t> Layout::PageOf(TextPos) { return std::nullopt; }

void Layout::Paint(draw::Painter&, size_t, const DrawingPainter&) const {}

LayoutDump Layout::Dump(std::string source) const {
  LayoutDump dump;
  dump.source = std::move(source);
  dump.producer = "Featherweight";
  return dump;
}

size_t Layout::MemoryBytes() const { return sizeof(Impl); }

}  // namespace fw::doc::layout
