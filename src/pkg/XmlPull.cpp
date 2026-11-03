#include "pkg/XmlPull.h"

#include <algorithm>
#include <unordered_set>

namespace fw::pkg {
namespace {

bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

bool IsNameStart(unsigned char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == ':' || c >= 0x80;
}

bool IsNameChar(unsigned char c) {
  return IsNameStart(c) || (c >= '0' && c <= '9') || c == '-' || c == '.';
}

bool IsXmlChar(uint32_t cp) {
  return cp == 0x9 || cp == 0xA || cp == 0xD || (cp >= 0x20 && cp <= 0xD7FF) || (cp >= 0xE000 && cp <= 0xFFFD) ||
         (cp >= 0x10000 && cp <= 0x10FFFF);
}

void AppendUtf8(std::string& out, uint32_t cp) {
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

// UTF-16 (either byte order) to UTF-8. False on an odd length or unpaired surrogates.
bool Utf16ToUtf8(std::string_view in, bool bigEndian, std::string& out) {
  if (in.size() % 2) return false;
  out.clear();
  out.reserve(in.size());
  auto unit = [&](size_t i) {
    auto b0 = static_cast<unsigned char>(in[i]), b1 = static_cast<unsigned char>(in[i + 1]);
    return static_cast<uint32_t>(bigEndian ? (b0 << 8) | b1 : (b1 << 8) | b0);
  };
  for (size_t i = 0; i < in.size(); i += 2) {
    uint32_t cp = unit(i);
    if (cp >= 0xD800 && cp <= 0xDBFF) {
      if (i + 3 >= in.size()) return false;
      uint32_t lo = unit(i + 2);
      if (lo < 0xDC00 || lo > 0xDFFF) return false;
      cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
      i += 2;
    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
      return false;
    }
    AppendUtf8(out, cp);
  }
  return true;
}

bool EqualsNoCaseAscii(std::string_view a, std::string_view b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
           return (x >= 'A' && x <= 'Z' ? x + 32 : x) == (y >= 'A' && y <= 'Z' ? y + 32 : y);
         });
}

// A name as XML Namespaces allows it: one colon at most, and not at either end.
bool IsQName(std::string_view name) {
  size_t colon = name.find(':');
  if (colon == std::string_view::npos) return !name.empty();
  return colon != 0 && colon + 1 < name.size() && name.find(':', colon + 1) == std::string_view::npos;
}

// The offset of the first byte that isn't part of a valid UTF-8 sequence for an XML character
// (control characters other than tab, LF and CR; surrogates; U+FFFE and U+FFFF), or npos.
size_t FindInvalidChar(std::string_view s) {
  const auto* p = reinterpret_cast<const unsigned char*>(s.data());
  const size_t n = s.size();
  size_t i = 0;
  while (i < n) {
    unsigned char c = p[i];
    if (c < 0x80) {
      if (c < 0x20 && c != '\t' && c != '\n' && c != '\r') return i;
      ++i;
      continue;
    }
    size_t len = c >= 0xC2 && c <= 0xDF ? 2 : (c & 0xF0) == 0xE0 ? 3 : c >= 0xF0 && c <= 0xF4 ? 4 : 0;
    if (len == 0 || i + len > n) return i;
    uint32_t cp = c & (len == 2 ? 0x1F : len == 3 ? 0x0F : 0x07);
    for (size_t k = 1; k < len; ++k) {
      if ((p[i + k] & 0xC0) != 0x80) return i;
      cp = (cp << 6) | (p[i + k] & 0x3F);
    }
    if ((len == 3 && cp < 0x800) || (len == 4 && (cp < 0x10000 || cp > 0x10FFFF)) || !IsXmlChar(cp)) return i;
    i += len;
  }
  return std::string_view::npos;
}

// Line ends as XML normalizes them: CR LF and a lone CR become LF.
std::string_view NormalizeEol(std::string_view raw, std::string& buf) {
  if (raw.find('\r') == std::string_view::npos) return raw;
  buf.clear();
  for (size_t i = 0; i < raw.size(); ++i) {
    if (raw[i] == '\r') {
      if (i + 1 < raw.size() && raw[i + 1] == '\n') ++i;
      buf += '\n';
    } else {
      buf += raw[i];
    }
  }
  return buf;
}

}  // namespace

