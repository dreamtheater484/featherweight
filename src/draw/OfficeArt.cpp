// OfficeArt ([MS-ODRAW]) pieces shared by VML, Excel 97-2003 and PowerPoint 97-2003: shape types
// as DrawingML presets, and BLIP records as picture files.
#include "draw/OfficeArt.h"

#include <algorithm>
#include <cmath>

#include "pkg/Zip.h"

namespace draw {
namespace {

uint32_t Le32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | uint32_t{p[3]} << 24; }

}  // namespace

std::string_view PresetForSpt(int spt) {
  struct Map {
    int spt;
    std::string_view preset;
  };
  static constexpr Map kMap[] = {
      {1, "rect"}, {2, "roundRect"}, {3, "ellipse"}, {4, "diamond"}, {5, "triangle"}, {6, "rtTriangle"},
      {7, "parallelogram"}, {8, "trapezoid"}, {9, "hexagon"}, {10, "octagon"}, {11, "plus"}, {12, "star5"},
      {13, "rightArrow"}, {15, "homePlate"}, {16, "cube"}, {20, "line"}, {21, "plaque"}, {22, "can"},
      {23, "donut"}, {32, "straightConnector1"}, {33, "bentConnector2"}, {34, "bentConnector3"},
      {35, "bentConnector4"}, {36, "bentConnector5"}, {37, "curvedConnector2"}, {38, "curvedConnector3"},
      {39, "curvedConnector4"}, {40, "curvedConnector5"}, {53, "ribbon"}, {54, "ribbon2"}, {55, "chevron"},
      {56, "pentagon"}, {57, "noSmoking"}, {58, "star8"}, {59, "star16"}, {60, "star32"}, {61, "wedgeRectCallout"},
      {62, "wedgeRoundRectCallout"}, {63, "wedgeEllipseCallout"}, {64, "wave"}, {65, "foldedCorner"},
      {66, "leftArrow"}, {67, "downArrow"}, {68, "upArrow"}, {69, "leftRightArrow"}, {70, "upDownArrow"},
      {71, "irregularSeal1"}, {72, "irregularSeal2"}, {73, "lightningBolt"}, {74, "heart"}, {75, "rect"},
      {76, "quadArrow"}, {84, "bevel"}, {85, "leftBracket"}, {86, "rightBracket"}, {87, "leftBrace"},
      {88, "rightBrace"}, {89, "leftUpArrow"}, {90, "bentUpArrow"}, {91, "bentArrow"}, {92, "star24"},
      {93, "stripedRightArrow"}, {94, "notchedRightArrow"}, {95, "blockArc"}, {96, "smileyFace"},
      {97, "verticalScroll"}, {98, "horizontalScroll"}, {99, "circularArrow"}, {109, "flowChartProcess"},
      {110, "flowChartDecision"}, {111, "flowChartInputOutput"}, {112, "flowChartPredefinedProcess"},
      {113, "flowChartInternalStorage"}, {114, "flowChartDocument"}, {115, "flowChartMultidocument"},
      {116, "flowChartTerminator"}, {117, "flowChartPreparation"}, {118, "flowChartManualInput"},
      {119, "flowChartManualOperation"}, {120, "flowChartConnector"}, {121, "flowChartPunchedCard"},
      {122, "flowChartPunchedTape"}, {123, "flowChartSummingJunction"}, {124, "flowChartOr"},
      {125, "flowChartCollate"}, {126, "flowChartSort"}, {127, "flowChartExtract"}, {128, "flowChartMerge"},
      {129, "flowChartOfflineStorage"}, {130, "flowChartOnlineStorage"}, {131, "flowChartMagneticTape"},
      {132, "flowChartMagneticDisk"}, {133, "flowChartMagneticDrum"}, {134, "flowChartDisplay"},
      {135, "flowChartDelay"}, {183, "sun"}, {184, "moon"}, {185, "bracketPair"}, {186, "bracePair"},
      {187, "star4"}, {188, "doubleWave"}, {202, "rect"}};
  for (const Map& m : kMap)
    if (m.spt == spt) return m.preset;
  return {};
}

