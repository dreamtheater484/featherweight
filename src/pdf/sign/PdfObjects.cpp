// PDF syntax only, no text a person reads.  // lang: skip-file
#include "pdf/sign/PdfObjects.h"

#include <miniz.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <set>

namespace pdfobj {

namespace {

constexpr int kMaxDepth = 32;
constexpr size_t kMaxItems = 1u << 20;
constexpr int kMaxObjects = 10'000'000;
constexpr size_t kMaxStream = size_t{64} << 20;
constexpr size_t kMaxEntries = 2'000'000;            // cross-reference entries kept, all sections together
constexpr size_t kMaxInflated = size_t{256} << 20;   // stream data inflated, all streams together

bool IsWhite(char c) { return c == 0 || c == '\t' || c == '\n' || c == '\f' || c == '\r' || c == ' '; }
bool IsDelim(char c) {
  return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' || c == ']' || c == '{' || c == '}' || c == '/' ||
         c == '%';
}

void SkipWhite(std::string_view s, size_t& at) {
  while (at < s.size()) {
    if (IsWhite(s[at])) {
      ++at;
    } else if (s[at] == '%') {
      while (at < s.size() && s[at] != '\r' && s[at] != '\n') ++at;
    } else {
      break;
    }
  }
}

std::string_view Token(std::string_view s, size_t& at) {
  const size_t from = at;
  while (at < s.size() && !IsWhite(s[at]) && !IsDelim(s[at])) ++at;
  return s.substr(from, at - from);
}

bool IsInt(std::string_view t) {
  if (t.empty()) return false;
  size_t i = (t[0] == '+' || t[0] == '-') ? 1 : 0;
  if (i == t.size()) return false;
  for (; i < t.size(); ++i)
    if (t[i] < '0' || t[i] > '9') return false;
  return true;
}

bool IsNumber(std::string_view t) {
  if (t.empty()) return false;
  size_t i = (t[0] == '+' || t[0] == '-') ? 1 : 0;
  bool digit = false, dot = false;
  for (; i < t.size(); ++i) {
    if (t[i] >= '0' && t[i] <= '9') digit = true;
    else if (t[i] == '.' && !dot) dot = true;
    else return false;
  }
  return digit;
}

bool ToInt(std::string_view t, long long& v) {
  if (!IsInt(t)) return false;
  if (t[0] == '+') t.remove_prefix(1);
  const auto [p, ec] = std::from_chars(t.data(), t.data() + t.size(), v);
  return ec == std::errc() && p == t.data() + t.size();
}

// A whole number token at `at`, moved past.
bool ReadInt(std::string_view s, size_t& at, long long& v) {
  SkipWhite(s, at);
  size_t p = at;
  if (!ToInt(Token(s, p), v)) return false;
  at = p;
  return true;
}

bool Keyword(std::string_view s, size_t& at, std::string_view word) {
  SkipWhite(s, at);
  size_t p = at;
  if (Token(s, p) != word) return false;
  at = p;
  return true;
}

}  // namespace

const Obj* Obj::Get(std::string_view key) const {
  for (const auto& [k, v] : dict)
    if (k == key) return &v;
  return nullptr;
}

void Obj::Set(const std::string& key, Obj value) {
  for (auto& [k, v] : dict)
    if (k == key) {
      v = std::move(value);
      return;
    }
  dict.emplace_back(key, std::move(value));
}

long long Obj::Int(long long fallback) const {
  long long v = 0;
  return type == Number && ToInt(text, v) ? v : fallback;
}

Obj MakeName(std::string name) {
  Obj o;
  o.type = Obj::Name;
  o.text = std::move(name);
  return o;
}

Obj MakeInt(long long v) {
  Obj o;
  o.type = Obj::Number;
  o.text = std::to_string(v);
  return o;
}

Obj MakeRef(int num, int gen) {
  Obj o;
  o.type = Obj::Ref;
  o.num = num;
  o.gen = gen;
  return o;
}

Obj MakeString(std::string_view bytes) {
  Obj o;
  o.type = Obj::String;
  for (char c : bytes) {
    if (c == '\\' || c == '(' || c == ')') o.text += '\\';
    if (c == '\r') {
      o.text += "\\r";
      continue;
    }
    o.text += c;
  }
  return o;
}

