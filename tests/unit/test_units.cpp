#include "TestHarness.h"

#include <type_traits>

#include "core/Units.h"

using namespace fw;

// The point of the types: units can't be mixed up or made from bare numbers by accident.
static_assert(!std::is_convertible_v<Emu, Twips>);
static_assert(!std::is_convertible_v<Twips, HalfPoints>);
static_assert(!std::is_convertible_v<int, Twips>);
static_assert(!std::is_convertible_v<FiftiethsPercent, Twips>);
static_assert(UnitCast<Twips>(Emu(914400)) == Twips(1440));  // conversions work at compile time

TEST(Units_Conversions) {
  CHECK(UnitCast<Twips>(Emu(914400)) == Twips(1440));  // one inch
  CHECK(UnitCast<Emu>(Twips(1)) == Emu(635));
  CHECK(UnitCast<Emu>(HalfPoints(24)) == Emu(152400));  // 12 pt
  CHECK(UnitCast<HalfPoints>(Twips(240)) == HalfPoints(24));
  CHECK(UnitCast<EighthPoints>(HalfPoints(1)) == EighthPoints(4));
  CHECK_NEAR(ToPoints(HalfPoints(21)), 10.5, 1e-9);
  CHECK_NEAR(ToPoints(EighthPoints(4)), 0.5, 1e-9);
  CHECK_NEAR(ToPoints(Emu(12700)), 1.0, 1e-9);
  CHECK_NEAR(ToDips(Twips(1440)), 96.0, 1e-9);
}

TEST(Units_RoundToNearest) {
  CHECK(UnitCast<Twips>(Emu(317)) == Twips(0));  // 0.499 twips
  CHECK(UnitCast<Twips>(Emu(318)) == Twips(1));  // 0.501 twips
  CHECK(UnitCast<Twips>(Emu(-318)) == Twips(-1));
  CHECK(UnitCast<HalfPoints>(Twips(5)) == HalfPoints(1));  // exactly half: away from zero
  CHECK(UnitCast<HalfPoints>(Twips(-5)) == HalfPoints(-1));
  CHECK(FromPoints<HalfPoints>(10.5) == HalfPoints(21));
  CHECK(FromPoints<Emu>(72.0) == Emu(914400));
}

TEST(Units_Arithmetic) {
  Twips a(720), b(360);
  CHECK(a + b == Twips(1080));
  CHECK(a - b == Twips(360));
  CHECK(a * 2 == Twips(1440));
  CHECK(a / 2 == b);
  CHECK(-a == Twips(-720));
  CHECK(b < a);
  a += b;
  CHECK(a.Value() == 1080);
  a -= Twips(80);
  CHECK(a == Twips(1000));
}

TEST(Units_FiftiethsPercent) {
  CHECK_NEAR(ToFraction(FiftiethsPercent(5000)), 1.0, 1e-9);
  CHECK_NEAR(ToFraction(FiftiethsPercent(2500)), 0.5, 1e-9);
  CHECK(FiftiethsFromFraction(0.25) == FiftiethsPercent(1250));
}
