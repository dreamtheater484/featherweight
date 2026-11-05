#include "word/DocFormat.h"

#include <dwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cwchar>

#include "Commands.h"
#include "core/Strings.h"
#include "doc/edit/Lists.h"
#include "doc/edit/TreeEdit.h"
#include "ui/Gfx.h"
#include "ui/Icons.h"

namespace edit = fw::doc::edit;
using fw::doc::Justification;
using Microsoft::WRL::ComPtr;

namespace {

struct Named {
  int cmd;
  const wchar_t* label;
  const wchar_t* keys;
};

// Labels and keys, for tooltips and command search, as Word names them.
constexpr Named kNames[] = {
    {CmdUndo, L"Undo", L"Ctrl+Z"},
    {CmdRedo, L"Redo", L"Ctrl+Y"},
    {CmdBold, L"Bold", L"Ctrl+B"},
    {CmdItalic, L"Italic", L"Ctrl+I"},
    {CmdUnderline, L"Underline", L"Ctrl+U"},
    {CmdStrike, L"Strikethrough", L""},
    {CmdSubscript, L"Subscript", L"Ctrl+="},
    {CmdSuperscript, L"Superscript", L"Ctrl+Shift+="},
    {CmdGrowFont, L"Increase font size", L"Ctrl+Shift+>"},
    {CmdShrinkFont, L"Decrease font size", L"Ctrl+Shift+<"},
    {CmdGrowFontPoint, L"Increase font size by 1 point", L"Ctrl+]"},
    {CmdShrinkFontPoint, L"Decrease font size by 1 point", L"Ctrl+["},
    {CmdClearCharFormat, L"Reset character formatting", L"Ctrl+Space"},
    {CmdClearParaFormat, L"Reset paragraph formatting", L"Ctrl+Q"},
    {CmdClearAllFormat, L"Clear all formatting", L""},
    {CmdFontColor, L"Font color", L""},
    {CmdHighlightColor, L"Text highlight color", L""},
    {CmdFormatPainter, L"Format painter", L""},
    {CmdCopyFormat, L"Copy formatting", L"Ctrl+Shift+C"},
    {CmdPasteFormat, L"Paste formatting", L"Ctrl+Shift+V"},
    {CmdAlignLeft, L"Align left", L"Ctrl+L"},
    {CmdAlignCenter, L"Center", L"Ctrl+E"},
    {CmdAlignRight, L"Align right", L"Ctrl+R"},
    {CmdAlignJustify, L"Justify", L"Ctrl+J"},
    {CmdSpacing1, L"Line spacing 1.0", L"Ctrl+1"},
    {CmdSpacing15, L"Line spacing 1.5", L"Ctrl+5"},
    {CmdSpacing2, L"Line spacing 2.0", L"Ctrl+2"},
    {CmdSpaceBefore, L"Add or remove space before paragraph", L"Ctrl+0"},
    {CmdBullets, L"Bullets", L"Ctrl+Shift+L"},
    {CmdNumbering, L"Numbering", L""},
    {CmdMultilevel, L"Multilevel list", L""},
    {CmdRestartNumbering, L"Restart numbering at 1", L""},
    {CmdContinueNumbering, L"Continue numbering", L""},
    {CmdIndent, L"Increase indent", L"Ctrl+M"},
    {CmdOutdent, L"Decrease indent", L"Ctrl+Shift+M"},
    {CmdHangingIndent, L"Hanging indent", L"Ctrl+T"},
    {CmdUnhangIndent, L"Remove hanging indent", L"Ctrl+Shift+T"},
    {CmdStyleNormal, L"Normal style", L"Ctrl+Shift+N"},
    {CmdStyleHeading1, L"Heading 1", L"Ctrl+Alt+1"},
    {CmdStyleHeading2, L"Heading 2", L"Ctrl+Alt+2"},
    {CmdStyleHeading3, L"Heading 3", L"Ctrl+Alt+3"},
    {CmdUpdateStyle, L"Update style to match selection", L""},
    {CmdInsertDate, L"Insert date", L""},
    {CmdInsertTime, L"Insert time", L""},
};

const Named* NameOf(int cmd) {
  for (const Named& n : kNames)
    if (n.cmd == cmd) return &n;
  return nullptr;
}

std::wstring W(std::string_view s) { return fw::Utf8ToWide(s); }
std::wstring W(const std::u16string& s) { return std::wstring(s.begin(), s.end()); }

// Word's size list.
const std::vector<std::wstring>& Sizes() {
  static const std::vector<std::wstring> sizes = {L"8",  L"9",  L"10", L"11", L"12", L"14", L"16", L"18",
                                                  L"20", L"22", L"24", L"26", L"28", L"36", L"48", L"72"};
  return sizes;
}

std::wstring PointsText(fw::HalfPoints size) {
  int half = size.Value();
  return std::to_wstring(half / 2) + (half % 2 ? L".5" : L"");
}

// The installed font families, sorted; read once, in idle time.
std::vector<std::wstring>& Families() {
  static std::vector<std::wstring> names;
  return names;
}
bool g_familiesRead = false;

}  // namespace

