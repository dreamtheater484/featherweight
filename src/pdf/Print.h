#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

#include "Outcome.h"

class Document;

namespace print {

// Shows the Windows print dialog and prints. Filled-in form values are included.
// `progress(done, total)` is called between pages so the window can repaint.
PrintOutcome PrintDocument(HWND owner, Document& doc, int currentPage, const std::wstring& title,
                           const std::function<void(int, int)>& progress, std::wstring& message);

#if FW_TEST_SCREENS
// The print options page (resource.rc) made hidden in the interface's language, and each label that
// doesn't fit its control ("text" needs N px, has M): for the clipping check (clipcheck).
std::vector<std::wstring> ClippedOptions(HWND parent);
#endif

}  // namespace print
