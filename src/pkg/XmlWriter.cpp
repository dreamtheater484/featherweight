#include "pkg/XmlWriter.h"

namespace fw::pkg {

void AppendEscapedText(std::string& out, std::string_view text) {
  size_t plain = 0;
  for (size_t i = 0; i < text.size(); ++i) {
    const char* rep = nullptr;
    switch (text[i]) {
      case '&': rep = "&amp;"; break;
      case '<': rep = "&lt;"; break;
      case '>': rep = "&gt;"; break;
      case '\r': rep = "&#xD;"; break;
      default: continue;
    }
    out.append(text, plain, i - plain);
    out += rep;
    plain = i + 1;
  }
  out.append(text, plain, text.size() - plain);
}

void AppendEscapedAttribute(std::string& out, std::string_view value) {
  size_t plain = 0;
  for (size_t i = 0; i < value.size(); ++i) {
    const char* rep = nullptr;
    switch (value[i]) {
      case '&': rep = "&amp;"; break;
      case '<': rep = "&lt;"; break;
      case '>': rep = "&gt;"; break;
      case '"': rep = "&quot;"; break;
      case '\t': rep = "&#x9;"; break;
      case '\n': rep = "&#xA;"; break;
      case '\r': rep = "&#xD;"; break;
      default: continue;
    }
    out.append(value, plain, i - plain);
    out += rep;
    plain = i + 1;
  }
  out.append(value, plain, value.size() - plain);
}

void XmlWriter::Declaration(bool standalone) {
  out_ += standalone ? "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n"
                     : "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n";
}

void XmlWriter::Declaration(const std::vector<std::pair<std::string_view, std::string_view>>& attrs) {
  out_ += "<?xml";
  for (const auto& [name, value] : attrs) {
    out_ += ' ';
    out_ += name;
    out_ += "=\"";
    AppendEscapedAttribute(out_, value);
    out_ += '"';
  }
  out_ += "?>\r\n";
}

void XmlWriter::CloseStartTag() {
  if (open_) {
    out_ += '>';
    open_ = false;
  }
}

void XmlWriter::StartElement(std::string_view qname) {
  CloseStartTag();
  out_ += '<';
  out_ += qname;
  stack_.emplace_back(qname);
  open_ = true;
}

void XmlWriter::Attribute(std::string_view qname, std::string_view value) {
  out_ += ' ';
  out_ += qname;
  out_ += "=\"";
  AppendEscapedAttribute(out_, value);
  out_ += '"';
}

void XmlWriter::EndElement() {
  if (stack_.empty()) return;
  if (open_) {
    out_ += "/>";
    open_ = false;
  } else {
    out_ += "</";
    out_ += stack_.back();
    out_ += '>';
  }
  stack_.pop_back();
}

void XmlWriter::Text(std::string_view text) {
  CloseStartTag();
  AppendEscapedText(out_, text);
}

void XmlWriter::CData(std::string_view text) {
  CloseStartTag();
  // "]]>" can't appear inside a CDATA section: split it across two.
  out_ += "<![CDATA[";
  size_t start = 0;
  for (size_t at; (at = text.find("]]>", start)) != std::string_view::npos; start = at + 2) {
    out_.append(text, start, at + 2 - start);
    out_ += "]]><![CDATA[";
  }
  out_.append(text, start, text.size() - start);
  out_ += "]]>";
}

void XmlWriter::Comment(std::string_view text) {
  CloseStartTag();
  out_ += "<!--";
  out_ += text;
  out_ += "-->";
}

void XmlWriter::ProcessingInstruction(std::string_view target, std::string_view data) {
  CloseStartTag();
  out_ += "<?";
  out_ += target;
  if (!data.empty()) {
    out_ += ' ';
    out_ += data;
  }
  out_ += "?>";
}

void XmlWriter::Raw(std::string_view xml) {
  CloseStartTag();
  out_ += xml;
}

}  // namespace fw::pkg
