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

// The password hash Excel keeps for a protected sheet or workbook (sheetProtection,
// workbookProtection: ECMA-376 Part 1 §18.3.1.85): H(salt + password in UTF-16LE), then `spins`
// times H(previous + the iteration as 4 bytes little-endian), in base64. `algorithm` as the file
// names it ("SHA-512"). Empty when Windows doesn't have that hash or the salt isn't base64.
std::string ProtectionHash(std::string_view algorithm, std::string_view saltBase64, std::wstring_view password,
                           uint32_t spins);
// A new salt for it: 16 random bytes in base64, as Excel makes them.
std::string NewProtectionSalt();
// A new random id as Office writes them: "{8-4-4-4-12}" in upper-case hex, version 4.
std::string NewGuid();

// Password-protected .doc files: RC4 (Word 97) or RC4 CryptoAPI (Word 2002 and later), in
// 512-byte blocks. Decrypted, the streams are a .doc without a password, as the .doc reader
// (msdoc::ToDocx) takes them: the FIB's fEncrypted flag and lKey are cleared, and the table stream
// keeps the encryption header at its start, which nothing in the FIB points into.
struct DocStreams {
  std::string table;  // "0Table" or "1Table"
  std::vector<uint8_t> wordDocument, tableStream, data;  // data: empty without a Data stream
};
// True when the compound file is a .doc whose FIB says it is encrypted.
bool IsEncryptedDoc(const CompoundFile& file);
Result DecryptDoc(const CompoundFile& file, std::wstring_view password, DocStreams& out);

}  // namespace crypto
}  // namespace fw::pkg
