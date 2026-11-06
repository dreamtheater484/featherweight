#pragma once

#include <string>
#include <utility>

namespace fw::pkg {

// The outcome of reading or writing untrusted input. The message is plain language (UTF-8),
// ready for a banner (principle 6): it says what is wrong, never an error code.
struct Status {
  enum class Code {
    Ok,
    IoError,      // the file couldn't be read or written
    Changed,      // the file changed on disk
    TooLarge,     // over one of the Limits: a zip bomb, a nesting bomb, ...
    Damaged,      // not a valid zip or XML file, or a checksum doesn't match
    Unsupported,  // valid, but uses something Office files don't (zip encryption, other encodings)
    Rejected,     // refused on purpose: a DTD, overlapping zip entries
    NotFound,     // a part that should exist doesn't
  };

  Code code = Code::Ok;
  std::string message;

  bool Ok() const { return code == Code::Ok; }

  static Status Fail(Code c, std::string msg) { return Status{c, std::move(msg)}; }
};

}  // namespace fw::pkg