bool DocFormat::FontsPending() { return !g_familiesRead; }

void DocFormat::ReadFonts() {
  if (g_familiesRead) return;
  g_familiesRead = true;
  ComPtr<IDWriteFontCollection> fonts;
  if (!gfx::DWrite() || FAILED(gfx::DWrite()->GetSystemFontCollection(&fonts))) return;
  wchar_t locale[LOCALE_NAME_MAX_LENGTH] = L"en-us";
  GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH);
  std::vector<std::wstring>& names = Families();
  for (UINT32 i = 0; i < fonts->GetFontFamilyCount(); ++i) {
    ComPtr<IDWriteFontFamily> family;
    ComPtr<IDWriteLocalizedStrings> strings;
    if (FAILED(fonts->GetFontFamily(i, &family)) || FAILED(family->GetFamilyNames(&strings))) continue;
    UINT32 index = 0;
    BOOL exists = FALSE;
    strings->FindLocaleName(locale, &index, &exists);
    if (!exists) strings->FindLocaleName(L"en-us", &index, &exists);
    if (!exists) index = 0;
    UINT32 length = 0;
    if (FAILED(strings->GetStringLength(index, &length))) continue;
    std::wstring name(length, L'\0');
    if (FAILED(strings->GetString(index, name.data(), length + 1)) || name.empty()) continue;
    names.push_back(std::move(name));
  }
  std::sort(names.begin(), names.end(), [](const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
  names.erase(std::unique(names.begin(), names.end()), names.end());
}

namespace {

// Word's highlight colors (w:highlight), by name.
struct HighlightName {
  const char* name;
  uint32_t rgb;
};
constexpr HighlightName kHighlights[] = {
    {"yellow", 0xFFFF00},    {"green", 0x00FF00},      {"cyan", 0x00FFFF},       {"magenta", 0xFF00FF},
    {"blue", 0x0000FF},      {"red", 0xFF0000},        {"darkBlue", 0x000080},   {"darkCyan", 0x008080},
    {"darkGreen", 0x008000}, {"darkMagenta", 0x800080}, {"darkRed", 0x800000},   {"darkYellow", 0x808000},
    {"darkGray", 0x808080},  {"lightGray", 0xC0C0C0},  {"black", 0x000000},     {"white", 0xFFFFFF},
};

// The highlight Word has nearest to a color.
std::string NearestHighlight(uint32_t rgb) {
  auto dist = [](uint32_t a, uint32_t b) {
    int dr = int(a >> 16 & 255) - int(b >> 16 & 255), dg = int(a >> 8 & 255) - int(b >> 8 & 255), db = int(a & 255) - int(b & 255);
    return dr * dr * 3 + dg * dg * 4 + db * db * 2;
  };
  const HighlightName* best = &kHighlights[0];
  for (const HighlightName& h : kHighlights)
    if (dist(h.rgb, rgb) < dist(best->rgb, rgb)) best = &h;
  return best->name;
}

uint32_t RgbOf(D2D1_COLOR_F c) {
  auto ch = [](float v) { return static_cast<uint32_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255)); };
  return ch(c.r) << 16 | ch(c.g) << 8 | ch(c.b);
}

bool SameRange(const edit::Range& a, const edit::Range& b) { return a.from == b.from && a.to == b.to; }

wchar_t AlignGlyph(std::optional<Justification> jc) {
  if (!jc) return icon::AlignLeft;
  switch (*jc) {
    case Justification::Center: return icon::AlignCenter;
    case Justification::End: return icon::AlignRight;
    case Justification::Both:
    case Justification::Distribute: return icon::AlignJustify;
    default: return icon::AlignLeft;
  }
}

}  // namespace

