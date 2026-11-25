// fwstrings: compiles the interface's catalogs into resources (F1, f1-lang; src/core/LangCatalog.h).
//
//   fwstrings <template.pot> <outdir> [--keys] [<code>.po ...]
//
// Writes <outdir>/lang_keys.bin (the template's keys), <outdir>/lang_<code>.bin per catalog (its
// texts in the template's order, deflated), and <outdir>/LangData.rc naming them for resource.rc.
// With no catalog and no --keys (test builds pass it, for the pseudo-language's check), the .rc is
// empty and English ships alone. A translation marked fuzzy, or of a key the template doesn't have,
// is left out; a missing one shows English.

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "core/LangCatalog.h"
#include "miniz.h"

namespace {

namespace catalog = fw::lang::catalog;

struct Entry {
  std::string context;  // empty: none
  bool hasContext = false;
  std::string id, str;
  bool fuzzy = false;
};

std::string Unquote(const std::string& line, size_t from) {
  const size_t a = line.find('"', from), b = line.rfind('"');
  std::string out;
  if (a == std::string::npos || b <= a) return out;
  for (size_t i = a + 1; i < b; ++i) {
    char c = line[i];
    if (c != '\\' || i + 1 >= b) {
      out += c;
      continue;
    }
    c = line[++i];
    switch (c) {
      case 'n': out += '\n'; break;
      case 't': out += '\t'; break;
      case 'r': out += '\r'; break;
      case 'a': out += '\a'; break;
      case 'b': out += '\b'; break;
      case 'f': out += '\f'; break;
      case 'v': out += '\v'; break;
      default:
        if (c >= '0' && c <= '7') {
          int v = 0, k = 0;
          while (k < 3 && i < b && line[i] >= '0' && line[i] <= '7') v = v * 8 + (line[i++] - '0'), ++k;
          --i;
          out += static_cast<char>(v);
        } else {
          out += c;
        }
    }
  }
  return out;
}

// A PO file's entries (the header, msgid "", left out).
bool ReadPo(const std::string& path, std::vector<Entry>& out, std::string& error) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    error = "can't read " + path;
    return false;
  }
  std::string line, field;
  Entry e;
  bool any = false;
  std::string* target = nullptr;
  auto flush = [&] {
    if (any && !e.id.empty()) out.push_back(e);
    e = Entry{};
    any = false;
    target = nullptr;
  };
  int n = 0;
  while (std::getline(f, line)) {
    ++n;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (n == 1 && line.rfind("\xEF\xBB\xBF", 0) == 0) line.erase(0, 3);
    if (line.empty()) {
      flush();
      continue;
    }
    if (line.rfind("#,", 0) == 0) {
      if (line.find("fuzzy") != std::string::npos) e.fuzzy = true;
      continue;
    }
    if (line[0] == '#') continue;
    if (line.rfind("msgctxt ", 0) == 0) {
      if (any && target != &e.context) flush();
      e.context = Unquote(line, 7), e.hasContext = true, target = &e.context, any = true;
    } else if (line.rfind("msgid ", 0) == 0) {
      if (any && target == &e.str) flush();
      e.id = Unquote(line, 5), target = &e.id, any = true;
    } else if (line.rfind("msgstr ", 0) == 0) {
      e.str = Unquote(line, 6), target = &e.str, any = true;
    } else if (line[0] == '"' && target) {
      *target += Unquote(line, 0);
    } else {
      error = path + ":" + std::to_string(n) + ": can't read this line";
      return false;
    }
  }
  flush();
  return true;
}

std::wstring Wide(const std::string& s) {
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<size_t>(n), L'\0');
  if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

bool Write(const std::string& path, const std::vector<uint8_t>& bytes) {
  std::ofstream f(path, std::ios::binary);
  f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  return static_cast<bool>(f);
}

// Writes `path` only if its bytes change, so the resource compiler and the link don't run again.
bool WriteIfChanged(const std::string& path, const std::string& text) {
  std::ifstream old(path, std::ios::binary);
  std::stringstream s;
  s << old.rdbuf();
  if (old && s.str() == text) return true;
  old.close();
  return Write(path, std::vector<uint8_t>(text.begin(), text.end()));
}

