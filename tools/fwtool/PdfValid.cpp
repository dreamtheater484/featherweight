// fwtool pdf-valid (F1, f1-gate): any PDF checked as a reader meets it, for the PDF tools' outputs
// (page tools, OCR, signatures) and the PDF torture run. Not only our own exports (pdf-check):
// files PDFium saved, incrementally or whole, with cross-reference tables or streams.
//  - Its structure, read without PDFium: the header and %%EOF; every cross-reference section
//    (tables and streams, and the /Prev chain of incremental saves), each object in use found
//    where its entry says; each stream's /Length; the trailer's /Root a catalog.
//  - PDFium: it opens, every page loads with a size, its text, and renders.
//  - The fonts it embeds (the legal row: no cloud-font family) and its signatures: each byte range,
//    and whether it covers the whole file (a change after signing leaves bytes outside it).
// Output: one line per file, and with --json the details. Exit 1 when a file has errors.
#include <windows.h>

#include <fpdf_edit.h>
#include <fpdf_signature.h>
#include <fpdf_text.h>
#include <fpdfview.h>

#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "Common.h"
#include "Reference.h"
#include "core/Strings.h"
#include "miniz.h"

namespace fwtool {
namespace {

bool IsWs(char c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == '\f' || c == '\0'; }
bool IsDelim(char c) { return IsWs(c) || c == '/' || c == '<' || c == '>' || c == '[' || c == ']' || c == '(' || c == ')' || c == '{' || c == '}' || c == '%'; }

// A small lexer over the file's bytes: enough to read dictionaries, numbers and references.
struct Lexer {
  std::string_view s;
  size_t pos = 0;
  void SkipWs() {
    while (pos < s.size()) {
      if (IsWs(s[pos])) ++pos;
      else if (s[pos] == '%') { while (pos < s.size() && s[pos] != '\n' && s[pos] != '\r') ++pos; }
      else break;
    }
  }
  bool At(std::string_view word) {
    SkipWs();
    return s.substr(pos, word.size()) == word;
  }
  bool Eat(std::string_view word) {
    if (!At(word)) return false;
    pos += word.size();
    return true;
  }
  bool Int(long long& v) {
    SkipWs();
    size_t p = pos;
    bool neg = false;
    if (p < s.size() && (s[p] == '+' || s[p] == '-')) neg = s[p++] == '-';
    if (p >= s.size() || s[p] < '0' || s[p] > '9') return false;
    long long x = 0;
    while (p < s.size() && s[p] >= '0' && s[p] <= '9') x = x * 10 + (s[p++] - '0');
    if (p < s.size() && s[p] == '.') return false;  // a real, not an integer
    v = neg ? -x : x;
    pos = p;
    return true;
  }
  // Skips one value (a dictionary, array, string, name, number or keyword). False at the end.
  bool SkipValue() {
    SkipWs();
    if (pos >= s.size()) return false;
    char c = s[pos];
    if (c == '<' && pos + 1 < s.size() && s[pos + 1] == '<') {
      pos += 2;
      while (true) {
        SkipWs();
        if (pos >= s.size()) return false;
        if (Eat(">>")) return true;
        if (!SkipValue()) return false;
      }
    }
    if (c == '[') {
      ++pos;
      while (true) {
        SkipWs();
        if (pos >= s.size()) return false;
        if (s[pos] == ']') { ++pos; return true; }
        if (!SkipValue()) return false;
      }
    }
    if (c == '(') {
      int depth = 0;
      for (; pos < s.size(); ++pos) {
        if (s[pos] == '\\') { ++pos; continue; }
        if (s[pos] == '(') ++depth;
        else if (s[pos] == ')' && --depth == 0) { ++pos; return true; }
      }
      return false;
    }
    if (c == '<') {
      size_t e = s.find('>', pos);
      if (e == std::string_view::npos) return false;
      pos = e + 1;
      return true;
    }
    if (c == '/') ++pos;
    else if (c == ']' || c == '>' || c == ')') return false;
    while (pos < s.size() && !IsDelim(s[pos])) ++pos;
    return true;
  }
};

struct Ref {
  long long num = -1, gen = 0;
};

// The top-level entries of the dictionary at `at` ("<<"): key -> the value's text.
std::map<std::string, std::string_view> ReadDict(std::string_view s, size_t at, size_t* end = nullptr) {
  std::map<std::string, std::string_view> out;
  Lexer lx{s, at};
  if (!lx.Eat("<<")) return out;
  while (true) {
    lx.SkipWs();
    if (lx.pos >= s.size()) return out;
    if (lx.Eat(">>")) break;
    if (s[lx.pos] != '/') return out;  // not a key: damaged
    size_t k = ++lx.pos;
    while (lx.pos < s.size() && !IsDelim(s[lx.pos])) ++lx.pos;
    std::string key(s.substr(k, lx.pos - k));
    lx.SkipWs();
    size_t v = lx.pos;
    if (!lx.SkipValue()) return out;
    // An indirect reference: "N G R" is three tokens.
    Lexer peek{s, lx.pos};
    long long gen = 0;
    if (s.substr(v, lx.pos - v).find_first_not_of("0123456789") == std::string_view::npos && peek.Int(gen) && peek.Eat("R")) lx.pos = peek.pos;
    out[key] = s.substr(v, lx.pos - v);
  }
  if (end) *end = lx.pos;
  return out;
}

bool AsInt(std::string_view v, long long& out) {
  Lexer lx{v, 0};
  return lx.Int(out);
}
bool AsRef(std::string_view v, Ref& r) {
  Lexer lx{v, 0};
  return lx.Int(r.num) && lx.Int(r.gen) && lx.Eat("R");
}
std::vector<long long> AsInts(std::string_view v) {
  std::vector<long long> out;
  Lexer lx{v, 0};
  if (!lx.Eat("[")) return out;
  long long x = 0;
  while (lx.Int(x)) out.push_back(x);
  return out;
}

struct Entry {
  int type = 0;  // 0 free, 1 at an offset, 2 in an object stream
  long long offset = 0, gen = 0;  // type 2: offset is the stream's number, gen the index in it
};

struct Structure {
  std::vector<std::string> errors;
  std::string xref;  // "table", "stream" or "hybrid"
  int sections = 0;
  std::map<long long, Entry> objects;  // the newest entry of each number
  Ref root;
  bool encrypted = false;
};

std::vector<uint8_t> Inflate(std::string_view data) {
  size_t outLen = 0;
  void* p = tinfl_decompress_mem_to_heap(data.data(), data.size(), &outLen, TINFL_FLAG_PARSE_ZLIB_HEADER);
  if (!p) return {};
  std::vector<uint8_t> out(static_cast<uint8_t*>(p), static_cast<uint8_t*>(p) + outLen);
  mz_free(p);
  return out;
}

// The data of the stream whose dictionary starts at `dictAt`, given its length. Empty if not found.
std::string_view StreamData(std::string_view s, size_t dictAt, long long length, std::string* error) {
  size_t end = 0;
  ReadDict(s, dictAt, &end);
  Lexer lx{s, end};
  if (!lx.Eat("stream")) { if (error) *error = "no 'stream' after the dictionary"; return {}; }
  size_t p = lx.pos;
  if (p < s.size() && s[p] == '\r') ++p;
  if (p < s.size() && s[p] == '\n') ++p;
  if (length < 0 || p + static_cast<size_t>(length) > s.size()) { if (error) *error = "/Length runs past the end of the file"; return {}; }
  Lexer after{s, p + static_cast<size_t>(length)};
  if (!after.Eat("endstream")) { if (error) *error = "no 'endstream' where /Length " + std::to_string(length) + " ends"; return {}; }
  return s.substr(p, static_cast<size_t>(length));
}

// "N G obj" at exactly `offset`: where the object's dictionary or value starts, or npos.
size_t ObjectAt(std::string_view s, long long offset, long long num, long long gen) {
  if (offset < 0 || static_cast<size_t>(offset) >= s.size()) return std::string_view::npos;
  Lexer lx{s, static_cast<size_t>(offset)};
  if (s[lx.pos] < '0' || s[lx.pos] > '9') return std::string_view::npos;
  long long n = -1, g = -1;
  if (!lx.Int(n) || !lx.Int(g) || !lx.Eat("obj") || n != num || g != gen) return std::string_view::npos;
  lx.SkipWs();
  return lx.pos;
}

// An object's /Length, which may itself be an indirect integer.
bool ResolveLength(std::string_view s, const Structure& st, std::string_view v, long long& length) {
  if (Ref r; AsRef(v, r)) {
    auto it = st.objects.find(r.num);
    if (it == st.objects.end() || it->second.type != 1) return false;
    size_t at = ObjectAt(s, it->second.offset, r.num, it->second.gen);
    if (at == std::string_view::npos) return false;
    Lexer lx{s, at};
    return lx.Int(length);
  }
  return AsInt(v, length);
}

void PngUp(std::vector<uint8_t>& data, int columns, int predictor) {
  if (predictor < 10 || columns <= 0) return;
  const size_t row = static_cast<size_t>(columns) + 1;
  std::vector<uint8_t> out, prev(static_cast<size_t>(columns), 0);
  for (size_t r = 0; r + row <= data.size(); r += row) {
    uint8_t filter = data[r];
    std::vector<uint8_t> cur(data.begin() + r + 1, data.begin() + r + row);
    for (size_t i = 0; i < cur.size(); ++i) {
      uint8_t left = i ? cur[i - 1] : 0, up = prev[i], ul = i ? prev[i - 1] : 0;
      switch (filter) {
        case 1: cur[i] = static_cast<uint8_t>(cur[i] + left); break;
        case 2: cur[i] = static_cast<uint8_t>(cur[i] + up); break;
        case 3: cur[i] = static_cast<uint8_t>(cur[i] + (left + up) / 2); break;
        case 4: {
          int p = left + up - ul, pa = std::abs(p - left), pb = std::abs(p - up), pc = std::abs(p - ul);
          cur[i] = static_cast<uint8_t>(cur[i] + (pa <= pb && pa <= pc ? left : pb <= pc ? up : ul));
          break;
        }
        default: break;
      }
    }
    out.insert(out.end(), cur.begin(), cur.end());
    prev = cur;
  }
  data.swap(out);
}

// One cross-reference section at `offset`. Adds entries not already known (newer sections first)
// and returns the trailer (or the stream's dictionary).
std::map<std::string, std::string_view> ReadSection(std::string_view s, long long offset, Structure& st, bool& isStream) {
  isStream = false;
  if (offset <= 0 || static_cast<size_t>(offset) >= s.size()) {
    st.errors.push_back("a cross-reference offset (" + std::to_string(offset) + ") is outside the file");
    return {};
  }
  Lexer lx{s, static_cast<size_t>(offset)};
  if (s.substr(lx.pos, 4) == "xref") {
    lx.pos += 4;
    while (true) {
      long long start = 0, count = 0;
      Lexer save = lx;
      if (!lx.Int(start) || !lx.Int(count)) { lx = save; break; }
      for (long long i = 0; i < count; ++i) {
        long long off = 0, gen = 0;
        if (!lx.Int(off) || !lx.Int(gen)) { st.errors.push_back("a cross-reference table entry can't be read (object " + std::to_string(start + i) + ")"); return {}; }
        lx.SkipWs();
        char kind = lx.pos < s.size() ? s[lx.pos++] : '?';
        if (kind != 'n' && kind != 'f') { st.errors.push_back("a cross-reference entry is neither n nor f (object " + std::to_string(start + i) + ")"); return {}; }
        if (!st.objects.count(start + i)) st.objects[start + i] = Entry{kind == 'n' ? 1 : 0, off, gen};
      }
    }
    if (!lx.Eat("trailer")) { st.errors.push_back("no trailer after the cross-reference table at " + std::to_string(offset)); return {}; }
    return ReadDict(s, (lx.SkipWs(), lx.pos));
  }
  // A cross-reference stream: "N G obj << /Type /XRef ... >> stream".
  long long num = -1, gen = -1;
  if (!lx.Int(num) || !lx.Int(gen) || !lx.Eat("obj")) {
    st.errors.push_back("startxref or /Prev points at neither 'xref' nor an object (offset " + std::to_string(offset) + ")");
    return {};
  }
  isStream = true;
  lx.SkipWs();
  size_t dictAt = lx.pos;
  auto dict = ReadDict(s, dictAt);
  if (dict["Type"] != "/XRef") { st.errors.push_back("the object at a cross-reference offset isn't /Type /XRef"); return {}; }
  long long length = -1;
  if (!AsInt(dict["Length"], length)) { st.errors.push_back("a cross-reference stream's /Length isn't a direct integer"); return dict; }
  std::string why;
  std::string_view raw = StreamData(s, dictAt, length, &why);
  if (!why.empty()) { st.errors.push_back("cross-reference stream: " + why); return dict; }
  std::vector<uint8_t> data;
  if (dict["Filter"].find("FlateDecode") != std::string_view::npos) data = Inflate(raw);
  else if (dict["Filter"].empty()) data.assign(raw.begin(), raw.end());
  else { st.errors.push_back("a cross-reference stream with filter " + std::string(dict["Filter"])); return dict; }
  if (data.empty()) { st.errors.push_back("a cross-reference stream can't be decompressed"); return dict; }
  std::vector<long long> w = AsInts(dict["W"]);
  if (w.size() != 3) { st.errors.push_back("a cross-reference stream's /W isn't three numbers"); return dict; }
  if (auto parms = dict.find("DecodeParms"); parms != dict.end()) {
    auto p = ReadDict(parms->second, 0);
    long long predictor = 1, columns = 1;
    AsInt(p["Predictor"], predictor);
    AsInt(p["Columns"], columns);
    PngUp(data, static_cast<int>(columns), static_cast<int>(predictor));
  }
  long long size = 0;
  AsInt(dict["Size"], size);
  std::vector<long long> index = AsInts(dict["Index"]);
  if (index.empty()) index = {0, size};
  const size_t rowBytes = static_cast<size_t>(w[0] + w[1] + w[2]);
  size_t at = 0;
  auto field = [&](size_t& p, long long width, long long fallback) {
    if (width == 0) return fallback;
    long long v = 0;
    for (long long b = 0; b < width; ++b) v = (v << 8) | data[p++];
    return v;
  };
  for (size_t i = 0; i + 1 < index.size(); i += 2) {
    for (long long k = 0; k < index[i + 1]; ++k) {
      if (at + rowBytes > data.size()) { st.errors.push_back("a cross-reference stream is shorter than its /Index says"); return dict; }
      size_t p = at;
      long long t = field(p, w[0], 1), a = field(p, w[1], 0), b = field(p, w[2], 0);
      at += rowBytes;
      const long long obj = index[i] + k;
      if (!st.objects.count(obj)) st.objects[obj] = Entry{t == 1 ? 1 : t == 2 ? 2 : 0, a, t == 1 ? b : b};
    }
  }
  return dict;
}

Structure CheckStructure(std::string_view s) {
  Structure st;
  if (s.substr(0, 1024).find("%PDF-") == std::string_view::npos) st.errors.push_back("no %PDF- header in the first 1,024 bytes");
  const size_t tail = s.size() > 1024 ? s.size() - 1024 : 0;
  if (s.find("%%EOF", tail) == std::string_view::npos) st.errors.push_back("no %%EOF in the last 1,024 bytes");
  size_t sx = s.rfind("startxref");
  if (sx == std::string_view::npos) { st.errors.push_back("no startxref"); return st; }
  Lexer lx{s, sx + 9};
  long long offset = 0;
  if (!lx.Int(offset)) { st.errors.push_back("startxref has no offset"); return st; }
  std::set<long long> seen;
  bool tables = false, streams = false;
  std::map<std::string, std::string_view> newest;
  while (offset > 0) {
    if (!seen.insert(offset).second) { st.errors.push_back("the /Prev chain loops"); break; }
    if (seen.size() > 10000) { st.errors.push_back("more than 10,000 cross-reference sections"); break; }
    bool isStream = false;
    auto trailer = ReadSection(s, offset, st, isStream);
    ++st.sections;
    (isStream ? streams : tables) = true;
    if (trailer.empty()) break;
    if (newest.empty()) newest = trailer;
    // A hybrid file's table names a stream with the objects only stream-aware readers see.
    long long xrefStm = 0;
    if (AsInt(trailer["XRefStm"], xrefStm) && xrefStm > 0) {
      bool s2 = false;
      ReadSection(s, xrefStm, st, s2);
      streams = true;
    }
    if (trailer.count("Encrypt")) st.encrypted = true;
    long long prev = 0;
    offset = AsInt(trailer["Prev"], prev) ? prev : 0;
  }
  st.xref = tables && streams ? "hybrid" : streams ? "stream" : "table";
  if (newest.empty()) return st;
  if (!AsRef(newest["Root"], st.root)) st.errors.push_back("the trailer has no /Root");
  long long size = 0;
  if (!AsInt(newest["Size"], size)) st.errors.push_back("the trailer has no /Size");
  // Every object in use where its entry says, and each stream as long as its /Length.
  int wrong = 0, lengths = 0;
  for (const auto& [num, e] : st.objects) {
    if (size && num >= size) { st.errors.push_back("object " + std::to_string(num) + " is beyond /Size " + std::to_string(size)); break; }
    if (e.type == 2) {
      auto it = st.objects.find(e.offset);
      if (it == st.objects.end() || it->second.type != 1) {
        if (++wrong <= 5) st.errors.push_back("object " + std::to_string(num) + " is in object stream " + std::to_string(e.offset) + ", which isn't in use");
      }
      continue;
    }
    if (e.type != 1) continue;
    size_t at = ObjectAt(s, e.offset, num, e.gen);
    if (at == std::string_view::npos) {
      if (++wrong <= 5) st.errors.push_back("object " + std::to_string(num) + " " + std::to_string(e.gen) + " isn't at offset " + std::to_string(e.offset));
      continue;
    }
    if (s.substr(at, 2) != "<<") continue;
    size_t end = 0;
    auto dict = ReadDict(s, at, &end);
    Lexer after{s, end};
    if (!after.At("stream")) continue;
    long long length = -1;
    std::string why;
    if (!ResolveLength(s, st, dict["Length"], length)) why = "its /Length can't be read";
    else StreamData(s, at, length, &why);
    if (!why.empty() && ++lengths <= 5) st.errors.push_back("stream object " + std::to_string(num) + ": " + why);
  }
  if (wrong > 5) st.errors.push_back(std::to_string(wrong) + " objects not where their entries say, in all");
  if (lengths > 5) st.errors.push_back(std::to_string(lengths) + " streams with a wrong /Length, in all");
  if (st.root.num >= 0) {
    auto it = st.objects.find(st.root.num);
    if (it == st.objects.end() || it->second.type == 0) st.errors.push_back("/Root names an object not in use");
    else if (it->second.type == 1) {
      size_t at = ObjectAt(s, it->second.offset, st.root.num, it->second.gen);
      if (at != std::string_view::npos && ReadDict(s, at)["Type"] != "/Catalog") st.errors.push_back("/Root isn't a /Catalog");
    }
  }
  return st;
}

struct Signature {
  std::string subFilter;
  std::vector<int> byteRange;
  bool byteRangeOk = false, coversWholeFile = false;
  unsigned long contentsBytes = 0;
};

struct Result {
  std::string file;
  size_t bytes = 0;
  std::vector<std::string> errors;
  Structure st;
  int version = 0, pages = 0;
  long long chars = 0;
  std::set<std::string> embedded, notEmbedded;
  std::vector<Signature> signatures;
};

void Fonts(FPDF_PAGEOBJECT obj, Result& r, int depth) {
  int type = FPDFPageObj_GetType(obj);
  if (type == FPDF_PAGEOBJ_TEXT) {
    if (FPDF_FONT font = FPDFTextObj_GetFont(obj)) {
      char name[256] = {};
      size_t n = FPDFFont_GetBaseFontName(font, name, sizeof name);
      std::string s = n > 1 ? std::string(name, std::min(n - 1, sizeof name - 1)) : std::string("(no name)");
      (FPDFFont_GetIsEmbedded(font) == 1 ? r.embedded : r.notEmbedded).insert(s);
    }
  } else if (type == FPDF_PAGEOBJ_FORM && depth < 8) {
    for (int i = 0, n = FPDFFormObj_CountObjects(obj); i < n; ++i) Fonts(FPDFFormObj_GetObject(obj, static_cast<unsigned long>(i)), r, depth + 1);
  }
}

const char* LoadError(unsigned long e) {
  switch (e) {
    case FPDF_ERR_FILE: return "file not found or can't be opened";
    case FPDF_ERR_FORMAT: return "not a PDF, or damaged";
    case FPDF_ERR_PASSWORD: return "password needed or wrong";
    case FPDF_ERR_SECURITY: return "unsupported security scheme";
    case FPDF_ERR_PAGE: return "page not found or content error";
    default: return "unknown error";
  }
}

Result Check(const std::filesystem::path& file, const std::wstring& password) {
  Result r;
  r.file = Display(file);
  std::ifstream in(file, std::ios::binary);
  std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  r.bytes = data.size();
  if (data.empty()) { r.errors.push_back("empty or unreadable"); return r; }
  r.st = CheckStructure(data);
  for (const auto& e : r.st.errors) r.errors.push_back("structure: " + e);
  const std::string pw = fw::WideToUtf8(password);
  FPDF_DOCUMENT doc = FPDF_LoadMemDocument64(data.data(), data.size(), pw.empty() ? nullptr : pw.c_str());
  if (!doc) {
    r.errors.push_back(std::string("PDFium: ") + LoadError(FPDF_GetLastError()));
    return r;
  }
  FPDF_GetFileVersion(doc, &r.version);
  r.pages = FPDF_GetPageCount(doc);
  if (r.pages <= 0) r.errors.push_back("PDFium: no pages");
  for (int i = 0; i < r.pages; ++i) {
    FPDF_PAGE page = FPDF_LoadPage(doc, i);
    if (!page) { r.errors.push_back("PDFium: page " + std::to_string(i + 1) + " doesn't load"); continue; }
    const float w = FPDF_GetPageWidthF(page), h = FPDF_GetPageHeightF(page);
    if (!(w > 0 && h > 0)) r.errors.push_back("PDFium: page " + std::to_string(i + 1) + " has no size");
    if (FPDF_TEXTPAGE text = FPDFText_LoadPage(page)) {
      int n = FPDFText_CountChars(text);
      if (n < 0) r.errors.push_back("PDFium: page " + std::to_string(i + 1) + "'s text can't be read");
      else r.chars += n;
      FPDFText_ClosePage(text);
    }
    for (int k = 0, n = FPDFPage_CountObjects(page); k < n; ++k) Fonts(FPDFPage_GetObject(page, k), r, 0);
    // Rendered small: a page that crashes or fails to draw shows here.
    const int bw = std::clamp(static_cast<int>(w / 4), 1, 400), bh = std::clamp(static_cast<int>(h / 4), 1, 400);
    if (FPDF_BITMAP bmp = FPDFBitmap_Create(bw, bh, 0)) {
      FPDFBitmap_FillRect(bmp, 0, 0, bw, bh, 0xFFFFFFFF);
      FPDF_RenderPageBitmap(bmp, page, 0, 0, bw, bh, 0, FPDF_ANNOT);
      FPDFBitmap_Destroy(bmp);
    }
    FPDF_ClosePage(page);
  }
  for (int i = 0, n = FPDF_GetSignatureCount(doc); i < n; ++i) {
    FPDF_SIGNATURE sig = FPDF_GetSignatureObject(doc, i);
    Signature s;
    char sub[128] = {};
    unsigned long len = FPDFSignatureObj_GetSubFilter(sig, sub, sizeof sub);
    s.subFilter = len > 1 ? std::string(sub, std::min<size_t>(len - 1, sizeof sub - 1)) : "";
    unsigned long count = FPDFSignatureObj_GetByteRange(sig, nullptr, 0);
    s.byteRange.resize(count);
    if (count) FPDFSignatureObj_GetByteRange(sig, s.byteRange.data(), count);
    s.contentsBytes = FPDFSignatureObj_GetContents(sig, nullptr, 0);
    if (s.byteRange.size() == 4) {
      const long long a = s.byteRange[0], b = s.byteRange[1], c = s.byteRange[2], d = s.byteRange[3];
      s.byteRangeOk = a == 0 && b > 0 && c > b && c + d <= static_cast<long long>(data.size()) &&
                      data[static_cast<size_t>(b)] == '<' && data[static_cast<size_t>(c - 1)] == '>';
      s.coversWholeFile = c + d == static_cast<long long>(data.size());
    }
    if (!s.byteRangeOk) r.errors.push_back("signature " + std::to_string(i + 1) + ": its /ByteRange doesn't frame its /Contents");
    r.signatures.push_back(s);
  }
  FPDF_CloseDocument(doc);
  return r;
}

std::string Json(const std::string& s) {
  std::string out = "\"";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') out += '\\', out += static_cast<char>(c);
    else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); out += b; }
    else out += static_cast<char>(c);
  }
  return out + "\"";
}
std::string JsonList(const std::set<std::string>& v) {
  std::string out = "[";
  for (const auto& s : v) out += (out.size() > 1 ? ", " : "") + Json(s);
  return out + "]";
}

}  // namespace

