#pragma once

// A small reader of a PDF file's objects (F1, f1-pdf), for what PDFium has no function for:
// writing a signature as an incremental update needs the object numbers of the catalog, the form
// and the first page, and their dictionaries to write them again with the signature field added.
// It reads cross-reference tables and streams (with /Prev and object streams) and keeps every
// token as written, so what it writes back is what was there. Untrusted input: every read is
// bounded, and anything unexpected is a failure, never a guess.

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pdfobj {

struct Obj {
  enum Type : uint8_t { Null, Bool, Number, String, HexString, Name, Array, Dict, Ref, Stream };
  Type type = Null;
  std::string text;  // Bool/Number: the token; String/HexString: the bytes between the delimiters as
                     // written; Name: without the slash, #xx as written
  int num = 0, gen = 0;  // Ref
  std::vector<Obj> items;  // Array
  std::vector<std::pair<std::string, Obj>> dict;  // Dict and Stream (the stream's dictionary)
  size_t streamAt = 0, streamLen = 0;  // Stream: where its data is in the file (or object stream)

  const Obj* Get(std::string_view key) const;
  void Set(const std::string& key, Obj value);
  bool IsName(std::string_view n) const { return type == Name && text == n; }
  long long Int(long long fallback = 0) const;
};

Obj MakeName(std::string name);
Obj MakeInt(long long v);
Obj MakeRef(int num, int gen = 0);
// A literal string; the bytes are escaped as needed.
Obj MakeString(std::string_view bytes);
// PDF text: UTF-16BE with its byte order mark, or plain when it is ASCII.
Obj MakeText(std::wstring_view text);

// The object as PDF syntax (a stream's dictionary only).
std::string Write(const Obj& o);

// Reads one object from `s` at `at` (moved past it). Exposed for the fuzzer.
bool ParseObject(std::string_view s, size_t& at, Obj& out, int depth = 0);

class File {
 public:
  // Reads the cross-reference chain. False when the file's structure can't be read strictly.
  bool Open(std::string_view bytes);

  const Obj& Trailer() const { return trailer_; }  // the newest trailer (or xref stream) dictionary
  size_t LastXref() const { return lastXref_; }
  bool LastXrefIsStream() const { return lastIsStream_; }
  // The next free object number: the trailer's /Size, or past the highest number any section lists
  // when /Size is less.
  int Size() const { return size_; }

  // Object `num` as the newest revision has it, with its generation. False if it's free or unreadable.
  bool Get(int num, Obj& out, int* gen = nullptr);
  // `o` itself, or the object it refers to.
  bool Resolve(const Obj& o, Obj& out);
  // Object `num` as written in the newest revision, between "num gen obj" and "endobj" (a stream's
  // data included). False for one kept in an object stream, or one that can't be read.
  bool ObjectText(int num, std::string_view& out);
  // A stream's data as stored in the file, and decoded (no filter or FlateDecode only).
  bool RawStream(const Obj& stream, std::string_view& out);
  bool DecodedStream(const Obj& stream, std::string& out) { return StreamData(stream, data_, out); }
  // The object numbers in use in the newest revision.
  std::vector<int> InUse() const;
  // The first page's object number (through the page tree, in order, past branches left empty
  // when their pages were deleted). -1 when there's none or the tree can't be read.
  int FirstPage();

 private:
  struct Entry {
    uint64_t a = 0;
    uint32_t b = 0;
    uint8_t type = 0;      // 0: not listed yet; 1: in the file at `a` with generation `b`; 2: in object stream
                           // `a`, index `b`; 3: free
    uint16_t section = 0;  // the cross-reference section that listed it (newest first)
  };
  bool ReadSection(size_t offset, int depth);
  bool ReadTable(size_t& at, Obj& trailer);
  // `hybrid`: the stream of a table section's /XRefStm, which fills in what the table lists as free.
  bool ReadXrefStream(const Obj& stream, std::string_view data, bool hybrid = false);
  // Entry `num`, made room for: null past what the file's size allows (one object per 8 bytes, at
  // least 2 million). An entry listed by a newer section stays as it is.
  Entry* Slot(long long num);
  const Entry* Find(int num) const;  // null when no section lists it
  bool StreamData(const Obj& stream, std::string_view source, std::string& out);
  bool ObjectAt(size_t offset, int num, Obj& out, int* gen);
  bool FromObjectStream(int stream, uint32_t index, int num, Obj& out);
  // FirstPage's walk: a leaf's number, -1 for a branch without pages, -2 for anything unexpected.
  int FirstLeaf(int num, int depth, std::set<int>& seen);

  std::string_view data_;
  std::vector<Entry> xref_;  // by object number: 16 bytes each
  struct ObjStream {
    std::string data;
    long long n = 0, first = 0;
  };
  std::map<int, std::shared_ptr<ObjStream>> objStreams_;
  Obj trailer_;
  size_t lastXref_ = 0;
  bool lastIsStream_ = false;
  int size_ = 0;
  int sections_ = 0;
  size_t inflated_ = 0;
};

// Whether `a` (read from `fa`) and `b` (from `fb`) are the same object however each is written:
// numbers by value, strings and names by their bytes, dictionaries in any key order, streams by
// their dictionary (less how the data is stored) and their data.
bool SameObject(File& fa, const Obj& a, File& fb, const Obj& b);

// Inflates zlib data (FlateDecode), at most `limit` bytes out.
bool Inflate(std::string_view in, std::string& out, size_t limit);

// One object of an incremental update: its number, generation and the object as PDF syntax.
struct Update {
  int num = 0, gen = 0;
  std::string text;
};

// `file` (opened as `f`) with an incremental update after it: the objects, then a cross-reference
// section of the kind the file's last one is (a table, or an uncompressed stream with /Type /XRef)
// with /Size `size` (the next free number, before a stream's own), /Prev, and the trailer's /Root,
// /Info and /ID. `at` gets the offset in `out` of each object's "num gen obj". Not for encrypted
// files (their strings would have to be encrypted). False when an offset doesn't fit.
bool AppendUpdate(std::string_view file, const File& f, const std::vector<Update>& objects, int size, std::string& out,
                  std::vector<size_t>* at = nullptr);

}  // namespace pdfobj
