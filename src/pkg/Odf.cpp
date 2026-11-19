#include "pkg/Odf.h"

#include <cctype>
#include <cmath>
#include <map>
#include <vector>

#include "pkg/XmlDom.h"
#include "pkg/Zip.h"

namespace fw::pkg::odf {

Status LoadPart(const ZipReader& zip, const char* name, XmlDom& dom, const Limits& limits, bool required,
                std::string_view missing) {
  const ZipEntry* e = zip.Find(name);
  if (!e) return required ? Status::Fail(Status::Code::Damaged, std::string(missing)) : Status{};
  std::vector<uint8_t> bytes;
  if (Status st = zip.Read(*e, bytes); !st.Ok()) return st;
  if (Status st = dom.Parse(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), limits, name);
      !st.Ok())
    return st;
  // ODF's usual prefixes, whatever the file declares (pugixml keeps names as written).
  static const std::pair<std::string_view, std::string_view> kCanon[] = {
      {"urn:oasis:names:tc:opendocument:xmlns:office:1.0", "office"},
      {"urn:oasis:names:tc:opendocument:xmlns:style:1.0", "style"},
      {"urn:oasis:names:tc:opendocument:xmlns:text:1.0", "text"},
      {"urn:oasis:names:tc:opendocument:xmlns:table:1.0", "table"},
      {"urn:oasis:names:tc:opendocument:xmlns:drawing:1.0", "draw"},
      {"urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0", "fo"},
      {"urn:oasis:names:tc:opendocument:xmlns:svg-compatible:1.0", "svg"},
      {"urn:oasis:names:tc:opendocument:xmlns:datastyle:1.0", "number"},
      {"urn:oasis:names:tc:opendocument:xmlns:config:1.0", "config"},
      {"http://openoffice.org/2009/table", "tableooo"},
      {"urn:org:documentfoundation:names:experimental:office:xmlns:loext:1.0", "loext"},
      {"http://www.w3.org/1999/xlink", "xlink"}};
  std::map<std::string, std::string> renames;
  for (pugi::xml_attribute a : dom.Root().attributes()) {
    const std::string_view n = a.name();
    if (!n.starts_with("xmlns:")) continue;
    for (const auto& [uri, prefix] : kCanon)
      if (uri == a.value() && n.substr(6) != prefix) renames[std::string(n.substr(6))] = std::string(prefix);
  }
  if (renames.empty()) return {};
  auto rename = [&](auto&& item) {
    const std::string_view n = item.name();
    const size_t colon = n.find(':');
    if (colon == std::string_view::npos || n.starts_with("xmlns")) return;
    if (auto it = renames.find(std::string(n.substr(0, colon))); it != renames.end())
      item.set_name((it->second + std::string(n.substr(colon))).c_str());
  };
  for (pugi::xml_node n = dom.Root(); n;) {
    if (n.type() == pugi::node_element) {
      rename(n);
      for (pugi::xml_attribute a : n.attributes()) rename(a);
    }
    if (n.first_child()) {
      n = n.first_child();
      continue;
    }
    while (n && !n.next_sibling() && n != dom.Root()) n = n.parent();
    if (!n || n == dom.Root()) break;
    n = n.next_sibling();
  }
  return {};
}

std::string MimeType(const ZipReader& zip) {
  const ZipEntry* m = zip.Find("mimetype");
  std::vector<uint8_t> type;
  if (!m || m->uncompressedSize > 256 || !zip.Read(*m, type).Ok()) return {};
  return std::string(type.begin(), type.end());
}

std::optional<double> Points(std::string_view v) {
  size_t i = 0;
  while (i < v.size() && (std::isdigit(static_cast<unsigned char>(v[i])) || v[i] == '.' || v[i] == '-' || v[i] == '+')) ++i;
  if (i == 0) return std::nullopt;
  double n = 0;
  try {
    n = std::stod(std::string(v.substr(0, i)));
  } catch (...) {
    return std::nullopt;
  }
  if (!std::isfinite(n) || std::abs(n) > 1e6) return std::nullopt;
  const std::string_view unit = v.substr(i);
  if (unit == "pt") return n;
  if (unit == "cm") return n * 72 / 2.54;
  if (unit == "mm") return n * 72 / 25.4;
  if (unit == "in" || unit == "inch") return n * 72;
  if (unit == "pc") return n * 12;
  if (unit == "px") return n * 0.75;
  return std::nullopt;
}

std::string Color(std::string_view v) {
  if (v.size() != 7 || v[0] != '#') return {};
  std::string out;
  for (char c : v.substr(1)) {
    if (!std::isxdigit(static_cast<unsigned char>(c))) return {};
    out += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  return out;
}

std::string DisplayName(std::string_view name) {
  std::string out;
  for (size_t i = 0; i < name.size(); ++i) {
    if (name[i] == '_' && i + 3 < name.size() && name[i + 3] == '_' && std::isxdigit(static_cast<unsigned char>(name[i + 1])) &&
        std::isxdigit(static_cast<unsigned char>(name[i + 2]))) {
      const int c = std::stoi(std::string(name.substr(i + 1, 2)), nullptr, 16);
      if (c >= 0x20 && c < 0x7F) {
        out += static_cast<char>(c);
        i += 3;
        continue;
      }
    }
    out += name[i];
  }
  return out;
}

std::string Escape(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      default: out += c;
    }
  }
  return out;
}

std::optional<Border> ParseBorder(std::string_view v) {
  if (v.empty()) return std::nullopt;
  Border b;
  if (v == "none" || v == "hidden") {
    b.none = true;
    return b;
  }
  size_t at = 0;
  while (at < v.size()) {
    size_t end = v.find(' ', at);
    if (end == std::string_view::npos) end = v.size();
    const std::string_view word = v.substr(at, end - at);
    if (auto c = Color(word); !c.empty()) b.color = c;
    else if (auto p = Points(word)) b.width = *p;
    else if (word == "none" || word == "hidden") b.none = true;
    else if (!word.empty() && std::isalpha(static_cast<unsigned char>(word[0]))) b.style = std::string(word);
    at = end + 1;
  }
  if (b.width <= 0) b.none = true;
  return b;
}

}  // namespace fw::pkg::odf