DocFormat::DocFormat(edit::Editor& editor, std::function<Target()> target, std::function<void(const Done&)> done)
    : editor_(editor), target_(std::move(target)), done_(std::move(done)) {
  editor_.OnChange([this](const edit::Change&) { ++version_; });
}

int DocFormat::Shortcut(UINT vk, UINT mods) {
  const bool ctrl = (mods & kModCtrl) != 0, shift = (mods & kModShift) != 0, alt = (mods & kModAlt) != 0;
  if (!ctrl) return 0;
  if (alt) {
    if (shift) return 0;
    switch (vk) {
      case '1': return CmdStyleHeading1;
      case '2': return CmdStyleHeading2;
      case '3': return CmdStyleHeading3;
    }
    return 0;
  }
  if (shift) {
    switch (vk) {
      case VK_OEM_PERIOD: return CmdGrowFont;
      case VK_OEM_COMMA: return CmdShrinkFont;
      case VK_OEM_PLUS: return CmdSuperscript;
      case 'N': return CmdStyleNormal;
      case 'L': return CmdBullets;
      case 'C': return CmdCopyFormat;
      case 'V': return CmdPasteFormat;
      case 'M': return CmdOutdent;
      case 'T': return CmdUnhangIndent;
    }
    return 0;
  }
  switch (vk) {
    case 'B': return CmdBold;
    case 'I': return CmdItalic;
    case 'U': return CmdUnderline;
    case VK_OEM_PLUS: return CmdSubscript;
    case VK_OEM_6: return CmdGrowFontPoint;
    case VK_OEM_4: return CmdShrinkFontPoint;
    case VK_SPACE: return CmdClearCharFormat;
    case 'Q': return CmdClearParaFormat;
    case 'L': return CmdAlignLeft;
    case 'E': return CmdAlignCenter;
    case 'R': return CmdAlignRight;
    case 'J': return CmdAlignJustify;
    case '1': return CmdSpacing1;
    case '2': return CmdSpacing2;
    case '5': return CmdSpacing15;
    case '0': return CmdSpaceBefore;
    case 'M': return CmdIndent;
    case 'T': return CmdHangingIndent;
    case 'Z': return CmdUndo;
    case 'Y': return CmdRedo;
  }
  return 0;
}

bool DocFormat::Owns(int cmd) { return cmd >= CmdUndo && cmd <= CmdInsertTime; }

std::vector<int> DocFormat::CommandIds() {
  std::vector<int> ids;
  for (const Named& n : kNames) ids.push_back(n.cmd);
  return ids;
}

const edit::FormatState& DocFormat::State() {
  Target t = target_();
  if (stateVersion_ != version_ || !SameRange(stateRange_, t.chars)) {
    state_ = t.any ? edit::StateOf(Doc(), t.chars) : edit::FormatState{};
    stateRange_ = t.chars;
    stateVersion_ = version_;
  }
  return state_;
}

const std::vector<edit::StyleEntry>& DocFormat::Gallery() {
  if (galleryVersion_ != version_) {
    gallery_ = edit::GalleryStyles(Doc());
    galleryVersion_ = version_;
  }
  return gallery_;
}

CommandState DocFormat::Query(int cmd) {
  CommandState s;
  if (const Named* n = NameOf(cmd)) {
    s.label = n->label;
    s.shortcut = n->keys;
  }
  const bool on = target_().any && Editable();
  s.enabled = on;
  switch (cmd) {
    case CmdUndo:
      s.enabled = editor_.CanUndo();
      if (s.enabled) s.label = L"Undo " + W(editor_.UndoName());
      return s;
    case CmdRedo:
      s.enabled = editor_.CanRedo();
      if (s.enabled) s.label = L"Redo " + W(editor_.RedoName());
      return s;
    case CmdPasteFormat: s.enabled = on && carried_.has; return s;
    case CmdFormatPainter: s.checked = PainterOn(); return s;
    default: break;
  }
  if (!on) return s;
  const edit::FormatState& st = State();
  switch (cmd) {
    case CmdBold: s.checked = st.bold; break;
    case CmdItalic: s.checked = st.italic; break;
    case CmdUnderline: s.checked = st.underline; break;
    case CmdStrike: s.checked = st.strike; break;
    case CmdSubscript: s.checked = st.subscript; break;
    case CmdSuperscript: s.checked = st.superscript; break;
    case CmdAlignLeft: s.checked = st.jc == Justification::Start; break;
    case CmdAlignCenter: s.checked = st.jc == Justification::Center; break;
    case CmdAlignRight: s.checked = st.jc == Justification::End; break;
    case CmdAlignJustify: s.checked = st.jc == Justification::Both; break;
    case CmdBullets: s.checked = st.bullets; break;
    case CmdNumbering: s.checked = st.numbered; break;
    case CmdRestartNumbering:
    case CmdContinueNumbering: s.enabled = st.numbered; break;
    default: break;
  }
  return s;
}

