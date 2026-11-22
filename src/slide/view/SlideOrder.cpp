#include "slide/view/SlideOrder.h"

#include <algorithm>
#include <unordered_map>

namespace fw::slide::view {

void SlideSelection::Reset(size_t count, size_t current) {
  count_ = count;
  current_ = count ? std::min(current, count - 1) : 0;
  anchor_ = current_;
  selected_.assign(count, false);
  if (count) selected_[current_] = true;
}

size_t SlideSelection::Click(size_t i, bool ctrl, bool shift) {
  if (i >= count_) return current_;
  if (shift) {
    if (!ctrl) selected_.assign(count_, false);
    const size_t a = std::min(anchor_, count_ - 1);
    for (size_t k = std::min(a, i); k <= std::max(a, i); ++k) selected_[k] = true;
    current_ = i;  // the anchor stays, so the next Shift+click grows or shrinks the same run
    return current_;
  }
  if (ctrl) {
    anchor_ = i;
    if (!selected_[i]) {
      selected_[i] = true;
      current_ = i;
      return current_;
    }
    if (SelectedCount() == 1) return current_;  // the last selected slide stays
    selected_[i] = false;
    if (current_ == i) {
      // The nearest selected slide is shown instead.
      size_t best = SIZE_MAX, distance = SIZE_MAX;
      for (size_t k = 0; k < count_; ++k)
        if (selected_[k]) {
          const size_t d = k > i ? k - i : i - k;
          if (d < distance) best = k, distance = d;
        }
      current_ = best;
    }
    return current_;
  }
  selected_.assign(count_, false);
  selected_[i] = true;
  current_ = anchor_ = i;
  return current_;
}

void SlideSelection::SelectAll() { selected_.assign(count_, true); }

void SlideSelection::Set(const std::vector<size_t>& slides, size_t current) {
  selected_.assign(count_, false);
  for (size_t s : slides)
    if (s < count_) selected_[s] = true;
  if (count_ == 0) return;
  current_ = std::min(current, count_ - 1);
  selected_[current_] = true;
  anchor_ = current_;
}

size_t SlideSelection::SelectedCount() const { return static_cast<size_t>(std::count(selected_.begin(), selected_.end(), true)); }

std::vector<size_t> SlideSelection::Indices() const {
  std::vector<size_t> out;
  for (size_t i = 0; i < selected_.size(); ++i)
    if (selected_[i]) out.push_back(i);
  return out;
}

size_t DropGap(const std::vector<ThumbRow>& rows, float y) {
  // The gap before the first row whose middle is below the pointer.
  for (size_t i = 0; i < rows.size(); ++i)
    if (y < (rows[i].row.top + rows[i].row.bottom) / 2) return i;
  return rows.size();
}

draw::Rect DropMarker(const std::vector<ThumbRow>& rows, size_t gap, float thickness) {
  if (rows.empty()) return {};
  float y;
  if (gap == 0) y = rows.front().row.top;
  else if (gap >= rows.size()) y = rows.back().row.bottom;
  else y = (rows[gap - 1].row.bottom + rows[gap].row.top) / 2;
  const draw::Rect& r = rows[std::min(gap, rows.size() - 1)].row;
  return {r.left, y - thickness / 2, r.right, y + thickness / 2};
}

std::vector<size_t> MovedOrder(size_t count, const std::vector<size_t>& slides, size_t gap) {
  std::vector<bool> moved(count, false);
  for (size_t s : slides)
    if (s < count) moved[s] = true;
  gap = std::min(gap, count);
  std::vector<size_t> order;
  order.reserve(count);
  auto movedRun = [&] {
    for (size_t s = 0; s < count; ++s)
      if (moved[s]) order.push_back(s);
  };
  for (size_t i = 0; i < count; ++i) {
    if (i == gap) movedRun();
    if (!moved[i]) order.push_back(i);
  }
  if (gap == count) movedRun();
  return order;
}

size_t MovedTo(size_t count, const std::vector<size_t>& slides, size_t gap) {
  // The slides staying before the gap.
  gap = std::min(gap, count);
  size_t before = gap;
  for (size_t s : slides)
    if (s < gap) --before;
  return before;
}

bool MoveChanges(size_t count, const std::vector<size_t>& slides, size_t gap) {
  const std::vector<size_t> order = MovedOrder(count, slides, gap);
  for (size_t i = 0; i < order.size(); ++i)
    if (order[i] != i) return true;
  return false;
}

std::vector<size_t> NewIndexes(const std::vector<size_t>& order) {
  std::vector<size_t> out(order.size());
  for (size_t i = 0; i < order.size(); ++i)
    if (order[i] < out.size()) out[order[i]] = i;
  return out;
}

std::vector<int> OldIndexes(const std::vector<uint32_t>& before, const std::vector<uint32_t>& after) {
  std::unordered_map<uint32_t, int> at;
  at.reserve(before.size());
  for (size_t i = 0; i < before.size(); ++i) at.emplace(before[i], static_cast<int>(i));
  std::vector<int> out(after.size(), -1);
  for (size_t i = 0; i < after.size(); ++i) {
    auto it = at.find(after[i]);
    if (it != at.end()) {
      out[i] = it->second;
      at.erase(it);  // ids are unique; a repeated one (a damaged deck) counts once
    }
  }
  return out;
}

}  // namespace fw::slide::view