Obj MakeText(std::wstring_view text) {
  bool ascii = true;
  for (wchar_t c : text) ascii = ascii && c >= 32 && c < 127;
  if (ascii) {
    std::string narrow;
    for (wchar_t c : text) narrow += static_cast<char>(c);
    return MakeString(narrow);
  }
  Obj o;
  o.type = Obj::HexString;
  static const char* hex = "0123456789ABCDEF";
  o.text = "FEFF";
  for (wchar_t c : text)
    for (int shift : {12, 8, 4, 0}) o.text += hex[(c >> shift) & 15];
  return o;
}

std::string Write(const Obj& o) {
  switch (o.type) {
    case Obj::Null: return "null";
    case Obj::Bool:
    case Obj::Number: return o.text;
    case Obj::String: return "(" + o.text + ")";
    case Obj::HexString: return "<" + o.text + ">";
    case Obj::Name: return "/" + o.text;
    case Obj::Ref: return std::to_string(o.num) + " " + std::to_string(o.gen) + " R";
    case Obj::Array: {
      std::string out = "[";
      for (size_t i = 0; i < o.items.size(); ++i) {
        if (i) out += ' ';
        out += Write(o.items[i]);
      }
      return out + "]";
    }
    case Obj::Dict:
    case Obj::Stream: {
      std::string out = "<<";
      for (const auto& [k, v] : o.dict) out += "/" + k + " " + Write(v);
      return out + ">>";
    }
  }
  return "null";
}

bool ParseObject(std::string_view s, size_t& at, Obj& out, int depth) {
  out = Obj{};
  if (depth > kMaxDepth) return false;
  SkipWhite(s, at);
  if (at >= s.size()) return false;
  const char c = s[at];
  if (c == '/') {
    ++at;
    out.type = Obj::Name;
    out.text = std::string(Token(s, at));
    return true;
  }
  if (c == '(') {
    size_t i = at + 1;
    int level = 1;
    while (i < s.size()) {
      if (s[i] == '\\') {
        i += 2;
        continue;
      }
      if (s[i] == '(') ++level;
      if (s[i] == ')' && --level == 0) break;
      ++i;
    }
    if (level != 0 || i >= s.size()) return false;
    out.type = Obj::String;
    out.text = std::string(s.substr(at + 1, i - at - 1));
    at = i + 1;
    return true;
  }
  if (c == '<' && at + 1 < s.size() && s[at + 1] == '<') {
    at += 2;
    out.type = Obj::Dict;
    for (;;) {
      SkipWhite(s, at);
      if (at + 1 < s.size() && s[at] == '>' && s[at + 1] == '>') {
        at += 2;
        break;
      }
      if (at >= s.size() || s[at] != '/' || out.dict.size() >= kMaxItems) return false;
      ++at;
      std::string key(Token(s, at));
      Obj value;
      if (!ParseObject(s, at, value, depth + 1)) return false;
      out.dict.emplace_back(std::move(key), std::move(value));
    }
    // A stream: its data starts after the keyword and one end of line.
    size_t p = at;
    SkipWhite(s, p);
    if (s.substr(p, 6) == "stream" && (p + 6 >= s.size() || s[p + 6] == '\r' || s[p + 6] == '\n')) {
      p += 6;
      if (p < s.size() && s[p] == '\r') ++p;
      if (p < s.size() && s[p] == '\n') ++p;
      out.type = Obj::Stream;
      out.streamAt = p;
      const Obj* len = out.Get("Length");
      out.streamLen = len && len->type == Obj::Number && len->Int(-1) >= 0 ? static_cast<size_t>(len->Int()) : 0;
      at = p;
    }
    return true;
  }
  if (c == '<') {
    const size_t end = s.find('>', at + 1);
    if (end == std::string_view::npos) return false;
    for (size_t i = at + 1; i < end; ++i)
      if (!IsWhite(s[i]) && !isxdigit(static_cast<unsigned char>(s[i]))) return false;
    out.type = Obj::HexString;
    out.text = std::string(s.substr(at + 1, end - at - 1));
    at = end + 1;
    return true;
  }
  if (c == '[') {
    ++at;
    out.type = Obj::Array;
    for (;;) {
      SkipWhite(s, at);
      if (at < s.size() && s[at] == ']') {
        ++at;
        break;
      }
      if (at >= s.size() || out.items.size() >= kMaxItems) return false;
      Obj item;
      if (!ParseObject(s, at, item, depth + 1)) return false;
      out.items.push_back(std::move(item));
    }
    return true;
  }
  const std::string_view t = Token(s, at);
  if (t.empty()) return false;
  if (t == "true" || t == "false") {
    out.type = Obj::Bool;
    out.text = std::string(t);
    return true;
  }
  if (t == "null") return true;
  if (!IsNumber(t)) return false;
  // "12 0 R" is a reference.
  long long num = 0;
  if (ToInt(t, num) && num >= 0 && num < kMaxObjects) {
    size_t p = at;
    long long gen = 0;
    if (ReadInt(s, p, gen) && gen >= 0 && gen <= 65535) {
      SkipWhite(s, p);
      if (p < s.size() && s[p] == 'R' && (p + 1 == s.size() || IsWhite(s[p + 1]) || IsDelim(s[p + 1]))) {
        out.type = Obj::Ref;
        out.num = static_cast<int>(num);
        out.gen = static_cast<int>(gen);
        at = p + 1;
        return true;
      }
    }
  }
  out.type = Obj::Number;
  out.text = std::string(t);
  return true;
}

