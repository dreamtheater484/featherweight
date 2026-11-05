#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace util {

std::wstring ExePath();
std::wstring ExeDir();
bool FileExists(const std::wstring& path);
bool DirectoryWritable(const std::wstring& dir);
std::wstring AppDataDir();  // %APPDATA%\Featherweight (created on demand)
std::wstring FullPath(const std::wstring& path);

bool ReadFileBytes(const std::wstring& path, std::string& out, size_t maxBytes = 64u << 20);
// Writes to a temporary file next to `path`, then replaces `path` in one step.
bool WriteFileAtomic(const std::wstring& path, const std::string& data);

// Puts text on the clipboard, with the same text as RTF ("Rich Text Format") and HTML (CF_HTML,
// "HTML Format") when given.
void SetClipboardText(const std::wstring& text, const std::string& rtf = {}, const std::string& html = {});
std::wstring GetClipboardText();
// The bytes of a clipboard format registered by name ("Rich Text Format", "HTML Format"), up to
// its first NUL; empty when the clipboard has none.
std::string GetClipboardBytes(const wchar_t* format);

int64_t UnixNow();

// Lowercase hex SHA-256 of a file or buffer; empty on failure.
std::string Sha256File(const std::wstring& path);
std::string Sha256(const void* data, size_t size);

// Opens http(s)/mailto links in the default browser or mail app. Other schemes are refused.
bool OpenExternalLink(const std::wstring& url);
bool IsSafeExternalLink(const std::wstring& url);

float DpiScale(HWND hwnd);
std::wstring LastErrorMessage(DWORD error);

}  // namespace util
