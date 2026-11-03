#pragma once

// Helpers shared by the fwpkg tests.
#include <windows.h>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "pkg/Package.h"
#include "pkg/Snapshot.h"
#include "pkg/Zip.h"

namespace test {

inline std::vector<uint8_t> Bytes(std::string_view s) { return std::vector<uint8_t>(s.begin(), s.end()); }

inline std::string Text(const std::vector<uint8_t>& b) { return std::string(b.begin(), b.end()); }

// A zip written by ZipWriter, entries in order.
inline std::vector<uint8_t> MakeZip(const std::vector<std::pair<std::string, std::string>>& entries,
                                    fw::pkg::ZipWriter::Compression c = fw::pkg::ZipWriter::Compression::Deflate,
                                    std::string_view comment = {}) {
  fw::pkg::MemorySink sink;
  fw::pkg::ZipWriter w(sink);
  for (const auto& [name, data] : entries) w.Add(name, Bytes(data), nullptr, c);
  w.Finish(comment);
  return sink.bytes;
}

// The smallest useful package: a document with a picture and a link.
inline std::vector<std::pair<std::string, std::string>> MiniPackage() {
  return {
      {"[Content_Types].xml",
       "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n"
       "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
       "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
       "<Default Extension=\"XML\" ContentType=\"application/xml\"/>"
       "<Default Extension=\"png\" ContentType=\"image/png\"/>"
       "<Override PartName=\"/Word/Document.xml\" ContentType=\"application/test.main+xml\"/>"
       "</Types>"},
      {"_rels/.rels",
       "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
       "<Relationship Id=\"rId1\" Type=\"urn:officeDocument\" Target=\"word/document.xml\"/></Relationships>"},
      {"word/", ""},
      {"word/document.xml",
       "<w:document xmlns:w=\"urn:w\" xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\">"
       "<w:pic r:embed=\"rId1\"/><w:link r:id=\"rId2\"/></w:document>"},
      {"word/_rels/document.xml.rels",
       "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
       "<Relationship Id=\"rId1\" Type=\"urn:image\" Target=\"media/a%20b.png\"/>"
       "<Relationship Id=\"rId2\" Type=\"urn:hyperlink\" Target=\"https://example.com/\" TargetMode=\"External\"/>"
       "</Relationships>"},
      {"word/media/a b.png", std::string("\x89PNG\r\n\x1a\n", 8) + std::string(100, '\x07')},
  };
}

inline std::unique_ptr<fw::pkg::Package> OpenBytes(std::vector<uint8_t> bytes, fw::pkg::Status& st,
                                                   const fw::pkg::Limits& limits = fw::pkg::Limits::Default()) {
  return fw::pkg::Package::Open(fw::pkg::Snapshot::FromBytes(std::move(bytes)), limits, st);
}

// A unique file or folder name in the temp folder.
inline std::wstring TempPath(const wchar_t* stem) {
  wchar_t dir[MAX_PATH];
  GetTempPathW(MAX_PATH, dir);
  static int counter = 0;
  return std::wstring(dir) + L"fw_tests_" + stem + L"_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
         std::to_wstring(++counter);
}

inline bool WriteFileBytes(const std::wstring& path, const std::vector<uint8_t>& data) {
  HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  DWORD put = 0;
  bool ok = WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &put, nullptr) && put == data.size();
  CloseHandle(h);
  return ok;
}

}  // namespace test