bool Inflate(std::string_view in, std::string& out, size_t limit) {
  struct Sink {
    std::string* out;
    size_t limit;
  } sink{&out, limit};
  out.clear();
  size_t inLen = in.size();
  const int ok = tinfl_decompress_mem_to_callback(
      in.data(), &inLen,
      [](const void* buf, int len, void* user) -> int {
        auto* k = static_cast<Sink*>(user);
        if (k->out->size() + static_cast<size_t>(len) > k->limit) return 0;
        k->out->append(static_cast<const char*>(buf), static_cast<size_t>(len));
        return 1;
      },
      &sink, TINFL_FLAG_PARSE_ZLIB_HEADER);
  return ok != 0;
}

bool File::Open(std::string_view bytes) {
  data_ = bytes;
  xref_.clear();
  objStreams_.clear();
  sections_ = 0;
  inflated_ = 0;
  if (bytes.size() < 32 || bytes.substr(0, 5) != "%PDF-") return false;
  const size_t tail = bytes.size() > 4096 ? bytes.size() - 4096 : 0;
  const size_t sx = bytes.rfind("startxref");
  if (sx == std::string_view::npos || sx < tail) return false;
  size_t at = sx + 9;
  long long offset = 0;
  if (!ReadInt(bytes, at, offset) || offset <= 0 || static_cast<size_t>(offset) >= bytes.size()) return false;
  lastXref_ = static_cast<size_t>(offset);
  if (!ReadSection(lastXref_, 0)) return false;
  const Obj* size = trailer_.Get("Size");
  size_ = size ? static_cast<int>(std::clamp<long long>(size->Int(0), 0, kMaxObjects)) : 0;
  return size_ > 0 && trailer_.Get("Root") && trailer_.Get("Root")->type == Obj::Ref;
}

bool File::ReadTable(size_t& at, Obj& trailer) {
  at += 4;  // "xref"
  std::map<int, Entry> local;
  for (;;) {
    SkipWhite(data_, at);
    if (data_.substr(at, 7) == "trailer") {
      at += 7;
      if (!ParseObject(data_, at, trailer) || trailer.type != Obj::Dict) return false;
      break;
    }
    long long first = 0, count = 0;
    if (!ReadInt(data_, at, first) || !ReadInt(data_, at, count) || first < 0 || count < 0 ||
        first + count > kMaxObjects)
      return false;
    for (long long i = 0; i < count; ++i) {
      long long offset = 0, gen = 0;
      if (!ReadInt(data_, at, offset) || !ReadInt(data_, at, gen)) return false;
      SkipWhite(data_, at);
      if (at >= data_.size() || (data_[at] != 'n' && data_[at] != 'f')) return false;
      Entry e;
      if (data_[at] == 'n') {
        e.type = 1;
        e.a = static_cast<uint64_t>(std::max(0ll, offset));
        e.b = static_cast<uint32_t>(std::clamp(gen, 0ll, 65535ll));
      }
      ++at;
      if (local.size() >= kMaxEntries) return false;
      local.emplace(static_cast<int>(first + i), e);
    }
  }
  // A hybrid file: the stream lists the objects in object streams, which the table shows as free.
  if (const Obj* stm = trailer.Get("XRefStm"); stm && stm->Int(-1) > 0 &&
                                                static_cast<size_t>(stm->Int()) < data_.size()) {
    size_t p = static_cast<size_t>(stm->Int());
    long long n = 0, g = 0;
    Obj xs;
    if (ReadInt(data_, p, n) && ReadInt(data_, p, g) && Keyword(data_, p, "obj") && ParseObject(data_, p, xs) &&
        xs.type == Obj::Stream) {
      std::map<int, Entry> saved;
      saved.swap(xref_);
      ReadXrefStream(xs, data_);
      for (auto& [num, e] : xref_) {
        auto it = local.find(num);
        if (it == local.end() || it->second.type == 0) local[num] = e;
      }
      xref_.swap(saved);
    }
  }
  for (auto& [num, e] : local) xref_.emplace(num, e);
  return true;
}

