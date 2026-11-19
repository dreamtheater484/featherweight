#pragma once

// Internal to sheet/pivot: XML helpers shared by reading and writing.
#include <cstdint>
#include <string>
#include <string_view>

#include <pugixml.hpp>

#include "sheet/pivot/Pivot.h"

namespace fw::sheet::pivot::detail {

std::string_view Local(pugi::xml_node n);
pugi::xml_node Child(pugi::xml_node n, std::string_view local);
pugi::xml_attribute Attr(pugi::xml_node n, std::string_view local);
bool Bool(pugi::xml_node n, std::string_view local, bool def);
uint32_t U32(pugi::xml_node n, std::string_view local, uint32_t def);
int64_t I64(pugi::xml_node n, std::string_view local, int64_t def);
std::string Str(pugi::xml_node n, std::string_view local);
double Number(std::string_view v, bool& ok);
bool IsoToSerial(std::string_view v, double& out);
bool TextEqual(std::string_view a, std::string_view b);  // Excel's: without case
bool ReadItem(pugi::xml_node n, Item& out);

}  // namespace fw::sheet::pivot::detail
