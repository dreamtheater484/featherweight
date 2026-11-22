#include "slide/view/SlideHeaderFooter.h"

#include <cstdio>
#include <cstdlib>
#include <random>

#include "core/Strings.h"
#include "draw/Xml.h"
#include "pkg/XmlWriter.h"
#include "slide/Presentation.h"

namespace fw::slide::view {

namespace {

using namespace draw::xml;

std::string Esc(std::string_view v) {
  std::string out;
  pkg::AppendEscapedAttribute(out, v);
  return out;
}

std::string Text(std::string_view v) {
  std::string out;
  pkg::AppendEscapedText(out, v);
  return out;
}

// The text of a paragraph's runs and fields (line breaks as spaces).
std::wstring ParagraphText(pugi::xml_node p) {
  std::wstring out;
  ForEachChild(p, [&](pugi::xml_node n) {
    const std::string_view k = LocalNameOf(n);
    if (NamespaceOf(n) != kA) return true;
    if (k == "r" || k == "fld") out += Utf8ToWide(A(n, "t").text().get());
    else if (k == "br") out += L' ';
    return true;
  });
  return out;
}

}  // namespace

const char* HfTypeName(HfKind kind) {
  switch (kind) {
    case HfKind::Date: return "dt";
    case HfKind::Footer: return "ftr";
    default: return "sldNum";
  }
}

std::vector<std::wstring> DateFormatSamples(const std::string& lang) {
  std::vector<std::wstring> out;
  for (int i = 1; i <= 13; ++i) out.push_back(DateFieldText("datetime" + std::to_string(i), lang));
  return out;
}

bool HfWanted(const HeaderFooterSettings& s, HfKind kind, bool titleLayout) {
  if (titleLayout && s.notOnTitle) return false;
  switch (kind) {
    case HfKind::Date: return s.date;
    case HfKind::Footer: return s.footer;
    default: return s.slideNumber;
  }
}

bool IsTitleLayout(const Layout* layout) {
  if (!layout) return false;
  return layout->type == "title" || layout->FindPlaceholder(PlaceholderType::CenteredTitle) != nullptr;
}

std::string HfTextBodyXml(HfKind kind, const HeaderFooterSettings& s, std::string_view fieldId, std::string_view lang,
                          const Prefixes& ns) {
  const std::string& a = ns.a;
  const std::string rPr = "<" + a + ":rPr lang=\"" + Esc(lang.empty() ? "en-US" : lang) + "\"/>";
  const std::string end = "<" + a + ":endParaRPr lang=\"" + Esc(lang.empty() ? "en-US" : lang) + "\"/>";
  auto field = [&](const std::string& type, const std::wstring& shown) {
    return "<" + a + ":fld id=\"" + Esc(fieldId) + "\" type=\"" + type + "\">" + rPr + "<" + a + ":t>" +
           Text(WideToUtf8(shown)) + "</" + a + ":t></" + a + ":fld>";
  };
  auto run = [&](const std::wstring& text) {
    if (text.empty()) return std::string();
    return "<" + a + ":r>" + rPr + "<" + a + ":t>" + Text(WideToUtf8(text)) + "</" + a + ":t></" + a + ":r>";
  };
  std::string body;
  switch (kind) {
    case HfKind::Date:
      if (s.dateAuto) {
        const int n = s.dateFormat >= 1 && s.dateFormat <= 13 ? s.dateFormat : 1;
        const std::string type = "datetime" + std::to_string(n);
        body = field(type, DateFieldText(type, std::string(lang)));
      } else {
        body = run(s.dateFixed);
      }
      break;
    case HfKind::Footer: body = run(s.footerText); break;
    default: body = field("slidenum", L"‹#›"); break;  // ‹#›: PowerPoint numbers it when shown
  }
  return "<" + ns.p + ":txBody><" + a + ":bodyPr/><" + a + ":lstStyle/><" + a + ":p>" + body + end + "</" + a + ":p></" + ns.p +
         ":txBody>";
}

std::string HfShapeXml(HfKind kind, uint32_t shapeId, uint32_t idx, std::string_view size, const HeaderFooterSettings& s,
                       std::string_view fieldId, std::string_view lang, const Prefixes& ns) {
  const std::string& p = ns.p;
  const std::string& a = ns.a;
  const char* name = kind == HfKind::Date ? "Date Placeholder " : kind == HfKind::Footer ? "Footer Placeholder " : "Slide Number Placeholder ";
  std::string x = "<" + p + ":sp><" + p + ":nvSpPr><" + p + ":cNvPr id=\"" + std::to_string(shapeId) + "\" name=\"" + name +
                  std::to_string(shapeId > 1 ? shapeId - 1 : shapeId) + "\"/><" + p + ":cNvSpPr><" + a +
                  ":spLocks noGrp=\"1\"/></" + p + ":cNvSpPr><" + p + ":nvPr><" + p + ":ph type=\"" + HfTypeName(kind) + "\"";
  if (!size.empty()) x += " sz=\"" + Esc(size) + "\"";
  x += " idx=\"" + std::to_string(idx) + "\"/></" + p + ":nvPr></" + p + ":nvSpPr><" + p + ":spPr/>";
  x += HfTextBodyXml(kind, s, fieldId, lang, ns);
  x += "</" + p + ":sp>";
  return x;
}

std::string NewFieldId() {
  static std::mt19937_64 rng{std::random_device{}()};
  const uint64_t hi = rng(), lo = rng();
  char buf[48];
  std::snprintf(buf, sizeof buf, "{%08X-%04X-4%03X-%04X-%012llX}", static_cast<unsigned>(hi >> 32),
                static_cast<unsigned>((hi >> 16) & 0xFFFF), static_cast<unsigned>(hi & 0xFFF),
                static_cast<unsigned>(0x8000 | ((lo >> 48) & 0x3FFF)), static_cast<unsigned long long>(lo & 0xFFFFFFFFFFFFull));
  return buf;
}

HeaderFooterSettings ReadHeaderFooter(const Slide& slide) {
  HeaderFooterSettings s;
  for (const Shape& sh : slide.shapes) {
    const PlaceholderType t = sh.placeholder.type;
    if (t != PlaceholderType::Date && t != PlaceholderType::Footer && t != PlaceholderType::SlideNumber) continue;
    pugi::xml_node body = Child(sh.node, kP, "txBody");
    if (!body) body = sh.textNode;
    std::wstring text;
    std::string dateType;
    ForEachChild(body, [&](pugi::xml_node p) {
      if (!Is(p, kA, "p")) return true;
      if (!text.empty()) text += L' ';
      text += ParagraphText(p);
      ForEachChild(p, [&](pugi::xml_node n) {
        if (Is(n, kA, "fld") && dateType.empty() && std::string_view(n.attribute("type").value()).starts_with("datetime"))
          dateType = n.attribute("type").value();
        // The runs' language: the date's format and the new runs' lang follow it.
        if (s.lang.empty() && (Is(n, kA, "r") || Is(n, kA, "fld"))) s.lang = A(n, "rPr").attribute("lang").value();
        return true;
      });
      return true;
    });
    switch (t) {
      case PlaceholderType::Date:
        s.date = true;
        s.dateAuto = !dateType.empty();
        if (s.dateAuto) {
          const int n = std::atoi(dateType.c_str() + 8);
          s.dateFormat = n >= 1 && n <= 13 ? n : 1;
        } else {
          s.dateFixed = text;
        }
        break;
      case PlaceholderType::Footer:
        s.footer = true;
        s.footerText = text;
        break;
      default: s.slideNumber = true; break;
    }
  }
  return s;
}

}  // namespace fw::slide::view
