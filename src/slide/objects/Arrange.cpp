#include "slide/objects/Arrange.h"

#include <algorithm>
#include <numeric>

#include "slide/objects/Frame.h"

namespace fw::slide::objects {

std::vector<draw::Point> Align(const std::vector<draw::Rect>& boxes, AlignTo how, bool toSlide, draw::Size slide) {
  std::vector<draw::Point> out(boxes.size());
  if (boxes.empty()) return out;
  draw::Rect u = toSlide ? draw::Rect{0, 0, slide.width, slide.height} : boxes[0];
  if (!toSlide)
    for (const draw::Rect& b : boxes) u = Union(u, b);
  for (size_t i = 0; i < boxes.size(); ++i) {
    const draw::Rect& b = boxes[i];
    switch (how) {
      case AlignTo::Left: out[i].x = u.left - b.left; break;
      case AlignTo::Centre: out[i].x = (u.left + u.right) / 2 - (b.left + b.right) / 2; break;
      case AlignTo::Right: out[i].x = u.right - b.right; break;
      case AlignTo::Top: out[i].y = u.top - b.top; break;
      case AlignTo::Middle: out[i].y = (u.top + u.bottom) / 2 - (b.top + b.bottom) / 2; break;
      case AlignTo::Bottom: out[i].y = u.bottom - b.bottom; break;
    }
  }
  return out;
}

std::vector<draw::Point> Distribute(const std::vector<draw::Rect>& boxes, bool horizontal, bool toSlide, draw::Size slide) {
  std::vector<draw::Point> out(boxes.size());
  const size_t n = boxes.size();
  if (n == 0 || (!toSlide && n < 3)) return out;
  auto lo = [&](size_t i) { return horizontal ? boxes[i].left : boxes[i].top; };
  auto hi = [&](size_t i) { return horizontal ? boxes[i].right : boxes[i].bottom; };
  std::vector<size_t> order(n);
  std::iota(order.begin(), order.end(), size_t{0});
  std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return lo(a) + hi(a) < lo(b) + hi(b); });
  float start = toSlide ? 0 : lo(order.front()), end = toSlide ? (horizontal ? slide.width : slide.height) : hi(order.back());
  if (!toSlide) {
    for (size_t i : order) start = std::min(start, lo(i)), end = std::max(end, hi(i));
  }
  float total = 0;
  for (size_t i = 0; i < n; ++i) total += hi(i) - lo(i);
  auto set = [&](size_t i, float pos) {
    if (horizontal) out[i].x = pos - lo(i); else out[i].y = pos - lo(i);
  };
  if (n == 1) {
    set(order[0], (start + end - total) / 2);
    return out;
  }
  const float gap = (end - start - total) / static_cast<float>(n - 1);
  float at = start;
  for (size_t i : order) {
    set(i, at);
    at += hi(i) - lo(i) + gap;
  }
  return out;
}

std::vector<size_t> Reorder(const std::vector<bool>& selected, Order how) {
  const size_t n = selected.size();
  std::vector<size_t> order(n);
  std::iota(order.begin(), order.end(), size_t{0});
  switch (how) {
    case Order::Forward:
      for (size_t i = n >= 2 ? n - 1 : 0; i-- > 0;)
        if (selected[order[i]] && !selected[order[i + 1]]) std::swap(order[i], order[i + 1]);
      break;
    case Order::Backward:
      for (size_t i = 1; i < n; ++i)
        if (selected[order[i]] && !selected[order[i - 1]]) std::swap(order[i - 1], order[i]);
      break;
    case Order::ToFront:
      std::stable_partition(order.begin(), order.end(), [&](size_t i) { return !selected[i]; });
      break;
    case Order::ToBack:
      std::stable_partition(order.begin(), order.end(), [&](size_t i) { return selected[i]; });
      break;
  }
  return order;
}

}  // namespace fw::slide::objects
