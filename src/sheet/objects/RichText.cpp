#include "sheet/objects/RichText.h"

#include <algorithm>

#include "core/Strings.h"
#include "draw/Xml.h"

namespace fw::sheet::objects {

namespace {

using namespace draw::xml;

constexpr size_t kMaxParagraphs = 2000;
constexpr size_t kMaxChars = 32767;

// "+mn-lt" and "+mj-lt" are the theme's minor and major Latin fonts.
std::wstring Face(std::string_view typeface, const draw::ColorContext& colors) {
  if (typeface.starts_with("+mn") || typeface.starts_with("+mj")) {
    if (!colors.theme) return {};
    const draw::ThemeFont& f = typeface.starts_with("+mj") ? colors.theme->MajorFont() : colors.theme->MinorFont();
    return Utf8ToWide(typeface.ends_with("-ea") ? f.eastAsian : typeface.ends_with("-cs") ? f.complexScript : f.latin);
  }
  return Utf8ToWide(typeface);
}

// a:rPr, a:defRPr, a:endParaRPr.
void ApplyRun(pugi::xml_node rPr, const draw::ColorContext& colors, RichSpan& s) {
  if (!rPr) return;
  if (rPr.attribute("sz")) s.style.size = std::clamp(static_cast<float>(Number(rPr, "sz", 1100) / 100), 1.f, 400.f);
  if (rPr.attribute("b")) s.style.bold = Bool(rPr, "b", false);
  if (rPr.attribute("i")) s.style.italic = Bool(rPr, "i", false);
  if (pugi::xml_attribute u = rPr.attribute("u")) s.underline = std::string_view(u.value()) != "none";
  if (pugi::xml_node f = A(rPr, "solidFill"))
    if (auto c = draw::ReadColor(f, colors)) s.color = *c;
  if (std::wstring face = Face(A(rPr, "latin").attribute("typeface").value(), colors); !face.empty()) s.style.family = face;
  if (std::wstring face = Face(A(rPr, "ea").attribute("typeface").value(), colors); !face.empty())
    s.style.eastAsiaFamily = face;
  if (std::wstring face = Face(A(rPr, "cs").attribute("typeface").value(), colors); !face.empty())
    s.style.complexFamily = face;
}

text::Alignment AlignmentOf(std::string_view algn, text::Alignment def) {
  if (algn == "l") return text::Alignment::Start;
  if (algn == "ctr") return text::Alignment::Center;
  if (algn == "r") return text::Alignment::End;
  if (algn == "just") return text::Alignment::Justify;
  if (algn == "dist") return text::Alignment::Distribute;
  return def;
}

}  // namespace

std::vector<RichParagraph> ReadShapeText(pugi::xml_node txBody, const draw::ColorContext& colors, draw::Color color,
                                         float size) {
  std::vector<RichParagraph> out;
  RichSpan base;
  base.style.size = size;
  base.color = color;
  base.style.family = colors.theme ? Utf8ToWide(colors.theme->MinorFont().latin) : std::wstring();
  if (base.style.family.empty()) base.style.family = L"Calibri";
  pugi::xml_node level1 = A(A(txBody, "lstStyle"), "lvl1pPr");
  ApplyRun(A(level1, "defRPr"), colors, base);
  const text::Alignment alignment = AlignmentOf(level1.attribute("algn").value(), text::Alignment::Start);
  size_t chars = 0;
  ForEachChild(txBody, [&](pugi::xml_node p) {
    if (LocalNameOf(p) != "p" || NamespaceOf(p) != kA) return true;
    RichParagraph para;
    RichSpan paraBase = base;
    pugi::xml_node pPr = A(p, "pPr");
    para.alignment = AlignmentOf(pPr.attribute("algn").value(), alignment);
    ApplyRun(A(pPr, "defRPr"), colors, paraBase);
    para.mark = paraBase.style;
    ForEachChild(p, [&](pugi::xml_node r) {
      const std::string_view k = LocalNameOf(r);
      RichSpan s = paraBase;
      if (k == "r" || k == "fld") {
        ApplyRun(A(r, "rPr"), colors, s);
        s.text = Utf8ToWide(A(r, "t").text().get());
      } else if (k == "br") {
        ApplyRun(A(r, "rPr"), colors, s);
        s.text = L"\n";
      } else if (k == "endParaRPr") {
        ApplyRun(r, colors, s);
        para.mark = s.style;
      }
      if (!s.text.empty() && chars < kMaxChars) {
        chars += s.text.size();
        para.spans.push_back(std::move(s));
      }
      return true;
    });
    out.push_back(std::move(para));
    return out.size() < kMaxParagraphs;
  });
  return out;
}

float LayoutText(text::Fonts& fonts, const std::vector<RichParagraph>& paragraphs, float width, draw::Painter* painter,
                 draw::Point origin) {
  width = std::max(width, 1.f);
  float y = 0;
  std::wstring buffer;
  for (const RichParagraph& rp : paragraphs) {
    buffer.clear();
    text::Paragraph para;
    for (const RichSpan& s : rp.spans) {
      buffer += s.text;
      para.runs.push_back({static_cast<uint32_t>(s.text.size()), &s.style});
    }
    para.text = buffer;
    para.markStyle = &rp.mark;
    para.format.alignment = rp.alignment;
    const std::vector<text::Line> lines = text::Layout(fonts, para, width);
    float height = 0;
    for (const text::Line& line : lines) {
      const float baseline = origin.y + y + line.top + line.ascent;
      for (const text::GlyphPiece& g : line.pieces) {
        if (!painter || g.glyphs.empty() || g.run >= rp.spans.size()) continue;
        const RichSpan& s = rp.spans[g.run];
        const draw::Brush brush = draw::Brush::Solid(s.color);
        painter->DrawGlyphs({origin.x + g.OriginX(), baseline - g.rise}, g.Run(), brush);
        if (s.underline) {
          const float t = std::max(s.style.size / 16, 0.5f);
          painter->FillRect({origin.x + g.x, baseline + t, origin.x + g.x + g.width, baseline + 2 * t}, brush);
        }
      }
      height = line.top + line.ascent + line.descent + line.lineGap;
    }
    y += height;
  }
  return y;
}

}  // namespace fw::sheet::objects
