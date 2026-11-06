// fwtool wrap: lays out paragraphs with fwtext and prints the lines, to compare line breaks with
// Word's quickly (plan §10). One paragraph per line of the input; every paragraph has one style.
#include <windows.h>

#include <dwrite_3.h>
#include <wrl/client.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "Common.h"
#include "core/Strings.h"
#include "text/Fonts.h"
#include "text/LineLayout.h"

namespace fwtool {
namespace {

using namespace fw::text;

constexpr const char* kWrapUsage =
    "Usage: fwtool wrap [options] (--file <utf-8 file> | <text>...)\n"
    "  Lays out each paragraph (one per line of the file, or the text) and prints its lines.\n"
    "  In text, \\t is a tab, \\n a line break, \\- an optional hyphen, \\_ a no-break space.\n"
    "  --width <pt>        column width (default 451.3, A4 with 2.5 cm margins)\n"
    "  --font <family>     (default Calibri)       --size <pt>  (default 11)\n"
    "  --bold, --italic, --caps, --small-caps, --spacing <pt>, --kern <min pt>, --ligatures\n"
    "  --scale <percent>   character width (w:w, default 100)\n"
    "  --align start|center|end|justify|distribute\n"
    "  --lang <tag>        (default en-US)         --hyphenate [--zone <pt>] [--limit <n>]\n"
    "  --indent <start>,<end>,<first line>         --rtl\n"
    "  --tabs <pos>[:start|center|end|decimal][:dot|hyphen|underscore],...\n"
    "  --default-tab <pt>  (default 36)\n"
    "  --glyphs            also print each line's glyph runs\n"
    "  --bench <n>         lay everything out n times and print the time per paragraph instead\n"
    "  --font-dir <dir>    also use the font files in this folder, without installing them\n";

std::wstring Unescape(const std::wstring& s) {
  std::wstring out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] != L'\\' || i + 1 == s.size()) {
      out += s[i];
      continue;
    }
    switch (s[++i]) {
      case L't': out += L'\t'; break;
      case L'n': out += L'\v'; break;
      case L'-': out += L'\x00AD'; break;
      case L'_': out += L'\x00A0'; break;
      default: out += s[i]; break;
    }
  }
  return out;
}

std::string Shown(std::wstring_view text) {
  std::wstring s;
  for (wchar_t c : text) {
    if (c == L'\t') s += L"2192";
    else if (c == L'\v') s += L"21B5";
    else if (c == 0x00AD) s += L"00AC";
    else s += c;
  }
  return fw::WideToUtf8(s);
}

const char* EndingName(LineEnd e) {
  switch (e) {
    case LineEnd::LineBreak: return "line break";
    case LineEnd::PageBreak: return "page break";
    case LineEnd::ColumnBreak: return "column break";
    case LineEnd::ParagraphEnd: return "end";
    default: return "wrap";
  }
}

// The installed fonts plus the font files in `dir`.
Microsoft::WRL::ComPtr<IDWriteFontCollection> CollectionWith(const std::wstring& dir) {
  using Microsoft::WRL::ComPtr;
  ComPtr<IDWriteFactory5> factory;
  ComPtr<IDWriteFontSetBuilder1> builder;
  ComPtr<IDWriteFontSet> system, set;
  ComPtr<IDWriteFontCollection1> collection;
  if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory5),
                                 reinterpret_cast<IUnknown**>(factory.GetAddressOf()))) ||
      FAILED(factory->CreateFontSetBuilder(&builder)) || FAILED(factory->GetSystemFontSet(&system)) ||
      FAILED(builder->AddFontSet(system.Get()))) {
    return nullptr;
  }
  std::error_code ec;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(dir, ec)) {
    std::wstring ext = entry.path().extension().wstring();
    if (ext != L".ttf" && ext != L".otf" && ext != L".ttc" && ext != L".TTF") continue;
    ComPtr<IDWriteFontFile> file;
    if (SUCCEEDED(factory->CreateFontFileReference(entry.path().c_str(), nullptr, &file))) builder->AddFontFile(file.Get());
  }
  if (FAILED(builder->CreateFontSet(&set)) || FAILED(factory->CreateFontCollectionFromFontSet(set.Get(), &collection)))
    return nullptr;
  return collection;
}

