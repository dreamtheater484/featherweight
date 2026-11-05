#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fw::pkg {

// Word's owner file: while Word has report.docx open, it keeps a small hidden file named
// ~$report.docx beside it with the name of the person editing it, and other copies of Word say
// "locked for editing by …". Featherweight reads it, and writes its own while it has unsaved
// edits, so Word users see the file is in use too (plan §6.3 "Working alongside Word").

// The owner file's path for a document. Word's rule: a name (without its extension) of up to 6
// characters gets "~$" in front, a 7-character name loses its first character to it, and a
// longer one its first two: report.docx -> ~$report.docx, Document1.docx -> ~$cument1.docx.
std::wstring OwnerFilePath(const std::wstring& docPath);

// The owner file's content for `user`, as Word writes it: 162 bytes, the name in the ANSI code
// page (its length in the first byte) and again in UTF-16 from offset 54.
std::vector<uint8_t> OwnerFileBytes(const std::wstring& user);
// The user an owner file names, or empty when it isn't one.
std::wstring ParseOwnerFile(const std::vector<uint8_t>& bytes);

// The user named in the owner file beside `docPath`, or empty when there is none.
std::wstring ReadOwner(const std::wstring& docPath);
// Writes our owner file beside `docPath`, hidden. Not when another user's is there already.
bool WriteOwner(const std::wstring& docPath, const std::wstring& user);
// Removes the owner file beside `docPath` if it names `user`: never another person's.
void RemoveOwner(const std::wstring& docPath, const std::wstring& user);

// True when another program has the file open for writing, as Word does while it's open.
bool IsLockedByAnother(const std::wstring& path);

// Who is editing a file elsewhere, for the "in use" banner: `locked` when another program holds
// it, and the name from its owner file (empty when there is none, or it names `self`, which is a
// leftover of ours).
struct InUse {
  bool locked = false;
  std::wstring user;
};
InUse CheckInUse(const std::wstring& docPath, const std::wstring& self);

}  // namespace fw::pkg