void DocFormat::Run(const std::function<void(edit::TreeEdit&, const Target&)>& change) {
  Target t = target_();
  if (!t.any) return;
  std::u16string why;
  if (!editor_.CanEdit(&why)) {
    done_({std::nullopt, W(why)});
    return;
  }
  edit::TreeEdit te(editor_);
  change(te, t);
  done_({});
}

void DocFormat::Execute(int cmd) {
  using edit::Toggle;
  auto toggle = [&](Toggle what) { Run([what](edit::TreeEdit& te, const Target& t) { edit::ToggleRuns(te, t.chars, what); }); };
  auto para = [&](const edit::ParaFormat& f) { Run([f](edit::TreeEdit& te, const Target& t) { edit::FormatParagraphs(te, t.sel, f); }); };
  auto align = [&](Justification jc) {
    // Word's buttons turn off again: centered text centered again is left-aligned.
    edit::ParaFormat f;
    f.jc = State().jc == jc && jc != Justification::Start ? Justification::Start : jc;
    para(f);
  };
  auto spacing = [&](int32_t line) {
    edit::ParaFormat f;
    f.line = line;
    f.lineRule = fw::doc::LineRule::Auto;
    para(f);
  };
  auto list = [&](edit::ListKind kind) { Run([kind](edit::TreeEdit& te, const Target& t) { edit::ToggleList(te, t.sel, kind); }); };
  switch (cmd) {
    case CmdUndo:
    case CmdRedo: {
      edit::Result r = cmd == CmdUndo ? editor_.Undo() : editor_.Redo();
      Done d;
      if (r && r.caret.para != fw::doc::kNone) d.caret = r.caret;
      done_(d);
      return;
    }
    case CmdBold: toggle(Toggle::Bold); return;
    case CmdItalic: toggle(Toggle::Italic); return;
    case CmdUnderline: toggle(Toggle::Underline); return;
    case CmdStrike: toggle(Toggle::Strike); return;
    case CmdSubscript: toggle(Toggle::Subscript); return;
    case CmdSuperscript: toggle(Toggle::Superscript); return;
    case CmdGrowFont:
    case CmdShrinkFont:
    case CmdGrowFontPoint:
    case CmdShrinkFontPoint: {
      bool grow = cmd == CmdGrowFont || cmd == CmdGrowFontPoint, point = cmd == CmdGrowFontPoint || cmd == CmdShrinkFontPoint;
      Run([grow, point](edit::TreeEdit& te, const Target& t) { edit::GrowFont(te, t.chars, grow, point); });
      return;
    }
    case CmdClearCharFormat: Run([](edit::TreeEdit& te, const Target& t) { edit::ClearRunFormatting(te, t.chars); }); return;
    case CmdClearParaFormat: Run([](edit::TreeEdit& te, const Target& t) { edit::ClearParaFormatting(te, t.sel); }); return;
    case CmdClearAllFormat: Run([](edit::TreeEdit& te, const Target& t) { edit::ClearAllFormatting(te, t.chars); }); return;
    case CmdFontColor: ApplyColor(fontColor_, false, false); return;
    case CmdHighlightColor: ApplyColor(highlight_, false, true); return;
    case CmdFormatPainter:
      if (PainterOn()) {
        painter_ = Painter::Off;
      } else {
        carried_ = Capture();
        painter_ = carried_.has ? Painter::Once : Painter::Off;
      }
      done_({});
      return;
    case CmdCopyFormat: carried_ = Capture(); return;
    case CmdPasteFormat:
      if (carried_.has) PaintTo(carried_, target_());
      return;
    case CmdAlignLeft: align(Justification::Start); return;
    case CmdAlignCenter: align(Justification::Center); return;
    case CmdAlignRight: align(Justification::End); return;
    case CmdAlignJustify: align(Justification::Both); return;
    case CmdSpacing1: spacing(240); return;
    case CmdSpacing15: spacing(360); return;
    case CmdSpacing2: spacing(480); return;
    case CmdSpaceBefore:
      Run([](edit::TreeEdit& te, const Target& t) {
        edit::ParaFormat f;
        f.spaceBefore = fw::Twips(te.Doc().ResolvedPara(t.sel.from.para).spaceBefore.Value() == 0 ? 240 : 0);
        edit::FormatParagraphs(te, t.sel, f);
      });
      return;
    case CmdBullets: list(edit::ListKind::Bullet); return;
    case CmdNumbering: list(edit::ListKind::Number); return;
    case CmdMultilevel: list(edit::ListKind::Multilevel); return;
    case CmdRestartNumbering:
      Run([](edit::TreeEdit& te, const Target& t) { edit::RestartNumbering(te, t.sel.from.para); });
      return;
    case CmdContinueNumbering: {
      bool ok = true;
      Run([&ok](edit::TreeEdit& te, const Target& t) { ok = edit::ContinueNumbering(te, t.sel.from.para); });
      if (!ok) done_({std::nullopt, L"There's no list before this one to continue."});
      return;
    }
    case CmdIndent:
    case CmdOutdent:
      Run([cmd](edit::TreeEdit& te, const Target& t) { edit::ShiftIndent(te, t.sel, cmd == CmdIndent); });
      return;
    case CmdHangingIndent:
    case CmdUnhangIndent:
      Run([cmd](edit::TreeEdit& te, const Target& t) {
        // The left indent moves a tab stop, the first line stays where it was.
        const fw::doc::Document& doc = te.Doc();
        const int32_t step = std::max<int32_t>(1, doc.Settings().defaultTabStop.Value());
        te.Begin(cmd == CmdHangingIndent ? u"Hanging indent" : u"Remove hanging indent");
        for (uint32_t p : edit::ParagraphsOf(doc, t.sel)) {
          const fw::doc::ParaProps& pp = doc.ResolvedPara(p);
          int32_t start = pp.indStart.Value(), hanging = std::max<int32_t>(0, -pp.firstLine.Value());
          int32_t delta = cmd == CmdHangingIndent ? step : -std::min(step, hanging);
          if (delta == 0) continue;
          edit::ParaFormat f;
          f.indStart = fw::Twips(std::max<int32_t>(0, start + delta));
          f.firstLine = fw::Twips(-(hanging + delta));
          edit::FormatParagraphs(te, {{p, 0}, {p, 0}}, f);
        }
        te.Commit();
      });
      return;
    case CmdStyleNormal: ApplyStyle(L"Normal"); return;
    case CmdStyleHeading1: ApplyStyle(L"heading 1"); return;
    case CmdStyleHeading2: ApplyStyle(L"heading 2"); return;
    case CmdStyleHeading3: ApplyStyle(L"heading 3"); return;
    case CmdUpdateStyle: Run([](edit::TreeEdit& te, const Target& t) { edit::UpdateStyleToMatch(te, t.sel); }); return;
    case CmdInsertDate: InsertDateTime(0); return;
    case CmdInsertTime: {
      std::vector<std::wstring> pictures, samples;
      DateFormats(pictures, samples);
      for (size_t i = 0; i < pictures.size(); ++i)
        if (pictures[i].starts_with(L"T")) return InsertDateTime(i);
      return;
    }
    default: return;
  }
}

