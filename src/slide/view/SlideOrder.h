#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "slide/view/SlideLayout.h"

// The thumbnails pane as an editor (P2, p2-slides): which slides are selected, where a dragged
// selection drops, the order a move gives, and how per-slide caches follow an edit that inserted,
// deleted or moved slides. Headless, so tests check it.
namespace fw::slide::view {

// The selected slides, as PowerPoint's thumbnails pane keeps them: a click selects one slide,
// Ctrl+click adds or removes one, Shift+click selects the run from the anchor. The current slide
// (the one shown) is always selected.
class SlideSelection {
 public:
  // `count` slides, only `current` selected.
  void Reset(size_t count, size_t current);
  // A click on slide `i` (with Ctrl and Shift as held). A Ctrl+click on the only selected slide
  // keeps it. Returns the slide to show.
  size_t Click(size_t i, bool ctrl, bool shift);
  void SelectAll();
  // These slides (sorted or not; out of range ones are dropped), `current` shown and selected.
  void Set(const std::vector<size_t>& slides, size_t current);

  size_t Count() const { return count_; }
  size_t Current() const { return current_; }
  bool Selected(size_t i) const { return i < selected_.size() && selected_[i]; }
  size_t SelectedCount() const;
  // In deck order.
  std::vector<size_t> Indices() const;
  // Whether several slides are selected.
  bool Many() const { return SelectedCount() > 1; }

 private:
  size_t count_ = 0, current_ = 0, anchor_ = 0;
  std::vector<bool> selected_;
};

// The gap a drag at `y` (the rows' own coordinates) drops into: 0 before the first slide, n after
// the last. Rows are ThumbRows, top to bottom; above the first is 0 and below the last is n.
size_t DropGap(const std::vector<ThumbRow>& rows, float y);
// The line drawn for gap `gap`: across the pane, between the rows (above the first row or below
// the last at the ends), `thickness` high.
draw::Rect DropMarker(const std::vector<ThumbRow>& rows, size_t gap, float thickness);

// Moving `slides` (indexes, sorted, no repeats) to gap `gap` (counted in the order before the
// move): the order afterwards, as each new position's old index. The moved slides keep their own
// order and end up together.
std::vector<size_t> MovedOrder(size_t count, const std::vector<size_t>& slides, size_t gap);
// Where the first moved slide is after that move.
size_t MovedTo(size_t count, const std::vector<size_t>& slides, size_t gap);
// Whether the move changes the order at all (dropping a run of slides next to itself doesn't).
bool MoveChanges(size_t count, const std::vector<size_t>& slides, size_t gap);
// Where a slide that was at `old` is after the move (its new index).
std::vector<size_t> NewIndexes(const std::vector<size_t>& order);

// After an edit: for each slide now in the deck (by its p:sldId id), the index it had before the
// edit, or -1 for a slide that's new (inserted, duplicated, pasted). Ids stay with their slides
// through moves; a deleted slide's id is gone.
std::vector<int> OldIndexes(const std::vector<uint32_t>& before, const std::vector<uint32_t>& after);

// Reorders a per-slide cache (thumbnails, prepared slides) to follow OldIndexes: an entry moves with
// its slide, a new slide's entry starts empty (T{}), a deleted slide's is dropped.
template <class T>
void FollowSlides(std::vector<T>& cache, const std::vector<int>& oldIndexes) {
  std::vector<T> next(oldIndexes.size());
  for (size_t i = 0; i < oldIndexes.size(); ++i) {
    const int o = oldIndexes[i];
    if (o >= 0 && static_cast<size_t>(o) < cache.size()) next[i] = std::move(cache[static_cast<size_t>(o)]);
  }
  cache = std::move(next);
}

}  // namespace fw::slide::view
