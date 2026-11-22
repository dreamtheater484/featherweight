#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "draw/Painter.h"

// Design for presentations (P2, p2-slides; plan §8.2 "Design", "Slides"): the slide sizes
// PowerPoint offers and how content scales to a new size, the built-in theme colors and themes
// (compact data: twelve colors, two fonts, light or dark, a background), and backgrounds. The XML
// these give is what the edit operations write (theme parts' a:clrScheme and a:fontScheme, a
// master's p:clrMap, p:bg). Headless.
namespace fw::slide {
struct SlideBase;
}

namespace fw::slide::view {

// ---- Slide size ----

struct SlideSizeChoice {
  const wchar_t* name;
  draw::Size size;    // points
  const char* type;   // p:sldSz type as PowerPoint writes it; empty: none (custom)
};
// Widescreen (16:9, 13.333 x 7.5 in) and Standard (4:3, 10 x 7.5 in), in that order.
const std::vector<SlideSizeChoice>& SlideSizes();
// The choice a size is, or -1 (a custom size).
int SlideSizeChoiceOf(draw::Size size);

// How content follows a new slide size: PowerPoint's two choices when the slide gets smaller in
// a direction (Maximize: as large as the new slide allows in its tighter direction, cut at the
// other; Ensure fit: all of it on the slide). A slide that grows keeps its content's size, centered.
enum class ContentScale : uint8_t { Maximize, EnsureFit };
// A uniform scale about the old slide's origin, then an offset: x' = x * scale + dx (points).
struct SizeTransform {
  float scale = 1;
  float dx = 0, dy = 0;
  bool Identity() const { return scale == 1 && dx == 0 && dy == 0; }
};
SizeTransform ScaleContent(draw::Size from, draw::Size to, ContentScale how);
// Whether PowerPoint asks how to scale (the new size is smaller in width or height).
bool AsksHowToScale(draw::Size from, draw::Size to);

// ---- XML ----

// The prefixes a part uses for DrawingML (a), PresentationML (p) and relationships (r): what the
// XML below is written with, so it matches the part it goes into.
struct Prefixes {
  std::string a = "a", p = "p", r = "r";
};

// ---- Theme colors and themes ----

// A theme's color scheme: dk1, lt1, dk2, lt2, accent1-6, hlink, folHlink (0xRRGGBB).
struct ColorScheme {
  const wchar_t* name;
  uint32_t colors[12];
};
// The built-in schemes, Office's first.
const std::vector<ColorScheme>& ColorSchemes();
// The scheme's a:clrScheme element (dk1 and lt1 black and white as Office writes them: sysClr).
std::string ColorSchemeXml(const ColorScheme& scheme, const Prefixes& ns = {});
// a:fontScheme with these heading (major) and body (minor) Latin typefaces, for a theme without
// one. A theme that has one keeps it and changes only its two a:latin typefaces (its East Asian
// and script fonts stay).
std::string FontSchemeXml(std::string_view name, std::string_view major, std::string_view minor, const Prefixes& ns = {});

// A background, as Format Background sets it.
struct Background {
  enum class Kind : uint8_t { Solid, Gradient, Picture, Theme };
  Kind kind = Kind::Solid;
  uint32_t color = 0xFFFFFF;    // Solid; Gradient: the first stop (0xRRGGBB)
  uint32_t color2 = 0xD9D9D9;   // Gradient: the last stop
  int angle = 90;               // Gradient: linear, degrees clockwise from left to right (90: top to bottom)
  int themeIndex = 1001;        // Theme: the theme's background fill (1001-1003) in bg1
  bool hideGraphics = false;    // the master's and layout's own shapes aren't drawn (showMasterSp="0")
};
// The p:bg element: p:bgPr with the fill (a picture's blip is `pictureRel`, the part's
// relationship id), or p:bgRef for a theme background.
std::string BackgroundXml(const Background& bg, std::string_view pictureRel = {}, const Prefixes& ns = {});
// The gradient presets Format Background offers (angles), with names.
struct GradientPreset {
  const wchar_t* name;
  int angle;
};
const std::vector<GradientPreset>& GradientPresets();
// The background a slide shows now, as Format Background starts from: its own p:bg, else its
// layout's or master's. Colors resolved against the slide's theme. A picture fill reads as Picture.
Background ReadBackground(const fw::slide::SlideBase& slide);
// The color scheme the slide's theme matches, or -1.
int ColorSchemeOf(const fw::slide::SlideBase& slide);

// A built-in theme: a color scheme, heading and body fonts, light or dark text, a background.
struct BuiltInTheme {
  const wchar_t* name;
  int colors;           // into ColorSchemes()
  const char* major;    // heading typeface
  const char* minor;    // body typeface
  bool dark;            // light text on a dark background (the master's p:clrMap swaps bg and tx)
  Background background;
};
const std::vector<BuiltInTheme>& BuiltInThemes();
// The p:clrMap attributes for light or dark slides, as name="value" pairs in p:clrMap's order.
std::vector<std::pair<const char*, const char*>> ColorMapOf(bool dark);

// 0xRRGGBB as DrawingML writes it ("4472C4").
std::string HexColor(uint32_t rgb);
draw::Color ColorOf(uint32_t rgb);

}  // namespace fw::slide::view
