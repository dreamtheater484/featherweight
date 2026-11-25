// The interface's catalog (F1, f1-lang): the compiled format reads back what was written, refuses
// damaged or mismatched data, and T() costs nothing in English.
#include <cstring>
#include <string>
#include <vector>

#include "TestHarness.h"
#include "core/Lang.h"
#include "core/LangCatalog.h"

namespace catalog = fw::lang::catalog;

namespace {

struct Built {
  std::vector<uint8_t> keys, texts;
  catalog::Keys table;
};

// Three keys in the template's order, one with a context, and their texts (the second untranslated).
Built Build() {
  Built b;
  const std::vector<uint32_t> hashes = {catalog::Hash(0, nullptr, L"Bold"), catalog::Hash(0, L"verb", L"Open"),
                                        catalog::Hash(0, nullptr, L"Open")};
  b.keys = catalog::WriteKeys(0, hashes);
  CHECK(catalog::ReadKeys(b.keys.data(), b.keys.size(), b.table));
  b.texts = catalog::WriteTexts(b.table, {"Vet", "", "Openen \xE2\x80\xA6"});
  return b;
}

}  // namespace

TEST(Lang_EnglishReturnsItsArgument) {
  const wchar_t* s = L"Increase font size";
  CHECK(T(s) == s);  // no catalog loaded: the same pointer, nothing copied
  const char* u = "Joins texts.";
  CHECK(T(u) == u);
  const char16_t* w = u"Typing";
  CHECK(T(w) == w);
  CHECK(std::wcscmp(UiText(L"Left"), L"Left") == 0);
  CHECK(std::wcscmp(fw::lang::Current(), L"en") == 0);
}

TEST(Lang_FmtFillsPlaceholders) {
  CHECK(Fmt(L"Page {0} of {1}", {L"3", L"12"}) == L"Page 3 of 12");
  CHECK(Fmt(L"{1} {0}", {L"a", L"b"}) == L"b a");  // a language may turn them round
  CHECK(Fmt(L"{0} and {0}", {L"x"}) == L"x and x");
  CHECK(Fmt(L"{2} {x} {", {L"a"}) == L" {x} {");  // a missing argument is empty; other braces stay
  CHECK(Fmt("{0} parts", {std::string("12")}) == "12 parts");
  CHECK(Fmt(u"Undo {0}", {u"Typing"}) == u"Undo Typing");
}

TEST(Lang_ContextSeparatesKeys) {
  CHECK(catalog::Hash(0, L"verb", L"Open") != catalog::Hash(0, nullptr, L"Open"));
  CHECK(catalog::Hash(1, nullptr, L"Open") != catalog::Hash(0, nullptr, L"Open"));
}

TEST(Lang_CatalogReadsBack) {
  Built b = Build();
  CHECK(b.table.count == 3);
  std::vector<wchar_t> text;
  std::vector<uint32_t> offsets;
  CHECK(catalog::ReadTexts(b.texts.data(), b.texts.size(), b.table, text, offsets));
  CHECK(offsets.size() == 3);
  // Each hash finds its place in the template, and the place its text.
  auto lookup = [&](const wchar_t* ctx, const wchar_t* en) -> std::wstring {
    const uint32_t h = catalog::Hash(b.table.seed, ctx, en);
    for (uint32_t i = 0; i < b.table.count; ++i)
      if (b.table.hashes[i] == h) {
        const uint32_t at = offsets[b.table.places[i]];
        return at == UINT32_MAX ? L"(English)" : std::wstring(text.data() + at);
      }
    return L"(none)";
  };
  CHECK(lookup(nullptr, L"Bold") == L"Vet");
  CHECK(lookup(L"verb", L"Open") == L"(English)");
  CHECK(lookup(nullptr, L"Open") == L"Openen …");
  CHECK(lookup(nullptr, L"Italic") == L"(none)");
}

TEST(Lang_CatalogRefusesDamage) {
  Built b = Build();
  std::vector<wchar_t> text;
  std::vector<uint32_t> offsets;
  // Another build's keys: the check differs.
  catalog::Keys other = b.table;
  std::vector<uint8_t> otherKeys = catalog::WriteKeys(7, {1, 2, 3});
  CHECK(catalog::ReadKeys(otherKeys.data(), otherKeys.size(), other));
  CHECK(!catalog::ReadTexts(b.texts.data(), b.texts.size(), other, text, offsets));
  // Cut short, a wrong count, broken UTF-8.
  CHECK(!catalog::ReadTexts(b.texts.data(), b.texts.size() - 1, b.table, text, offsets));
  std::vector<uint8_t> bad = b.texts;
  bad[8] = 9;
  CHECK(!catalog::ReadTexts(bad.data(), bad.size(), b.table, text, offsets));
  bad = b.texts;
  bad[16] = static_cast<uint8_t>(0xC3);  // "Vet" -> a lead byte and "et"
  bad[17] = 0x28;
  CHECK(!catalog::ReadTexts(bad.data(), bad.size(), b.table, text, offsets));
  // Format 2, byte for byte (tests/fuzz/make_catalog_seeds.py writes the same): the hashes 1, 2, 3 as
  // the differences 1, 1, 1, then 2 bits a place: 0, 1, 2.
  const std::vector<uint8_t> want = {'F', 'W', 'L', 'K', 2, 0, 0, 0, 7, 0, 0, 0, 3, 0, 0, 0, 1, 1, 1, 2, 0x24};
  CHECK(otherKeys == want);
  CHECK(other.hashes == std::vector<uint32_t>({1, 2, 3}) && other.places == std::vector<uint16_t>({0, 1, 2}));
  // Two keys with one hash (a difference of 0), a place past the end, too few bits a place, a
  // difference past 32 bits, cut short.
  catalog::Keys k;
  auto refused = [&](size_t at, uint8_t value) {
    std::vector<uint8_t> keys = want;
    keys[at] = value;
    return !catalog::ReadKeys(keys.data(), keys.size(), k);
  };
  CHECK(refused(17, 0));
  CHECK(refused(20, 0x34));
  CHECK(refused(19, 1));
  CHECK(refused(19, 0));
  std::vector<uint8_t> keys = want;
  for (size_t i = 16; i < 21; ++i) keys[i] = 0x80;
  CHECK(!catalog::ReadKeys(keys.data(), keys.size(), k));
  keys = want;
  keys[16] = 0xFF, keys[17] = 0xFF, keys[18] = 0xFF, keys[19] = 0xFF, keys[20] = 0x1F;  // 2^33 - 1
  CHECK(!catalog::ReadKeys(keys.data(), keys.size(), k));
  CHECK(!catalog::ReadKeys(want.data(), want.size() - 1, k));
  CHECK(!catalog::ReadKeys(want.data(), 12, k));
  CHECK(catalog::ReadKeys(b.keys.data(), b.keys.size(), k) && k.hashes == b.table.hashes);
}
