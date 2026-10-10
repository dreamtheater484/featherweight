#include "Anim.h"

#include <windows.h>

#include "Theme.h"

namespace anim {
namespace {
double g_until = 0;
}

double Now() {
  static LARGE_INTEGER freq = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return f;
  }();
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return static_cast<double>(t.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
}

void KeepAlive(double untilMs) {
  if (untilMs > g_until) g_until = untilMs;
}

bool Running() { return Now() < g_until; }

}  // namespace anim

void Anim::To(float target, float durationMs) {
  if (target == to_) return;
  from_ = V();
  to_ = target;
  start_ = anim::Now();
  duration_ = theme::AnimationsEnabled() ? durationMs : 0;
  anim::KeepAlive(start_ + duration_ + 20);
}

void Anim::Snap(float v) {
  from_ = to_ = v;
  duration_ = 0;
}

float Anim::V() const {
  if (duration_ <= 0) return to_;
  double t = (anim::Now() - start_) / duration_;
  if (t >= 1) return to_;
  if (t <= 0) return from_;
  double inv = 1 - t;
  double eased = 1 - inv * inv * inv;  // ease-out cubic
  return static_cast<float>(from_ + (to_ - from_) * eased);
}
