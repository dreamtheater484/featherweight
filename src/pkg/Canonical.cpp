#include "pkg/Canonical.h"

#include <algorithm>
#include <vector>

#include "pkg/Mce.h"
#include "pkg/XmlPull.h"
#include "pkg/XmlWriter.h"

namespace fw::pkg {
namespace {

bool AllSpace(std::string_view s) {
  return std::all_of(s.begin(), s.end(), [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; });
}

void AppendName(std::string& out, std::string_view nsUri, std::string_view local) {
  out += '{';
  out += nsUri;
  out += '}';
  out += local;
}

// "w14 wp14" -> "{uri1} {uri2}"; "w:rPr w14:*" -> "{uri}rPr {uri14}*". Unknown prefixes stay as written.
std::string ResolvePrefixList(const XmlPull& pull, std::string_view list, bool qualified) {
  std::string out;
  size_t i = 0;
  while (i < list.size()) {
    while (i < list.size() && AllSpace(list.substr(i, 1))) ++i;
    size_t start = i;
    while (i < list.size() && !AllSpace(list.substr(i, 1))) ++i;
    if (start == i) break;
    std::string_view token = list.substr(start, i - start);
    std::string_view prefix = qualified ? QNamePrefix(token) : token;
    auto ns = pull.LookupNamespace(prefix);
    if (!out.empty()) out += ' ';
    if (!ns || (qualified && prefix.empty())) {
      out += token;
    } else {
      AppendName(out, *ns, qualified ? QNameLocal(token) : std::string_view());
    }
  }
  return out;
}

struct Frame {
  bool hasMarkup = false;  // has a child element, comment or processing instruction
  std::string pendingSpace;
};

}  // namespace

Status CanonicalizeXml(std::string_view xml, std::string& out, const Limits& limits, std::string_view label) {
  out.clear();
  XmlPull pull(xml, limits, label);
  std::vector<Frame> frames;
  struct Attr {
    std::string name, value;
  };
  std::vector<Attr> attrs;
  auto markupChild = [&] {
    if (frames.empty()) return;
    frames.back().hasMarkup = true;
    frames.back().pendingSpace.clear();
  };
  for (;;) {
    switch (pull.Next()) {
      case XmlPull::Event::StartElement: {
        markupChild();
        frames.emplace_back();
        out += '<';
        AppendName(out, pull.NamespaceUri(), pull.LocalName());
        attrs.clear();
        bool isChoice = pull.NamespaceUri() == mce::kNamespace && pull.LocalName() == "Choice";
        for (const auto& a : pull.Attributes()) {
          if (a.IsNamespaceDecl()) continue;
          Attr c;
          AppendName(c.name, a.nsUri, a.local);
          if (a.nsUri == mce::kNamespace && (a.local == "Ignorable" || a.local == "MustUnderstand"))
            c.value = ResolvePrefixList(pull, a.value, false);
          else if (a.nsUri == mce::kNamespace && (a.local == "ProcessContent" || a.local == "PreserveElements" ||
                                                   a.local == "PreserveAttributes"))
            c.value = ResolvePrefixList(pull, a.value, true);
          else if (isChoice && a.nsUri.empty() && a.local == "Requires")
            c.value = ResolvePrefixList(pull, a.value, false);
          else
            c.value = a.value;
          attrs.push_back(std::move(c));
        }
        std::sort(attrs.begin(), attrs.end(), [](const Attr& x, const Attr& y) { return x.name < y.name; });
        for (const auto& a : attrs) {
          out += ' ';
          out += a.name;
          out += "=\"";
          AppendEscapedAttribute(out, a.value);
          out += '"';
        }
        out += '>';
        break;
      }
      case XmlPull::Event::EndElement:
        if (!frames.back().hasMarkup) out += frames.back().pendingSpace;
        frames.pop_back();
        out += "</";
        AppendName(out, pull.NamespaceUri(), pull.LocalName());
        out += '>';
        break;
      case XmlPull::Event::Text:
      case XmlPull::Event::CData:
        if (frames.empty()) break;
        if (AllSpace(pull.Text())) {
          if (!frames.back().hasMarkup) frames.back().pendingSpace += pull.Text();
        } else {
          out += frames.back().pendingSpace;
          frames.back().pendingSpace.clear();
          AppendEscapedText(out, pull.Text());
        }
        break;
      case XmlPull::Event::Comment:
        markupChild();
        out += "<!--";
        out += pull.Text();
        out += "-->";
        break;
      case XmlPull::Event::ProcessingInstruction:
        markupChild();
        out += "<?";
        out += pull.QName();
        out += ' ';
        out += pull.Text();
        out += "?>";
        break;
      case XmlPull::Event::EndDocument:
        return Status{};
      case XmlPull::Event::Error:
        out.clear();
        return pull.GetStatus();
    }
  }
}

}  // namespace fw::pkg
