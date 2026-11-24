#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// OfficeArt ([MS-ODRAW]), the binary drawing format of Office 97-2003 files, as far as the formats
// that carry it share it: VML's shape types (o:spt), Excel 97-2003's sheet drawings (src/sheet/xls)
// and PowerPoint 97-2003's slides (src/slide/ppt).
namespace draw {

// Office's shape type numbers (MSOSPT, o:spt) with a DrawingML preset of the same shape; empty when
// there is none.
std::string_view PresetForSpt(int spt);

// An OfficeArtBlip record (its 8-byte header first: a BLIP store entry's picture, or one in a
// PowerPoint file's Pictures stream) as a picture file: its extension ("png", "jpeg", "bmp", "tiff",
// "emf", "wmf") and bytes. Metafiles are inflated, a WMF gets its placeable header, a DIB its .bmp
// file header. False for other kinds or damaged data.
bool DecodeBlip(std::span<const uint8_t> record, std::string& ext, std::vector<uint8_t>& out);

}  // namespace draw
