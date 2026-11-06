#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fw::pkg {

class CompoundFile;

// [MS-OFFCRYPTO] password-protected Office Open XML files (milestone D5): a compound file whose
// EncryptionInfo stream says how the package was encrypted and whose EncryptedPackage stream holds
// it. Agile encryption (Office 2010 and later) and standard encryption (Office 2007) are read;
// saving writes agile encryption, as Word does. The cryptography is Windows' own (CNG).
namespace crypto {

enum class Result { Ok, WrongPassword, Unsupported, Damaged };

// What saving needs to encrypt the package again with the same password, without keeping the
// password: the package key, and the password's encrypted copy of it. Wiped when it goes away.
struct Key {
  std::vector<uint8_t> secret;  // encrypts the package (AES)
  std::string dataHash;         // for the package's IVs and its HMAC: "SHA512"
  // The password key encryptor, as EncryptionInfo stores it.
  std::string passwordHash;
  uint32_t spinCount = 0;
  std::vector<uint8_t> salt, verifierInput, verifierHash, encryptedSecret;

  Key() = default;
  Key(const Key&) = default;
  Key& operator=(const Key&) = default;
  ~Key();
  bool Empty() const { return secret.empty(); }
};

// True when the compound file is a password-protected Office Open XML file.
bool IsEncryptedPackage(const CompoundFile& file);
// Decrypts the package. On Ok, `package` is the zip, and `key` saves it again with the same
// password (a file with standard encryption gets a new agile key).
Result Decrypt(const CompoundFile& file, std::wstring_view password, std::vector<uint8_t>& package, Key& key);
// A new key for `password`: AES-256 and SHA-512 with 100,000 spins, as Word makes them.
Key NewKey(std::wstring_view password);
// The package encrypted with `key`: the compound file Word writes. Empty when Windows'
// cryptography fails.
std::vector<uint8_t> Encrypt(std::span<const uint8_t> package, const Key& key);

// Password-protected .doc files: RC4 (Word 97) or RC4 CryptoAPI (Word 2002 and later), in
// 512-byte blocks. Decrypted, the streams are what the .doc reader reads; the FIB keeps its
// fEncrypted flag and lKey, and the table stream keeps the encryption header at its start, which
// nothing in the FIB points into.
struct DocStreams {
  std::string table;  // "0Table" or "1Table"
  std::vector<uint8_t> wordDocument, tableStream, data;  // data: empty without a Data stream
};
// True when the compound file is a .doc whose FIB says it is encrypted.
bool IsEncryptedDoc(const CompoundFile& file);
Result DecryptDoc(const CompoundFile& file, std::wstring_view password, DocStreams& out);

}  // namespace crypto
}  // namespace fw::pkg
