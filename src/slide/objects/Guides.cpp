#include "slide/objects/Guides.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "slide/objects/Frame.h"

namespace fw::slide::objects {

namespace {

// A box by axis: lo[0]/hi[0] are left/right, lo[1]/hi[1] top/bottom.
struct Box {
  float lo[2], hi[2];
  explicit Box(const draw::Rect& r) : lo{r.left, r.top}, hi{r.right, r.bottom} {}
  float Mid(int a) const { return (lo[a] + hi[a]) / 2; }
};

struct Gap {
  float from, to, at;  // along the axis, and where it's drawn across it
};

struct AxisSnap {
  bool found = false;
  float adj = std::numeric_limits<float>::max();
  std::vector<float> targets;      // alignment: positions matched
  std::vector<int> sources;        // which other (-1: the slide) each target came from
  std::vector<Gap> gaps;           // equal spacing: the gaps shown
};

bool Overlaps(const Box& a, const Box& b, int across) { return a.lo[across] < b.hi[across] && b.lo[across] < a.hi[across]; }

float OverlapMid(const Box& a, const Box& b, int across) {
  return (std::max(a.lo[across], b.lo[across]) + std::min(a.hi[across], b.hi[across])) / 2;
}

void Offer(AxisSnap& s, float adj, float tolerance) {
  if (std::fabs(adj) > tolerance) return;
  if (!s.found || std::fabs(adj) < std::fabs(s.adj) - 0.01f) {
    s.found = true;
    s.adj = adj;
    s.targets.clear();
    s.sources.clear();
    s.gaps.clear();
  }
}

// Alignment along axis `a`: each feature (positions on the moving box) against each target.
void Align(AxisSnap& s, const std::vector<float>& features, const std::vector<Box>& others, float slideExtent,
           const SnapSettings& set, int a) {
  auto consider = [&](float target, int source) {
    for (float f : features) {
      const float adj = target - f;
      Offer(s, adj, set.tolerance);
      if (s.found && std::fabs(adj - s.adj) <= 0.01f && std::fabs(adj) <= set.tolerance) {
        s.targets.push_back(target);
        s.sources.push_back(source);
      }
    }
  };
  if (set.slide)
    for (float t : {0.f, slideExtent / 2, slideExtent}) consider(t, -1);
  if (set.objects)
    for (size_t i = 0; i < others.size(); ++i)
      for (float t : {others[i].lo[a], others[i].Mid(a), others[i].hi[a]}) consider(t, static_cast<int>(i));
}

// Equal spacing along axis `a` between the moving box and its neighbours in the same row (or column).
void Spacing(AxisSnap& s, const Box& m, const std::vector<Box>& others, const SnapSettings& set, int a) {
  const int across = 1 - a;
  const float size = m.hi[a] - m.lo[a], tol = set.tolerance;
  // Neighbours before (ending at or before the moving box's start, give or take the tolerance) and after.
  auto before = [&](float limit, const Box& ref) {
    int best = -1;
    for (size_t i = 0; i < others.size(); ++i) {
      const Box& o = others[i];
      if (!Overlaps(o, ref, across) || o.hi[a] > limit + tol || o.Mid(a) >= ref.Mid(a)) continue;
      if (best < 0 || o.hi[a] > others[best].hi[a]) best = static_cast<int>(i);
    }
    return best;
  };
  auto after = [&](float limit, const Box& ref) {
    int best = -1;
    for (size_t i = 0; i < others.size(); ++i) {
      const Box& o = others[i];
      if (!Overlaps(o, ref, across) || o.lo[a] < limit - tol || o.Mid(a) <= ref.Mid(a)) continue;
      if (best < 0 || o.lo[a] < others[best].lo[a]) best = static_cast<int>(i);
    }
    return best;
  };
  const int p = before(m.lo[a], m), n = after(m.hi[a], m);
  auto offer = [&](float newLo, std::vector<Gap> gaps) {
    const float adj = newLo - m.lo[a];
    if (std::fabs(adj) > tol) return;
    const bool better = !s.found || std::fabs(adj) < std::fabs(s.adj) - 0.01f;
    const bool same = s.found && std::fabs(adj - s.adj) <= 0.01f;
    if (better) {
      Offer(s, adj, tol);
    } else if (!same) {
      return;
    }
    s.gaps.insert(s.gaps.end(), gaps.begin(), gaps.end());
  };
  if (p >= 0 && n >= 0) {  // centred between the two
    const Box &P = others[p], &N = others[n];
    const float lo = (P.hi[a] + N.lo[a] - size) / 2;
    if (lo >= P.hi[a])
      offer(lo, {{P.hi[a], lo, OverlapMid(m, P, across)}, {lo + size, N.lo[a], OverlapMid(m, N, across)}});
  }
  if (p >= 0) {  // as far from the one before as it is from its own neighbour before
    const Box& P = others[p];
    const int pp = before(P.lo[a], P);
    if (pp >= 0) {
      const Box& PP = others[pp];
      const float gap = P.lo[a] - PP.hi[a];
      if (gap >= 0)
        offer(P.hi[a] + gap, {{PP.hi[a], P.lo[a], OverlapMid(P, PP, across)}, {P.hi[a], P.hi[a] + gap, OverlapMid(m, P, across)}});
    }
  }
  if (n >= 0) {
    const Box& N = others[n];
    const int nn = after(N.hi[a], N);
    if (nn >= 0) {
      const Box& NN = others[nn];
      const float gap = NN.lo[a] - N.hi[a];
      if (gap >= 0)
        offer(N.lo[a] - gap - size,
              {{N.lo[a] - gap, N.lo[a], OverlapMid(m, N, across)}, {N.hi[a], NN.lo[a], OverlapMid(N, NN, across)}});
    }
  }
}

// The guides an axis's alignment shows, for the final box.
void AlignGuides(const AxisSnap& s, const Box& box, const std::vector<Box>& others, draw::Size slide, int a,
                 std::vector<Guide>& out) {
  const int across = 1 - a;
  const float slideAcross = a == 0 ? slide.height : slide.width;
  for (size_t i = 0; i < s.targets.size(); ++i) {
    const float t = s.targets[i];
    // One line per position, spanning everything that matched it.
    bool seen = false;
    for (size_t j = 0; j < i; ++j) seen = seen || std::fabs(s.targets[j] - t) <= 0.01f;
    if (seen) continue;
    float from = box.lo[across], to = box.hi[across];
    for (size_t j = i; j < s.targets.size(); ++j) {
      if (std::fabs(s.targets[j] - t) > 0.01f) continue;
      if (s.sources[j] < 0) {
        from = 0, to = slideAcross;
      } else {
        from = std::min(from, others[s.sources[j]].lo[across]);
        to = std::max(to, others[s.sources[j]].hi[across]);
      }
    }
    out.push_back({a == 0 ? Guide::Kind::Vertical : Guide::Kind::Horizontal, t, from, to});
  }
  for (const Gap& g : s.gaps) out.push_back({a == 0 ? Guide::Kind::GapX : Guide::Kind::GapY, g.at, g.from, g.to});
}

std::vector<Box> Boxes(const std::vector<draw::Rect>& rects) {
  std::vector<Box> out;
  out.reserve(rects.size());
  for (const draw::Rect& r : rects) out.emplace_back(r);
  return out;
}

float GridAdjust(float value, float grid) { return grid > 0 ? std::round(value / grid) * grid - value : 0; }

}  // namespace

Snap SnapMove(const draw::Rect& moving, const std::vector<draw::Rect>& others, draw::Size slide, const SnapSettings& set) {
  const std::vector<Box> boxes = Boxes(others);
  const Box m(moving);
  AxisSnap axes[2];
  for (int a = 0; a < 2; ++a) {
    Align(axes[a], {m.lo[a], m.Mid(a), m.hi[a]}, boxes, a == 0 ? slide.width : slide.height, set, a);
    if (set.spacing) Spacing(axes[a], m, boxes, set, a);
  }
  Snap out;
  out.dx = axes[0].found ? axes[0].adj : GridAdjust(m.lo[0], set.grid);
  out.dy = axes[1].found ? axes[1].adj : GridAdjust(m.lo[1], set.grid);
  const Box final(Offset(moving, out.dx, out.dy));
  for (int a = 0; a < 2; ++a)
    if (axes[a].found) AlignGuides(axes[a], final, boxes, slide, a, out.guides);
  return out;
}

Snap SnapResize(const draw::Rect& box, Edges edges, const std::vector<draw::Rect>& others, draw::Size slide,
                const SnapSettings& set) {
  const std::vector<Box> boxes = Boxes(others);
  const Box m(box);
  AxisSnap axes[2];
  const bool lo[2] = {edges.left, edges.top}, hi[2] = {edges.right, edges.bottom};
  for (int a = 0; a < 2; ++a) {
    std::vector<float> features;
    if (lo[a]) features.push_back(m.lo[a]);
    if (hi[a]) features.push_back(m.hi[a]);
    if (!features.empty()) Align(axes[a], features, boxes, a == 0 ? slide.width : slide.height, set, a);
  }
  Snap out;
  const float gx = edges.left ? m.lo[0] : m.hi[0], gy = edges.top ? m.lo[1] : m.hi[1];
  out.dx = axes[0].found ? axes[0].adj : (edges.left || edges.right) ? GridAdjust(gx, set.grid) : 0;
  out.dy = axes[1].found ? axes[1].adj : (edges.top || edges.bottom) ? GridAdjust(gy, set.grid) : 0;
  draw::Rect r = box;
  if (edges.left) r.left += out.dx; else if (edges.right) r.right += out.dx;
  if (edges.top) r.top += out.dy; else if (edges.bottom) r.bottom += out.dy;
  const Box final(r);
  for (int a = 0; a < 2; ++a)
    if (axes[a].found) AlignGuides(axes[a], final, boxes, slide, a, out.guides);
  return out;
}

}  // namespace fw::slide::objects