std::string_view QNamePrefix(std::string_view qname) {
  size_t colon = qname.find(':');
  return colon == std::string_view::npos ? std::string_view() : qname.substr(0, colon);
}

std::string_view QNameLocal(std::string_view qname) {
  size_t colon = qname.find(':');
  return colon == std::string_view::npos ? qname : qname.substr(colon + 1);
}

XmlPull::XmlPull(std::string_view xml, const Limits& limits, std::string_view label)
    : limits_(limits), label_(label.empty() ? "This XML" : "Part " + std::string(label)) {
  auto b = [&](size_t i) { return i < xml.size() ? static_cast<unsigned char>(xml[i]) : 0x100u; };
  bool utf16 = false, bigEndian = false;
  size_t skip = 0;
  if (b(0) == 0xEF && b(1) == 0xBB && b(2) == 0xBF) {
    skip = 3;
  } else if (b(0) == 0xFF && b(1) == 0xFE) {
    utf16 = true, skip = 2;
  } else if (b(0) == 0xFE && b(1) == 0xFF) {
    utf16 = true, bigEndian = true, skip = 2;
  } else if (b(0) == '<' && b(1) == 0 && b(2) == '?' && b(3) == 0) {
    utf16 = true;
  } else if (b(0) == 0 && b(1) == '<' && b(2) == 0 && b(3) == '?') {
    utf16 = true, bigEndian = true;
  }
  if (utf16) {
    if (!Utf16ToUtf8(xml.substr(skip), bigEndian, owned_)) {
      Damaged("its UTF-16 text is broken");
      return;
    }
    src_ = owned_;
  } else {
    src_ = xml.substr(skip);
  }
  // One pass over everything, so no later step meets a character XML forbids (a NUL would
  // end the text early for the DOM) or broken UTF-8.
  size_t bad = FindInvalidChar(src_);
  if (bad != std::string_view::npos) {
    pos_ = bad;
    Damaged("it contains a character that isn't allowed in XML");
  }
}

XmlPull::Event XmlPull::Fail(Status::Code code, std::string_view what) {
  status_ = Status::Fail(code, label_ + std::string(what));
  event_ = Event::Error;
  return event_;
}

XmlPull::Event XmlPull::Damaged(std::string_view why) {
  size_t line = 1 + static_cast<size_t>(std::count(src_.begin(), src_.begin() + std::min(pos_, src_.size()), '\n'));
  return Fail(Status::Code::Damaged, " is damaged near line " + std::to_string(line) + ": " + std::string(why) + ".");
}

std::string_view XmlPull::Prefix() const { return QNamePrefix(qname_); }
std::string_view XmlPull::LocalName() const { return QNameLocal(qname_); }

const XmlPull::Attribute* XmlPull::FindAttribute(std::string_view nsUri, std::string_view local) const {
  for (const auto& a : attrs_)
    if (a.local == local && a.nsUri == nsUri) return &a;
  return nullptr;
}

bool XmlPull::IsWhitespace() const {
  return std::all_of(text_.begin(), text_.end(), IsSpace);
}

std::optional<std::string_view> XmlPull::LookupNamespace(std::string_view prefix) const {
  if (prefix == "xml") return kXmlNamespace;
  if (prefix == "xmlns") return kXmlnsNamespace;
  for (size_t i = bindings_.size(); i-- > 0;)
    if (bindings_[i].prefix == prefix) return bindings_[i].uri;
  if (prefix.empty()) return std::string_view();
  return std::nullopt;
}

