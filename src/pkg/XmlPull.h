#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pkg/Limits.h"
#include "pkg/Status.h"

namespace fw::pkg {

inline constexpr std::string_view kXmlNamespace = "http://www.w3.org/XML/1998/namespace";
inline constexpr std::string_view kXmlnsNamespace = "http://www.w3.org/2000/xmlns/";

// A streaming XML reader, for parts too large for a DOM (sheet data, shared strings) and for
// checking every part before the DOM sees it. It resolves namespaces, decodes entities, normalizes
// line ends as XML requires, and enforces the Limits. DTDs are rejected, so the only entities are
// the five predefined ones and character references.
//
// Input is UTF-8 (with or without a BOM) or UTF-16, which is converted to UTF-8 first.
class XmlPull {
 public:
  enum class Event { StartElement, EndElement, Text, CData, Comment, ProcessingInstruction, EndDocument, Error };

  struct Attribute {
    std::string_view qname, prefix, local, nsUri;
    std::string_view value;  // decoded and normalized
    bool IsNamespaceDecl() const { return nsUri == kXmlnsNamespace; }
  };

  // `label` names the part in messages, e.g. "word/document.xml".
  explicit XmlPull(std::string_view xml, const Limits& limits = Limits::Default(), std::string_view label = {});
  XmlPull(const XmlPull&) = delete;
  XmlPull& operator=(const XmlPull&) = delete;

  Event Next();

  // StartElement and EndElement.
  std::string_view QName() const { return qname_; }
  std::string_view Prefix() const;
  std::string_view LocalName() const;
  std::string_view NamespaceUri() const { return nsUri_; }
  bool IsEmptyElement() const { return empty_; }  // <a/>: the EndElement follows immediately
  const std::vector<Attribute>& Attributes() const { return attrs_; }
  const Attribute* FindAttribute(std::string_view nsUri, std::string_view local) const;
  int Depth() const { return static_cast<int>(stack_.size()); }  // 1 inside the root element

  // Text, CData and Comment: the decoded content. ProcessingInstruction: the target is QName().
  std::string_view Text() const { return text_; }
  bool IsWhitespace() const;

  // The namespace bound to `prefix` ("" is the default namespace) at the current position.
  std::optional<std::string_view> LookupNamespace(std::string_view prefix) const;

  const Status& GetStatus() const { return status_; }
  // The UTF-8 text being read: the input, or its conversion from UTF-16.
  std::string_view Source() const { return src_; }

  // Reads a whole document, checking that it is well-formed and within the limits.
  static Status Check(std::string_view xml, const Limits& limits = Limits::Default(), std::string_view label = {});

 private:
  struct Binding {
    std::string_view prefix, uri;
    bool owned = false;  // uri points into ownedUris_
  };

  Event Fail(Status::Code code, std::string_view what);
  Event Damaged(std::string_view why);
  Event ParseStartTag();
  Event ParseEndTag();
  Event ParseMarkup();
  bool ParseDeclaration(std::string_view body);
  std::string_view ParseName();
  bool SkipSpace();
  void PopElement();
  bool Decode(std::string_view raw, bool attribute, std::string& buf, std::string_view& out);

  std::string owned_;
  std::string_view src_;
  size_t pos_ = 0;
  Limits limits_;
  std::string label_;
  Status status_;
  Event event_ = Event::Text;

  std::string_view qname_, nsUri_, text_;
  bool empty_ = false;
  bool pendingEnd_ = false;  // an empty element's EndElement comes next
  bool popAfter_ = false;    // the element ended: pop it before reading on
  bool seenRoot_ = false;
  std::vector<Attribute> attrs_;
  std::vector<std::string> attrBuf_;
  std::string textBuf_;
  std::vector<std::string_view> stack_;
  std::vector<Binding> bindings_;
  std::vector<size_t> scopes_;
  std::vector<std::unique_ptr<std::string>> ownedUris_;
};

// Splits "w:p" into "w" and "p"; an unprefixed name has an empty prefix.
std::string_view QNamePrefix(std::string_view qname);
std::string_view QNameLocal(std::string_view qname);

}  // namespace fw::pkg
