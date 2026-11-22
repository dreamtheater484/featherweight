#include "slide/view/SlideDesign.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "core/Strings.h"
#include "pkg/XmlWriter.h"

namespace fw::slide::view {

namespace {

std::string Attr(std::string_view v) {
  std::string out;
  pkg::AppendEscapedAttribute(out, v);
  return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Slide size

const std::vector<SlideSizeChoice>& SlideSizes() {
  // 12192000 x 6858000 and 9144000 x 6858000 EMU, as PowerPoint writes them.
  static const std::vector<SlideSizeChoice> sizes = {
      {L"Widescreen (16:9)", {960, 540}, ""},
      {L"Standard (4:3)", {720, 540}, "screen4x3"},
  };
  return sizes;
}

int SlideSizeChoiceOf(draw::Size size) {
  const std::vector<SlideSizeChoice>& all = SlideSizes();
  for (size_t i = 0; i < all.size(); ++i)
    if (std::abs(all[i].size.width - size.width) < 0.5f && std::abs(all[i].size.height - size.height) < 0.5f)
      return static_cast<int>(i);
  return -1;
}

bool AsksHowToScale(draw::Size from, draw::Size to) {
  return to.width < from.width - 0.01f || to.height < from.height - 0.01f;
}

SizeTransform ScaleContent(draw::Size from, draw::Size to, ContentScale how) {
  SizeTransform t;
  if (from.width <= 0 || from.height <= 0 || to.width <= 0 || to.height <= 0) return t;
  const float rw = to.width / from.width, rh = to.height / from.height;
  if (!AsksHowToScale(from, to)) {
    t.scale = 1;  // a larger slide: the content keeps its size
  } else {
    t.scale = how == ContentScale::Maximize ? std::max(rw, rh) : std::min(rw, rh);
    t.scale = std::min(t.scale, 1.0f);
  }
  // Centered on the new slide.
  t.dx = (to.width - from.width * t.scale) / 2;
  t.dy = (to.height - from.height * t.scale) / 2;
  if (std::abs(t.dx) < 0.005f) t.dx = 0;
  if (std::abs(t.dy) < 0.005f) t.dy = 0;
  return t;
}

// ---------------------------------------------------------------------------
// Colors and themes

std::string HexColor(uint32_t rgb) {
  char buf[8];
  std::snprintf(buf, sizeof buf, "%06X", static_cast<unsigned>(rgb & 0xFFFFFF));
  return buf;
}

draw::Color ColorOf(uint32_t rgb) {
  return {((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, 1};
}

const std::vector<ColorScheme>& ColorSchemes() {
  //                                      dk1       lt1       dk2       lt2       accent1-6                                                 hlink     folHlink
  static const std::vector<ColorScheme> schemes = {
      {L"Office", {0x000000, 0xFFFFFF, 0x44546A, 0xE7E6E6, 0x4472C4, 0xED7D31, 0xA5A5A5, 0xFFC000, 0x5B9BD5, 0x70AD47, 0x0563C1, 0x954F72}},
      {L"Office 2007-2010", {0x000000, 0xFFFFFF, 0x1F497D, 0xEEECE1, 0x4F81BD, 0xC0504D, 0x9BBB59, 0x8064A2, 0x4BACC6, 0xF79646, 0x0000FF, 0x800080}},
      {L"Grayscale", {0x000000, 0xFFFFFF, 0x000000, 0xF8F8F8, 0xDDDDDD, 0xB2B2B2, 0x969696, 0x808080, 0x5F5F5F, 0x4D4D4D, 0x5F5F5F, 0x919191}},
      {L"Blue Warm", {0x000000, 0xFFFFFF, 0x242852, 0xACCBF9, 0x4A66AC, 0x629DD1, 0x297FD5, 0x7F8FA9, 0x5AA2AE, 0x9D90A0, 0x9454C3, 0x3EBBF0}},
      {L"Blue", {0x000000, 0xFFFFFF, 0x17406D, 0xDBEFF9, 0x0F6FC6, 0x009DD9, 0x0BD0D9, 0x10CF9B, 0x7CCA62, 0xA5C249, 0xF49100, 0x85DFD0}},
      {L"Blue Green", {0x000000, 0xFFFFFF, 0x373545, 0xCEDBE6, 0x3494BA, 0x58B6C0, 0x75BDA7, 0x7A8C8E, 0x84ACB6, 0x2683C6, 0x6B9F25, 0x9F6715}},
      {L"Green", {0x000000, 0xFFFFFF, 0x455F51, 0xE3DED1, 0x549E39, 0x8AB833, 0xC0CF3A, 0x029676, 0x4AB5C4, 0x0989B1, 0x6B9F25, 0xBA6906}},
      {L"Yellow", {0x000000, 0xFFFFFF, 0x39302A, 0xE5DEDB, 0xFFCA08, 0xF8931D, 0xCE8D3E, 0xEC7016, 0xE64823, 0x9C6A6A, 0x2998E3, 0x7F723D}},
      {L"Orange", {0x000000, 0xFFFFFF, 0x637052, 0xCCDDEA, 0xE48312, 0xBD582C, 0x865640, 0x9B8357, 0xC2BC80, 0x94A088, 0x2998E3, 0x8C8C8C}},
      {L"Red", {0x000000, 0xFFFFFF, 0x323232, 0xE5C243, 0xA5300F, 0xD55816, 0xE19825, 0xB19C7D, 0x7F5F52, 0xB27D49, 0x6B9F25, 0xB26B02}},
      {L"Violet", {0x000000, 0xFFFFFF, 0x373545, 0xDCD8DC, 0xAD84C6, 0x8784C7, 0x5D739A, 0x6997AF, 0x84ACB6, 0x6F8183, 0x69A020, 0x8C8C8C}},
      {L"Paper", {0x000000, 0xFFFFFF, 0x444D26, 0xFEFAC9, 0xA5B592, 0xF3A447, 0xE7BC29, 0xD092A7, 0x9C85C0, 0x809EC2, 0x8E58B6, 0x7F6F6F}},
  };
  return schemes;
}

std::string ColorSchemeXml(const ColorScheme& scheme, const Prefixes& ns) {
  static const char* const kNames[12] = {"dk1",     "lt1",     "dk2",     "lt2",     "accent1", "accent2",
                                         "accent3", "accent4", "accent5", "accent6", "hlink",   "folHlink"};
  const std::string& a = ns.a;
  std::string x = "<" + a + ":clrScheme name=\"" + Attr(WideToUtf8(scheme.name)) + "\">";
  for (int i = 0; i < 12; ++i) {
    const uint32_t c = scheme.colors[i] & 0xFFFFFF;
    x += "<" + a + ":" + kNames[i] + ">";
    // Office writes black text and a white background as the system's window colors.
    if (i == 0 && c == 0x000000) x += "<" + a + ":sysClr val=\"windowText\" lastClr=\"000000\"/>";
    else if (i == 1 && c == 0xFFFFFF) x += "<" + a + ":sysClr val=\"window\" lastClr=\"FFFFFF\"/>";
    else x += "<" + a + ":srgbClr val=\"" + HexColor(c) + "\"/>";
    x += "</" + a + ":" + kNames[i] + ">";
  }
  x += "</" + a + ":clrScheme>";
  return x;
}

std::string FontSchemeXml(std::string_view name, std::string_view major, std::string_view minor, const Prefixes& ns) {
  const std::string& a = ns.a;
  auto font = [&](const char* which, std::string_view face) {
    return "<" + a + ":" + which + "><" + a + ":latin typeface=\"" + Attr(face) + "\"/><" + a + ":ea typeface=\"\"/><" + a +
           ":cs typeface=\"\"/></" + a + ":" + which + ">";
  };
  return "<" + a + ":fontScheme name=\"" + Attr(name) + "\">" + font("majorFont", major) + font("minorFont", minor) + "</" + a +
         ":fontScheme>";
}

std::string BackgroundXml(const Background& bg, std::string_view pictureRel, const Prefixes& ns) {
  const std::string& a = ns.a;
  const std::string& p = ns.p;
  const std::string open = "<" + p + ":bg>", close = "</" + p + ":bg>";
  if (bg.kind == Background::Kind::Theme) {
    const int idx = std::clamp(bg.themeIndex, 1001, 1003);
    return open + "<" + p + ":bgRef idx=\"" + std::to_string(idx) + "\"><" + a + ":schemeClr val=\"bg1\"/></" + p + ":bgRef>" + close;
  }
  std::string fill;
  switch (bg.kind) {
    case Background::Kind::Gradient: {
      // DrawingML angles: 60000ths of a degree, clockwise from left to right.
      const long long ang = static_cast<long long>(((bg.angle % 360) + 360) % 360) * 60000;
      fill = "<" + a + ":gradFill flip=\"none\" rotWithShape=\"1\"><" + a + ":gsLst><" + a + ":gs pos=\"0\"><" + a +
             ":srgbClr val=\"" + HexColor(bg.color) + "\"/></" + a + ":gs><" + a + ":gs pos=\"100000\"><" + a +
             ":srgbClr val=\"" + HexColor(bg.color2) + "\"/></" + a + ":gs></" + a + ":gsLst><" + a + ":lin ang=\"" +
             std::to_string(ang) + "\" scaled=\"1\"/><" + a + ":tileRect/></" + a + ":gradFill>";
      break;
    }
    case Background::Kind::Picture:
      if (pictureRel.empty()) return {};
      fill = "<" + a + ":blipFill dpi=\"0\" rotWithShape=\"1\"><" + a + ":blip " + ns.r + ":embed=\"" + Attr(pictureRel) +
             "\"/><" + a + ":srcRect/><" + a + ":stretch><" + a + ":fillRect/></" + a + ":stretch></" + a + ":blipFill>";
      break;
    default:
      fill = "<" + a + ":solidFill><" + a + ":srgbClr val=\"" + HexColor(bg.color) + "\"/></" + a + ":solidFill>";
      break;
  }
  return open + "<" + p + ":bgPr>" + fill + "<" + a + ":effectLst/></" + p + ":bgPr>" + close;
}

const std::vector<GradientPreset>& GradientPresets() {
  static const std::vector<GradientPreset> presets = {
      {L"Top to bottom", 90}, {L"Left to right", 0}, {L"Diagonal down", 45}, {L"Diagonal up", 315},
      {L"Bottom to top", 270}, {L"Right to left", 180},
  };
  return presets;
}

const std::vector<BuiltInTheme>& BuiltInThemes() {
  using K = Background::Kind;
  // Fonts every Windows PC has, so slides look the same here and in PowerPoint.
  static const std::vector<BuiltInTheme> themes = {
      {L"Office", 0, "Calibri Light", "Calibri", false, {K::Theme, 0xFFFFFF, 0xFFFFFF, 90, 1001, false}},
      {L"Paper", 11, "Georgia", "Georgia", false, {K::Solid, 0xFEFAC9, 0xFEFAC9, 90, 1001, false}},
      {L"Ocean", 4, "Segoe UI Semibold", "Segoe UI", false, {K::Gradient, 0xFFFFFF, 0xDBEFF9, 90, 1001, false}},
      {L"Grove", 6, "Cambria", "Calibri", false, {K::Solid, 0xF4F2EC, 0xF4F2EC, 90, 1001, false}},
      {L"Monochrome", 2, "Arial", "Arial", false, {K::Solid, 0xFFFFFF, 0xFFFFFF, 90, 1001, false}},
      {L"Slate", 3, "Segoe UI Light", "Segoe UI", true, {K::Solid, 0x242852, 0x242852, 90, 1001, false}},
      {L"Ember", 9, "Franklin Gothic Medium", "Corbel", true, {K::Gradient, 0x3A3A3A, 0x141414, 90, 1001, false}},
  };
  return themes;
}

std::vector<std::pair<const char*, const char*>> ColorMapOf(bool dark) {
  return {{"bg1", dark ? "dk1" : "lt1"},     {"tx1", dark ? "lt1" : "dk1"},       {"bg2", dark ? "dk2" : "lt2"},
          {"tx2", dark ? "lt2" : "dk2"},     {"accent1", "accent1"},              {"accent2", "accent2"},
          {"accent3", "accent3"},            {"accent4", "accent4"},              {"accent5", "accent5"},
          {"accent6", "accent6"},            {"hlink", "hlink"},                  {"folHlink", "folHlink"}};
}

}  // namespace fw::slide::view
