#pragma once

#include <cstdint>
#include <string>

namespace fw::pkg {

// Caps for untrusted packages (plan §4.7), so a malicious file fails with a plain message instead
// of hanging or using up memory. The defaults are far above what real Office files need.
struct Limits {
  // Zip
  uint32_t maxEntries = 50000;
  uint64_t maxEntrySize = 1ull << 30;   // uncompressed bytes in one entry
  uint64_t maxTotalSize = 4ull << 30;   // uncompressed bytes in all entries together
  // Uncompressed / compressed. Deflate tops out near 1032:1; real XML rarely passes 100:1.
  // Entries smaller than ratioGraceSize are exempt, since tiny parts can compress oddly well.
  uint32_t maxCompressionRatio = 200;
  uint64_t ratioGraceSize = 16ull << 20;

  // XML. DTDs are always rejected: OOXML forbids them, and that also blocks entity expansion.
  uint32_t maxDepth = 512;                 // nested elements
  uint64_t maxStringLength = 64ull << 20;  // one name, attribute value, text run or comment
  uint32_t maxAttributes = 10000;          // on one element

  static const Limits& Default();
};

// "1 GB", "16 MB", "512 bytes": for messages.
std::string FormatBytes(uint64_t bytes);

}  // namespace fw::pkg
