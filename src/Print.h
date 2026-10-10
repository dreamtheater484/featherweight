#pragma once

#include <windows.h>

#include <functional>
#include <string>

class Document;

namespace print {

enum class Outcome { Printed, Cancelled, NotAllowed, Failed };

// Shows the Windows print dialog and prints. Filled-in form values are included.
// `progress(done, total)` is called between pages so the window can repaint.
Outcome PrintDocument(HWND owner, Document& doc, int currentPage, const std::wstring& title,
                      const std::function<void(int, int)>& progress, std::wstring& message);

}  // namespace print
