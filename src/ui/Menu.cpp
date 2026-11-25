#include "ui/Menu.h"

#include "core/Lang.h"

namespace menu {

void AppendItem(HMENU m, UINT id, const std::wstring& text, bool enabled, bool checked, bool radio) {
  MENUITEMINFOW mi{sizeof(mi)};
  mi.fMask = MIIM_ID | MIIM_STRING | MIIM_STATE | MIIM_FTYPE;
  mi.fType = MFT_STRING | (radio ? MFT_RADIOCHECK : 0);
  mi.fState = (enabled ? MFS_ENABLED : MFS_DISABLED) | (checked ? MFS_CHECKED : 0);
  mi.wID = id;
  // The shortcut after the tab as the language writes it (Strg+S in German).
  const size_t tab = text.find(L'\t');
  const std::wstring shown = tab == std::wstring::npos ? text : text.substr(0, tab) + fw::lang::KeyNames(text.substr(tab));
  mi.dwTypeData = const_cast<wchar_t*>(shown.c_str());
  InsertMenuItemW(m, GetMenuItemCount(m), TRUE, &mi);
}

void AppendSeparator(HMENU m) { AppendMenuW(m, MF_SEPARATOR, 0, nullptr); }

void AppendSubmenu(HMENU m, HMENU sub, const std::wstring& text, bool enabled) {
  AppendMenuW(m, MF_POPUP | (enabled ? 0 : MF_GRAYED), reinterpret_cast<UINT_PTR>(sub), text.c_str());
}

}  // namespace menu