int RunPdfValid(const Options& options) {
  std::string error;
  if (!InitPdfium(error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 1;
  }
  std::vector<std::filesystem::path> files;
  auto add = [&](const std::filesystem::path& p) {
    std::error_code ec;
    if (std::filesystem::is_directory(p, ec)) {
      for (const auto& e : std::filesystem::recursive_directory_iterator(p, ec))
        if (e.is_regular_file() && fw::ToLower(e.path().extension().wstring()) == L".pdf") files.push_back(e.path());
    } else files.push_back(p);
  };
  for (const auto& p : options.inputs) add(p);
  if (!options.samples.empty()) {
    std::ifstream list(options.samples, std::ios::binary);
    std::string line;
    while (std::getline(list, line)) {
      while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
      if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF) line.erase(0, 3);  // a BOM
      if (!line.empty()) add(fw::Utf8ToWide(line));
    }
  }
  std::sort(files.begin(), files.end());
  int bad = 0;
  std::string json = "{\n  \"format\": \"featherweight-pdf-valid\",\n  \"files\": [";
  for (size_t i = 0; i < files.size(); ++i) {
    Result r = Check(files[i], options.password);
    if (!r.errors.empty()) ++bad;
    std::printf("[%s] %s: %d pages, %s xref (%d sections), %zu embedded fonts, %zu signatures%s%s\n",
                r.errors.empty() ? "OK" : "FAIL", r.file.c_str(), r.pages, r.st.xref.c_str(), r.st.sections,
                r.embedded.size(), r.signatures.size(), r.errors.empty() ? "" : ": ", r.errors.empty() ? "" : r.errors.front().c_str());
    for (size_t e = 1; e < r.errors.size() && e < 6; ++e) std::printf("    %s\n", r.errors[e].c_str());
    json += std::string(i ? ",\n" : "\n") + "    {\"file\": " + Json(fw::WideToUtf8(std::filesystem::absolute(files[i]).wstring())) +
            ", \"bytes\": " + std::to_string(r.bytes) + ", \"ok\": " + (r.errors.empty() ? "true" : "false") +
            ", \"version\": " + std::to_string(r.version) + ", \"pages\": " + std::to_string(r.pages) +
            ", \"chars\": " + std::to_string(r.chars) + ", \"xref\": " + Json(r.st.xref) +
            ", \"sections\": " + std::to_string(r.st.sections) + ", \"objects\": " + std::to_string(r.st.objects.size()) +
            ", \"encrypted\": " + (r.st.encrypted ? "true" : "false") + ",\n     \"fonts\": " + JsonList(r.embedded) +
            ", \"fontsNotEmbedded\": " + JsonList(r.notEmbedded) + ",\n     \"signatures\": [";
    for (size_t k = 0; k < r.signatures.size(); ++k) {
      const auto& s = r.signatures[k];
      std::string range;
      for (int v : s.byteRange) range += (range.empty() ? "" : ", ") + std::to_string(v);
      json += std::string(k ? ", " : "") + "{\"subFilter\": " + Json(s.subFilter) + ", \"byteRange\": [" + range +
              "], \"byteRangeOk\": " + (s.byteRangeOk ? "true" : "false") + ", \"coversWholeFile\": " +
              (s.coversWholeFile ? "true" : "false") + ", \"contentsBytes\": " + std::to_string(s.contentsBytes) + "}";
    }
    json += "],\n     \"errors\": [";
    for (size_t e = 0; e < r.errors.size(); ++e) json += std::string(e ? ", " : "") + Json(r.errors[e]);
    json += "]}";
  }
  json += "\n  ]\n}\n";
  std::printf("%zu PDFs, %d with errors\n", files.size(), bad);
  if (!options.jsonOut.empty()) {
    std::ofstream out(options.jsonOut, std::ios::binary);
    out << json;
  }
  return bad || files.empty() ? 1 : 0;
}

}  // namespace fwtool
