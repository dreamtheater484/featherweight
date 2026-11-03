#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fw::pkg {

// Writes UTF-8 XML. Escaping is chosen so that a reader gets back exactly what was written:
// carriage returns, and tabs and line breaks in attribute values, become character references,
// since XML would otherwise normalize them away.
class XmlWriter {
 public:
  // <?xml version="1.0" encoding="UTF-8" standalone="yes"?> and a line break, as Office writes it.
  void Declaration(bool standalone = true);
  // A declaration with the given pseudo-attributes, e.g. kept from the original part.
  void Declaration(const std::vector<std::pair<std::string_view, std::string_view>>& attrs);

  void StartElement(std::string_view qname);
  void Attribute(std::string_view qname, std::string_view value);  // only right after StartElement
  void EndElement();  // writes "/>" when the element has no content

  void Text(std::string_view text);
  void CData(std::string_view text);
  void Comment(std::string_view text);
  void ProcessingInstruction(std::string_view target, std::string_view data);
  // Markup that is already serialized, such as a fragment kept by the round-trip store.
  void Raw(std::string_view xml);

  const std::string& Output() const { return out_; }
  std::string Take() { return std::move(out_); }

 private:
  void CloseStartTag();

  std::string out_;
  std::vector<std::string> stack_;
  bool open_ = false;  // a start tag is waiting for attributes or content
};

void AppendEscapedText(std::string& out, std::string_view text);
void AppendEscapedAttribute(std::string& out, std::string_view value);

}  // namespace fw::pkg