std::string Key(const Entry& e) { return (e.hasContext ? e.context + '\x04' : std::string()) + e.id; }

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: fwstrings <template.pot> <outdir> [--keys] [<code>.po ...]\n");
    return 2;
  }
  const std::string pot = argv[1], outDir = argv[2];
  bool keysAlways = false;
  std::vector<std::string> pos;
  for (int i = 3; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--keys") keysAlways = true;
    else if (!arg.empty()) pos.push_back(arg);
  }
  CreateDirectoryA(outDir.c_str(), nullptr);
  std::string error;
  std::vector<Entry> keys;
  if (!ReadPo(pot, keys, error)) {
    std::fprintf(stderr, "fwstrings: %s\n", error.c_str());
    return 1;
  }
  std::string rc = "// Made by fwstrings (tools/strings) from strings/: the interface's catalogs.\n";
  if (keys.empty() || (pos.empty() && !keysAlways)) {
    return WriteIfChanged(outDir + "/LangData.rc", rc) ? 0 : 1;
  }
  if (keys.size() > 65535) {
    std::fprintf(stderr, "fwstrings: too many strings (%zu)\n", keys.size());
    return 1;
  }
  std::vector<std::pair<std::string, std::vector<Entry>>> languages;  // (code, entries)
  for (const std::string& po : pos) {
    std::vector<Entry> entries;
    if (!ReadPo(po, entries, error)) {
      std::fprintf(stderr, "fwstrings: %s\n", error.c_str());
      return 1;
    }
    std::string code = po;
    std::replace(code.begin(), code.end(), '\\', '/');
    code = code.substr(code.rfind('/') + 1);
    languages.emplace_back(code.substr(0, code.find('.')), std::move(entries));
  }
  // The texts' order, the same in every language: by their French, which packs the four catalogs
  // 1.4 KB smaller than the template's order (by English); untranslated ones by their English.
  for (const auto& [code, entries] : languages) {
    if (code != "fr") continue;
    std::map<std::string, std::string> french;
    for (const Entry& e : entries)
      if (!e.fuzzy && !e.str.empty()) french[Key(e)] = e.str;
    auto sortText = [&](const Entry& k) -> const std::string& {
      auto it = french.find(Key(k));
      return it == french.end() ? k.id : it->second;
    };
    std::stable_sort(keys.begin(), keys.end(), [&](const Entry& a, const Entry& b) { return sortText(a) < sortText(b); });
  }
  // A seed under which no two keys share a hash.
  std::vector<uint32_t> hashes(keys.size());
  uint32_t seed = 0;
  for (;; ++seed) {
    std::set<uint32_t> seen;
    bool clash = false;
    for (size_t i = 0; i < keys.size() && !clash; ++i) {
      const std::wstring ctx = Wide(keys[i].context), id = Wide(keys[i].id);
      hashes[i] = catalog::Hash(seed, keys[i].hasContext ? ctx.c_str() : nullptr, id.c_str());
      clash = !seen.insert(hashes[i]).second;
    }
    if (!clash) break;
  }
  const std::vector<uint8_t> keyBytes = catalog::WriteKeys(seed, hashes);
  catalog::Keys table;
  if (!catalog::ReadKeys(keyBytes.data(), keyBytes.size(), table)) {
    std::fprintf(stderr, "fwstrings: the key table doesn't read back\n");
    return 1;
  }
  auto fwd = [](std::string p) {
    std::replace(p.begin(), p.end(), '\\', '/');
    return p;
  };
  if (!Write(outDir + "/lang_keys.bin", keyBytes)) return 1;
  rc += "LANG_KEYS RCDATA \"" + fwd(outDir) + "/lang_keys.bin\"\n";
  std::map<std::string, size_t> place;
  for (size_t i = 0; i < keys.size(); ++i) place[Key(keys[i])] = i;
  for (const auto& [code, entries] : languages) {
    std::vector<std::string> texts(keys.size());
    size_t translated = 0;
    for (const Entry& e : entries) {
      auto it = place.find(Key(e));
      if (it == place.end() || e.fuzzy || e.str.empty()) continue;
      ++translated;
      if (e.str != e.id) texts[it->second] = e.str;  // the same as the English: T() returns that anyway
    }
    const std::vector<uint8_t> raw = catalog::WriteTexts(table, texts);
    size_t packedSize = 0;
    const int flags = static_cast<int>(tdefl_create_comp_flags_from_zip_params(10, -15, MZ_DEFAULT_STRATEGY));
    void* packed = tdefl_compress_mem_to_heap(raw.data(), raw.size(), &packedSize, flags);
    if (!packed) {
      std::fprintf(stderr, "fwstrings: %s doesn't compress\n", code.c_str());
      return 1;
    }
    std::vector<uint8_t> out = {'F', 'W', 'L', 'Z'};
    const uint32_t rawSize = static_cast<uint32_t>(raw.size());
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(rawSize >> (8 * i)));
    out.insert(out.end(), static_cast<uint8_t*>(packed), static_cast<uint8_t*>(packed) + packedSize);
    mz_free(packed);
    std::string upper = code;
    std::transform(upper.begin(), upper.end(), upper.begin(), [](char c) { return static_cast<char>(toupper(c)); });
    if (!Write(outDir + "/lang_" + code + ".bin", out)) return 1;
    rc += "LANG_" + upper + " RCDATA \"" + fwd(outDir) + "/lang_" + code + ".bin\"\n";
    std::printf("fwstrings: %s: %zu of %zu translated, %zu bytes\n", code.c_str(), translated, keys.size(), out.size());
  }
  return WriteIfChanged(outDir + "/LangData.rc", rc) ? 0 : 1;
}