void DocFormat::ApplyStyle(const std::wstring& nameOrId) {
  const std::string name = fw::WideToUtf8(nameOrId);
  Run([&name](edit::TreeEdit& te, const Target& t) {
    // A caret applies a paragraph style to its paragraph, not its word's linked character style.
    edit::Range r = t.sel;
    edit::ApplyStyle(te, r, name);
  });
}

void DocFormat::ApplyFont(const std::wstring& font) {
  if (font.empty()) return;
  edit::RunFormat f;
  f.font = fw::WideToUtf8(font);
  Run([&f](edit::TreeEdit& te, const Target& t) { edit::FormatRuns(te, t.chars, f); });
}

void DocFormat::ApplySize(double points) {
  if (!(points >= 1 && points <= 1638)) return;
  edit::RunFormat f;
  f.size = fw::HalfPoints(static_cast<int32_t>(std::lround(points * 2)));
  Run([&f](edit::TreeEdit& te, const Target& t) { edit::FormatRuns(te, t.chars, f); });
}

void DocFormat::ApplyColor(D2D1_COLOR_F color, bool automatic, bool highlight) {
  edit::RunFormat f;
  if (highlight) f.highlight = automatic ? std::string("none") : NearestHighlight(RgbOf(color));
  else f.color = automatic ? fw::doc::Color{} : fw::doc::Color::Rgb(RgbOf(color));
  Run([&f](edit::TreeEdit& te, const Target& t) { edit::FormatRuns(te, t.chars, f); });
}

