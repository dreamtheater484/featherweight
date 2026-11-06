#pragma once

#include <windows.h>

#include <string>

// Small helpers for building native popup menus.
namespace menu {

void AppendItem(HMENU m, UINT id, const std::wstring& text, bool enabled = true, bool checked = false,
                bool radio = false);
void AppendSeparator(HMENU m);
void AppendSubmenu(HMENU m, HMENU sub, const std::wstring& text, bool enabled = true);

}  // namespace menu
