// fwtool encrypt / decrypt: password-protected Office files (D5, src/pkg/OfficeCrypto.h) outside
// the app. encrypt writes each package as Featherweight saves a protected document, for Word to
// open (scripts\word-open-check.ps1 -Password); decrypt opens protected files as the app does and
// checks that encrypting the package again with the key from opening gives a file the same
// password opens to the same package. A protected .doc is decrypted into a .doc without a password
// (its FIB's fEncrypted flag cleared), for Word to open: the check that the streams are right.
#include <algorithm>
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
  std::vector<std::filesystem::path> files = CollectFiles(options.inputs);
  if (!encrypt)  // .doc files too
    for (const auto& in : options.inputs) {
      std::error_code ec2;
      if (std::filesystem::is_directory(in)) {
        for (auto it = std::filesystem::recursive_directory_iterator(in, ec2); !ec2 && it != std::filesystem::end(it); it.increment(ec2))
          if (it->is_regular_file(ec2) && fw::ToLower(it->path().extension().wstring()) == L".doc") files.push_back(it->path());
      } else if (fw::ToLower(in.extension().wstring()) == L".doc" && std::find(files.begin(), files.end(), in) == files.end()) {
        files.push_back(in);
      }
    }
  for (const auto& file : files) {
    const std::string name = Display(file);
    std::vector<uint8_t> bytes;
    if (!ReadAll(file, bytes)) {
      std::printf("FAIL %s: can't be read\n", name.c_str());
      ++failed;
      continue;
    }
    std::vector<uint8_t> out;
    auto doc = encrypt ? nullptr : fw::pkg::CompoundFile::Open(bytes);
    if (doc && crypto::IsEncryptedDoc(*doc)) {
      crypto::DocStreams streams;
      const crypto::Result r = crypto::DecryptDoc(*doc, options.password, streams);
      if (r != crypto::Result::Ok) {
        std::printf("FAIL %s: %s\n", name.c_str(), Name(r));
        ++failed;
        continue;
      }
      // fEncrypted off and lKey 0: a .doc without a password.
      streams.wordDocument[0x0B] &= ~0x01;
      for (int i = 0; i < 4; ++i) streams.wordDocument[0x0E + i] = 0;
      std::vector<std::vector<uint8_t>> kept;
      std::vector<fw::pkg::CompoundStream> list;
      for (const auto& path : doc->Streams()) {
        if (path == "WordDocument" || path == streams.table || path == "Data") continue;
        if (auto s = doc->Stream(path)) {
          kept.push_back(std::move(*s));
          list.push_back({path, {}});
        }
      }
      for (size_t i = 0; i < kept.size(); ++i) list[i].data = kept[i];
      list.push_back({"WordDocument", streams.wordDocument});
      list.push_back({streams.table, streams.tableStream});
      if (!streams.data.empty()) list.push_back({"Data", streams.data});
      out = fw::pkg::WriteCompoundFile(list);
    } else if (encrypt) {
      out = crypto::Encrypt(bytes, key);
    } else {
      auto cf = std::move(doc);
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

int RunCfb(const Options& options) {
  int failed = 0;
  for (const auto& in : options.inputs) {
    std::vector<std::filesystem::path> files;
    std::error_code ec;
    if (std::filesystem::is_directory(in)) {
      for (auto it = std::filesystem::recursive_directory_iterator(in, ec); !ec && it != std::filesystem::end(it); it.increment(ec))
        if (it->is_regular_file(ec)) files.push_back(it->path());
    } else {
      files.push_back(in);
    }
    std::sort(files.begin(), files.end());
    for (const auto& file : files) {
      const std::string name = Display(file);
      std::vector<uint8_t> bytes;
      if (!ReadAll(file, bytes) || !fw::pkg::CompoundFile::HasSignature(bytes)) continue;
      auto cf = fw::pkg::CompoundFile::Open(bytes);
      if (!cf) {
        std::printf("FAIL %s: not read\n", name.c_str());
        ++failed;
        continue;
      }
      std::vector<std::vector<uint8_t>> data;
      std::vector<fw::pkg::CompoundStream> streams;
      bool ok = true;
      for (const auto& path : cf->Streams()) {
        auto s = cf->Stream(path);
        if (!s) {
          std::printf("FAIL %s: stream %s not read\n", name.c_str(), path.c_str());
          ok = false;
          continue;
        }
        data.push_back(std::move(*s));
        streams.push_back({path, {}});
      }
      for (size_t i = 0; i < data.size(); ++i) streams[i].data = data[i];
      const auto written = fw::pkg::WriteCompoundFile(streams);
      auto again = fw::pkg::CompoundFile::Open(written);
      for (size_t i = 0; ok && i < streams.size(); ++i)
        if (!again || again->Stream(streams[i].path) != data[i]) {
          std::printf("FAIL %s: %s differs once written\n", name.c_str(), streams[i].path.c_str());
          ok = false;
        }
      if (!ok) {
        ++failed;
        continue;
      }
      std::printf("ok   %s: %zu streams\n", name.c_str(), streams.size());
      if (!options.all) continue;
      for (size_t i = 0; i < streams.size(); ++i) {
        std::string shown;
        for (char c : streams[i].path) shown += uint8_t(c) >= 0x20 ? std::string(1, c) : "\\x0" + std::to_string(int(c));
        std::printf("       %-60s %zu", shown.c_str(), data[i].size());
        if (data[i].size() <= 256) {
          std::printf("\n        ");
          for (uint8_t b : data[i]) std::printf("%02x", b);
        }
        std::printf("\n");
      }
    }
  }
  return failed ? 1 : 0;
}

}  // namespace fwtool