bool DecodeBlip(std::span<const uint8_t> record, std::string& ext, std::vector<uint8_t>& out) {
  out.clear();
  if (record.size() < 8) return false;
  const uint16_t inst = static_cast<uint16_t>((record[0] | record[1] << 8) >> 4);
  const uint16_t type = static_cast<uint16_t>(record[2] | record[3] << 8);
  std::span<const uint8_t> b = record.subspan(8, std::min<size_t>(Le32(&record[4]), record.size() - 8));
  const size_t uids = inst & 1 ? 32 : 16;  // one or two UIDs
  if (b.size() < uids) return false;
  b = b.subspan(uids);
  switch (type) {
    case 0xF01D: case 0xF02A: ext = "jpeg"; break;
    case 0xF01E: ext = "png"; break;
    case 0xF01F: ext = "bmp"; break;
    case 0xF029: ext = "tiff"; break;
    case 0xF01A: ext = "emf"; break;
    case 0xF01B: ext = "wmf"; break;
    default: return false;
  }
  if (type == 0xF01A || type == 0xF01B) {
    // OfficeArtMetafileHeader: sizes, bounds, and the data deflated (zlib) or not.
    if (b.size() < 34) return false;
    const uint32_t size = Le32(&b[0]);
    int32_t bounds[4];
    for (int i = 0; i < 4; ++i) bounds[i] = static_cast<int32_t>(Le32(&b[4 + 4 * i]));
    const int32_t emuW = static_cast<int32_t>(Le32(&b[20]));
    const uint32_t saved = Le32(&b[28]);
    const uint8_t compression = b[32];
    std::span<const uint8_t> data = b.subspan(34, std::min<size_t>(saved, b.size() - 34));
    if (size > (64u << 20)) return false;
    if (compression == 0) {
      if (data.size() > 2 && (data[0] & 0x0F) == 8 && ((data[0] << 8) | data[1]) % 31 == 0) data = data.subspan(2);
      if (!fw::pkg::Inflate(data, size, out)) return false;
    } else {
      out.assign(data.begin(), data.end());
    }
    if (type == 0xF01B && !(out.size() >= 4 && out[0] == 0xD7 && out[1] == 0xCD && out[2] == 0xC6 && out[3] == 0x9A)) {
      // A WMF without its placeable header: one from the bounds, at their units per inch.
      const int32_t w = bounds[2] - bounds[0];
      const double inch = emuW > 0 && w > 0 ? w * 914400.0 / emuW : 1440;
      uint16_t hdr[11] = {0xCDD7, 0x9AC6, 0, static_cast<uint16_t>(bounds[0]), static_cast<uint16_t>(bounds[1]),
                          static_cast<uint16_t>(bounds[2]), static_cast<uint16_t>(bounds[3]),
                          static_cast<uint16_t>(std::clamp(std::lround(inch), 1L, 65535L)), 0, 0, 0};
      for (int i = 0; i < 10; ++i) hdr[10] ^= hdr[i];
      std::vector<uint8_t> head(22);
      for (int i = 0; i < 11; ++i) head[2 * i] = static_cast<uint8_t>(hdr[i]), head[2 * i + 1] = static_cast<uint8_t>(hdr[i] >> 8);
      out.insert(out.begin(), head.begin(), head.end());
    }
    return !out.empty();
  }
  if (b.size() < 2) return false;
  const std::span<const uint8_t> data = b.subspan(1);  // after the tag
  if (type == 0xF01F) {
    // A DIB: a .bmp needs its file header, with where the pixels start.
    if (data.size() < 40) return false;
    const uint32_t infoSize = Le32(&data[0]);
    const uint16_t bits = static_cast<uint16_t>(data[14] | data[15] << 8);
    const uint32_t compression = Le32(&data[16]);
    const uint32_t used = Le32(&data[32]);
    if (infoSize < 12 || infoSize > data.size()) return false;
    const uint64_t colors = used ? used : bits <= 8 ? (1u << bits) : 0;
    const uint64_t offset = 14 + infoSize + colors * 4 + (compression == 3 && infoSize == 40 ? 12 : 0);
    const uint64_t total = 14 + data.size();
    if (offset > total) return false;
    const uint8_t head[14] = {'B', 'M', static_cast<uint8_t>(total), static_cast<uint8_t>(total >> 8), static_cast<uint8_t>(total >> 16),
                              static_cast<uint8_t>(total >> 24), 0, 0, 0, 0, static_cast<uint8_t>(offset),
                              static_cast<uint8_t>(offset >> 8), static_cast<uint8_t>(offset >> 16), static_cast<uint8_t>(offset >> 24)};
    out.assign(std::begin(head), std::end(head));
  }
  out.insert(out.end(), data.begin(), data.end());
  return true;
}

}  // namespace draw