bool File::ReadSection(size_t offset, int depth) {
  if (depth > 64 || ++sections_ > 512 || offset >= data_.size()) return false;
  size_t at = offset;
  SkipWhite(data_, at);
  Obj trailer;
  bool stream = false;
  if (data_.substr(at, 4) == "xref") {
    if (!ReadTable(at, trailer)) return false;
  } else {
    long long n = 0, g = 0;
    Obj xs;
    if (!ReadInt(data_, at, n) || !ReadInt(data_, at, g) || !Keyword(data_, at, "obj") || !ParseObject(data_, at, xs) ||
        xs.type != Obj::Stream || !xs.Get("Type") || !xs.Get("Type")->IsName("XRef"))
      return false;
    if (!ReadXrefStream(xs, data_)) return false;
    trailer = xs;
    trailer.type = Obj::Dict;
    stream = true;
  }
  if (depth == 0) {
    trailer_ = trailer;
    lastIsStream_ = stream;
  }
  if (const Obj* prev = trailer.Get("Prev")) {
    const long long p = prev->Int(-1);
    if (p <= 0 || static_cast<size_t>(p) >= data_.size() || static_cast<size_t>(p) == offset) return false;
    return ReadSection(static_cast<size_t>(p), depth + 1);
  }
  return true;
}

bool File::StreamData(const Obj& stream, std::string_view source, std::string& out) {
  size_t len = stream.streamLen;
  if (const Obj* l = stream.Get("Length"); l && l->type == Obj::Ref) {
    Obj v;
    len = Get(l->num, v) && v.Int(-1) >= 0 ? static_cast<size_t>(v.Int()) : 0;
  }
  const size_t from = stream.streamAt;
  if (from > source.size()) return false;
  if (len == 0 || from + len > source.size() || len > kMaxStream) {
    const size_t end = source.find("endstream", from);
    if (end == std::string_view::npos) return false;
    len = end - from;
    while (len > 0 && (source[from + len - 1] == '\n' || source[from + len - 1] == '\r')) --len;
  }
  const std::string_view raw = source.substr(from, len);
  const Obj* filter = stream.Get("Filter");
  if (filter && filter->type == Obj::Array && filter->items.size() == 1) filter = &filter->items[0];
  if (!filter) {
    out.assign(raw);
  } else if (filter->IsName("FlateDecode")) {
    if (inflated_ >= kMaxInflated || !Inflate(raw, out, std::min(kMaxStream, kMaxInflated - inflated_))) return false;
    inflated_ += out.size();
  } else {
    return false;
  }
  // PNG predictors (Predictor 10-15), which cross-reference streams usually have.
  const Obj* parms = stream.Get("DecodeParms");
  if (parms && parms->type == Obj::Array && parms->items.size() == 1) parms = &parms->items[0];
  if (!parms || parms->type != Obj::Dict) return true;
  const long long predictor = parms->Get("Predictor") ? parms->Get("Predictor")->Int(1) : 1;
  if (predictor < 10) return predictor == 1;
  const long long columns = parms->Get("Columns") ? parms->Get("Columns")->Int(1) : 1;
  const long long colors = parms->Get("Colors") ? parms->Get("Colors")->Int(1) : 1;
  const long long bits = parms->Get("BitsPerComponent") ? parms->Get("BitsPerComponent")->Int(8) : 8;
  if (columns < 1 || columns > 65536 || colors < 1 || colors > 4 || bits != 8) return false;
  const size_t bpp = static_cast<size_t>(colors), row = static_cast<size_t>(columns * colors);
  std::string result;
  std::string prior(row, '\0');
  for (size_t at = 0; at + 1 + row <= out.size(); at += 1 + row) {
    const auto filterType = static_cast<unsigned char>(out[at]);
    std::string cur(out.data() + at + 1, row);
    for (size_t i = 0; i < row; ++i) {
      const auto a = static_cast<unsigned char>(i >= bpp ? cur[i - bpp] : 0);
      const auto b = static_cast<unsigned char>(prior[i]);
      const auto c = static_cast<unsigned char>(i >= bpp ? prior[i - bpp] : 0);
      unsigned char add = 0;
      switch (filterType) {
        case 0: break;
        case 1: add = a; break;
        case 2: add = b; break;
        case 3: add = static_cast<unsigned char>((a + b) / 2); break;
        case 4: {
          const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
          add = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
          break;
        }
        default: return false;
      }
      cur[i] = static_cast<char>(static_cast<unsigned char>(cur[i]) + add);
    }
    result += cur;
    prior = cur;
  }
  out.swap(result);
  return true;
}

