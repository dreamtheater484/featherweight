#include <algorithm>
#include <string>

#include "TestHarness.h"

#include "PkgTestUtil.h"
#include "pkg/CompoundFile.h"

using namespace fw::pkg;
using test::Bytes;
using test::Text;

namespace {

std::vector<uint8_t> Pattern(size_t n, uint8_t seed) {
  std::vector<uint8_t> v(n);
  for (size_t i = 0; i < n; ++i) v[i] = uint8_t(seed + i * 7 + (i >> 9));
  return v;
}

uint32_t Get32(const std::vector<uint8_t>& v, size_t at) {
  return uint32_t(v[at]) | uint32_t(v[at + 1]) << 8 | uint32_t(v[at + 2]) << 16 | uint32_t(v[at + 3]) << 24;
}
void Put32(std::vector<uint8_t>& v, size_t at, uint32_t x) {
  for (int i = 0; i < 4; ++i) v[at + i] = uint8_t(x >> (8 * i));
}

}  // namespace

TEST(Cfb_WriteAndRead) {
  const auto info = Bytes("small stream, in the mini stream");
  const auto big = Pattern(10000, 3);
  const auto exact = Pattern(4096, 9);  // the cutoff: a regular stream
  const auto version = Bytes("version");
  const std::vector<uint8_t> empty;
  const auto file = WriteCompoundFile({{"EncryptionInfo", info},
                                       {"EncryptedPackage", big},
                                       {"\x06" "DataSpaces/Version", version},
                                       {"\x06" "DataSpaces/TransformInfo/StrongEncryptionDataSpace/\x06Primary", exact},
                                       {"Empty", empty}});
  CHECK(CompoundFile::HasSignature(file));
  auto cf = CompoundFile::Open(file);
  CHECK(cf != nullptr);
  if (!cf) return;
  CHECK_EQ(cf->Streams().size(), size_t{5});
  CHECK(cf->Stream("EncryptionInfo") == info);
  CHECK(cf->Stream("encryptedpackage") == big);  // names ignore case
  CHECK(cf->Stream("\x06" "DataSpaces/Version") == version);
  CHECK(cf->Stream("\x06" "DataSpaces/TransformInfo/StrongEncryptionDataSpace/\x06Primary") == exact);
  CHECK(cf->Stream("Empty") == empty);
  CHECK(!cf->Stream("Missing"));
  CHECK(!cf->Stream("\x06" "DataSpaces"));  // a storage, not a stream
}

TEST(Cfb_ManyStreamsMakeBalancedRedBlackTrees) {
  std::vector<std::vector<uint8_t>> data;
  std::vector<CompoundStream> streams;
  for (int i = 0; i < 40; ++i) data.push_back(Pattern(size_t(i) * 300 + 1, uint8_t(i)));
  for (int i = 0; i < 40; ++i) streams.push_back({"S" + std::to_string(i * 37 % 41), data[i]});
  const auto file = WriteCompoundFile(streams);
  auto cf = CompoundFile::Open(file);
  CHECK(cf != nullptr);
  if (!cf) return;
  CHECK_EQ(cf->Streams().size(), size_t{40});
  for (int i = 0; i < 40; ++i) CHECK(cf->Stream(streams[i].path) == data[i]);
  // Every path from the root's child down has the same number of black entries, and no red
  // entry has a red child.
  const uint32_t dir = (Get32(file, 0x30) + 1) * 512;
  std::vector<int> blacks;
  struct At { uint32_t entry; int black; bool red; };
  std::vector<At> todo{{Get32(file, dir + 0x4C), 0, false}};
  while (!todo.empty()) {
    At a = todo.back();
    todo.pop_back();
    if (a.entry == 0xFFFFFFFF) {
      blacks.push_back(a.black);
      continue;
    }
    const size_t e = dir + 128 * size_t(a.entry);
    const bool red = file[e + 0x43] == 0;
    CHECK(!(red && a.red));
    todo.push_back({Get32(file, e + 0x44), a.black + (red ? 0 : 1), red});
    todo.push_back({Get32(file, e + 0x48), a.black + (red ? 0 : 1), red});
  }
  CHECK(std::all_of(blacks.begin(), blacks.end(), [&](int b) { return b == blacks[0]; }));
}

TEST(Cfb_LargeFileUsesTheDifat) {
  // Over 109 FAT sectors (about 7 MB) the FAT's sectors continue in the DIFAT.
  const auto big = Pattern(8u << 20, 1);
  const auto file = WriteCompoundFile({{"EncryptedPackage", big}, {"EncryptionInfo", Bytes("x")}});
  CHECK(Get32(file, 0x48) >= 1);  // DIFAT sectors
  auto cf = CompoundFile::Open(file);
  CHECK(cf != nullptr && cf->Stream("EncryptedPackage") == big && Text(*cf->Stream("EncryptionInfo")) == "x");
}

TEST(Cfb_DamageIsRefusedNotFollowed) {
  const auto big = Pattern(5000, 2);
  const auto file = WriteCompoundFile({{"Big", big}, {"Small", Bytes("tiny")}});
  CHECK(!CompoundFile::Open(std::span(file).first(511)));
  CHECK(!CompoundFile::Open(Bytes("not a compound file at all")));
  auto bad = file;
  bad[0x1E] = 10;  // a sector size CFB doesn't have
  CHECK(!CompoundFile::Open(bad));

  // A chain that loops: Big's first sector points to itself.
  auto cf = CompoundFile::Open(file);
  CHECK(cf != nullptr);
  bad = file;
  const uint32_t fat = (Get32(file, 0x4C) + 1) * 512;
  uint32_t start = 0;
  for (uint32_t s = 0; s < 128; ++s)
    if (Get32(file, fat + 4 * s) == s + 1 && Get32(file, fat + 4 * (s + 9)) == 0xFFFFFFFE) start = s;  // Big: 10 sectors
  Put32(bad, fat + 4 * start, start);
  cf = CompoundFile::Open(bad);
  CHECK(cf != nullptr && !cf->Stream("Big") && cf->Stream("Small") == Bytes("tiny"));

  // A chain that runs off the end of the file.
  bad = file;
  Put32(bad, fat + 4 * start, 100000);
  cf = CompoundFile::Open(bad);
  CHECK(cf != nullptr && !cf->Stream("Big"));

  // A stream size far beyond the file.
  bad = file;
  const uint32_t dir = (Get32(file, 0x30) + 1) * 512;
  for (uint32_t e = 1; e < 4; ++e)
    if (bad[dir + 128 * e] == 'B') Put32(bad, dir + 128 * e + 0x78, 0x7FFFFFFF);
  cf = CompoundFile::Open(bad);
  CHECK(cf != nullptr && !cf->Stream("Big"));

  // A directory whose entries link back to each other still lists each stream once.
  bad = file;
  for (uint32_t e = 1; e < 3; ++e) Put32(bad, dir + 128 * e + 0x44, 1), Put32(bad, dir + 128 * e + 0x48, 2);
  cf = CompoundFile::Open(bad);
  CHECK(cf != nullptr && cf->Streams().size() <= 2);
}