bool ParseTabs(const std::wstring& spec, std::vector<TabStop>& tabs) {
  std::wstringstream all(spec);
  std::wstring one;
  while (std::getline(all, one, L',')) {
    std::wstringstream parts(one);
    std::wstring part;
    TabStop t;
    int k = 0;
    while (std::getline(parts, part, L':')) {
      if (k == 0) t.position = std::stof(part);
      else if (part == L"center") t.align = TabAlign::Center;
      else if (part == L"end" || part == L"right") t.align = TabAlign::End;
      else if (part == L"decimal") t.align = TabAlign::Decimal;
      else if (part == L"dot") t.leader = TabLeader::Dot;
      else if (part == L"hyphen") t.leader = TabLeader::Hyphen;
      else if (part == L"underscore") t.leader = TabLeader::Underscore;
      else if (part != L"start" && part != L"left") return false;
      ++k;
    }
    tabs.push_back(t);
  }
  return true;
}

}  // namespace

int RunWrap(int argc, wchar_t** argv) {
  TextStyle style;
  style.family = L"Calibri";
  style.language = L"en-US";
  ParagraphFormat format;
  float width = 451.3f;
  bool glyphs = false;
  int bench = 0;
  std::wstring file, fontDir;
  std::vector<std::wstring> words;
  try {
    for (int i = 2; i < argc; ++i) {
      std::wstring a = argv[i];
      auto value = [&]() -> std::wstring {
        if (i + 1 >= argc) throw std::invalid_argument("missing value");
        return argv[++i];
      };
      if (a == L"--width") width = std::stof(value());
      else if (a == L"--font") style.family = value();
      else if (a == L"--size") style.size = std::stof(value());
      else if (a == L"--bold") style.bold = true;
      else if (a == L"--italic") style.italic = true;
      else if (a == L"--caps") style.caps = Caps::All;
      else if (a == L"--small-caps") style.caps = Caps::Small;
      else if (a == L"--spacing") style.letterSpacing = std::stof(value());
      else if (a == L"--kern") style.kerningMinSize = std::stof(value());
      else if (a == L"--scale") style.scale = std::stof(value()) / 100;
      else if (a == L"--ligatures") style.ligatures = true;
      else if (a == L"--lang") style.language = value();
      else if (a == L"--hyphenate") format.hyphenation.automatic = true;
      else if (a == L"--zone") format.hyphenation.zone = std::stof(value());
      else if (a == L"--limit") format.hyphenation.consecutiveLimit = std::stoi(value());
      else if (a == L"--rtl") format.rightToLeft = true;
      else if (a == L"--default-tab") format.defaultTabStop = std::stof(value());
      else if (a == L"--glyphs") glyphs = true;
      else if (a == L"--bench") bench = std::stoi(value());
      else if (a == L"--file") file = value();
      else if (a == L"--font-dir") fontDir = value();
      else if (a == L"--indent") {
        float s = 0, e = 0, f = 0;
        if (swscanf_s(value().c_str(), L"%f,%f,%f", &s, &e, &f) < 1) throw std::invalid_argument("indent");
        format.startIndent = s;
        format.endIndent = e;
        format.firstLineIndent = f;
      } else if (a == L"--tabs") {
        if (!ParseTabs(value(), format.tabs)) throw std::invalid_argument("tabs");
      } else if (a == L"--align") {
        std::wstring v = value();
        if (v == L"center") format.alignment = Alignment::Center;
        else if (v == L"end" || v == L"right") format.alignment = Alignment::End;
        else if (v == L"justify" || v == L"both") format.alignment = Alignment::Justify;
        else if (v == L"distribute") format.alignment = Alignment::Distribute;
        else if (v != L"start" && v != L"left") throw std::invalid_argument("align");
      } else if (a.starts_with(L"--")) {
        throw std::invalid_argument("option");
      } else {
        words.push_back(a);
      }
    }
  } catch (const std::exception&) {
    std::fputs(kWrapUsage, stderr);
    return 2;
  }

  std::vector<std::wstring> paragraphs;
  if (!file.empty()) {
    std::ifstream in(std::filesystem::path(file), std::ios::binary);
    if (!in) {
      std::fprintf(stderr, "Can't read %s\n", fw::WideToUtf8(file).c_str());
      return 1;
    }
    std::string line;
    while (std::getline(in, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (paragraphs.empty() && line.starts_with("\xEF\xBB\xBF")) line.erase(0, 3);
      paragraphs.push_back(Unescape(fw::Utf8ToWide(line)));
    }
  } else if (!words.empty()) {
    std::wstring joined;
    for (const auto& w : words) joined += (joined.empty() ? L"" : L" ") + w;
    paragraphs.push_back(Unescape(joined));
  }
  if (paragraphs.empty()) {
    std::fputs(kWrapUsage, stderr);
    return 2;
  }

  Microsoft::WRL::ComPtr<IDWriteFontCollection> collection;
  if (!fontDir.empty() && !(collection = CollectionWith(fontDir))) {
    std::fprintf(stderr, "Can't load the fonts in %s\n", fw::WideToUtf8(fontDir).c_str());
    return 1;
  }
  Fonts fonts({}, collection.Get());
  if (!fonts.Ok()) {
    std::fputs("DirectWrite isn't available\n", stderr);
    return 1;
  }
  auto paragraph = [&](const std::wstring& text) {
    Paragraph para;
    para.text = text;
    para.runs.push_back({static_cast<uint32_t>(text.size()), &style});
    para.markStyle = &style;
    para.format = format;
    return para;
  };
  if (bench > 0) {
    // The first pass also loads the fonts and hyphenation patterns, so it's timed on its own.
    size_t lineCount = 0;
    auto t0 = std::chrono::steady_clock::now();
    for (const auto& text : paragraphs) lineCount += Layout(fonts, paragraph(text), width).size();
    auto t1 = std::chrono::steady_clock::now();
    for (int r = 0; r < bench; ++r)
      for (const auto& text : paragraphs) Layout(fonts, paragraph(text), width);
    auto t2 = std::chrono::steady_clock::now();
    const double first = std::chrono::duration<double, std::milli>(t1 - t0).count();
    const double us = std::chrono::duration<double, std::micro>(t2 - t1).count() / (double(bench) * paragraphs.size());
    std::printf("%zu paragraphs, %zu lines: first pass %.1f ms, then %.1f us per paragraph (%.0f paragraphs/s)\n",
                paragraphs.size(), lineCount, first, us, 1e6 / us);
    return 0;
  }
  for (size_t pi = 0; pi < paragraphs.size(); ++pi) {
    const std::wstring& text = paragraphs[pi];
    std::vector<Line> lines = Layout(fonts, paragraph(text), width);
    if (paragraphs.size() > 1) std::printf("paragraph %zu\n", pi + 1);
    for (size_t li = 0; li < lines.size(); ++li) {
      const Line& l = lines[li];
      std::printf("%3zu  %4u-%-4u  x %7.2f-%-7.2f  asc %5.2f desc %5.2f gap %4.2f  %-10s%s |%s|\n", li + 1, l.start,
                  l.end, l.contentLeft, l.contentRight, l.ascent, l.descent, l.lineGap, EndingName(l.ending),
                  l.hyphenated ? " -" : "  ", Shown(std::wstring_view(text).substr(l.start, l.end - l.start)).c_str());
      if (!glyphs) continue;
      for (const auto& g : l.pieces) {
        std::printf("       %s x %7.2f w %6.2f size %5.2f rise %5.2f level %u text %u-%u glyphs %zu%s\n",
                    g.hyphen ? "hyphen" : g.leader ? "leader" : "run   ", g.x, g.width, g.emSize, g.rise, g.bidiLevel,
                    g.textStart, g.textEnd, g.glyphs.size(), "");
      }
    }
  }
  for (const auto& s : fonts.Substitutions()) {
    std::printf("substituted: %s -> %s\n", fw::WideToUtf8(s.requested).c_str(), fw::WideToUtf8(s.used).c_str());
  }
  return 0;
}

}  // namespace fwtool
