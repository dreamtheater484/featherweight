#include "TestHarness.h"

#include "PkgTestUtil.h"
#include "pkg/Snapshot.h"
#include "pkg/Zip.h"

using namespace fw::pkg;
using test::Bytes;
using test::MakeZip;
using test::Text;

// ---------- Zip ----------

TEST(Zip_WriteAndRead) {
  auto zip = MakeZip({{"a.xml", "<a/>"}, {"dir/", ""}, {"dir/b.txt", std::string(5000, 'b')}, {"empty", ""}});
  auto snap = Snapshot::FromBytes(zip);
  ZipReader reader;
  CHECK(reader.Open(*snap).Ok());
  CHECK_EQ(reader.Entries().size(), size_t{4});
  CHECK(reader.Entries()[1].IsDirectory());
  std::vector<uint8_t> out;
  CHECK(reader.Read(*reader.Find("dir/b.txt"), out).Ok());
  CHECK(Text(out) == std::string(5000, 'b'));
  CHECK(reader.Find("dir/b.txt")->compressedSize < 100);  // deflated
  CHECK(reader.Read(*reader.Find("empty"), out).Ok() && out.empty());
  CHECK(reader.Find("missing") == nullptr);
}

TEST(Zip_StoredEntriesAndComment) {
  auto zip = MakeZip({{"x", "hello"}}, ZipWriter::Compression::Store, "a comment");
  auto snap = Snapshot::FromBytes(zip);
  ZipReader reader;
  CHECK(reader.Open(*snap).Ok());
  CHECK(reader.Comment() == "a comment");
  CHECK_EQ(reader.Entries()[0].method, uint16_t{0});
  std::vector<uint8_t> out;
  CHECK(reader.Read(reader.Entries()[0], out).Ok() && Text(out) == "hello");
}

TEST(Zip_CopyRawIsByteIdentical) {
  auto zip = MakeZip({{"one.xml", "<one>1</one>"}, {"two.bin", std::string(3000, '\x42')}});
  auto snap = Snapshot::FromBytes(zip);
  ZipReader reader;
  CHECK(reader.Open(*snap).Ok());
  MemorySink sink;
  ZipWriter writer(sink);
  for (const auto& e : reader.Entries()) CHECK(writer.CopyRaw(reader, e).Ok());
  CHECK(writer.Finish(reader.Comment()).Ok());
  CHECK(sink.bytes == zip);  // the whole archive, since ZipWriter wrote the original
}

TEST(Zip_AddKeepsMetadataOfReplacedEntry) {
  ZipEntry like;
  like.modTime = 0x6123;
  like.modDate = 0x5A21;
  like.externalAttrs = 0x20;
  like.extra = std::string("\x0a\x00\x04\x00\x01\x02\x03\x04", 8);
  MemorySink sink;
  ZipWriter w(sink);
  CHECK(w.Add("p.xml", Bytes("<p/>"), &like).Ok());
  CHECK(w.Finish().Ok());
  auto snap = Snapshot::FromBytes(sink.bytes);
  ZipReader reader;
  CHECK(reader.Open(*snap).Ok());
  const ZipEntry& e = reader.Entries()[0];
  CHECK_EQ(e.modTime, uint16_t{0x6123});
  CHECK_EQ(e.modDate, uint16_t{0x5A21});
  CHECK_EQ(e.externalAttrs, uint32_t{0x20});
  CHECK(e.extra == like.extra);
}

TEST(Zip_NonAsciiNamesAreUtf8) {
  auto zip = MakeZip({{"word/media/caf\xC3\xA9.png", "x"}});
  auto snap = Snapshot::FromBytes(zip);
  ZipReader reader;
  CHECK(reader.Open(*snap).Ok());
  CHECK(reader.Entries()[0].flags & 0x0800);
}

TEST(Zip_Zip64ForManyEntries) {
  std::vector<std::pair<std::string, std::string>> many;
  for (int i = 0; i < 66000; ++i) many.push_back({"e" + std::to_string(i), ""});
  auto zip = MakeZip(many, ZipWriter::Compression::Store);
  auto snap = Snapshot::FromBytes(zip);
  Limits limits;
  limits.maxEntries = 70000;
  ZipReader reader;
  CHECK(reader.Open(*snap, limits).Ok());
  CHECK_EQ(reader.Entries().size(), size_t{66000});
  CHECK(reader.Entries().back().name == "e65999");
  CHECK_EQ(reader.Open(*snap).code, Status::Code::TooLarge);  // the default cap is lower
}

TEST(Zip_Limits) {
  auto zip = MakeZip({{"a", std::string(1000, 'a')}, {"b", "b"}});
  auto snap = Snapshot::FromBytes(zip);
  ZipReader reader;
  Limits few;
  few.maxEntries = 1;
  CHECK_EQ(reader.Open(*snap, few).code, Status::Code::TooLarge);
  Limits small;
  small.maxEntrySize = 999;
  CHECK_EQ(reader.Open(*snap, small).code, Status::Code::TooLarge);
  Limits total;
  total.maxTotalSize = 1000;
  CHECK_EQ(reader.Open(*snap, total).code, Status::Code::TooLarge);
  Limits ratio;
  ratio.ratioGraceSize = 100;
  ratio.maxCompressionRatio = 10;  // 1000 'a' compress far better than 10:1
  CHECK_EQ(reader.Open(*snap, ratio).code, Status::Code::TooLarge);
  ratio.ratioGraceSize = 1000;  // at or under the grace size, the ratio isn't checked
  CHECK(reader.Open(*snap, ratio).Ok());
}

