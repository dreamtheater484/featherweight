#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fw::pkg {

// Word's owner file: while Word has report.docx open, it keeps a small hidden file named
// ~$report.docx beside it with the name of the person editing it, and other copies of Word say
// "locked for editing by …". Featherweight reads it, and writes its own while it has unsaved
// edits, so Word users see the file is in use too (plan §6.3 "Working alongside Word").

// Excel keeps one beside an open workbook too, named and laid out its own way (OwnerStyle::Excel).
enum class OwnerStyle : uint8_t { Word, Excel };

// The owner file's path for a document. Word's rule: a name (without its extension) of up to 6
// characters gets "~$" in front, a 7-character name loses its first character to it, and a
// longer one its first two: report.docx -> ~$report.docx, Document1.docx -> ~$cument1.docx.
// Excel's: "~$" before the whole name, Budget 2026.xlsx -> ~$Budget 2026.xlsx.
std::wstring OwnerFilePath(const std::wstring& docPath, OwnerStyle style = OwnerStyle::Word);

// The owner file's content for `user`. Word's: 162 bytes, the name in the ANSI code page (its
// length in the first byte), then its UTF-16 length (2 bytes) and the name from offset 54, zeros
// after. Excel's (measured on Excel 16): 165 bytes, the same with the UTF-16 copy from offset 55
// and both padded with spaces.
std::vector<uint8_t> OwnerFileBytes(const std::wstring& user, OwnerStyle style = OwnerStyle::Word);
// The user an owner file of either kind names, or empty when it isn't one.
std::wstring ParseOwnerFile(const std::vector<uint8_t>& bytes);

// The user named in the owner file beside `docPath`, or empty when there is none.
std::wstring ReadOwner(const std::wstring& docPath, OwnerStyle style = OwnerStyle::Word);
// Writes our owner file beside `docPath`, hidden. Not when another user's is there already.
bool WriteOwner(const std::wstring& docPath, const std::wstring& user, OwnerStyle style = OwnerStyle::Word);
// Removes the owner file beside `docPath` if it names `user`: never another person's.
void RemoveOwner(const std::wstring& docPath, const std::wstring& user, OwnerStyle style = OwnerStyle::Word);

// True when another program has the file open for writing, as Word does while it's open.
bool IsLockedByAnother(const std::wstring& path);

// Who is editing a file elsewhere, for the "in use" banner: `locked` when another program holds
// it, and the name from its owner file (empty when there is none, or it names `self`, which is a
// leftover of ours).
struct InUse {
  bool locked = false;
  std::wstring user;
};
InUse CheckInUse(const std::wstring& docPath, const std::wstring& self, OwnerStyle style = OwnerStyle::Word);

}  // namespace fw::pkg
