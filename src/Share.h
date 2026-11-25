#pragma once

// Windows' share sheet (F1, f1-pdf): Send as attachment and Share as PDF, in the three editors
// and the PDF tab. The share UI and the Windows Runtime parts it needs load only when something
// is shared; the file goes to whatever app the person picks (mail, a chat, nearby sharing), never
// to anything of Featherweight's. ShareSheet.cpp is the Windows Runtime part (no app code, so the
// unit tests reach it); Share.cpp the tab's part.

#include <windows.h>
#include <unknwn.h>

#include <string>

class TabContent;

namespace share {

// Opens the share sheet for the file at `path`, titled `title`. False when Windows can't share
// from this window (the sheet itself shows nothing then).
bool ShareFile(HWND hwnd, const std::wstring& path, const std::wstring& title);

// For the unit tests: puts the file at `path` into `package` (an IDataPackage) as the share
// sheet's request does, and sets the event `done` once it's there (or Windows couldn't find it).
bool PutFileForTesting(IUnknown* package, const std::wstring& path, HANDLE done);

// Shares the tab's file as it is saved (`asPdf` false: Send as attachment), or the tab as a PDF
// (Export PDF to a copy in TempFolder). A PDF with unsaved changes is shared as a copy with them.
// False with a plain message when there's nothing to share yet (a new document: save it first).
bool ShareTab(HWND hwnd, TabContent& tab, bool asPdf, std::wstring& message);

// A folder of its own in %TEMP% for copies made to be shared (Share as PDF): the copies of
// earlier shares are removed once they're a day old.
std::wstring TempFolder();

}  // namespace share