bool XmlPull::SkipSpace() {
  size_t start = pos_;
  while (pos_ < src_.size() && IsSpace(src_[pos_])) ++pos_;
  return pos_ != start;
}

std::string_view XmlPull::ParseName() {
  size_t start = pos_;
  if (pos_ >= src_.size() || !IsNameStart(static_cast<unsigned char>(src_[pos_]))) return {};
  while (pos_ < src_.size() && IsNameChar(static_cast<unsigned char>(src_[pos_]))) ++pos_;
  return src_.substr(start, pos_ - start);
}

// Decodes entities and normalizes line ends (and, in attributes, whitespace). `out` is `raw`
// itself when nothing needs decoding, otherwise it points into `buf`.
bool XmlPull::Decode(std::string_view raw, bool attribute, std::string& buf, std::string_view& out) {
  bool plain = raw.find('&') == std::string_view::npos && raw.find('\r') == std::string_view::npos &&
               (!attribute || (raw.find('\n') == std::string_view::npos && raw.find('\t') == std::string_view::npos));
  if (plain) {
    out = raw;
    return true;
  }
  buf.clear();
  buf.reserve(raw.size());
  for (size_t i = 0; i < raw.size(); ++i) {
    char c = raw[i];
    if (c == '\r') {
      if (i + 1 < raw.size() && raw[i + 1] == '\n') ++i;
      buf += attribute ? ' ' : '\n';
    } else if (attribute && (c == '\n' || c == '\t')) {
      buf += ' ';
    } else if (c != '&') {
      buf += c;
    } else {
      size_t semi = raw.find(';', i);
      if (semi == std::string_view::npos || semi - i > 12) return false;
      std::string_view ent = raw.substr(i + 1, semi - i - 1);
      if (ent == "lt") buf += '<';
      else if (ent == "gt") buf += '>';
      else if (ent == "amp") buf += '&';
      else if (ent == "apos") buf += '\'';
      else if (ent == "quot") buf += '"';
      else if (ent.size() >= 2 && ent[0] == '#') {
        bool hex = ent[1] == 'x';
        std::string_view digits = ent.substr(hex ? 2 : 1);
        if (digits.empty()) return false;
        uint32_t cp = 0;
        for (char d : digits) {
          int v = d >= '0' && d <= '9' ? d - '0'
                  : hex && d >= 'a' && d <= 'f' ? d - 'a' + 10
                  : hex && d >= 'A' && d <= 'F' ? d - 'A' + 10
                                                : -1;
          if (v < 0) return false;
          cp = cp * (hex ? 16 : 10) + static_cast<uint32_t>(v);
          if (cp > 0x10FFFF) return false;
        }
        if (!IsXmlChar(cp)) return false;
        AppendUtf8(buf, cp);
      } else {
        return false;
      }
      i = semi;
    }
  }
  out = buf;
  return true;
}

void XmlPull::PopElement() {
  stack_.pop_back();
  size_t keep = scopes_.back();
  scopes_.pop_back();
  while (bindings_.size() > keep) {
    if (bindings_.back().owned) ownedUris_.pop_back();
    bindings_.pop_back();
  }
}

XmlPull::Event XmlPull::Next() {
  if (event_ == Event::Error || event_ == Event::EndDocument) return event_;
  if (pendingEnd_) {
    pendingEnd_ = false;
    popAfter_ = true;
    return event_ = Event::EndElement;
  }
  if (popAfter_) {
    PopElement();
    popAfter_ = false;
  }
  for (;;) {
    if (pos_ >= src_.size()) {
      if (!stack_.empty() || !seenRoot_) return Damaged("it ends unexpectedly");
      return event_ = Event::EndDocument;
    }
    if (src_[pos_] == '<') {
      Event e = ParseMarkup();
      if (e == Event::EndDocument) continue;  // the XML declaration: nothing to report
      return e;
    }
    size_t end = std::min(src_.find('<', pos_), src_.size());
    std::string_view raw = src_.substr(pos_, end - pos_);
    if (stack_.empty()) {
      if (!std::all_of(raw.begin(), raw.end(), IsSpace)) return Damaged("there is text outside the root element");
      pos_ = end;
      continue;
    }
    if (raw.size() > limits_.maxStringLength)
      return Fail(Status::Code::TooLarge, " contains a piece of text longer than " + FormatBytes(limits_.maxStringLength) +
                                              ", which is more than Featherweight allows.");
    if (!Decode(raw, false, textBuf_, text_)) return Damaged("it uses an entity that isn't defined");
    pos_ = end;
    return event_ = Event::Text;
  }
}

