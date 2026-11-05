#pragma once

#include <cstddef>

// The hyphenation patterns built into the program: one raw-deflate stream per pattern set, made
// from the pinned downloads by PackPatterns.cmake (HyphenationData.cpp is generated).
namespace fw::text::detail {

struct PatternBlob {
  const char* id;  // the hyph-utf8 name ("en-us", "de-1996") or "nl-opentaal"
  const unsigned char* data;
  size_t size;
  size_t rawSize;  // after inflating
};

extern const PatternBlob kPatternBlobs[];
extern const size_t kPatternBlobCount;

}  // namespace fw::text::detail
