// fwtool encrypt / decrypt: password-protected Office files (D5, src/pkg/OfficeCrypto.h) outside
// the app. encrypt writes each package as Featherweight saves a protected document, for Word to
// open (scripts\word-open-check.ps1 -Password); decrypt opens protected files as the app does and
// checks that encrypting the package again with the key from opening gives a file the same
// password opens to the same package.
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "Common.h"
#include "core/Strings.h"
#include "pkg/CompoundFile.h"
#include "pkg/OfficeCrypto.h"

namespace fwtool {
namespace {

bool ReadAll(const std::filesystem::path& file, std::vector<uint8_t>& out) {
  std::ifstream in(file, std::ios::binary);
  if (!in) return false;
  out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return true;
}

bool WriteAll(const std::filesystem::path& file, const std::vector<uint8_t>& bytes) {
  std::ofstream out(file, std::ios::binary);
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  return out.good();
}

const char* Name(fw::pkg::crypto::Result r) {
  switch (r) {
    case fw::pkg::crypto::Result::Ok: return "ok";
    case fw::pkg::crypto::Result::WrongPassword: return "wrong password";
    case fw::pkg::crypto::Result::Unsupported: return "unsupported";
    case fw::pkg::crypto::Result::Damaged: return "damaged";
  }
  return "?";
}

}  // namespace

int RunCrypt(const Options& options, bool encrypt) {
  namespace crypto = fw::pkg::crypto;
  if (options.password.empty() || options.outDir.empty()) {
    std::fprintf(stderr, "Give the password with --password and the folder to write to with --out.\n");
    return 2;
  }
  std::error_code ec;
  std::filesystem::create_directories(options.outDir, ec);
  const crypto::Key key = encrypt ? crypto::NewKey(options.password) : crypto::Key();
  int failed = 0, done = 0;
  for (const auto& file : CollectFiles(options.inputs)) {
    const std::string name = Display(file);
    std::vector<uint8_t> bytes;
    if (!ReadAll(file, bytes)) {
      std::printf("FAIL %s: can't be read\n", name.c_str());
      ++failed;
      continue;
    }
    std::vector<uint8_t> out;
    if (encrypt) {
      out = crypto::Encrypt(bytes, key);
    } else {
      auto cf = fw::pkg::CompoundFile::Open(bytes);
      if (!cf || !crypto::IsEncryptedPackage(*cf)) {
        std::printf("FAIL %s: not a password-protected Office file\n", name.c_str());
        ++failed;
        continue;
      }
      crypto::Key opened;
      const crypto::Result r = crypto::Decrypt(*cf, options.password, out, opened);
      if (r != crypto::Result::Ok) {
        std::printf("FAIL %s: %s\n", name.c_str(), Name(r));
        ++failed;
        continue;
      }
      // Saved again with the key from opening, it opens to the same package.
      const auto again = crypto::Encrypt(out, opened);
      auto cf2 = fw::pkg::CompoundFile::Open(again);
      std::vector<uint8_t> back;
      crypto::Key key2;
      if (!cf2 || crypto::Decrypt(*cf2, options.password, back, key2) != crypto::Result::Ok || back != out) {
        std::printf("FAIL %s: encrypting it again doesn't give the same package back\n", name.c_str());
        ++failed;
        continue;
      }
    }
    if (out.empty() || !WriteAll(options.outDir / file.filename(), out)) {
      std::printf("FAIL %s: couldn't be written\n", name.c_str());
      ++failed;
      continue;
    }
    std::printf("ok   %s (%zu bytes)\n", name.c_str(), out.size());
    ++done;
  }
  std::printf("%d %s, %d failed\n", done, encrypt ? "encrypted" : "decrypted", failed);
  return failed || !done ? 1 : 0;
}

}  // namespace fwtool
