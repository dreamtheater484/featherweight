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
  int Size() const { return size_; }  // the trailer's /Size: the next free object number

  // Object `num` as the newest revision has it, with its generation. False if it's free or unreadable.
  bool Get(int num, Obj& out, int* gen = nullptr);
  // `o` itself, or the object it refers to.
  bool Resolve(const Obj& o, Obj& out);
  // The first page's object number (through the page tree).
  int FirstPage();

 private:
  struct Entry {
    uint8_t type = 0;  // 1: in the file at `a` with generation `b`; 2: in object stream `a`, index `b`
    uint64_t a = 0;
    uint32_t b = 0;
  };
  bool ReadSection(size_t offset, int depth);
  bool ReadTable(size_t& at, Obj& trailer);
  bool ReadXrefStream(const Obj& stream, std::string_view data);
  bool StreamData(const Obj& stream, std::string_view source, std::string& out);
  bool ObjectAt(size_t offset, int num, Obj& out, int* gen);
  bool FromObjectStream(int stream, uint32_t index, int num, Obj& out);

  std::string_view data_;
  std::map<int, Entry> xref_;
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

// Inflates zlib data (FlateDecode), at most `limit` bytes out.
bool Inflate(std::string_view in, std::string& out, size_t limit);

}  // namespace pdfobj