XmlPull::Event XmlPull::ParseMarkup() {
  std::string_view rest = src_.substr(pos_);
  auto tooLong = [&] {
    return Fail(Status::Code::TooLarge, " contains a piece of text longer than " + FormatBytes(limits_.maxStringLength) +
                                            ", which is more than Featherweight allows.");
  };
  if (rest.starts_with("<?")) {
    size_t end = rest.find("?>");
    if (end == std::string_view::npos) return Damaged("a processing instruction isn't closed");
    if (end > limits_.maxStringLength) return tooLong();
    size_t start = pos_;
    pos_ += 2;
    std::string_view target = ParseName();
    if (target.empty()) return Damaged("a processing instruction has no name");
    std::string_view body = src_.substr(pos_, start + end - pos_);
    pos_ = start + end + 2;
    if (EqualsNoCaseAscii(target, "xml")) {
      if (start != 0 || target != "xml") return Damaged("the XML declaration is in the wrong place");
      if (!ParseDeclaration(body)) return event_;
      return Event::EndDocument;  // handled; Next() reads on
    }
    if (!body.empty() && !IsSpace(body.front())) return Damaged("a processing instruction is malformed");
    while (!body.empty() && IsSpace(body.front())) body.remove_prefix(1);
    qname_ = target;
    nsUri_ = {};
    text_ = NormalizeEol(body, textBuf_);
    return event_ = Event::ProcessingInstruction;
  }
  if (rest.starts_with("<!--")) {
    size_t end = rest.find("-->", 4);
    if (end == std::string_view::npos) return Damaged("a comment isn't closed");
    if (end - 4 > limits_.maxStringLength) return tooLong();
    text_ = NormalizeEol(rest.substr(4, end - 4), textBuf_);  // comments have no entities
    pos_ += end + 3;
    return event_ = Event::Comment;
  }
  if (rest.starts_with("<![CDATA[")) {
    if (stack_.empty()) return Damaged("there is text outside the root element");
    size_t end = rest.find("]]>", 9);
    if (end == std::string_view::npos) return Damaged("a CDATA section isn't closed");
    if (end - 9 > limits_.maxStringLength) return tooLong();
    text_ = NormalizeEol(rest.substr(9, end - 9), textBuf_);
    pos_ += end + 3;
    return event_ = Event::CData;
  }
  if (rest.starts_with("<!DOCTYPE") || rest.starts_with("<!doctype"))
    return Fail(Status::Code::Rejected,
                " contains a document type definition (DTD). Office files never do, so Featherweight won't open it.");
  if (rest.starts_with("<!")) return Damaged("it contains unexpected markup");
  if (rest.starts_with("</")) return ParseEndTag();
  return ParseStartTag();
}

