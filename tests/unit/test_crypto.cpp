#include <string>

#include "TestHarness.h"

#include "PkgTestUtil.h"
#include "pkg/CompoundFile.h"
#include "pkg/OfficeCrypto.h"

using namespace fw::pkg;
using test::MakeZip;

namespace {

const std::wstring kPassword = L"Fw-pässword";

}  // namespace

TEST(Crypto_EncryptedPackageOpensWithItsPasswordOnly) {
  const auto zip = MakeZip({{"[Content_Types].xml", "<Types/>"}, {"word/document.xml", std::string(10000, 'x')}},
                           ZipWriter::Compression::Store);
  const crypto::Key key = crypto::NewKey(kPassword);
  CHECK(!key.Empty());
  const auto file = crypto::Encrypt(zip, key);
  auto cf = CompoundFile::Open(file);
  CHECK(cf && crypto::IsEncryptedPackage(*cf));
  if (!cf) return;
  std::vector<uint8_t> out;
  crypto::Key opened;
  CHECK(crypto::Decrypt(*cf, kPassword, out, opened) == crypto::Result::Ok);
  CHECK(out == zip);
  CHECK(opened.secret == key.secret);
  CHECK(crypto::Decrypt(*cf, L"Fw-passwort", out, opened) == crypto::Result::WrongPassword);
  CHECK(crypto::Decrypt(*cf, L"", out, opened) == crypto::Result::WrongPassword);
  CHECK(out.empty());

  // A plain compound file isn't an encrypted package.
  const std::vector<uint8_t> word(100, 1);
  auto doc = CompoundFile::Open(WriteCompoundFile({{"WordDocument", word}}));
  CHECK(doc && !crypto::IsEncryptedPackage(*doc));
}

TEST(Crypto_SavingAgainKeepsThePassword) {
  // Each size around the segment (4096) and block (16) boundaries.
  const crypto::Key key = crypto::NewKey(kPassword);
  for (size_t size : {size_t{0}, size_t{1}, size_t{16}, size_t{4095}, size_t{4096}, size_t{4097}, size_t{100000}}) {
    std::vector<uint8_t> package(size);
    for (size_t i = 0; i < size; ++i) package[i] = uint8_t(i * 31 + 7);
    const auto first = crypto::Encrypt(package, key);
    const auto second = crypto::Encrypt(package, key);
    CHECK(!first.empty() && first != second);  // new salts each time
    for (const auto* file : {&first, &second}) {
      auto cf = CompoundFile::Open(*file);
      std::vector<uint8_t> out;
      crypto::Key opened;
      CHECK(cf && crypto::Decrypt(*cf, kPassword, out, opened) == crypto::Result::Ok && out == package);
    }
  }
}

TEST(Crypto_DamagedEncryptionInfoIsRefused) {
  const auto zip = MakeZip({{"a", "b"}});
  const auto file = crypto::Encrypt(zip, crypto::NewKey(kPassword));
  auto cf = CompoundFile::Open(file);
  CHECK(cf != nullptr);
  if (!cf) return;
  auto info = *cf->Stream("EncryptionInfo");
  const auto encrypted = *cf->Stream("EncryptedPackage");
  auto with = [&](const std::vector<uint8_t>& i, const std::vector<uint8_t>& e) {
    const auto f = WriteCompoundFile({{"EncryptionInfo", i}, {"EncryptedPackage", e}});
    auto c = CompoundFile::Open(f);
    std::vector<uint8_t> out;
    crypto::Key key;
    return c ? crypto::Decrypt(*c, kPassword, out, key) : crypto::Result::Damaged;
  };
  CHECK(with(info, encrypted) == crypto::Result::Ok);
  std::string xml(info.begin() + 8, info.end());
  auto replace = [&](const std::string& from, const std::string& to) {
    std::string x = xml;
    x.replace(x.find(from), from.size(), to);
    std::vector<uint8_t> i(info.begin(), info.begin() + 8);
    i.insert(i.end(), x.begin(), x.end());
    return i;
  };
  CHECK(with(replace("spinCount=\"100000\"", "spinCount=\"4000000000\""), encrypted) == crypto::Result::Unsupported);
  CHECK(with(replace("cipherAlgorithm=\"AES\"", "cipherAlgorithm=\"RC2\""), encrypted) == crypto::Result::Unsupported);
  CHECK(with(replace("<encryption", "<broken"), encrypted) == crypto::Result::Damaged);
  CHECK(with(replace("keyEncryptor/password\"><", "keyEncryptor/certificate\"><"), encrypted) == crypto::Result::Unsupported);
  auto version = info;
  version[2] = 3;  // extensible encryption
  CHECK(with(version, encrypted) == crypto::Result::Unsupported);
  auto shortPackage = encrypted;
  shortPackage.resize(20);
  CHECK(with(info, shortPackage) == crypto::Result::Damaged);
}
