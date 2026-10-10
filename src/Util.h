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

void SetClipboardText(const std::wstring& text);
std::wstring GetClipboardText();

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