bool XmlPull::ParseDeclaration(std::string_view body) {
  // Pseudo-attributes, written like attributes: version, encoding, standalone. Only the
  // encoding matters: Office parts are UTF-8 or UTF-16, which were handled above.
  size_t i = 0;
  auto space = [&] {
    size_t start = i;
    while (i < body.size() && IsSpace(body[i])) ++i;
    return i != start;
  };
  for (;;) {
    bool separated = space();
    if (i >= body.size()) return true;
    if (!separated) break;
    size_t nameStart = i;
    if (i < body.size() && IsNameStart(static_cast<unsigned char>(body[i]))) ++i;
    while (i > nameStart && i < body.size() && IsNameChar(static_cast<unsigned char>(body[i]))) ++i;
    std::string_view name = body.substr(nameStart, i - nameStart);
    space();
    if (name.empty() || i >= body.size() || body[i] != '=') break;
    ++i;
    space();
    if (i >= body.size() || (body[i] != '"' && body[i] != '\'')) break;
    size_t end = body.find(body[i], i + 1);
    if (end == std::string_view::npos) break;
    std::string_view value = body.substr(i + 1, end - i - 1);
    i = end + 1;
    std::string_view decoded;
    if (value.find('<') != std::string_view::npos || !Decode(value, true, textBuf_, decoded)) break;
    if (name != "encoding") continue;
    bool known = false;
    for (std::string_view ok : {"utf-8", "utf8", "utf-16", "utf-16le", "utf-16be", "us-ascii", "ascii"})
      known = known || EqualsNoCaseAscii(value, ok);
    if (!known) {
      Fail(Status::Code::Unsupported,
           " uses the " + std::string(value) + " text encoding, which Office files don't use.");
      return false;
    }
  }
  Damaged("the XML declaration is malformed");
  return false;
}

XmlPull::Event XmlPull::ParseEndTag() {
  pos_ += 2;
  std::string_view name = ParseName();
  SkipSpace();
  if (name.empty() || pos_ >= src_.size() || src_[pos_] != '>') return Damaged("an end tag is malformed");
  ++pos_;
  if (stack_.empty() || stack_.back() != name) return Damaged("the tags don't match");
  qname_ = name;
  nsUri_ = *LookupNamespace(QNamePrefix(name));  // resolved when the element started
  attrs_.clear();
  empty_ = false;
  popAfter_ = true;
  return event_ = Event::EndElement;
}

