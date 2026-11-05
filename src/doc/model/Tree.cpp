#include "doc/model/Tree.h"

#include <charconv>
#include <cmath>

namespace fw::doc {

StrPool::StrPool() { strings_.emplace_back(); }

StrId StrPool::Intern(std::string_view s) {
  if (s.empty()) return 0;
  auto it = index_.find(s);
  if (it != index_.end()) return it->second;
  auto id = static_cast<StrId>(strings_.size());
  strings_.emplace_back(s);
  bytes_ += sizeof(std::string) + (s.size() > 15 ? s.size() + 1 : 0) + 48;  // the string and its map entry
  index_.emplace(std::string_view(strings_.back()), id);
  return id;
}

ElemId Tree::Add(ElemDefId def, ElemId parent) {
  Elem e;
  e.def = def;
  e.parent = parent;
  elems.push_back(e);
  return static_cast<ElemId>(elems.size() - 1);
}

size_t Tree::BytesUsed() const {
  return elems.capacity() * sizeof(Elem) + attrs.capacity() * sizeof(AttrVal) + strings.BytesUsed();
}

uint32_t Node::NameHash() const { return Schema::Get().Elem(Raw().def).name; }
std::string_view Node::LocalName() const { return tree_ ? Schema::Get().Elem(Raw().def).local : std::string_view(); }
Ns Node::Namespace() const { return tree_ ? Schema::Get().Elem(Raw().def).ns : Ns::None; }
TypeId Node::Type() const { return tree_ ? Schema::Get().Elem(Raw().def).type : kNoDef; }
Role Node::GetRole() const { return tree_ ? Schema::Get().Type(Type()).role : Role::Plain; }

Node Node::Child(uint32_t name) const {
  if (!tree_) return {};
  const Schema& s = Schema::Get();
  for (ElemId c = Raw().first; c != kNoElem; c = tree_->elems[c].next)
    if (s.Elem(tree_->elems[c].def).name == name) return Node(tree_, c);
  return {};
}

Node Node::Path(std::initializer_list<uint32_t> names) const {
  Node n = *this;
  for (uint32_t name : names) n = n.Child(name);
  return n;
}

std::optional<std::string_view> Node::Attr(uint32_t name) const {
  if (!tree_) return std::nullopt;
  const Elem& e = Raw();
  const Schema& s = Schema::Get();
  for (uint32_t i = 0; i < e.attrCount; ++i) {
    const AttrVal& a = tree_->attrs[e.attrs + i];
    if (s.Attr(a.def).name == name) return tree_->strings.Get(a.value);
  }
  return std::nullopt;
}

std::string_view Node::AttrOr(uint32_t name, std::string_view fallback) const {
  auto v = Attr(name);
  return v ? *v : fallback;
}

const AttrDef& Node::AttrDefAt(size_t i) const { return Schema::Get().Attr(tree_->attrs[Raw().attrs + i].def); }
std::string_view Node::AttrValueAt(size_t i) const { return tree_->strings.Get(tree_->attrs[Raw().attrs + i].value); }

std::optional<int64_t> Node::Int(uint32_t name) const {
  auto v = Attr(name);
  return v ? ParseInt(*v) : std::nullopt;
}

std::optional<uint32_t> Node::Hex(uint32_t name) const {
  auto v = Attr(name);
  return v ? ParseHex(*v) : std::nullopt;
}

std::optional<int32_t> Node::Twips(uint32_t name) const {
  auto v = Attr(name);
  if (!v) return std::nullopt;
  auto m = ParseMeasure(*v, 20);
  if (!m || *m < INT32_MIN || *m > INT32_MAX) return std::nullopt;
  return static_cast<int32_t>(*m);
}

std::optional<bool> Node::OnOff(uint32_t name) const {
  auto v = Attr(name);
  return v ? ParseOnOff(*v) : std::nullopt;
}

bool Node::On() const {
  if (!tree_) return false;
  auto v = Attr(Name("val"));
  if (!v) return true;
  return ParseOnOff(*v).value_or(true);
}

std::optional<bool> ParseOnOff(std::string_view v) {
  if (v == "true" || v == "1" || v == "on") return true;
  if (v == "false" || v == "0" || v == "off") return false;
  return std::nullopt;
}

namespace {

std::string_view TrimSpace(std::string_view v) {
  while (!v.empty() && (v.front() == ' ' || v.front() == '\t' || v.front() == '\n' || v.front() == '\r')) v.remove_prefix(1);
  while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\n' || v.back() == '\r')) v.remove_suffix(1);
  return v;
}

std::optional<double> ParseNumber(std::string_view v) {
  v = TrimSpace(v);
  if (!v.empty() && v.front() == '+') v.remove_prefix(1);
  if (v.empty()) return std::nullopt;
  double d = 0;
  auto [p, ec] = std::from_chars(v.data(), v.data() + v.size(), d);
  if (ec != std::errc() || p != v.data() + v.size() || !std::isfinite(d)) return std::nullopt;
  return d;
}

}  // namespace

std::optional<int64_t> ParseInt(std::string_view v) {
  v = TrimSpace(v);
  if (!v.empty() && v.front() == '+') v.remove_prefix(1);
  int64_t n = 0;
  auto [p, ec] = std::from_chars(v.data(), v.data() + v.size(), n);
  if (ec == std::errc() && p == v.data() + v.size() && !v.empty()) return n;
  auto d = ParseNumber(v);
  if (!d || std::fabs(*d) > 9.0e15) return std::nullopt;
  return std::llround(*d);
}

std::optional<uint32_t> ParseHex(std::string_view v) {
  v = TrimSpace(v);
  if (v.empty() || v.size() > 8) return std::nullopt;
  uint32_t n = 0;
  auto [p, ec] = std::from_chars(v.data(), v.data() + v.size(), n, 16);
  if (ec != std::errc() || p != v.data() + v.size()) return std::nullopt;
  return n;
}

std::optional<int64_t> ParseMeasure(std::string_view v, int perPoint) {
  v = TrimSpace(v);
  double pointsPerUnit = 0;
  if (v.size() > 2) {
    std::string_view unit = v.substr(v.size() - 2);
    if (unit == "pt") pointsPerUnit = 1;
    else if (unit == "in") pointsPerUnit = 72;
    else if (unit == "cm") pointsPerUnit = 72 / 2.54;
    else if (unit == "mm") pointsPerUnit = 72 / 25.4;
    else if (unit == "pc" || unit == "pi") pointsPerUnit = 12;
  }
  if (pointsPerUnit == 0) return ParseInt(v);
  auto d = ParseNumber(v.substr(0, v.size() - 2));
  if (!d) return std::nullopt;
  double units = *d * pointsPerUnit * perPoint;
  if (std::fabs(units) > 9.0e15) return std::nullopt;
  return std::llround(units);
}

}  // namespace fw::doc