bool File::ReadXrefStream(const Obj& stream, std::string_view data) {
  std::string rows;
  if (!StreamData(stream, data, rows)) return false;
  const Obj* w = stream.Get("W");
  if (!w || w->type != Obj::Array || w->items.size() != 3) return false;
  int widths[3];
  for (int i = 0; i < 3; ++i) {
    const long long v = w->items[i].Int(-1);
    if (v < 0 || v > 8) return false;
    widths[i] = static_cast<int>(v);
  }
  const size_t rowLen = static_cast<size_t>(widths[0] + widths[1] + widths[2]);
  if (rowLen == 0) return false;
  std::vector<std::pair<long long, long long>> ranges;
  if (const Obj* index = stream.Get("Index"); index && index->type == Obj::Array) {
    for (size_t i = 0; i + 1 < index->items.size(); i += 2)
      ranges.emplace_back(index->items[i].Int(-1), index->items[i + 1].Int(-1));
  } else {
    ranges.emplace_back(0, stream.Get("Size") ? stream.Get("Size")->Int(-1) : -1);
  }
  size_t at = 0;
  auto field = [&](int width, uint64_t fallback) {
    if (width == 0) return fallback;
    uint64_t v = 0;
    for (int i = 0; i < width; ++i) v = v << 8 | static_cast<unsigned char>(rows[at++]);
    return v;
  };
  for (const auto& [first, count] : ranges) {
    if (first < 0 || count < 0 || first + count > kMaxObjects) return false;
    for (long long i = 0; i < count; ++i) {
      if (at + rowLen > rows.size()) return false;
      Entry e;
      const uint64_t type = field(widths[0], 1), a = field(widths[1], 0), b = field(widths[2], 0);
      if (type == 1 || type == 2) {
        e.type = static_cast<uint8_t>(type);
        e.a = a;
        e.b = static_cast<uint32_t>(std::min<uint64_t>(b, UINT32_MAX));
      }
      if (xref_.size() >= kMaxEntries) return false;
      xref_.emplace(static_cast<int>(first + i), e);
    }
  }
  return true;
}

bool File::ObjectAt(size_t offset, int num, Obj& out, int* gen) {
  if (offset >= data_.size()) return false;
  size_t at = offset;
  long long n = 0, g = 0;
  if (!ReadInt(data_, at, n) || n != num || !ReadInt(data_, at, g) || !Keyword(data_, at, "obj")) return false;
  if (gen) *gen = static_cast<int>(g);
  return ParseObject(data_, at, out);
}

bool File::FromObjectStream(int stream, uint32_t index, int num, Obj& out) {
  auto cached = objStreams_.find(stream);
  if (cached == objStreams_.end()) {
    auto it = xref_.find(stream);
    Obj s;
    auto decoded = std::make_shared<ObjStream>();
    if (it == xref_.end() || it->second.type != 1 || !ObjectAt(static_cast<size_t>(it->second.a), stream, s, nullptr) ||
        s.type != Obj::Stream || !s.Get("Type") || !s.Get("Type")->IsName("ObjStm") ||
        !StreamData(s, data_, decoded->data))
      return false;
    decoded->n = s.Get("N") ? s.Get("N")->Int(-1) : -1;
    decoded->first = s.Get("First") ? s.Get("First")->Int(-1) : -1;
    if (decoded->n < 0 || decoded->n > 1'000'000 || decoded->first < 0 ||
        static_cast<size_t>(decoded->first) > decoded->data.size())
      return false;
    cached = objStreams_.emplace(stream, decoded).first;
  }
  const ObjStream& os = *cached->second;
  const std::string_view d = os.data;
  if (index >= static_cast<uint64_t>(os.n)) return false;
  // The header: a number and an offset (from First) for each object, in order.
  size_t at = 0;
  long long objNum = -1, offset = -1;
  for (uint32_t i = 0; i <= index; ++i)
    if (!ReadInt(d, at, objNum) || !ReadInt(d, at, offset)) return false;
  if (objNum != num || offset < 0) return false;
  size_t p = static_cast<size_t>(os.first) + static_cast<size_t>(offset);
  if (p >= d.size()) return false;
  return ParseObject(d, p, out) && out.type != Obj::Stream;
}

