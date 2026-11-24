#pragma once

#include <string>
#include <vector>

// The user's own templates (S4, plan §7.1): the personal templates folder Word and Excel save
// templates to, for File → New. That is the folder Word's, Excel's or PowerPoint's PersonalTemplates option
// names when set, else "Documents\Custom Office Templates", Office's default.
namespace usertemplates {

// The folders, the options' first (those that exist), the default last (whether or not it exists).
std::vector<std::wstring> Folders();
// The templates there that a shipped editor opens (.dotx, .dotm, .ott; .xltx, .xltm, .ots; .potx, .potm, .otp), by
// name, at most `max`.
std::vector<std::wstring> List(size_t max);
std::vector<std::wstring> List(size_t max, const std::vector<std::wstring>& folders);  // from these

}  // namespace usertemplates