DocFormat::Format DocFormat::Capture() {
  Format f;
  Target t = target_();
  if (!t.any) return f;
  const fw::doc::Document& doc = Doc();
  // The first character's format, and its paragraph's.
  edit::Range one{t.chars.from, t.chars.from};
  if (!t.chars.Empty() && t.chars.from.offset < doc.Para(t.chars.from.para).text.size())
    one.to = {t.chars.from.para, t.chars.from.offset + 1};
  edit::FormatState s = edit::StateOf(doc, one);
  f.run.font = s.font;
  f.run.size = s.size;
  f.run.bold = s.bold;
  f.run.italic = s.italic;
  f.run.strike = s.strike;
  f.run.underline = s.underline ? fw::doc::Underline::Single : fw::doc::Underline::None;
  f.run.vertAlign = s.superscript ? fw::doc::VertAlign::Superscript
                    : s.subscript ? fw::doc::VertAlign::Subscript
                                  : fw::doc::VertAlign::Baseline;
  if (s.color) f.run.color = *s.color;
  if (s.highlight) f.run.highlight = s.highlight->present ? NearestHighlight(s.highlight->rgb) : std::string("none");
  f.run.charStyle = s.charStyle.value_or(std::string());
  const fw::doc::ParaProps& pp = doc.ResolvedPara(t.sel.from.para);
  f.style = std::string(pp.style);
  f.para.jc = pp.jc;
  f.para.indStart = pp.indStart;
  f.para.indEnd = pp.indEnd;
  f.para.firstLine = pp.firstLine;
  f.para.spaceBefore = pp.spaceBefore;
  f.para.spaceAfter = pp.spaceAfter;
  f.para.line = pp.line;
  f.para.lineRule = pp.lineRule;
  f.has = true;
  return f;
}

void DocFormat::PaintTo(const Format& f, const Target& t) {
  if (!t.any || !f.has || !Editable()) return;
  const fw::doc::Document& doc = Doc();
  // Paragraph formatting goes with whole paragraphs, as Word's painter does.
  const bool whole = t.sel.from.offset == 0 &&
                     (t.sel.to.para > t.sel.from.para || t.sel.to.offset >= doc.Para(t.sel.from.para).text.size());
  editor_.BeginGroup(u"Format Painter");
  {
    edit::TreeEdit te(editor_);
    if (whole) {
      if (!f.style.empty()) edit::ApplyStyle(te, t.sel, f.style);
      edit::FormatParagraphs(te, t.sel, f.para);
    }
    edit::FormatRuns(te, t.chars, f.run);
  }
  editor_.EndGroup();
  done_({});
}

void DocFormat::SelectionMade() {
  if (painter_ == Painter::Off) return;
  painter_ = Painter::Off;
  PaintTo(carried_, target_());
}

void DocFormat::DateFormats(std::vector<std::wstring>& pictures, std::vector<std::wstring>& samples) {
  std::wstring locale = W(std::string_view(State().lang));
  if (locale.empty() || !IsValidLocaleName(locale.c_str())) {
    wchar_t buf[LOCALE_NAME_MAX_LENGTH] = L"en-US";
    GetUserDefaultLocaleName(buf, LOCALE_NAME_MAX_LENGTH);
    locale = buf;
  }
  auto info = [&](LCTYPE type) {
    wchar_t buf[128] = {};
    return GetLocaleInfoEx(locale.c_str(), type, buf, 128) ? std::wstring(buf) : std::wstring();
  };
  SYSTEMTIME now;
  GetLocalTime(&now);
  auto format = [&](const std::wstring& picture) {
    // "D" dates, "T" times, "B" both (a date picture, a tab, a time picture).
    wchar_t buf[256] = {};
    std::wstring out;
    size_t tab = picture.find(L'\t');
    std::wstring date = picture.substr(1, tab == std::wstring::npos ? std::wstring::npos : tab - 1);
    if (picture[0] == L'T') {
      if (GetTimeFormatEx(locale.c_str(), 0, &now, date.c_str(), buf, 256)) out = buf;
      return out;
    }
    if (GetDateFormatEx(locale.c_str(), 0, &now, date.c_str(), buf, 256, nullptr)) out = buf;
    if (picture[0] == L'B' && tab != std::wstring::npos &&
        GetTimeFormatEx(locale.c_str(), 0, &now, picture.substr(tab + 1).c_str(), buf, 256))
      out += std::wstring(L" ") + buf;
    return out;
  };
  const std::wstring shortDate = info(LOCALE_SSHORTDATE), longDate = info(LOCALE_SLONGDATE);
  const std::wstring shortTime = info(LOCALE_SSHORTTIME), longTime = info(LOCALE_STIMEFORMAT);
  std::vector<std::wstring> candidates = {L"D" + shortDate, L"D" + longDate, L"Dd MMMM yyyy", L"Dyyyy-MM-dd",
                                          L"B" + shortDate + L"\t" + shortTime, L"T" + shortTime, L"T" + longTime};
  for (const std::wstring& c : candidates) {
    if (c.size() < 2) continue;
    std::wstring sample = format(c);
    if (sample.empty() || std::find(samples.begin(), samples.end(), sample) != samples.end()) continue;
    pictures.push_back(c);
    samples.push_back(sample);
  }
}