XmlPull::Event XmlPull::ParseStartTag() {
  if (seenRoot_ && stack_.empty()) return Damaged("there is more than one root element");
  ++pos_;
  std::string_view name = ParseName();
  if (!IsQName(name)) return Damaged("a tag is malformed");
  if (name.size() > limits_.maxStringLength)
    return Fail(Status::Code::TooLarge, " contains a name longer than " + FormatBytes(limits_.maxStringLength) +
                                            ", which is more than Featherweight allows.");

  // Raw attributes first; values are decoded once their number is known, so the decoded
  // strings never move while views into them exist.
  struct Raw {
    std::string_view name, value;
  };
  std::vector<Raw> raws;
  bool isEmpty = false;
  for (;;) {
    bool space = SkipSpace();
    if (pos_ >= src_.size()) return Damaged("a tag isn't closed");
    if (src_[pos_] == '/') {
      if (pos_ + 1 >= src_.size() || src_[pos_ + 1] != '>') return Damaged("a tag is malformed");
      pos_ += 2;
      isEmpty = true;
      break;
    }
    if (src_[pos_] == '>') {
      ++pos_;
      break;
    }
    if (!space) return Damaged("a tag is malformed");
    std::string_view aname = ParseName();
    if (!IsQName(aname)) return Damaged("an attribute is malformed");
    SkipSpace();
    if (pos_ >= src_.size() || src_[pos_] != '=') return Damaged("an attribute has no value");
    ++pos_;
    SkipSpace();
    if (pos_ >= src_.size() || (src_[pos_] != '"' && src_[pos_] != '\'')) return Damaged("an attribute value isn't quoted");
    char quote = src_[pos_++];
    size_t end = src_.find(quote, pos_);
    if (end == std::string_view::npos) return Damaged("an attribute value isn't closed");
    std::string_view value = src_.substr(pos_, end - pos_);
    if (value.find('<') != std::string_view::npos) return Damaged("an attribute value contains '<'");
    if (value.size() > limits_.maxStringLength || aname.size() > limits_.maxStringLength)
      return Fail(Status::Code::TooLarge, " contains an attribute longer than " + FormatBytes(limits_.maxStringLength) +
                                              ", which is more than Featherweight allows.");
    pos_ = end + 1;
    raws.push_back({aname, value});
    if (raws.size() > limits_.maxAttributes)
      return Fail(Status::Code::TooLarge, " has an element with more than " + std::to_string(limits_.maxAttributes) +
                                              " attributes, which is more than Featherweight allows.");
  }
  if (stack_.size() + 1 > limits_.maxDepth)
    return Fail(Status::Code::TooLarge, " is nested more than " + std::to_string(limits_.maxDepth) +
                                            " levels deep, which is more than Featherweight allows.");

  attrs_.clear();
  if (attrBuf_.size() < raws.size()) attrBuf_.resize(raws.size());
  for (size_t i = 0; i < raws.size(); ++i) {
    Attribute a;
    a.qname = raws[i].name;
    a.prefix = QNamePrefix(a.qname);
    a.local = QNameLocal(a.qname);
    if (!Decode(raws[i].value, true, attrBuf_[i], a.value)) return Damaged("it uses an entity that isn't defined");
    attrs_.push_back(a);
  }

  // Namespace declarations take effect on the element that carries them.
  scopes_.push_back(bindings_.size());
  stack_.push_back(name);
  for (auto& a : attrs_) {
    bool isDefault = a.qname == "xmlns";
    if (!isDefault && a.prefix != "xmlns") continue;
    Binding bind;
    bind.prefix = isDefault ? std::string_view() : a.local;
    if (!isDefault && a.value.empty()) return Damaged("a namespace prefix is undeclared");
    if (a.value.data() >= src_.data() && a.value.data() < src_.data() + src_.size()) {
      bind.uri = a.value;
    } else {
      ownedUris_.push_back(std::make_unique<std::string>(a.value));
      bind.uri = *ownedUris_.back();
      bind.owned = true;
    }
    bindings_.push_back(bind);
  }
  auto elementNs = LookupNamespace(QNamePrefix(name));
  if (!elementNs) return Damaged("it uses an undeclared namespace prefix");
  for (auto& a : attrs_) {
    if (a.qname == "xmlns" || a.prefix == "xmlns") {
      a.nsUri = kXmlnsNamespace;
    } else if (!a.prefix.empty()) {
      auto ns = LookupNamespace(a.prefix);
      if (!ns) return Damaged("it uses an undeclared namespace prefix");
      a.nsUri = *ns;
    }
  }
  // No attribute may appear twice, by name or by namespace and local name.
  if (attrs_.size() > 1) {
    if (attrs_.size() <= 8) {
      for (size_t i = 0; i < attrs_.size(); ++i)
        for (size_t j = i + 1; j < attrs_.size(); ++j)
          if (attrs_[i].qname == attrs_[j].qname || (attrs_[i].local == attrs_[j].local && attrs_[i].nsUri == attrs_[j].nsUri))
            return Damaged("an attribute appears twice");
    } else {
      std::unordered_set<std::string> seen;
      for (const auto& a : attrs_) {
        std::string key(a.nsUri);
        key += '\0';
        key += a.local;
        if (!seen.insert(std::move(key)).second) return Damaged("an attribute appears twice");
      }
    }
  }

  qname_ = name;
  nsUri_ = *elementNs;
  empty_ = isEmpty;
  pendingEnd_ = isEmpty;
  seenRoot_ = true;
  return event_ = Event::StartElement;
}

Status XmlPull::Check(std::string_view xml, const Limits& limits, std::string_view label) {
  XmlPull pull(xml, limits, label);
  for (;;) {
    Event e = pull.Next();
    if (e == Event::EndDocument) return Status{};
    if (e == Event::Error) return pull.GetStatus();
  }
}

}  // namespace fw::pkg