bool File::Get(int num, Obj& out, int* gen) {
  auto it = xref_.find(num);
  if (it == xref_.end() || it->second.type == 0) return false;
  if (it->second.type == 1) return ObjectAt(static_cast<size_t>(it->second.a), num, out, gen);
  if (gen) *gen = 0;
  if (it->second.a >= static_cast<uint64_t>(kMaxObjects)) return false;
  return FromObjectStream(static_cast<int>(it->second.a), it->second.b, num, out);
}

bool File::Resolve(const Obj& o, Obj& out) {
  if (o.type != Obj::Ref) {
    out = o;
    return true;
  }
  return Get(o.num, out);
}

int File::FirstPage() {
  const Obj* root = trailer_.Get("Root");
  Obj catalog;
  if (!root || root->type != Obj::Ref || !Get(root->num, catalog) || catalog.type != Obj::Dict) return -1;
  const Obj* pages = catalog.Get("Pages");
  if (!pages || pages->type != Obj::Ref) return -1;
  std::set<int> seen;
  const int first = FirstLeaf(pages->num, 0, seen);
  return first >= 0 ? first : -1;
}

int File::FirstLeaf(int num, int depth, std::set<int>& seen) {
  if (depth > 64 || seen.size() > 100'000 || !seen.insert(num).second) return -2;
  Obj node;
  if (!Get(num, node) || node.type != Obj::Dict) return -2;
  const Obj* kids = node.Get("Kids");
  if (!kids) return node.Get("Type") && node.Get("Type")->IsName("Page") ? num : -2;
  Obj list;
  if (!Resolve(*kids, list) || list.type != Obj::Array) return -2;
  for (const Obj& kid : list.items) {
    if (kid.type != Obj::Ref) return -2;
    const int leaf = FirstLeaf(kid.num, depth + 1, seen);
    if (leaf != -1) return leaf;  // a page, or a failure
  }
  return -1;
}

bool File::RawStream(const Obj& stream, std::string_view& out) {
  size_t len = stream.streamLen;
  if (const Obj* l = stream.Get("Length"); l && l->type == Obj::Ref) {
    Obj v;
    len = Get(l->num, v) && v.Int(-1) >= 0 ? static_cast<size_t>(v.Int()) : 0;
  }
  const size_t from = stream.streamAt;
  if (from > data_.size()) return false;
  if (len == 0 || from + len > data_.size() || len > kMaxStream) {
    const size_t end = data_.find("endstream", from);
    if (end == std::string_view::npos) return false;
    len = end - from;
    while (len > 0 && (data_[from + len - 1] == '\n' || data_[from + len - 1] == '\r')) --len;
  }
  out = data_.substr(from, len);
  return true;
}

bool File::ObjectText(int num, std::string_view& out) {
  auto it = xref_.find(num);
  if (it == xref_.end() || it->second.type != 1 || it->second.a >= data_.size()) return false;
  size_t at = static_cast<size_t>(it->second.a);
  long long n = 0, g = 0;
  if (!ReadInt(data_, at, n) || n != num || !ReadInt(data_, at, g) || !Keyword(data_, at, "obj")) return false;
  const size_t start = at;
  size_t end = at;
  Obj o;
  if (!ParseObject(data_, end, o)) return false;
  if (o.type == Obj::Stream) {
    std::string_view raw;
    if (!RawStream(o, raw)) return false;
    end = o.streamAt + raw.size();
  }
  const size_t close = data_.find("endobj", end);
  if (close == std::string_view::npos) return false;
  std::string_view text = data_.substr(start, close - start);
  while (!text.empty() && IsWhite(text.front())) text.remove_prefix(1);
  while (!text.empty() && IsWhite(text.back())) text.remove_suffix(1);
  out = text;
  return true;
}

std::vector<int> File::InUse() const {
  std::vector<int> nums;
  for (const auto& [num, e] : xref_)
    if (num > 0 && e.type != 0) nums.push_back(num);
  return nums;
}

namespace {

int HexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// A string's bytes, however it's written (ISO 32000-1, 7.3.4).
std::string StringBytes(const Obj& o) {
  std::string out;
  const std::string& t = o.text;
  if (o.type == Obj::HexString) {
    int hi = -1;
    for (char c : t) {
      const int v = HexValue(c);
      if (v < 0) continue;
      if (hi < 0) {
        hi = v;
      } else {
        out += static_cast<char>(hi * 16 + v);
        hi = -1;
      }
    }
    if (hi >= 0) out += static_cast<char>(hi * 16);
    return out;
  }
  for (size_t i = 0; i < t.size(); ++i) {
    char c = t[i];
    if (c == '\r') {  // an end of line in a literal string is a line feed
      if (i + 1 < t.size() && t[i + 1] == '\n') ++i;
      out += '\n';
      continue;
    }
    if (c != '\\' || i + 1 >= t.size()) {
      out += c;
      continue;
    }
    c = t[++i];
    switch (c) {
      case 'n': out += '\n'; break;
      case 'r': out += '\r'; break;
      case 't': out += '\t'; break;
      case 'b': out += '\b'; break;
      case 'f': out += '\f'; break;
      case '\r':  // a line continued
        if (i + 1 < t.size() && t[i + 1] == '\n') ++i;
        break;
      case '\n': break;
      default:
        if (c >= '0' && c <= '7') {
          int v = c - '0';
          for (int k = 0; k < 2 && i + 1 < t.size() && t[i + 1] >= '0' && t[i + 1] <= '7'; ++k) v = v * 8 + (t[++i] - '0');
          out += static_cast<char>(v & 0xFF);
        } else {
          out += c;  // the escaped parentheses and backslash, and any other: the character itself
        }
    }
  }
  return out;
}

// A name's bytes, #xx decoded.
std::string NameBytes(const std::string& t) {
  std::string out;
  for (size_t i = 0; i < t.size(); ++i) {
    if (t[i] == '#' && i + 2 < t.size() && HexValue(t[i + 1]) >= 0 && HexValue(t[i + 2]) >= 0) {
      out += static_cast<char>(HexValue(t[i + 1]) * 16 + HexValue(t[i + 2]));
      i += 2;
    } else {
      out += t[i];
    }
  }
  return out;
}

bool SameValue(const Obj& a, const Obj& b, bool streamDict) {
  const bool aString = a.type == Obj::String || a.type == Obj::HexString;
  const bool bString = b.type == Obj::String || b.type == Obj::HexString;
  if (aString || bString) return aString && bString && StringBytes(a) == StringBytes(b);
  if (a.type != b.type) return false;
  switch (a.type) {
    case Obj::Null: return true;
    case Obj::Bool: return a.text == b.text;
    case Obj::Number: {
      const double x = std::strtod(a.text.c_str(), nullptr), y = std::strtod(b.text.c_str(), nullptr);
      return std::fabs(x - y) <= 1e-4 * std::max(1.0, std::fabs(x));
    }
    case Obj::Name: return NameBytes(a.text) == NameBytes(b.text);
    case Obj::Ref: return a.num == b.num && a.gen == b.gen;
    case Obj::Array:
      if (a.items.size() != b.items.size()) return false;
      for (size_t i = 0; i < a.items.size(); ++i)
        if (!SameValue(a.items[i], b.items[i], false)) return false;
      return true;
    case Obj::Dict:
    case Obj::Stream: {
      // A stream's /Length, /Filter and /DecodeParms say how its data is stored, not what it is.
      auto skip = [streamDict](const std::string& key) {
        if (!streamDict) return false;
        const std::string k = NameBytes(key);
        return k == "Length" || k == "Filter" || k == "DecodeParms";
      };
      size_t mine = 0, theirs = 0;
      for (const auto& [key, value] : a.dict) {
        if (skip(key)) continue;
        ++mine;
        const std::string name = NameBytes(key);
        const Obj* other = nullptr;
        for (const auto& [k, v] : b.dict)
          if (NameBytes(k) == name) other = &v;
        if (!other || !SameValue(value, *other, false)) return false;
      }
      for (const auto& entry : b.dict)
        if (!skip(entry.first)) ++theirs;
      return mine == theirs;
    }
    default: return false;
  }
}

}  // namespace

bool SameObject(File& fa, const Obj& a, File& fb, const Obj& b) {
  if ((a.type == Obj::Stream) != (b.type == Obj::Stream)) return false;
  if (a.type != Obj::Stream) return SameValue(a, b, false);
  if (!SameValue(a, b, true)) return false;
  std::string_view ra, rb;
  if (!fa.RawStream(a, ra) || !fb.RawStream(b, rb)) return false;
  if (ra == rb && SameValue(a, b, false)) return true;  // stored the same way
  std::string da, db;
  return fa.DecodedStream(a, da) && fb.DecodedStream(b, db) && da == db;
}

bool AppendUpdate(std::string_view file, const File& f, const std::vector<Update>& objects, int size, std::string& out,
                  std::vector<size_t>* at) {
  std::string inc;
  if (!file.empty() && file.back() != '\n' && file.back() != '\r') inc += '\n';
  const size_t base = file.size();
  std::vector<std::pair<int, std::pair<size_t, int>>> written;  // number, (offset, generation)
  if (at) at->clear();
  for (const Update& u : objects) {
    written.push_back({u.num, {base + inc.size(), u.gen}});
    if (at) at->push_back(base + inc.size());
    inc += std::to_string(u.num) + " " + std::to_string(u.gen) + " obj\n" + u.text + "\nendobj\n";
  }
  std::string trailerKeys;
  if (const Obj* root = f.Trailer().Get("Root")) trailerKeys += " /Root " + Write(*root);
  for (const char* key : {"Info", "ID"})
    if (const Obj* v = f.Trailer().Get(key)) trailerKeys += std::string(" /") + key + " " + Write(*v);
  trailerKeys += " /Prev " + std::to_string(f.LastXref());
  const size_t xrefAt = base + inc.size();
  std::sort(written.begin(), written.end());
  if (!f.LastXrefIsStream()) {
    inc += "xref\n";
    for (size_t i = 0; i < written.size();) {
      size_t j = i;
      while (j + 1 < written.size() && written[j + 1].first == written[j].first + 1) ++j;
      inc += std::to_string(written[i].first) + " " + std::to_string(j - i + 1) + "\n";
      for (size_t k = i; k <= j; ++k) {
        char e[24];
        snprintf(e, sizeof e, "%010llu %05d n\r\n", static_cast<unsigned long long>(written[k].second.first),
                 written[k].second.second);
        inc += e;
      }
      i = j + 1;
    }
    inc += "trailer\n<</Size " + std::to_string(size) + trailerKeys + ">>\n";
  } else {
    // The file's last cross-reference is a stream: the update's is one too (not compressed).
    const int xrefNum = size++;
    written.push_back({xrefNum, {xrefAt, 0}});
    std::string rows, index;
    for (size_t i = 0; i < written.size();) {
      size_t j = i;
      while (j + 1 < written.size() && written[j + 1].first == written[j].first + 1) ++j;
      index += (index.empty() ? "" : " ") + std::to_string(written[i].first) + " " + std::to_string(j - i + 1);
      for (size_t k = i; k <= j; ++k) {
        const uint64_t off = written[k].second.first;
        const auto gen = static_cast<uint16_t>(written[k].second.second);
        rows += '\x01';
        for (int shift : {24, 16, 8, 0}) rows += static_cast<char>((off >> shift) & 0xFF);
        rows += static_cast<char>(gen >> 8);
        rows += static_cast<char>(gen & 0xFF);
      }
      i = j + 1;
    }
    if (xrefAt > 0xFFFFFFFFull) return false;
    inc += std::to_string(xrefNum) + " 0 obj\n<</Type /XRef /Size " + std::to_string(size) + " /W [1 4 2] /Index [" +
           index + "]" + trailerKeys + " /Length " + std::to_string(rows.size()) + ">>\nstream\n" + rows +
           "\nendstream\nendobj\n";
  }
  inc += "startxref\n" + std::to_string(xrefAt) + "\n%%EOF\n";
  out.reserve(file.size() + inc.size());
  out.assign(file);
  out += inc;
  return true;
}

}  // namespace pdfobj
