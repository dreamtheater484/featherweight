#pragma once

#include <windows.h>

#include <functional>
#include <string>

#include "Outcome.h"

class Document;

namespace print {

// Shows the Windows print dialog and prints. Filled-in form values are included.
// `progress(done, total)` is called between pages so the window can repaint.
PrintOutcome PrintDocument(HWND owner, Document& doc, int currentPage, const std::wstring& title,
                           const std::function<void(int, int)>& progress, std::wstring& message);

}  // namespace print