void DocFormat::InsertDateTime(size_t index) {
  std::vector<std::wstring> pictures, samples;
  DateFormats(pictures, samples);
  Target t = target_();
  if (index >= samples.size() || !t.any) return;
  std::u16string why;
  if (!editor_.CanEdit(&why)) {
    done_({std::nullopt, W(why)});
    return;
  }
  std::u16string text(samples[index].begin(), samples[index].end());
  edit::Result r = t.sel.Empty() ? editor_.InsertText(t.sel.from, text) : editor_.Replace(t.sel.from, t.sel.to, text);
  Done d;
  if (r) d.caret = r.caret;
  else d.message = W(r.message);
  done_(d);
}

std::wstring DocFormat::Language() {
  if (!target_().any) return {};
  std::wstring tag = W(std::string_view(State().lang));
  if (tag.empty() || !IsValidLocaleName(tag.c_str())) return {};
  wchar_t name[128] = {};
  if (!GetLocaleInfoEx(tag.c_str(), LOCALE_SLOCALIZEDDISPLAYNAME, name, 128)) return {};
  return name;
}

void DocFormat::AddToolbar(ToolbarModel& m) {
  const bool on = target_().any && Editable();
  const edit::FormatState& st = State();
  auto button = [&](int cmd, wchar_t glyph) {
    CommandState q = Query(cmd);
    ToolItem t;
    t.cmd = cmd;
    t.glyph = glyph;
    t.tooltip = q.Tooltip();
    t.enabled = q.enabled;
    t.checked = q.checked;
    return t;
  };
  auto toggleButton = [&](int cmd, wchar_t glyph) {
    ToolItem t = button(cmd, glyph);
    t.toggle = true;
    return t;
  };

  ToolItem undo = button(CmdUndo, icon::Undo);
  undo.sepBefore = true;
  m.left.push_back(undo);
  m.left.push_back(button(CmdRedo, icon::Redo));

  // Styles, font and size.
  ToolItem style;
  style.kind = ToolKind::Dropdown;
  style.cmd = CmdStyleBox;
  style.width = 124;
  style.tooltip = L"Styles";
  style.enabled = on;
  style.sepBefore = true;
  const auto& gallery = Gallery();
  for (size_t i = 0; i < gallery.size(); ++i) {
    style.choices.push_back(W(std::string_view(gallery[i].name)));
    if (st.paraStyle && !gallery[i].id.empty() && gallery[i].id == *st.paraStyle) style.choice = static_cast<int>(i);
  }
  if (style.choice < 0 && st.paraStyle)
    if (const fw::doc::Style* s = Doc().FindStyle(*st.paraStyle)) style.text = W(std::string_view(edit::DisplayName(s->name)));
  style.onChange = [this](const ToolValue& v) {
    const auto& g = Gallery();
    if (v.index >= 0 && v.index < static_cast<int>(g.size()))
      ApplyStyle(W(std::string_view(g[v.index].id.empty() ? g[v.index].name : g[v.index].id)));
  };
  m.left.push_back(style);

  ToolItem font;
  font.kind = ToolKind::ComboBox;
  font.cmd = CmdFontBox;
  font.width = 124;
  font.tooltip = L"Font";
  font.enabled = on;
  font.choices = Families();
  font.text = st.font ? W(std::string_view(*st.font)) : std::wstring();
  font.onChange = [this](const ToolValue& v) { ApplyFont(v.text); };
  m.left.push_back(font);

  ToolItem size;
  size.kind = ToolKind::ComboBox;
  size.cmd = CmdSizeBox;
  size.width = 56;
  size.tooltip = L"Font size";
  size.enabled = on;
  size.choices = Sizes();
  size.text = st.size ? PointsText(*st.size) : std::wstring();
  size.validate = [](std::wstring& text) {
    wchar_t* end = nullptr;
    double v = std::wcstod(text.c_str(), &end);
    if (end == text.c_str() || !(v >= 1 && v <= 1638)) return false;
    text = PointsText(fw::HalfPoints(static_cast<int32_t>(std::lround(v * 2))));
    return true;
  };
  size.onChange = [this](const ToolValue& v) { ApplySize(std::wcstod(v.text.c_str(), nullptr)); };
  m.left.push_back(size);

  m.left.push_back(toggleButton(CmdBold, icon::Bold));
  m.left.push_back(toggleButton(CmdItalic, icon::Italic));
  m.left.push_back(toggleButton(CmdUnderline, icon::Underline));

  auto colorButton = [&](int cmd, wchar_t glyph, D2D1_COLOR_F color, const wchar_t* automaticLabel, bool highlight) {
    ToolItem t = button(cmd, glyph);
    t.kind = ToolKind::SplitButton;
    t.width = 52;
    t.colors = true;
    t.color = color;
    t.automaticLabel = automaticLabel;
    t.onChange = [this, highlight](const ToolValue& v) {
      if (!v.automatic) (highlight ? highlight_ : fontColor_) = v.color;
      ApplyColor(v.color, v.automatic, highlight);
    };
    return t;
  };
  m.left.push_back(colorButton(CmdFontColor, icon::FontColor, fontColor_, L"Automatic", false));
  m.left.push_back(colorButton(CmdHighlightColor, icon::Highlight, highlight_, L"No color", true));
  ToolItem painter = toggleButton(CmdFormatPainter, icon::Brush);
  painter.enabled = on;
  m.left.push_back(painter);

  // Paragraphs and lists.
  ToolItem alignBox;
  alignBox.kind = ToolKind::Dropdown;
  alignBox.cmd = CmdAlignBox;
  alignBox.width = 52;
  alignBox.glyph = AlignGlyph(st.jc);
  alignBox.tooltip = L"Alignment";
  alignBox.enabled = on;
  alignBox.sepBefore = true;
  alignBox.choices = {L"Align left (Ctrl+L)", L"Center (Ctrl+E)", L"Align right (Ctrl+R)", L"Justify (Ctrl+J)"};
  alignBox.onChange = [this](const ToolValue& v) {
    if (v.index >= 0 && v.index < 4) Execute(CmdAlignLeft + v.index);
  };
  m.left.push_back(alignBox);
  m.left.push_back(toggleButton(CmdBullets, icon::BulletedList));
  ToolItem numbering = button(CmdNumbering, icon::NumberedList);
  numbering.kind = ToolKind::SplitButton;
  numbering.width = 52;
  numbering.choices = {L"Numbering 1. a. i.", L"Multilevel list 1. 1.1. 1.1.1.", L"Restart numbering at 1", L"Continue numbering"};
  numbering.onChange = [this](const ToolValue& v) {
    static constexpr int kCmds[] = {CmdNumbering, CmdMultilevel, CmdRestartNumbering, CmdContinueNumbering};
    if (v.index >= 0 && v.index < 4) Execute(kCmds[v.index]);
  };
  m.left.push_back(numbering);
  m.left.push_back(button(CmdOutdent, icon::DecreaseIndent));
  m.left.push_back(button(CmdIndent, icon::IncreaseIndent));

  // Insert: date and time, for now.
  ToolItem insert;
  insert.kind = ToolKind::Dropdown;
  insert.cmd = CmdInsertBox;
  insert.width = 104;
  insert.glyph = icon::Add;
  insert.text = L"Insert";
  insert.tooltip = L"Insert the date or time";
  insert.enabled = on;
  insert.sepBefore = true;
  std::vector<std::wstring> pictures;
  DateFormats(pictures, insert.choices);
  insert.onChange = [this](const ToolValue& v) {
    if (v.index >= 0) InsertDateTime(static_cast<size_t>(v.index));
  };
  m.left.push_back(insert);
}