TEST(Zip_DamageIsDetected) {
  auto zip = MakeZip({{"a.xml", std::string(2000, 'z') + "end"}});
  std::vector<uint8_t> out;
  {
    auto bad = zip;
    auto snap0 = Snapshot::FromBytes(zip);
    ZipReader r0;
    CHECK(r0.Open(*snap0).Ok());
    bad[static_cast<size_t>(r0.Entries()[0].dataOffset) + 2] ^= 0x55;  // inside the compressed data
    auto snap = Snapshot::FromBytes(bad);
    ZipReader reader;
    CHECK(reader.Open(*snap).Ok());
    CHECK_EQ(reader.Read(reader.Entries()[0], out).code, Status::Code::Damaged);
  }
  {
    auto snap = Snapshot::FromBytes(std::vector<uint8_t>(zip.begin(), zip.begin() + zip.size() / 2));
    ZipReader reader;
    CHECK_EQ(reader.Open(*snap).code, Status::Code::Damaged);
  }
  {
    auto snap = Snapshot::FromBytes(Bytes("not a zip at all, just some text that is long enough"));
    ZipReader reader;
    CHECK_EQ(reader.Open(*snap).code, Status::Code::Damaged);
  }
  {
    auto snap = Snapshot::FromBytes({});
    ZipReader reader;
    CHECK_EQ(reader.Open(*snap).code, Status::Code::Damaged);
  }
}

TEST(Zip_DuplicateNamesAreRefused) {
  auto zip = MakeZip({{"a", "1"}, {"a", "2"}});
  auto snap = Snapshot::FromBytes(zip);
  ZipReader reader;
  Status st = reader.Open(*snap);
  CHECK_EQ(st.code, Status::Code::Damaged);
  CHECK(st.message.find("two parts named a") != std::string::npos);
}

TEST(Zip_InflateNeverGrowsPastDeclaredSize) {
  std::vector<uint8_t> data(100000, 0);
  auto packed = Deflate(data);
  std::vector<uint8_t> out;
  CHECK(Inflate(packed, data.size(), out) && out == data);
  CHECK(!Inflate(packed, 1000, out));    // a header claiming less: refused, not grown
  CHECK(!Inflate(packed, 200000, out));  // claiming more: refused too
  CHECK_EQ(Crc32(Bytes("123456789")), uint32_t{0xCBF43926});
}

// ---------- Snapshot ----------

TEST(Snapshot_ChangeDetection) {
  std::wstring path = test::TempPath(L"snap.docx");
  CHECK(test::WriteFileBytes(path, Bytes("first version")));
  Status st;
  auto snap = Snapshot::Open(path, {}, st);
  CHECK(st.Ok() && snap);
  CHECK(snap->InMemory());
  CHECK(Text(std::vector<uint8_t>(snap->Bytes().begin(), snap->Bytes().end())) == "first version");
  CHECK(snap->GetStamp().hash == HashBytes(Bytes("first version")));
  CHECK(snap->CheckDisk() == Snapshot::DiskState::Unchanged);

  // Same size, same time: only the hash can tell.
  FILETIME written{};
  HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  GetFileTime(h, nullptr, nullptr, &written);
  CloseHandle(h);
  CHECK(test::WriteFileBytes(path, Bytes("FIRST VERSION")));
  h = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, 0, nullptr, OPEN_EXISTING, 0, nullptr);
  SetFileTime(h, nullptr, nullptr, &written);
  CloseHandle(h);
  CHECK(snap->CheckDisk() == Snapshot::DiskState::Changed);
  // The snapshot itself never changes.
  CHECK(snap->Bytes()[0] == 'f');

  DeleteFileW(path.c_str());
  CHECK(snap->CheckDisk() == Snapshot::DiskState::Missing);
  CHECK(Snapshot::Open(path, {}, st) == nullptr);
  CHECK_EQ(st.code, Status::Code::NotFound);
}

TEST(Snapshot_LargeFilesAreCopiedToRecovery) {
  std::wstring path = test::TempPath(L"big.docx");
  std::wstring recovery = test::TempPath(L"recovery");
  auto zip = MakeZip({{"a.xml", "<a>large file</a>"}, {"b.bin", std::string(4000, 'q')}});
  CHECK(test::WriteFileBytes(path, zip));
  Snapshot::Options options;
  options.memoryLimit = 16;  // anything larger goes to the recovery folder
  options.recoveryDir = recovery;
  Status st;
  {
    auto snap = Snapshot::Open(path, options, st);
    CHECK(st.Ok() && snap);
    CHECK(!snap->InMemory());
    CHECK(snap->Bytes().empty());
    CHECK_EQ(snap->Size(), zip.size());
    CHECK(snap->GetStamp().hash == HashBytes(zip));
    // The live file can change or go away: the private copy is what gets read.
    DeleteFileW(path.c_str());
    ZipReader reader;
    CHECK(reader.Open(*snap).Ok());
    std::vector<uint8_t> out;
    CHECK(reader.Read(*reader.Find("a.xml"), out).Ok() && Text(out) == "<a>large file</a>");
    uint8_t byte = 0;
    CHECK(!snap->ReadAt(snap->Size(), &byte, 1));
    MemorySink sink;
    ZipWriter w(sink);
    for (const auto& e : reader.Entries()) CHECK(w.CopyRaw(reader, e).Ok());
    CHECK(w.Finish().Ok());
    CHECK(sink.bytes == zip);
  }
  // The copy is deleted with the snapshot.
  CHECK(RemoveDirectoryW(recovery.c_str()));

  options.recoveryDir.clear();
  CHECK(test::WriteFileBytes(path, zip));
  CHECK(Snapshot::Open(path, options, st) == nullptr);
  CHECK_EQ(st.code, Status::Code::IoError);
  DeleteFileW(path.c_str());
}
