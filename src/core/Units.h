#pragma once

#include <cmath>
#include <compare>
#include <cstdint>

namespace fw {

// Office files measure lengths in several integer units. Each one is its own type, so they can't
// be mixed up (plan §4.3 rule 9): adding twips to EMUs, or passing half-points where eighths of a
// point are expected, doesn't compile. Conversions are explicit and round to the nearest unit.
//
//   1 inch = 72 pt = 1440 twips = 914400 EMU
//   1 pt   = 20 twips = 2 half-points = 8 eighths of a point = 12700 EMU
//   100 %  = 5000 fiftieths of a percent

template <typename Tag, typename Rep>
class Quantity {
 public:
  using tag = Tag;
  using rep = Rep;

  constexpr Quantity() = default;
  constexpr explicit Quantity(Rep v) : v_(v) {}

  constexpr Rep Value() const { return v_; }

  constexpr auto operator<=>(const Quantity&) const = default;

  constexpr Quantity operator-() const { return Quantity(static_cast<Rep>(-v_)); }
  constexpr Quantity operator+(Quantity o) const { return Quantity(static_cast<Rep>(v_ + o.v_)); }
  constexpr Quantity operator-(Quantity o) const { return Quantity(static_cast<Rep>(v_ - o.v_)); }
  constexpr Quantity operator*(Rep k) const { return Quantity(static_cast<Rep>(v_ * k)); }
  constexpr Quantity operator/(Rep k) const { return Quantity(static_cast<Rep>(v_ / k)); }
  constexpr Quantity& operator+=(Quantity o) { return *this = *this + o; }
  constexpr Quantity& operator-=(Quantity o) { return *this = *this - o; }

 private:
  Rep v_ = 0;
};

// Lengths. kPerPoint is how many of the unit make one point.
struct EmuTag { static constexpr int64_t kPerPoint = 12700; };
struct TwipsTag { static constexpr int64_t kPerPoint = 20; };
struct HalfPointsTag { static constexpr int64_t kPerPoint = 2; };
struct EighthPointsTag { static constexpr int64_t kPerPoint = 8; };

using Emu = Quantity<EmuTag, int64_t>;                    // DrawingML positions and sizes
using Twips = Quantity<TwipsTag, int32_t>;                // Word page, paragraph and table measures
using HalfPoints = Quantity<HalfPointsTag, int32_t>;      // Word font sizes (w:sz)
using EighthPoints = Quantity<EighthPointsTag, int32_t>;  // Word border widths (w:bdr/@w:sz)

// Fiftieths of a percent: Word table widths of type "pct" (5000 == 100 %). Not a length.
struct FiftiethsPercentTag {};
using FiftiethsPercent = Quantity<FiftiethsPercentTag, int32_t>;

// a * num / den, rounded to the nearest integer, halves away from zero.
constexpr int64_t MulDivRound(int64_t a, int64_t num, int64_t den) {
  int64_t n = a * num;
  return n >= 0 ? (n + den / 2) / den : -((-n + den / 2) / den);
}

// Converts between length units, rounding to the nearest unit of `To`.
template <typename To, typename FromTag, typename FromRep>
constexpr To UnitCast(Quantity<FromTag, FromRep> from) {
  return To(static_cast<typename To::rep>(
      MulDivRound(static_cast<int64_t>(from.Value()), To::tag::kPerPoint, FromTag::kPerPoint)));
}

template <typename Tag, typename Rep>
constexpr double ToPoints(Quantity<Tag, Rep> q) {
  return static_cast<double>(q.Value()) / static_cast<double>(Tag::kPerPoint);
}

// Device-independent pixels (1/96 inch), the unit Direct2D draws in.
template <typename Tag, typename Rep>
constexpr double ToDips(Quantity<Tag, Rep> q) {
  return ToPoints(q) * 96.0 / 72.0;
}

template <typename To>
To FromPoints(double pt) {
  return To(static_cast<typename To::rep>(std::llround(pt * static_cast<double>(To::tag::kPerPoint))));
}

// 1.0 == 100 %.
constexpr double ToFraction(FiftiethsPercent p) { return p.Value() / 5000.0; }
inline FiftiethsPercent FiftiethsFromFraction(double f) {
  return FiftiethsPercent(static_cast<int32_t>(std::lround(f * 5000.0)));
}

}  // namespace fw
