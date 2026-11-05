// fwtool torture: the "open, edit and save" never-damage test (plan §10), D2's exit gate. Each Word
// document gets rounds of seeded random edits through the editor, aimed at what breaks files:
// deletes across fields, bookmarks, comments, content controls, tables and tracked changes,
// paragraph splits and joins, formatting, typing, undo and redo. After each round the document is
// saved, the saved file must open, pass fwtool check (no errors the original didn't have) and read
// back into the model as the edited document. A failure is shrunk to the fewest edits that still
// fail and kept as a reproducer. scripts\torture.ps1 then runs the validator and Word on the saved
// copies.
#include <windows.h>
#include <objbase.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>

#include "Common.h"
#include "Torture.h"
#include "core/Strings.h"
#include "doc/docx/Docx.h"
#include "doc/docx/Save.h"
#include "doc/model/Schema.h"
#include "doc/model/Utf.h"
#include "pkg/Check.h"
#include "pkg/Snapshot.h"

// The editor arrives with D2 (doc/edit/Editor.h); until then the harness runs without it.
#if __has_include("doc/edit/Editor.h")
#include "doc/edit/Editor.h"
#include "doc/edit/Format.h"
#include "doc/edit/Lists.h"
#include "doc/edit/Styles.h"
#include "doc/edit/TreeEdit.h"
#define FW_TORTURE_EDITOR 1
// D3's operations, as each lands: tables (Editor.h), pictures, fields, links, bookmarks, headers
// and footers.
#if __has_include("doc/edit/Pictures.h") && __has_include("doc/edit/Inline.h") && __has_include("doc/edit/HeaderFooter.h")
#include "doc/edit/HeaderFooter.h"
#include "doc/edit/Inline.h"
#include "doc/edit/Pictures.h"
#define FW_TORTURE_D3 1
#else
#define FW_TORTURE_D3 0
#endif
#if FW_TORTURE_D3 && __has_include("doc/edit/Page.h") && __has_include("doc/edit/TableStyles.h")
#include "doc/edit/Page.h"
#include "doc/edit/TableStyles.h"
#define FW_TORTURE_PAGE 1
#else
#define FW_TORTURE_PAGE 0
#endif
// Copy and paste with formatting (D2's clipboard), through the formats the clipboard carries.
#if __has_include("doc/edit/Clip.h")
#include "doc/edit/Clip.h"
#define FW_TORTURE_CLIP 1
#else
#define FW_TORTURE_CLIP 0
#endif
// D3's audit of the Home tab: change case (Case.h); a new style from the selection and theme fonts
// and colors (Styles.h, Format.h), which came with the Style panel (word/DocStyles.h).
#if __has_include("doc/edit/Case.h")
#include "doc/edit/Case.h"
#define FW_TORTURE_CASE 1
#else
#define FW_TORTURE_CASE 0
#endif
// D4: forms, comments and tracked changes, notes, shapes, watermarks, and Save as RTF and text.
#if FW_TORTURE_D3 && __has_include("doc/edit/Forms.h") && __has_include("doc/edit/Comments.h") && \
    __has_include("doc/edit/Notes.h") && __has_include("doc/edit/Shapes.h") && __has_include("doc/edit/Watermark.h") && \
    __has_include("doc/edit/Export.h")
#include "doc/edit/Comments.h"
#include "doc/edit/Export.h"
#include "doc/edit/Forms.h"
#include "doc/edit/Notes.h"
#include "doc/edit/Shapes.h"
#include "doc/edit/Watermark.h"
#define FW_TORTURE_D4 1
#else
#define FW_TORTURE_D4 0
#endif
#if __has_include("word/DocStyles.h")
#define FW_TORTURE_THEME 1
#else
#define FW_TORTURE_THEME 0
#endif
#else
#define FW_TORTURE_EDITOR 0
#define FW_TORTURE_CLIP 0
#define FW_TORTURE_CASE 0
#define FW_TORTURE_THEME 0
#define FW_TORTURE_D4 0
#endif

namespace fwtool {

namespace torture {

namespace {

std::string PosText(fw::doc::Pos p) { return std::to_string(p.para) + ":" + std::to_string(p.offset); }

bool ParsePos(std::string_view s, fw::doc::Pos& p) {
  size_t colon = s.find(':');
  if (colon == std::string_view::npos) return false;
  p.para = static_cast<uint32_t>(std::strtoul(std::string(s.substr(0, colon)).c_str(), nullptr, 10));
  p.offset = static_cast<uint32_t>(std::strtoul(std::string(s.substr(colon + 1)).c_str(), nullptr, 10));
  return true;
}

// Typed text in a reproducer: UTF-8 in quotes, with \t, \", \\ and \uXXXX for other control characters.
std::string Quote(std::u16string_view text) {
  std::string out = "\"";
  for (char16_t c : text) {
    if (c == u'"') out += "\\\"";
    else if (c == u'\\') out += "\\\\";
    else if (c == u'\t') out += "\\t";
    else if (c < 0x20) {
      char buf[8];
      std::snprintf(buf, sizeof buf, "\\u%04X", static_cast<unsigned>(c));
      out += buf;
    } else fw::doc::AppendUtf8(out, std::u16string_view(&c, 1));
  }
  return out + "\"";
}

bool Unquote(std::string_view s, std::u16string& out) {
  if (s.size() < 2 || s.front() != '"' || s.back() != '"') return false;
  std::string utf8;
  for (size_t i = 1; i + 1 < s.size(); ++i) {
    if (s[i] != '\\' || i + 2 >= s.size()) {
      utf8 += s[i];
      continue;
    }
    char e = s[++i];
    if (e == 't') utf8 += '\t';
    else if (e == 'u' && i + 4 < s.size()) {
      char16_t c = static_cast<char16_t>(std::strtoul(std::string(s.substr(i + 1, 4)).c_str(), nullptr, 16));
      fw::doc::AppendUtf8(utf8, std::u16string_view(&c, 1));
      i += 4;
    } else utf8 += e;
  }
  out = fw::doc::ToUtf16(utf8);
  return true;
}

}  // namespace

std::string FormatEdit(const Edit& e) {
  std::string line;
  switch (e.kind) {
    case EditKind::Type: line = "type " + PosText(e.at) + " " + Quote(e.text); break;
    case EditKind::Delete: line = "delete " + PosText(e.at) + " " + PosText(e.to); break;
    case EditKind::Split: line = "split " + PosText(e.at); break;
    case EditKind::Join: line = "join " + std::to_string(e.at.para); break;
    case EditKind::Format: line = "format " + PosText(e.at) + " " + PosText(e.to) + " " + e.format; break;
    case EditKind::Undo: line = "undo " + std::to_string(e.count); break;
    case EditKind::Redo: line = "redo " + std::to_string(e.count); break;
    case EditKind::Replace: line = "replace " + PosText(e.at) + " " + PosText(e.to) + " " + Quote(e.text); break;
    case EditKind::Group: line = "group " + std::to_string(e.count); break;
    case EditKind::Command:
      line = "command " + PosText(e.at) + " " + PosText(e.to) + " " + Quote(fw::doc::ToUtf16(e.format));
      break;
    case EditKind::Paint: line = "paint " + PosText(e.at) + " " + PosText(e.to) + " " + PosText(e.from); break;
    case EditKind::Save: line = "save"; break;
  }
  if (!e.target.empty()) line += "  # " + e.target;
  return line;
}

bool ParseEdit(std::string_view line, Edit& e) {
  e = Edit{};
  // The comment after the edit names its target; quoted text may contain '#'.
  size_t hash = std::string_view::npos;
  bool quoted = false;
  for (size_t i = 0; i < line.size(); ++i) {
    if (line[i] == '\\' && quoted) ++i;
    else if (line[i] == '"') quoted = !quoted;
    else if (line[i] == '#' && !quoted) {
      hash = i;
      break;
    }
  }
  if (hash != std::string_view::npos) {
    std::string_view t = line.substr(hash + 1);
    while (!t.empty() && t.front() == ' ') t.remove_prefix(1);
    e.target = std::string(t);
    line = line.substr(0, hash);
  }
  while (!line.empty() && (line.back() == ' ' || line.back() == '\r')) line.remove_suffix(1);
  auto word = [&]() {
    while (!line.empty() && line.front() == ' ') line.remove_prefix(1);
    size_t end = line.front() == '"' ? line.size() : std::min(line.find(' '), line.size());
    std::string_view w = line.substr(0, end);
    line.remove_prefix(end);
    return w;
  };
  if (line.empty()) return false;
  std::string_view verb = word();
  if (verb == "type") {
    e.kind = EditKind::Type;
    return ParsePos(word(), e.at) && Unquote(word(), e.text);
  }
  if (verb == "delete" || verb == "format") {
    e.kind = verb == "delete" ? EditKind::Delete : EditKind::Format;
    if (!ParsePos(word(), e.at) || !ParsePos(word(), e.to)) return false;
    if (e.kind == EditKind::Format) e.format = std::string(word());
    return e.kind == EditKind::Delete || !e.format.empty();
  }
  if (verb == "split") {
    e.kind = EditKind::Split;
    return ParsePos(word(), e.at);
  }
  if (verb == "join") {
    e.kind = EditKind::Join;
    e.at.para = static_cast<uint32_t>(std::strtoul(std::string(word()).c_str(), nullptr, 10));
    return true;
  }
  if (verb == "replace") {
    e.kind = EditKind::Replace;
    return ParsePos(word(), e.at) && ParsePos(word(), e.to) && Unquote(word(), e.text);
  }
  if (verb == "command") {
    e.kind = EditKind::Command;
    std::u16string spec;
    if (!ParsePos(word(), e.at) || !ParsePos(word(), e.to) || !Unquote(word(), spec)) return false;
    e.format = fw::doc::ToUtf8(spec);
    return !e.format.empty();
  }
  if (verb == "paint") {
    e.kind = EditKind::Paint;
    return ParsePos(word(), e.at) && ParsePos(word(), e.to) && ParsePos(word(), e.from);
  }
  if (verb == "save") {
    e.kind = EditKind::Save;
    return true;
  }
  if (verb == "undo" || verb == "redo" || verb == "group") {
    e.kind = verb == "undo" ? EditKind::Undo : verb == "redo" ? EditKind::Redo : EditKind::Group;
    e.count = static_cast<uint32_t>(std::strtoul(std::string(word()).c_str(), nullptr, 10));
    return e.count > 0;
  }
  return false;
}

namespace {

// A picture from tests/fuzz/seeds/picture, found from the current folder or above fwtool.exe (it
// lives in build\tools\fwtool\<config>).
bool SeedPicture(const std::string& name, std::vector<uint8_t>& bytes) {
  if (name.find_first_of("/\\:") != std::string::npos || name.starts_with(".")) return false;
  std::vector<std::filesystem::path> roots = {std::filesystem::current_path()};
  wchar_t exe[MAX_PATH] = {};
  if (GetModuleFileNameW(nullptr, exe, MAX_PATH))
    for (std::filesystem::path p = std::filesystem::path(exe).parent_path(); p.has_relative_path(); p = p.parent_path())
      roots.push_back(p);
  for (const std::filesystem::path& root : roots) {
    std::ifstream in(root / "tests" / "fuzz" / "seeds" / "picture" / name, std::ios::binary);
    if (!in) continue;
    bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return !bytes.empty();
  }
  return false;
}

// A 4 x 3 red PNG: the picture inserted when no seed file is named, and a picture watermark.
const std::vector<uint8_t>& RedPng() {
  static const std::vector<uint8_t> png = {
      0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00,
      0x04, 0x00, 0x00, 0x00, 0x03, 0x08, 0x02, 0x00, 0x00, 0x00, 0x3B, 0x96, 0x39, 0x91, 0x00, 0x00, 0x00, 0x10, 0x49,
      0x44, 0x41, 0x54, 0x78, 0xDA, 0x63, 0x38, 0xA1, 0xA1, 0x01, 0x47, 0x0C, 0x38, 0x39, 0x00, 0x07, 0xA6, 0x0D, 0x21,
      0x7C, 0xB1, 0xBC, 0x73, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};
  return png;
}

// Saves as the app does (DocFile::Save): docx::Serialize writes the changed parts from the model,
// updates the core properties when something was edited, saves the package and checks it opens
// again. With nothing edited it is the file exactly as it was opened. The time is fixed, so that a
// replay gives the same bytes.
fw::pkg::Status SaveParts(fw::doc::Document& doc, const std::vector<uint32_t>& parts, bool edited,
                          std::vector<uint8_t>& out, std::vector<std::string>& written) {
  written.clear();
  for (uint32_t part : parts) written.push_back(doc.Parts()[part].name);
  fw::doc::docx::SaveOptions options;
  options.changedParts = parts;
  options.edited = edited;
  options.author = "fwtool torture";
  options.now = 134116992000000000LL;  // 2026-01-01T00:00:00Z
  return fw::doc::docx::Serialize(doc, options, out);
}

#if FW_TORTURE_EDITOR
// The editor (D2). Edits whose positions don't exist in the document (from a shrunk or replayed
// list) are refused here, so that the editor only sees edits a person could make.
class EditorTarget : public EditTarget {
 public:
  explicit EditorTarget(std::unique_ptr<fw::doc::Document> doc) : doc_(std::move(doc)), editor_(*doc_) {
#if FW_TORTURE_D4
    // Tracked changes and comments by one author at one moment: a replay makes the same file.
    editor_.SetAuthor("Torture");
    editor_.SetFixedDate("2026-01-01T00:00:00Z");
#endif
  }
  const char* Name() const override { return "the editor (doc/edit/Editor.h)"; }
  const fw::doc::Document& Doc() const override { return *doc_; }

  Applied Apply(const Edit& e, std::string& why) override {
    if (e.kind == EditKind::Group) {  // the next e.count edits, one undo step
      if (grouping_) return why = "a group is open already", Applied::Refused;
      editor_.BeginGroup(u"Torture");
      grouping_ = e.count;
      return Applied::Done;
    }
    if (e.kind == EditKind::Undo || e.kind == EditKind::Redo) CloseGroup();  // as the app does: undo ends a group
    Applied done = ApplyOne(e, why);
    if (grouping_ && --grouping_ == 0) editor_.EndGroup();
    return done;
  }

  void CloseGroup() {
    if (grouping_) editor_.EndGroup();
    grouping_ = 0;
  }

  // A place a person can put the caret: in the paragraph, and not between the halves of a
  // surrogate pair (an emoji), which no selection can split.
  bool Exists(fw::doc::Pos p) const {
    if (p.para >= doc_->Paragraphs().size() || p.offset > editor_.ParaLength(p.para)) return false;
    const std::u16string& text = doc_->Para(p.para).text;
    return !(p.offset > 0 && p.offset < text.size() && text[p.offset - 1] >= 0xD800 && text[p.offset - 1] <= 0xDBFF &&
             text[p.offset] >= 0xDC00 && text[p.offset] <= 0xDFFF);
  }

  Applied ApplyOne(const Edit& e, std::string& why) {
    namespace ed = fw::doc::edit;
    auto exists = [&](fw::doc::Pos p) { return Exists(p); };
    auto refuse = [&](const std::string& reason) {
      why = reason;
      return Applied::Refused;
    };
    ed::Result r;
    switch (e.kind) {
      case EditKind::Type:
        if (!exists(e.at)) return refuse("no such position");
        r = editor_.InsertText(e.at, e.text);
        break;
      case EditKind::Delete:
        if (!exists(e.at) || !exists(e.to) || editor_.Compare(e.at, e.to) > 0) return refuse("no such range");
        r = editor_.Delete(e.at, e.to);
        break;
      case EditKind::Replace:
        if (!exists(e.at) || !exists(e.to) || editor_.Compare(e.at, e.to) > 0) return refuse("no such range");
        r = editor_.Replace(e.at, e.to, e.text);
        break;
      case EditKind::Split:
        if (!exists(e.at)) return refuse("no such position");
        r = editor_.SplitParagraph(e.at);
        break;
      case EditKind::Join:
        if (e.at.para >= doc_->Paragraphs().size()) return refuse("no such paragraph");
        r = editor_.JoinParagraphs(e.at.para);
        break;
      case EditKind::Format: {
        if (!exists(e.at) || !exists(e.to) || editor_.Compare(e.at, e.to) > 0) return refuse("no such range");
        bool mark = e.format.starts_with("mark:");  // the paragraph marks' run properties
        std::string format = mark ? e.format.substr(5) : e.format;
        size_t eq = format.find('=');
        std::string name = format.substr(0, eq), value = eq == std::string::npos ? "" : format.substr(eq + 1);
        bool needsValue = name == "align" || name == "style" || name == "size" || name == "color" || name == "highlight";
        if (needsValue == value.empty()) return refuse("format " + name + (needsValue ? " needs =value" : " takes no value"));
        ed::PropEdit props;
        if (mark && (name == "align" || name == "style")) return refuse("a paragraph format isn't a mark's");
        if (name == "align") {
          r = editor_.SetParaProps(e.at.para, e.to.para, props.Set("jc", {{"val", value}}));
        } else if (name == "style") {
          r = editor_.SetParaProps(e.at.para, e.to.para, props.Set("pStyle", {{"val", value}}));
        } else {
          if (name == "bold") props.Set("b");
          else if (name == "italic") props.Set("i");
          else if (name == "strike") props.Set("strike");
          else if (name == "underline") props.Set("u", {{"val", "single"}});
          else if (name == "size") props.Set("sz", {{"val", value}});
          else if (name == "color") props.Set("color", {{"val", value}});
          else if (name == "highlight") props.Set("highlight", {{"val", value}});
          else if (name == "plain") props.Remove("b").Remove("i").Remove("u");
          else return refuse("unknown format");
          r = mark ? editor_.SetMarkProps(e.at.para, e.to.para, props) : editor_.SetRunProps(e.at, e.to, props);
        }
        break;
      }
      case EditKind::Command:
      case EditKind::Paint: {
        if (!exists(e.at) || !exists(e.to) || editor_.Compare(e.at, e.to) > 0) return refuse("no such range");
        if (doc_->Para(e.at.para).story != doc_->Para(e.to.para).story) return refuse("a range is in one story");
        if (e.kind == EditKind::Paint && !exists(e.from)) return refuse("no such position to copy from");
        std::u16string whyNot;
        if (!editor_.CanEdit(&whyNot)) return refuse(fw::doc::ToUtf8(whyNot));
        return e.kind == EditKind::Paint ? Paint(e, why) : Command(e, why);
      }
      case EditKind::Save:
        return refuse("saves are made by the round");
      case EditKind::Group:
        return refuse("groups are opened by Apply");
      case EditKind::Undo:
      case EditKind::Redo:
        for (uint32_t i = 0; i < e.count; ++i) {
          bool undo = e.kind == EditKind::Undo;
          if (undo ? !editor_.CanUndo() : !editor_.CanRedo()) return i ? Applied::Done : refuse("nothing to undo or redo");
          if (!(r = undo ? editor_.Undo() : editor_.Redo())) break;
        }
        break;
    }
    if (!r) {
      why = fw::doc::ToUtf8(r.message);
      return Applied::Refused;
    }
    return Applied::Done;
  }

  // As the Word tab saves (DocView::Save → DocFile::Save): the editor's changed parts, and
  // "edited" while the document differs from the file.
  fw::pkg::Status Save(std::vector<uint8_t>& out) override {
    CloseGroup();
    return SaveParts(*doc_, editor_.DirtyParts(), editor_.Modified(), out, written);
  }
  void Saved() override { editor_.MarkSaved(); }
  std::string State() const override {
    std::string s = std::to_string(doc_->Paragraphs().size()) + " paragraphs, " + std::to_string(doc_->Sections().size()) +
                    " sections, " + std::to_string(doc_->Package().Parts().size()) + " parts" +
                    (editor_.Modified() ? ", modified" : "") + (editor_.CanUndo() ? ", can undo" : "") + ", changed parts:";
    for (uint32_t p : editor_.DirtyParts()) s += " " + std::to_string(p);
    return s;
  }

 private:
  // A formatting command as the Word tab runs it (DocFormat::Run): one TreeEdit, the command's own
  // transaction, the same range for characters and paragraphs.
  Applied Command(const Edit& e, std::string& why) {
    namespace ed = fw::doc::edit;
#if FW_TORTURE_CLIP
    if (e.format.starts_with("paste=") || e.format.starts_with("pastecells=")) return Paste(e, why);
#endif
#if FW_TORTURE_CASE
    if (e.format.starts_with("case=")) {  // Change Case (Aa, Shift+F3), an editor edit of its own
      static const std::pair<const char*, ed::TextCase> cases[] = {
          {"sentence", ed::TextCase::Sentence}, {"lower", ed::TextCase::Lower}, {"upper", ed::TextCase::Upper},
          {"capitalize", ed::TextCase::Capitalize}, {"toggle", ed::TextCase::Toggle}};
      auto c = std::find_if(std::begin(cases), std::end(cases), [&](const auto& p) { return e.format.substr(5) == p.first; });
      if (c == std::end(cases)) return why = "unknown case", Applied::Refused;
      const ed::Result r = ed::ChangeCase(editor_, e.at, e.to, c->second);
      if (!r) return why = fw::doc::ToUtf8(r.message), Applied::Refused;
      return Applied::Done;
    }
#endif
#if FW_TORTURE_D4
    if (std::optional<Applied> d4 = D4(e, why)) return *d4;
#endif
#if FW_TORTURE_D3
    if (std::optional<Applied> d3 = D3(e, why)) return *d3;
#endif
    const ed::Range range{e.at, e.to};
    const size_t eq = e.format.find('=');
    const std::string name = e.format.substr(0, eq), value = eq == std::string::npos ? "" : e.format.substr(eq + 1);
    ed::TreeEdit te(editor_, range.from);
    ed::RunFormat run;
    ed::ParaFormat para;
    if (name == "bullets") ed::ToggleList(te, range, ed::ListKind::Bullet);
    else if (name == "numbering") ed::ToggleList(te, range, ed::ListKind::Number);
    else if (name == "multilevel") ed::ToggleList(te, range, ed::ListKind::Multilevel);
    else if (name == "nolist") ed::RemoveList(te, range);
    else if (name == "restart") ed::RestartNumbering(te, range.from.para);
    else if (name == "continue") ed::ContinueNumbering(te, range.from.para);
    else if (name == "indent" || name == "outdent") ed::ShiftIndent(te, range, name == "indent");
    else if (name == "apply" && !value.empty()) ed::ApplyStyle(te, range, value);
    else if (name == "update") ed::UpdateStyleToMatch(te, range);
    else if (name == "modify" && !value.empty()) {
      run.italic = true;
      para.spaceAfter = fw::Twips(120);
      ed::ModifyStyle(te, value, run, para);
    } else if (name == "toggle") {
      static const std::pair<const char*, ed::Toggle> toggles[] = {
          {"bold", ed::Toggle::Bold},     {"italic", ed::Toggle::Italic},           {"underline", ed::Toggle::Underline},
          {"strike", ed::Toggle::Strike}, {"super", ed::Toggle::Superscript}, {"sub", ed::Toggle::Subscript}};
      auto t = std::find_if(std::begin(toggles), std::end(toggles), [&](const auto& p) { return value == p.first; });
      if (t == std::end(toggles)) return why = "unknown toggle", Applied::Refused;
      ed::ToggleRuns(te, range, t->second);
    } else if (name == "grow" || name == "shrink" || name == "grow1" || name == "shrink1") {
      ed::GrowFont(te, range, name.starts_with("grow"), name.ends_with("1"));
    } else if (name == "clearchars") ed::ClearRunFormatting(te, range);
    else if (name == "clearpara") ed::ClearParaFormatting(te, range);
    else if (name == "clearall") ed::ClearAllFormatting(te, range);
    else if (name == "font" && !value.empty()) {
      run.font = value;
      ed::FormatRuns(te, range, run);
    } else if (name == "spacing" && !value.empty()) {
      para.line = std::atoi(value.c_str());
      para.lineRule = fw::doc::LineRule::Auto;
      ed::FormatParagraphs(te, range, para);
    }
#if FW_TORTURE_THEME
    else if (name == "newstyle" && !value.empty()) {  // New style from the selection
      std::u16string whyNot;
      if (ed::NewStyleFromSelection(te, range, value, whyNot).empty())
        return why = fw::doc::ToUtf8(whyNot.empty() ? te.Refused() : whyNot), Applied::Refused;
    } else if (name == "themefont" && !value.empty()) {  // "+Headings" or "+Body" in the font box
      run.themeFont = value == "headings" ? ed::ThemeFontSlot::Headings : ed::ThemeFontSlot::Body;
      ed::FormatRuns(te, range, run);
    } else if (name == "themecolor" && !value.empty()) {  // "<name>[:tint:shade]", hex 00-FF or -1
      const size_t c1 = value.find(':'), c2 = c1 == std::string::npos ? c1 : value.find(':', c1 + 1);
      ed::ThemeColorRef color;
      color.name = value.substr(0, c1);
      if (c1 != std::string::npos) color.tint = static_cast<int>(std::strtol(value.c_str() + c1 + 1, nullptr, 16));
      if (c2 != std::string::npos) color.shade = static_cast<int>(std::strtol(value.c_str() + c2 + 1, nullptr, 16));
      run.themeColor = color;
      ed::FormatRuns(te, range, run);
    }
#endif
    else if (name == "para" && !value.empty()) {
      // The Paragraph panel (FormatParagraphs): "para=<field>:<value>", one field at a time. Line
      // spacing at least, exactly (twips) or auto (240ths); indents and the first line (negative:
      // hanging); space before and after; keep with next, keep lines together, page break before,
      // widow control and contextual spacing (1 or 0).
      const size_t c = value.find(':');
      const std::string field = value.substr(0, c);
      const int n = c == std::string::npos ? 0 : std::atoi(value.c_str() + c + 1);
      if (field == "atleast" || field == "exact" || field == "auto") {
        para.lineRule = field == "atleast" ? fw::doc::LineRule::AtLeast : field == "exact" ? fw::doc::LineRule::Exact : fw::doc::LineRule::Auto;
        para.line = n;
      } else if (field == "indstart") para.indStart = fw::Twips(n);
      else if (field == "indend") para.indEnd = fw::Twips(n);
      else if (field == "firstline") para.firstLine = fw::Twips(n);
      else if (field == "before") para.spaceBefore = fw::Twips(n);
      else if (field == "after") para.spaceAfter = fw::Twips(n);
      else if (field == "keepnext") para.keepNext = n != 0;
      else if (field == "keeplines") para.keepLines = n != 0;
      else if (field == "pagebreak") para.pageBreakBefore = n != 0;
      else if (field == "widow") para.widowControl = n != 0;
      else if (field == "contextual") para.contextualSpacing = n != 0;
      else return why = "unknown paragraph field", Applied::Refused;
      ed::FormatParagraphs(te, range, para);
    } else if (name == "spacebefore") {
      para.spaceBefore = fw::Twips(te.Doc().ResolvedPara(range.from.para).spaceBefore.Value() == 0 ? 240 : 0);
      ed::FormatParagraphs(te, range, para);
    } else if (name == "border") {
      para.borders = value == "box" ? ed::BorderSides::Outside : value == "bottom" ? ed::BorderSides::Bottom : ed::BorderSides::None;
      ed::FormatParagraphs(te, range, para);
    }
#if FW_TORTURE_PAGE
    else if (name == "tabs" && !value.empty()) {
      // The ruler's tab stops: "tabs=<twips>:<start|center|end|decimal|bar>[:dot]" sets one there,
      // "tabs=<twips>:<align>:remove" takes away the one there.
      static const std::pair<const char*, fw::doc::TabAlign> aligns[] = {
          {"start", fw::doc::TabAlign::Start}, {"center", fw::doc::TabAlign::Center}, {"end", fw::doc::TabAlign::End},
          {"decimal", fw::doc::TabAlign::Decimal}, {"bar", fw::doc::TabAlign::Bar}};
      const size_t c1 = value.find(':'), c2 = c1 == std::string::npos ? c1 : value.find(':', c1 + 1);
      const std::string align = c1 == std::string::npos ? "start" : value.substr(c1 + 1, c2 == std::string::npos ? c2 : c2 - c1 - 1);
      const std::string more = c2 == std::string::npos ? "" : value.substr(c2 + 1);
      auto a = std::find_if(std::begin(aligns), std::end(aligns), [&](const auto& p) { return align == p.first; });
      if (a == std::end(aligns)) return why = "unknown tab alignment", Applied::Refused;
      const fw::Twips pos(std::atoi(value.c_str()));
      if (more == "remove") {
        ed::ChangeTabStops(te, range, {pos}, {});
      } else {
        fw::doc::TabStop stop;
        stop.pos = pos;
        stop.align = a->second;
        if (more == "dot") stop.leader = fw::doc::TabLeader::Dot;
        else if (more == "hyphen") stop.leader = fw::doc::TabLeader::Hyphen;
        else if (more == "underscore") stop.leader = fw::doc::TabLeader::Underscore;
        else if (more == "heavy") stop.leader = fw::doc::TabLeader::Heavy;
        else if (more == "middledot") stop.leader = fw::doc::TabLeader::MiddleDot;
        ed::ChangeTabStops(te, range, {}, {stop});
      }
    }
#endif
    else {
      return why = "unknown command", Applied::Refused;
    }
    // TreeEdit puts back a change to text it doesn't edit (a text box, kept content) and says why.
    if (!te.Refused().empty()) return why = fw::doc::ToUtf8(te.Refused()), Applied::Refused;
    return Applied::Done;
  }

#if FW_TORTURE_CLIP
  // Copy and paste as the Word tab does: what's copied from [at, to) ("paste") or the block of cells
  // between them ("pastecells") goes through the clipboard's HTML (Featherweight's own, as it pastes
  // its own copies), its RTF (as from another program) or as it is, and is pasted at P:O, or over
  // [P:O, Q:R). Spec: "paste=<html|rtf|clip>:P:O[:Q:R]". Pasting is one undo step.
  Applied Paste(const Edit& e, std::string& why) {
    namespace ed = fw::doc::edit;
    constexpr uint64_t kSource = 0x46575454;  // this document, for lists pasted back into it
    std::vector<std::string> args;
    const std::string value = e.format.substr(e.format.find('=') + 1);
    for (size_t start = 0; start <= value.size();) {
      size_t colon = std::min(value.find(':', start), value.size());
      args.push_back(value.substr(start, colon - start));
      start = colon + 1;
    }
    auto refuse = [&](std::string reason) {
      why = std::move(reason);
      return Applied::Refused;
    };
    if (args.size() != 3 && args.size() != 5) return refuse("paste=<via>:P:O[:Q:R]");
    auto num = [&](size_t i) { return static_cast<uint32_t>(std::strtoul(args[i].c_str(), nullptr, 10)); };
    const fw::doc::Pos at{num(1), num(2)}, to = args.size() == 5 ? fw::doc::Pos{num(3), num(4)} : at;
    if (!Exists(at) || !Exists(to) || editor_.Compare(at, to) > 0) return refuse("no such place to paste");
    if (doc_->Para(at.para).story != doc_->Para(to.para).story) return refuse("a range is in one story");
    std::optional<ed::Clip> clip;
#if FW_TORTURE_PAGE
    if (e.format.starts_with("pastecells=")) {
      clip = ed::CopyCells(editor_, e.at, e.to, kSource);
      if (!clip) return refuse("not cells of one table");
    } else
#endif
      clip = ed::CopyClip(*doc_, e.at, e.to, kSource);
    if (clip->Empty()) return refuse("nothing copied");
    const std::string& via = args[0];
    if (via == "html") clip = ed::FromHtml(ed::ToHtml(*clip));
    else if (via == "rtf") clip = ed::FromRtf(ed::ToRtf(*clip));
    else if (via != "clip") return refuse("unknown clipboard format");
    if (!clip) return refuse("the clipboard's " + via + " didn't read back");
    const ed::Result r = ed::PasteClip(editor_, at, to, *clip, kSource);
    if (!r) return refuse(fw::doc::ToUtf8(r.message));
    return Applied::Done;
  }
#endif

  // The format painter (DocFormat::Capture and PaintTo): the formatting of the character at
  // e.from and of its paragraph, painted onto [at, to). Paragraph formatting goes with whole
  // paragraphs only.
  Applied Paint(const Edit& e, std::string& why) {
    namespace ed = fw::doc::edit;
    const fw::doc::Document& doc = *doc_;
    ed::Range one{e.from, e.from};
    if (e.from.offset < doc.Para(e.from.para).text.size()) one.to = {e.from.para, e.from.offset + 1};
    const ed::FormatState s = ed::StateOf(doc, one);
    ed::RunFormat run;
    run.font = s.font;
    run.size = s.size;
    run.bold = s.bold;
    run.italic = s.italic;
    run.strike = s.strike;
    run.underline = s.underline ? fw::doc::Underline::Single : fw::doc::Underline::None;
    run.vertAlign = s.superscript ? fw::doc::VertAlign::Superscript
                    : s.subscript ? fw::doc::VertAlign::Subscript
                                  : fw::doc::VertAlign::Baseline;
    if (s.color) run.color = *s.color;
    if (s.highlight) run.highlight = s.highlight->present ? NearestHighlight(s.highlight->rgb) : std::string("none");
    run.charStyle = s.charStyle.value_or(std::string());
    const fw::doc::ParaProps& pp = doc.ResolvedPara(e.from.para);
    const std::string style(pp.style);
    ed::ParaFormat para;
    para.jc = pp.jc;
    para.indStart = pp.indStart;
    para.indEnd = pp.indEnd;
    para.firstLine = pp.firstLine;
    para.spaceBefore = pp.spaceBefore;
    para.spaceAfter = pp.spaceAfter;
    para.line = pp.line;
    para.lineRule = pp.lineRule;
    const ed::Range range{e.at, e.to};
    const bool whole = e.at.offset == 0 && (e.to.para > e.at.para || e.to.offset >= doc.Para(e.at.para).text.size());
    std::u16string refused;
    editor_.BeginGroup(u"Format Painter");
    {
      ed::TreeEdit te(editor_, range.from);
      if (whole) {
        if (!style.empty()) ed::ApplyStyle(te, range, style);
        ed::FormatParagraphs(te, range, para);
      }
      ed::FormatRuns(te, range, run);
      refused = te.Refused();
    }
    editor_.EndGroup();
    if (!refused.empty()) return why = fw::doc::ToUtf8(refused), Applied::Refused;
    return Applied::Done;
  }

#if FW_TORTURE_D3
  // D3's operations, as the Word tab calls them: tables (Editor.h), pictures (Pictures.h), fields,
  // links and bookmarks (Inline.h), headers, footers and page numbers (HeaderFooter.h). The spec's
  // arguments are separated by ':'. nullopt: not one of them.
  std::optional<Applied> D3(const Edit& e, std::string& why) {
    namespace ed = fw::doc::edit;
    const size_t eq = e.format.find('=');
    const std::string name = e.format.substr(0, eq), value = eq == std::string::npos ? "" : e.format.substr(eq + 1);
    std::vector<std::string> args;
    for (size_t start = 0; start <= value.size();) {
      size_t colon = std::min(value.find(':', start), value.size());
      args.push_back(value.substr(start, colon - start));
      start = colon + 1;
    }
    auto num = [&](size_t i, int def = 0) { return i < args.size() && !args[i].empty() ? std::atoi(args[i].c_str()) : def; };
    auto arg = [&](size_t i) { return i < args.size() ? args[i] : std::string(); };
    auto grid = [&](uint32_t& rows, uint32_t& cols) {  // "RxC"
      const size_t x = value.find('x');
      rows = static_cast<uint32_t>(std::atoi(value.c_str()));
      cols = x == std::string::npos ? 1 : static_cast<uint32_t>(std::atoi(value.c_str() + x + 1));
    };
    auto refuse = [&](std::string reason) {
      why = std::move(reason);
      return Applied::Refused;
    };
    const auto type = [&](size_t i) { return static_cast<fw::doc::HdrFtrType>(std::clamp(num(i), 0, 2)); };
    const auto section = [&](size_t i) { return static_cast<uint32_t>(num(i)); };
    const bool sectionOk = section(0) < doc_->Sections().size();
    ed::Result r;
    ed::PropEdit props;
    uint32_t rows = 1, cols = 1;
    // Tables
    if (name == "table") grid(rows, cols), r = editor_.InsertTable(e.at, rows, cols);
    else if (name == "texttotable") r = editor_.TextToTable(e.at.para, e.to.para);
    else if (name == "tabletotext") r = editor_.TableToText(e.at);
    else if (name == "deletetable") r = editor_.DeleteTable(e.at);
    else if (name == "rows") r = editor_.InsertRows(e.at, e.to, value == "below");
    else if (name == "cols") r = editor_.InsertColumns(e.at, e.to, value == "right");
    else if (name == "deleterows") r = editor_.DeleteRows(e.at, e.to);
    else if (name == "deletecols") r = editor_.DeleteColumns(e.at, e.to);
    else if (name == "clearcells") r = editor_.ClearCells(e.at, e.to);
    else if (name == "merge") r = editor_.MergeCells(e.at, e.to);
    else if (name == "splitcell") grid(rows, cols), r = editor_.SplitCell(e.at, rows, cols);
    else if (name == "colwidth") r = editor_.SetColumnWidth(e.at, e.to, num(0, 1440));
    else if (name == "colborder") r = editor_.MoveColumnBorder(e.at, num(0), num(1));
    else if (name == "rowheight") r = editor_.SetRowHeight(e.at, e.to, num(0), arg(1).empty() ? "atLeast" : arg(1));
    else if (name == "autofit") r = editor_.AutoFit(e.at, value == "fixed" ? fw::doc::edit::Editor::Fit::Fixed : fw::doc::edit::Editor::Fit::Window);
    else if (name == "cellborders") {
      static const std::pair<const char*, ed::Editor::CellBorders> kinds[] = {
          {"all", ed::Editor::CellBorders::All},         {"none", ed::Editor::CellBorders::None},
          {"outside", ed::Editor::CellBorders::Outside}, {"inside", ed::Editor::CellBorders::Inside},
          {"top", ed::Editor::CellBorders::Top},         {"bottom", ed::Editor::CellBorders::Bottom},
          {"left", ed::Editor::CellBorders::Left},       {"right", ed::Editor::CellBorders::Right}};
      auto k = std::find_if(std::begin(kinds), std::end(kinds), [&](const auto& p) { return value == p.first; });
      if (k == std::end(kinds)) return refuse("unknown borders");
      r = editor_.SetCellBorders(e.at, e.to, k->second);
    } else if (name == "cellshade") {
      r = editor_.SetCellProps(e.at, e.to, props.Set("shd", {{"val", "clear"}, {"color", "auto"}, {"fill", value}}));
    } else if (name == "headerrow") {
      r = editor_.SetRowProps(e.at, e.to, props.Set("tblHeader"));
    } else if (name == "tablealign") {
      r = editor_.SetTableProps(e.at, props.Set("jc", {{"val", value}}));
    }
    // Pictures: inserted here; changed by their index in Document::Drawings()
    else if (name == "picture") {
      if (value.empty()) {
        r = pictures_.Insert(e.at, RedPng(), fw::Emu(0), u"torture.png");
      } else {  // "picture=<file>": one of tests/fuzz/seeds/picture (each format Insert takes, and one it doesn't)
        std::vector<uint8_t> bytes;
        if (!SeedPicture(value, bytes)) return refuse("no picture " + value + " in tests/fuzz/seeds/picture");
        r = pictures_.Insert(e.at, bytes, fw::Emu(0), fw::doc::ToUtf16(value));
      }
    } else if (name.starts_with("pic")) {
      const uint32_t d = static_cast<uint32_t>(num(0));
      if (d >= doc_->Drawings().size()) return refuse("no such drawing");
      const ed::PictureInfo info = pictures_.Info(d);
      if (!info.picture) return refuse("not a picture: " + fw::doc::ToUtf8(info.why));
      if (name == "picsize") {
        const int64_t pct = num(1, 50);
        r = pictures_.Resize(d, fw::Emu(info.cx.Value() * pct / 100), fw::Emu(info.cy.Value() * pct / 100));
      } else if (name == "picrotate") {
        r = pictures_.Rotate(d, num(1));
      } else if (name == "piccrop") {
        ed::Crop crop;
        crop.left = crop.top = num(1) / 100.0;
        r = pictures_.SetCrop(d, crop);
      } else if (name == "picwrap") {
        static const std::pair<const char*, ed::Wrap> wraps[] = {
            {"inline", ed::Wrap::Inline},   {"square", ed::Wrap::Square}, {"tight", ed::Wrap::Tight},
            {"topbottom", ed::Wrap::TopAndBottom}, {"behind", ed::Wrap::Behind}, {"front", ed::Wrap::InFront}};
        auto w = std::find_if(std::begin(wraps), std::end(wraps), [&](const auto& p) { return arg(1) == p.first; });
        if (w == std::end(wraps)) return refuse("unknown wrap");
        r = pictures_.SetWrap(d, w->second, fw::Emu(914400), fw::Emu(914400));
      } else if (name == "picmove") {
        r = info.wrap == ed::Wrap::Inline ? pictures_.MoveInline(d, e.at)
                                          : pictures_.MoveFloating(d, fw::Emu(num(1) * 12700LL), fw::Emu(num(2) * 12700LL));
      } else if (name == "picalt") {
        r = pictures_.SetAltText(d, u"A picture from the torture test");
      } else if (name == "picreset") {
        r = pictures_.ResetSize(d);
      } else {
        return refuse("unknown picture change");
      }
    }
    // Fields, links and bookmarks
    else if (name == "field") {
      const std::string instr = value == "DATE" ? " DATE \\@ \"yyyy-MM-dd\" " : " " + value + " ";
      r = ed::InsertField(editor_, e.at, instr, value == "DATE" ? u"2026-01-01" : value == "AUTHOR" ? u"Torture" : u"1");
    } else if (name == "link") {
      r = ed::InsertLink(editor_, e.at, e.to, value, u"a link");
    } else if (name == "editlink") {
      r = ed::EditLink(editor_, e.at, "https://example.org/changed", u"a changed link");
    } else if (name == "unlink") {
      r = ed::RemoveLink(editor_, e.at);
    } else if (name == "bookmark") {
      r = ed::InsertBookmark(editor_, e.at, e.to, value);
    } else if (name == "delbookmark") {
      r = ed::DeleteBookmark(editor_, value);
    } else if (name == "parabookmark") {  // as a link to a heading makes one
      editor_.BeginGroup(u"Link");
      const std::string made = ed::BookmarkForParagraph(editor_, e.at.para);
      editor_.EndGroup();
      if (made.empty()) return refuse("no bookmark for the paragraph");
    }
    // Headers, footers and page numbers, by section
    else if (name == "header" || name == "footer") {
      if (!sectionOk) return refuse("no such section");
      std::u16string whyNot;
      if (ed::EnsureHeaderFooter(editor_, section(0), name == "footer", type(1), whyNot) == fw::doc::kNone)
        return refuse(fw::doc::ToUtf8(whyNot));
    } else if (name == "linkprev") {
      if (!sectionOk) return refuse("no such section");
      r = ed::SetLinkToPrevious(editor_, section(0), num(1) != 0, type(2), num(3) != 0);
    } else if (name == "firstpage") {
      if (!sectionOk) return refuse("no such section");
      r = ed::SetDifferentFirstPage(editor_, section(0), num(1) != 0);
    } else if (name == "oddeven") {
      r = ed::SetDifferentOddEven(editor_, value == "on");
    } else if (name == "pagenumbering") {
      if (!sectionOk) return refuse("no such section");
      r = ed::SetPageNumbering(editor_, section(0), arg(1), args.size() > 2 ? std::optional<int32_t>(num(2)) : std::nullopt);
    } else if (name == "pagenumber") {
      if (!sectionOk) return refuse("no such section");
      const fw::doc::Justification align = num(3) == 0 ? fw::doc::Justification::Start
                                           : num(3) == 1 ? fw::doc::Justification::Center
                                                         : fw::doc::Justification::End;
      r = ed::InsertPageNumber(editor_, section(0), num(1) != 0, type(2), align, num(4) != 0);
    }
#if FW_TORTURE_PAGE
    // Page layout (Page.h), by section; breaks at e.at; table styles (TableStyles.h)
    else if (name == "pagesetup") {  // section:landscape|portrait|margins:twips|size:WxH|header:twips
      if (!sectionOk) return refuse("no such section");
      ed::PageSetup setup;
      if (arg(1) == "landscape" || arg(1) == "portrait") setup.landscape = arg(1) == "landscape";
      else if (arg(1) == "margins") setup.top = setup.bottom = setup.left = setup.right = fw::Twips(num(2, 720));
      else if (arg(1) == "size") {
        const size_t x = arg(2).find('x');
        setup.width = fw::Twips(std::atoi(arg(2).c_str()));
        setup.height = fw::Twips(x == std::string::npos ? 15840 : std::atoi(arg(2).c_str() + x + 1));
      } else if (arg(1) == "header") setup.header = setup.footer = fw::Twips(num(2, 360));
      else return refuse("unknown page setup");
      r = ed::SetPageSetup(editor_, section(0), section(0), setup);
    } else if (name == "columns") {  // section:count:space:line
      if (!sectionOk) return refuse("no such section");
      ed::ColumnSetup columns;
      columns.count = num(1, 2);
      columns.space = fw::Twips(num(2, 720));
      columns.line = num(3) != 0;
      r = ed::SetColumns(editor_, section(0), section(0), columns);
    } else if (name == "sectionstart" || name == "sectionbreak") {
      static const std::pair<const char*, fw::doc::SectionStart> starts[] = {
          {"nextPage", fw::doc::SectionStart::NextPage}, {"continuous", fw::doc::SectionStart::Continuous},
          {"evenPage", fw::doc::SectionStart::EvenPage}, {"oddPage", fw::doc::SectionStart::OddPage}};
      const std::string which = name == "sectionstart" ? arg(1) : value;
      auto s = std::find_if(std::begin(starts), std::end(starts), [&](const auto& p) { return which == p.first; });
      if (s == std::end(starts)) return refuse("unknown section start");
      if (name == "sectionbreak") r = ed::InsertSectionBreak(editor_, e.at, s->second);
      else if (!sectionOk) return refuse("no such section");
      else r = ed::SetSectionStart(editor_, section(0), s->second);
    } else if (name == "delsectionbreak") {
      if (!sectionOk) return refuse("no such section");
      r = ed::DeleteSectionBreak(editor_, section(0));
    } else if (name == "pagebreak" || name == "columnbreak") {
      r = ed::InsertBreak(editor_, e.at, name == "columnbreak");
    } else if (name == "hyphenation") {
      ed::Hyphenation h;
      h.automatic = value == "on";
      if (value == "on") h.zone = fw::Twips(360), h.limit = 2;
      r = ed::SetHyphenation(editor_, h);
    } else if (name == "tablestyle") {
      r = ed::ApplyTableStyle(editor_, e.at, value);
    }
#endif
    else {
      return std::nullopt;
    }
    if (!r) return refuse(fw::doc::ToUtf8(r.message));
    return Applied::Done;
  }
#endif

#if FW_TORTURE_D4
  // D4's operations. Forms at e.at: the content control there (ControlAt) or the legacy form
  // field (FormFieldAt). Comments by id, notes by kind and id, shapes by their index in
  // Document::Drawings(). Where the model contradicts itself (Tab picks a range that isn't
  // there) the edit is refused with "wrong: ", which fails the round.
  std::optional<Applied> D4(const Edit& e, std::string& why) {
    namespace ed = fw::doc::edit;
    using fw::doc::ToUtf16;
    using fw::doc::ToUtf8;
    const size_t eq = e.format.find('=');
    const std::string name = e.format.substr(0, eq), value = eq == std::string::npos ? "" : e.format.substr(eq + 1);
    std::vector<std::string> args;
    for (size_t start = 0; start <= value.size();) {
      size_t colon = std::min(value.find(':', start), value.size());
      args.push_back(value.substr(start, colon - start));
      start = colon + 1;
    }
    auto num = [&](size_t i, int def = 0) { return i < args.size() && !args[i].empty() ? std::atoi(args[i].c_str()) : def; };
    auto arg = [&](size_t i) { return i < args.size() ? args[i] : std::string(); };
    auto refuse = [&](std::string reason) {
      why = std::move(reason);
      return Applied::Refused;
    };
    ed::Result r;
    // Forms: filling in a content control or legacy form field, inserting a control, Tab.
    if (name == "fill" || name == "placeholder" || name == "check" || name == "pickitem" || name == "pickdate") {
      const std::optional<ed::ContentControl> c = ed::ControlAt(*doc_, e.at);
      if (!c) return refuse("no content control here");
      if (name == "fill") r = ed::FillControl(editor_, *c, ToUtf16(value));
      else if (name == "placeholder") r = ed::ShowPlaceholder(editor_, *c);
      else if (name == "check") r = ed::SetChecked(editor_, *c, value == "1");
      else if (name == "pickitem") r = ed::PickItem(editor_, *c, static_cast<size_t>(num(0)));
      else {  // "pickdate=<year>:<month>:<day>"
        fw::doc::DateTime date;
        date.year = num(0, 2026), date.month = num(1, 1), date.day = num(2, 1);
        r = ed::PickDate(editor_, *c, date);
      }
    } else if (name == "formcheck" || name == "formentry") {
      const std::optional<ed::FormField> f = ed::FormFieldAt(*doc_, e.at);
      if (!f) return refuse("no form field here");
      r = name == "formcheck" ? ed::SetFormCheckBox(editor_, *f, value == "1")
                              : ed::PickFormEntry(editor_, *f, static_cast<size_t>(num(0)));
    } else if (name == "control") {  // "control=<kind>[:item:item...]"
      static const std::pair<const char*, ed::ControlKind> kinds[] = {
          {"richtext", ed::ControlKind::RichText}, {"plaintext", ed::ControlKind::PlainText},
          {"checkbox", ed::ControlKind::CheckBox}, {"dropdown", ed::ControlKind::DropDown},
          {"combobox", ed::ControlKind::ComboBox}, {"date", ed::ControlKind::Date}};
      auto k = std::find_if(std::begin(kinds), std::end(kinds), [&](const auto& p) { return arg(0) == p.first; });
      if (k == std::end(kinds)) return refuse("unknown content control");
      std::vector<std::u16string> items;
      for (size_t i = 1; i < args.size(); ++i) items.push_back(ToUtf16(args[i]));
      r = ed::InsertControl(editor_, e.at, k->second, items);
    } else if (name == "tabfill") {
      // Tab (Shift+Tab: "tabfill=back") in a form, then what a person does there: types over a
      // text, ticks a check box, picks the next item.
      const std::optional<ed::Range> next = ed::NextFillIn(*doc_, e.at, value == "back");
      if (!next) return refuse("nothing to fill in");
      if (!Exists(next->from) || !Exists(next->to) || editor_.Compare(next->from, next->to) > 0)
        return refuse("wrong: NextFillIn gave " + PosText(next->from) + " to " + PosText(next->to) + ", which isn't a range here");
      if (const std::optional<ed::ContentControl> c = ed::ControlAt(*doc_, next->from); c && !c->Typable()) {
        if (c->kind == ed::ControlKind::CheckBox) r = ed::SetChecked(editor_, *c, !c->checked);
        else if (c->kind == ed::ControlKind::DropDown) r = ed::PickItem(editor_, *c, c->items.empty() ? 0 : 1);
        else return refuse("a control that isn't filled in here");
      } else if (const std::optional<ed::FormField> f = ed::FormFieldAt(*doc_, next->from); f && f->kind != ed::FormFieldKind::Text) {
        r = f->kind == ed::FormFieldKind::CheckBox ? ed::SetFormCheckBox(editor_, *f, !f->checked)
                                                   : ed::PickFormEntry(editor_, *f, f->entries.size() > 1 ? 1 : 0);
      } else {
        r = editor_.Replace(next->from, next->to, u"Tabbed in");
      }
    }
    // Comments; their text by id.
    else if (name == "comment") r = ed::AddComment(editor_, e.at, e.to, ToUtf16(value));
    else if (name == "reply") r = ed::ReplyToComment(editor_, value, u"A reply from the torture test");
    else if (name == "editcomment") r = ed::EditComment(editor_, value, u"An edited comment\nin two paragraphs");
    else if (name == "delcomment") r = ed::DeleteComment(editor_, value);
    else if (name == "resolve") r = ed::ResolveComment(editor_, arg(0), num(1) != 0);
    // Footnotes and endnotes: "delnote=<f|e>:<id>".
    else if (name == "footnote" || name == "endnote") r = ed::InsertNote(editor_, e.at, name == "endnote");
    else if (name == "delnote") {
      const uint32_t story = doc_->NoteStory(arg(0) == "e" ? fw::doc::StoryKind::Endnote : fw::doc::StoryKind::Footnote, arg(1));
      if (story == fw::doc::kNone) return refuse("no such note");
      r = ed::DeleteNote(editor_, story);
    }
    // Track Changes, and accepting or rejecting what it recorded.
    else if (name == "track") r = editor_.SetTracking(value == "on");
    else if (name == "review") r = editor_.ReviewChanges(e.at, e.to, value == "accept");
    else if (name == "reviewall") r = editor_.ReviewAll(value == "accept");
    // Shapes: inserted at the start of e.at's paragraph; changed by drawing index.
    else if (name == "shape") {  // "shape=<kind>"
      static const std::pair<const char*, ed::ShapeKind> kinds[] = {
          {"textbox", ed::ShapeKind::TextBox}, {"rect", ed::ShapeKind::Rectangle},
          {"roundrect", ed::ShapeKind::RoundedRectangle}, {"oval", ed::ShapeKind::Oval},
          {"line", ed::ShapeKind::Line}, {"arrow", ed::ShapeKind::Arrow}, {"callout", ed::ShapeKind::Callout}};
      auto k = std::find_if(std::begin(kinds), std::end(kinds), [&](const auto& p) { return value == p.first; });
      if (k == std::end(kinds)) return refuse("unknown shape");
      r = shapes_.Insert(e.at.para, k->second, fw::Emu(914400), fw::Emu(914400), fw::Emu(1828800), fw::Emu(914400));
    } else if (name.starts_with("shape")) {  // "shape<change>=<drawing>[:...]"
      const uint32_t d = static_cast<uint32_t>(num(0));
      if (d >= doc_->Drawings().size()) return refuse("no such drawing");
      const ed::ShapeInfo info = shapes_.Info(d);
      if (!info.shape) return refuse("not a shape: " + ToUtf8(info.why));
      auto rgb = [&](size_t i) -> std::optional<uint32_t> {
        if (arg(i) == "none") return std::nullopt;
        return static_cast<uint32_t>(std::strtoul(arg(i).c_str(), nullptr, 16));
      };
      if (name == "shapesize") {
        const int64_t pct = num(1, 50);
        r = shapes_.Resize(d, fw::Emu(std::max<int64_t>(1, info.cx.Value() * pct / 100)),
                           fw::Emu(std::max<int64_t>(1, info.cy.Value() * pct / 100)));
      } else if (name == "shaperotate") {
        r = shapes_.Rotate(d, num(1));
      } else if (name == "shapemove") {
        r = shapes_.Move(d, fw::Emu(num(1) * 12700LL), fw::Emu(num(2) * 12700LL));
      } else if (name == "shapewrap") {
        static const std::pair<const char*, ed::Wrap> wraps[] = {
            {"inline", ed::Wrap::Inline},   {"square", ed::Wrap::Square}, {"tight", ed::Wrap::Tight},
            {"topbottom", ed::Wrap::TopAndBottom}, {"behind", ed::Wrap::Behind}, {"front", ed::Wrap::InFront}};
        auto w = std::find_if(std::begin(wraps), std::end(wraps), [&](const auto& p) { return arg(1) == p.first; });
        if (w == std::end(wraps)) return refuse("unknown wrap");
        r = shapes_.SetWrap(d, w->second, fw::Emu(914400), fw::Emu(914400));
      } else if (name == "shapefill") {
        r = shapes_.SetFill(d, rgb(1));
      } else if (name == "shapeline") {
        r = shapes_.SetOutline(d, rgb(1), num(2));
      } else if (name == "shapetext") {
        r = shapes_.AddText(d);
      } else {
        return refuse("unknown shape change");
      }
    }
    // Watermarks: "watermark=text" (DRAFT), "=<other text>", "=picture" or "=none".
    else if (name == "watermark") {
      if (value == "none") {
        r = ed::RemoveWatermark(editor_);
      } else if (value == "picture") {
        r = ed::SetPictureWatermark(editor_, RedPng());
      } else {
        ed::TextWatermark mark;
        if (value != "text" && !value.empty()) mark.text = ToUtf16(value), mark.diagonal = false;
        r = ed::SetTextWatermark(editor_, mark);
      }
    } else {
      return std::nullopt;
    }
    if (!r) return refuse(ToUtf8(r.message));
    return Applied::Done;
  }
#endif

  // Word's highlight color nearest to a color, as DocFormat picks it.
  static std::string NearestHighlight(uint32_t rgb) {
    static const std::pair<const char*, uint32_t> names[] = {
        {"yellow", 0xFFFF00},    {"green", 0x00FF00},       {"cyan", 0x00FFFF},     {"magenta", 0xFF00FF},
        {"blue", 0x0000FF},      {"red", 0xFF0000},         {"darkBlue", 0x000080}, {"darkCyan", 0x008080},
        {"darkGreen", 0x008000}, {"darkMagenta", 0x800080}, {"darkRed", 0x800000},  {"darkYellow", 0x808000},
        {"darkGray", 0x808080},  {"lightGray", 0xC0C0C0},   {"black", 0x000000},    {"white", 0xFFFFFF}};
    auto dist = [](uint32_t a, uint32_t b) {
      int dr = int(a >> 16 & 255) - int(b >> 16 & 255), dg = int(a >> 8 & 255) - int(b >> 8 & 255), db = int(a & 255) - int(b & 255);
      return dr * dr * 3 + dg * dg * 4 + db * db * 2;
    };
    const auto* best = &names[0];
    for (const auto& n : names)
      if (dist(n.second, rgb) < dist(best->second, rgb)) best = &n;
    return best->first;
  }

  std::unique_ptr<fw::doc::Document> doc_;
  fw::doc::edit::Editor editor_;
#if FW_TORTURE_D3
  fw::doc::edit::Pictures pictures_{editor_};
#endif
#if FW_TORTURE_D4
  fw::doc::edit::Shapes shapes_{editor_};
#endif
  uint32_t grouping_ = 0;  // edits left in an open group
};
#endif

// Without the editor: the document as read, nothing applied, every part the model owns written.
class ModelOnlyTarget : public EditTarget {
 public:
  explicit ModelOnlyTarget(std::unique_ptr<fw::doc::Document> doc) : doc_(std::move(doc)) {}
  const char* Name() const override { return "model only (no editor yet)"; }
  const fw::doc::Document& Doc() const override { return *doc_; }
  Applied Apply(const Edit&, std::string& why) override {
    why = "no editor";
    return Applied::Refused;
  }
  fw::pkg::Status Save(std::vector<uint8_t>& out) override {
    std::vector<uint32_t> all;
    for (uint32_t i = 0; i < doc_->Parts().size(); ++i)
      if (doc_->Parts()[i].writable && doc_->Parts()[i].Editable()) all.push_back(i);
    return SaveParts(*doc_, all, true, out, written);
  }

 private:
  std::unique_ptr<fw::doc::Document> doc_;
};

}  // namespace

std::unique_ptr<EditTarget> OpenTarget(std::unique_ptr<fw::doc::Document> doc, fw::pkg::Status& status) {
  status = {};
#if FW_TORTURE_EDITOR
  return std::make_unique<EditorTarget>(std::move(doc));
#else
  return std::make_unique<ModelOnlyTarget>(std::move(doc));
#endif
}

}  // namespace torture

namespace {

using namespace fw::doc;
using torture::Applied;
using torture::Edit;
using torture::EditKind;
using torture::EditTarget;
namespace fs = std::filesystem;

// SplitMix64: the same draws on every compiler and machine, so a seed always means the same edits.
struct Rng {
  uint64_t state;
  explicit Rng(uint64_t seed) : state(seed) {}
  uint64_t Next() {
    uint64_t z = (state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }
  uint32_t Below(uint32_t n) { return n ? static_cast<uint32_t>(Next() % n) : 0; }
  uint32_t Between(uint32_t lo, uint32_t hi) { return hi <= lo ? lo : lo + Below(hi - lo + 1); }
  bool Percent(uint32_t p) { return Below(100) < p; }
  template <class T> const T& Pick(const std::vector<T>& v) { return v[Below(static_cast<uint32_t>(v.size()))]; }
};

// Each file and round gets its own seed from the run's seed and the file's name.
uint64_t RoundSeed(uint64_t seed, const std::string& name, uint32_t round) {
  uint64_t h = 1469598103934665603ull;
  for (unsigned char c : name) h = (h ^ c) * 1099511628211ull;
  Rng mix(seed ^ h ^ (static_cast<uint64_t>(round) << 32));
  return mix.Next();
}

// A stretch of a story the edits aim at: a field, a bookmark, a comment, a content control...
struct Span {
  std::string what;
  uint32_t story = kNone;
  Pos a, b;  // b is just past its end
};

class Generator {
 public:
  Generator(const Document& doc, Rng& rng) : doc_(doc), rng_(rng) {
    char buf[256] = {};
    if (GetEnvironmentVariableA("FW_TORTURE_SKIP", buf, sizeof(buf)) && buf[0]) {
      std::string list(buf);
      for (size_t start = 0; start <= list.size();) {
        size_t comma = std::min(list.find(',', start), list.size());
        skip_.insert(list.substr(start, comma - start));
        start = comma + 1;
      }
    }
  }

  // The next edit for the document as it is now. `undone` is how many edits can be redone.
  Edit Next(uint32_t applied, uint32_t undone) {
    Index();
    Edit e;
    if (!pending_.empty()) {  // the rest of a save, undo past it, save again
      e = pending_.front();
      pending_.erase(pending_.begin());
      if (e.kind != EditKind::Undo || applied) return e;
      e = Edit{};
    }
    if (stories_.empty()) {
      e.kind = EditKind::Undo;
      return e;
    }
#if FW_TORTURE_D4
    // A fifth of the rounds edit with Track Changes on from the start.
    if (!started_) {
      started_ = true;
      if (!skip_.contains("d4") && rng_.Percent(20)) {
        e.kind = EditKind::Command;
        e.at = e.to = RandomPos(RandomPara());
        e.format = "track=on";
        e.target = "Track Changes";
        return e;
      }
    }
    // In a protected form most edits are refused: fill it in often.
    if (!skip_.contains("d4") && fw::doc::edit::FormsProtected(doc_) && rng_.Percent(40)) {
      e.kind = EditKind::Command;
      e.at = e.to = RandomPos(RandomPara());
      Forms(e);
      return e;
    }
#endif
    uint32_t roll = rng_.Below(100);
    // FW_TORTURE_SKIP=command,paint (save, format, command, d3, d4, paint) types instead, to look
    // past an open finding; "headers" leaves D3's header and footer changes out.
    auto skipped = [&](const char* what, uint32_t lo, uint32_t hi) { return roll >= lo && roll < hi && skip_.contains(what); };
    if (skipped("save", 9, 11) || skipped("format", 77, 81) || skipped("command", 81, 87) || skipped("d3", 87, 93) ||
        skipped("d4", 93, 98) || skipped("paint", 98, 100))
      roll = 40;
    if (roll < 4 && applied) {
      e.kind = EditKind::Undo;
      e.count = rng_.Between(1, std::min<uint32_t>(applied, 4));
    } else if (roll < 7 && undone) {
      e.kind = EditKind::Redo;
      e.count = rng_.Between(1, std::min<uint32_t>(undone, 4));
    } else if (roll < 9) {
      e.kind = EditKind::Group;  // the next few edits, undone and redone as one (as Paste and Replace All do)
      e.count = rng_.Between(2, 4);
    } else if (roll < 11) {
      e.kind = EditKind::Save;
      if (applied && rng_.Percent(60)) {  // then undo back past it, and save again
        Edit undo;
        undo.kind = EditKind::Undo;
        undo.count = rng_.Between(1, std::min<uint32_t>(applied, 4));
        Edit save;
        save.kind = EditKind::Save;
        pending_ = {undo, save};
      }
    } else if (roll < 37) {
      Structural(e);
    } else if (roll < 55) {
      e.kind = EditKind::Type;
      e.at = RandomPos(RandomPara());
      e.text = RandomText();
    } else if (roll < 62) {
      PlainDelete(e);
    } else if (roll < 65) {
      PlainDelete(e);
      e.kind = EditKind::Replace;
      e.text = RandomText();
    } else if (roll < 72) {
      e.kind = EditKind::Split;
      e.at = RandomPos(RandomPara());
    } else if (roll < 77) {
      Join(e);
    } else if (roll < 81) {
      Formatting(e);
    } else if (roll < 87) {
      Command(e);
#if FW_TORTURE_D4
    } else if (roll < 93) {
      D3(e);
    } else if (roll < 98) {
      D4(e);
#elif FW_TORTURE_D3
    } else if (roll < 98) {
      D3(e);
#endif
    } else {
      Paint(e);
    }
    return e;
  }

 private:
  // The editable stories and the order of their paragraphs.
  void Index() {
    stories_.clear();
    order_.clear();
    paras_.clear();
    // A header or footer no section shows (its section break deleted, a new one undone) can't be
    // reached by a person.
    std::vector<bool> shown(doc_.Stories().size());
    for (const Section& section : doc_.Sections())
      for (const auto* refs : {&section.headers, &section.footers})
        for (uint32_t h : *refs)
          if (h < shown.size()) shown[h] = true;
    for (uint32_t s = 0; s < doc_.Stories().size(); ++s) {
      const Story& story = doc_.Stories()[s];
      if (story.part >= doc_.Parts().size() || !doc_.Parts()[story.part].Editable()) continue;
      if ((story.kind == StoryKind::Header || story.kind == StoryKind::Footer) && !shown[s]) continue;
      std::vector<uint32_t> ps = doc_.StoryParagraphs(s);
      std::vector<uint32_t> keep;
      for (uint32_t p : ps)
        if (!doc_.ParaNode(p).DisplayOnly()) keep.push_back(p);
      if (keep.empty()) continue;
      for (size_t i = 0; i < keep.size(); ++i) order_[keep[i]] = {s, static_cast<uint32_t>(i)};
      stories_.push_back(s);
      paras_[s] = std::move(keep);
    }
  }
  uint32_t Len(uint32_t para) const { return static_cast<uint32_t>(doc_.Para(para).text.size()); }
  // Mostly the main text; the other stories now and then.
  uint32_t RandomStory() {
    uint32_t main = doc_.MainStory();
    if (paras_.contains(main) && (stories_.size() == 1 || rng_.Percent(75))) return main;
    return rng_.Pick(stories_);
  }
  uint32_t RandomPara() { return rng_.Pick(paras_[RandomStory()]); }
  Pos RandomPos(uint32_t para) { return Pos{para, rng_.Between(0, Len(para))}; }
  // A position up to `reach` paragraphs after p in its story, at or after p.
  Pos Later(Pos p, uint32_t reach) {
    auto [story, index] = order_[p.para];
    const auto& ps = paras_[story];
    uint32_t to = std::min<uint32_t>(index + rng_.Between(0, reach), static_cast<uint32_t>(ps.size()) - 1);
    if (to == index) return Pos{p.para, rng_.Between(p.offset, Len(p.para))};
    return RandomPos(ps[to]);
  }
  bool Known(Pos p) const { return p.para != kNone && order_.contains(p.para) && p.offset <= Len(p.para); }
  bool Before(Pos a, Pos b) const {
    auto ia = order_.at(a.para), ib = order_.at(b.para);
    return ia.second != ib.second ? ia.second < ib.second : a.offset < b.offset;
  }

  std::u16string RandomText() {
    static const std::vector<std::u16string> pieces = {
        u"a", u"e", u"the ", u"word ", u"Featherweight", u"  ", u" ", u".", u", ", u"12", u"é", u"ß", u"ü", u"ñ",
        u"ç", u"€", u"—", u"“quoted”", u"日本", u"\t", u"o"};
    // Breaks typed as characters (InsertText): a line break, and with D3 a page and a column break,
    // and optional and non-breaking hyphens (w:softHyphen, w:noBreakHyphen), mid-word too.
#if FW_TORTURE_PAGE
    static const std::vector<std::u16string> breaks = {u"\v", u"\v", u"\f", u"\x0E", u"co\u00ADop", u"e\u2011mail", u"\u00AD", u"\u2011"};
#else
    static const std::vector<std::u16string> breaks = {u"\v"};
#endif
    std::u16string t;
    for (uint32_t n = rng_.Between(1, 4); n; --n) t += rng_.Percent(4) ? rng_.Pick(breaks) : rng_.Pick(pieces);
    return t;
  }

  void PlainDelete(Edit& e) {
    e.kind = EditKind::Delete;
    e.at = RandomPos(RandomPara());
    e.to = Later(e.at, rng_.Percent(70) ? 0 : 3);
    if (e.to == e.at && Len(e.at.para) > e.at.offset) e.to.offset = e.at.offset + 1;
  }

  // Joining two paragraphs is deleting the first one's mark.
  void Join(Edit& e) {
    uint32_t story = RandomStory();
    const auto& ps = paras_[story];
    if (ps.size() < 2) return PlainDelete(e);
    uint32_t i = rng_.Below(static_cast<uint32_t>(ps.size()) - 1);
    if (rng_.Percent(50)) {
      e.kind = EditKind::Join;
      e.at = Pos{ps[i], 0};
      return;
    }
    e.kind = EditKind::Delete;
    e.at = Pos{ps[i], Len(ps[i])};
    e.to = Pos{ps[i + 1], 0};
    e.target = "paragraph join";
  }

  void Formatting(Edit& e) {
    static const std::vector<std::string> run = {"bold", "italic", "underline", "strike", "size=16", "size=28",
                                                 "color=C00000", "color=1F4E79", "highlight=yellow", "plain"};
    static const std::vector<std::string> para = {"align=center", "align=right", "align=both", "align=left"};
    e.kind = EditKind::Format;
    e.at = RandomPos(RandomPara());
    e.to = Later(e.at, rng_.Percent(80) ? 0 : 2);
    if (rng_.Percent(80)) {
      e.format = rng_.Pick(run);
      if (rng_.Percent(20)) e.format = "mark:" + e.format;  // the paragraph marks' formatting
    } else if (rng_.Percent(50) && !doc_.Styles().empty()) {
      std::vector<std::string> styles;
      for (const Style& s : doc_.Styles())
        if (s.type == StyleType::Paragraph && !s.id.empty()) styles.emplace_back(s.id);
      e.format = styles.empty() ? rng_.Pick(para) : "style=" + rng_.Pick(styles);
    } else {
      e.format = rng_.Pick(para);
    }
  }

  // A formatting command (Format.h, Lists.h, Styles.h) on a range of one story, mostly in one
  // paragraph, sometimes across a few.
  void Command(Edit& e) {
    static const std::vector<std::string> lists = {"bullets", "numbering", "multilevel", "nolist",
                                                   "restart", "continue",  "indent",     "outdent"};
    static const std::vector<std::string> formats = {
        "toggle=bold", "toggle=italic", "toggle=underline", "toggle=strike", "toggle=super", "toggle=sub",
        "grow",        "shrink",        "grow1",            "shrink1",       "clearchars",   "clearpara",
        "clearall",    "font=Arial",    "font=Times New Roman", "spacing=240", "spacing=360", "spacing=480",
        "spacebefore", "border=box",    "border=bottom",    "border=none"};
#if FW_TORTURE_CLIP
    if (!skip_.contains("paste") && rng_.Percent(15)) return Paste(e);
#endif
    e.kind = EditKind::Command;
    e.at = RandomPos(RandomPara());
    e.to = Later(e.at, rng_.Percent(70) ? 0 : 3);
#if FW_TORTURE_CASE
    if (rng_.Percent(15)) {  // change case; a new style from the selection, theme fonts and colors
      static const std::vector<std::string> audit = {
          "case=sentence", "case=lower", "case=upper", "case=capitalize", "case=toggle",
#if FW_TORTURE_THEME
          "themefont=headings", "themefont=body", "themecolor=accent1", "themecolor=accent2:99:-1",
          "themecolor=text1:-1:BF", "themecolor=background2",
#endif
      };
      e.format = rng_.Pick(audit);
#if FW_TORTURE_THEME
      if (rng_.Percent(15)) e.format = "newstyle=Torture " + std::to_string(rng_.Between(1, 4));
#endif
      return;
    }
#endif
    uint32_t roll = rng_.Below(100);
    if (roll < 35) {
      e.format = rng_.Pick(lists);
    } else if (roll < 65) {
      // A style from the gallery (built-ins the document lacks get Word's definition), or one the
      // document has, paragraph or character; now and then the style itself changes.
      std::vector<std::string> mine;
      for (const Style& s : doc_.Styles())
        if ((s.type == StyleType::Paragraph || s.type == StyleType::Character) && !s.id.empty()) mine.emplace_back(s.id);
      uint32_t which = rng_.Below(100);
      if (which < 12) {
        e.format = "update";
      } else if (which < 25 && !mine.empty()) {
        e.format = "modify=" + rng_.Pick(mine);
      } else if (which < 65 || mine.empty()) {
        std::vector<fw::doc::edit::StyleEntry> gallery = fw::doc::edit::GalleryStyles(doc_);
        e.format = gallery.empty() ? "apply=Normal" : "apply=" + rng_.Pick(gallery).name;
      } else {
        e.format = "apply=" + rng_.Pick(mine);
      }
    } else {
      e.format = rng_.Pick(formats);
      if (rng_.Percent(20)) {  // the Paragraph panel, one field at a time
        static const std::vector<std::string> fields = {
            "atleast:360", "exact:280", "auto:720",  "indstart:720", "indstart:0",   "indend:1440", "firstline:360",
            "firstline:-360", "before:120", "after:0", "after:240",   "keepnext:1",   "keepnext:0",  "keeplines:1",
            "pagebreak:1",    "pagebreak:0", "widow:0", "widow:1",    "contextual:1"};
        e.format = "para=" + rng_.Pick(fields);
      }
#if FW_TORTURE_PAGE
      if (rng_.Percent(15)) {  // the ruler's tab stops; a stop the paragraph has, to take away
        static const std::vector<std::string> tabs = {"tabs=1440:start", "tabs=2880:center", "tabs=4320:end",
                                                      "tabs=5760:decimal", "tabs=720:bar", "tabs=4320:end:dot",
                                                      "tabs=5040:end:hyphen", "tabs=3600:start:underscore",
                                                      "tabs=6480:end:heavy", "tabs=4320:center:middledot"};
        const std::vector<fw::doc::TabStop>& have = doc_.ResolvedPara(e.at.para).tabs;
        e.format = !have.empty() && rng_.Percent(40)
                       ? "tabs=" + std::to_string(rng_.Pick(have).pos.Value()) + ":start:remove"
                       : rng_.Pick(tabs);
      }
#endif
    }
  }

#if FW_TORTURE_CLIP
  // Copy and paste (EditorTarget::Paste has the spec): a range of one story, or a block of cells,
  // pasted in body text or in a cell, now and then over a selection; mostly through the clipboard's
  // HTML, as Featherweight pastes its own copies.
  void Paste(Edit& e) {
    static const std::vector<std::string> vias = {"html", "html", "html", "rtf", "clip"};
    e.kind = EditKind::Command;
    std::string kind = "paste=";
    e.at = RandomPos(RandomPara());
    e.to = Later(e.at, rng_.Percent(60) ? 0 : 4);
    Pos where = RandomPos(RandomPara());
#if FW_TORTURE_PAGE
    const std::vector<std::vector<uint32_t>> tables = Tables();
    if (!tables.empty() && rng_.Percent(35)) {
      const std::vector<uint32_t>& cells = rng_.Pick(tables);
      e.at = RandomPos(rng_.Pick(cells));
      e.to = RandomPos(rng_.Pick(cells));
      if (Before(e.to, e.at)) std::swap(e.at, e.to);
      kind = "pastecells=";
    }
    if (!tables.empty() && rng_.Percent(40)) where = RandomPos(rng_.Pick(rng_.Pick(tables)));
#endif
    const Pos end = rng_.Percent(25) ? Later(where, 1) : where;
    auto at = [](Pos p) { return std::to_string(p.para) + ":" + std::to_string(p.offset); };
    e.format = kind + rng_.Pick(vias) + ":" + at(where) + (end == where ? "" : ":" + at(end));
    e.target = kind == "pastecells=" ? "copied cells" : "copied text";
  }
#endif

#if FW_TORTURE_D3
  // D3's operations (EditorTarget::D3 has the specs): tables, pictures, fields, links and
  // bookmarks, headers and footers.
  void D3(Edit& e) {
    e.kind = EditKind::Command;
    e.at = RandomPos(RandomPara());
    e.to = e.at;
    const uint32_t roll = rng_.Below(100);
    if (roll < 32) Table(e);
    else if (roll < 50) Picture(e);
    else if (roll < 70 || (roll < 85 && skip_.contains("headers"))) Inline(e);
    else if (roll < 85) HeaderFooter(e);
#if FW_TORTURE_PAGE
    else Page(e);
#else
    else Table(e);
#endif
  }

#if FW_TORTURE_PAGE
  // Page layout by section, and breaks in the main text.
  void Page(Edit& e) {
    const std::string s = std::to_string(rng_.Below(std::max<uint32_t>(1, static_cast<uint32_t>(doc_.Sections().size()))));
    static const std::vector<std::string> setups = {":landscape", ":portrait", ":margins:720", ":margins:2880",
                                                    ":size:12240x15840", ":size:11906x16838", ":header:200"};
    static const std::vector<std::string> starts = {"nextPage", "continuous", "evenPage", "oddPage"};
    const uint32_t roll = rng_.Below(100);
    const std::vector<uint32_t>* body = paras_.contains(doc_.MainStory()) ? &paras_[doc_.MainStory()] : nullptr;
    if (roll < 25) e.format = "pagesetup=" + s + rng_.Pick(setups);
    else if (roll < 42) e.format = "columns=" + s + ":" + std::to_string(rng_.Between(1, 3)) + ":" + std::to_string(rng_.Pick(std::vector<uint32_t>{360, 720})) + ":" + std::to_string(rng_.Below(2));
    else if (roll < 52) e.format = "sectionstart=" + s + ":" + rng_.Pick(starts);
    else if (roll < 67 && body) e.at = e.to = RandomPos(rng_.Pick(*body)), e.format = "sectionbreak=" + rng_.Pick(starts);
    else if (roll < 77) e.format = "delsectionbreak=" + s;
    else if (roll < 92) e.format = rng_.Percent(70) ? "pagebreak" : "columnbreak";
    else e.format = rng_.Percent(50) ? "hyphenation=on" : "hyphenation=off";
    e.target = "section " + s;
  }
#endif

  // The tables of the editable stories, each as its cells' paragraphs (nested tables apart).
  std::vector<std::vector<uint32_t>> Tables() {
    std::vector<std::vector<uint32_t>> tables;
    std::function<void(const std::vector<Block>&)> walk = [&](const std::vector<Block>& blocks) {
      for (const Block& b : blocks) {
        if (b.kind == BlockKind::ContentControl) walk(doc_.Blocks(b.node));
        if (b.kind != BlockKind::Table) continue;
        std::vector<uint32_t> cells;
        for (Node tr : doc_.TableRows(b.node))
          for (Node tc : doc_.RowCells(tr)) {
            std::vector<Block> inner = doc_.Blocks(tc);
            for (const Block& i : inner)
              if (i.kind == BlockKind::Paragraph && order_.contains(i.para)) cells.push_back(i.para);
            walk(inner);
          }
        if (!cells.empty()) tables.push_back(std::move(cells));
      }
    };
    for (uint32_t s : stories_) walk(doc_.Blocks(s));
    return tables;
  }

  void Table(Edit& e) {
    static const std::vector<std::string> changes = {
        "rows=above", "rows=below", "cols=left", "cols=right", "deleterows", "deletecols", "clearcells", "merge",
        "merge", "splitcell=2x1", "splitcell=1x2", "splitcell=2x2", "colwidth=1440", "colwidth=2880",
        "colborder=1:360", "colborder=1:-360", "colborder=0:200", "rowheight=600", "rowheight=400:exact",
        "autofit=window", "autofit=fixed", "cellborders=all", "cellborders=none", "cellborders=outside",
        "cellborders=inside", "cellborders=bottom", "cellshade=D9E2F3", "headerrow", "tablealign=center",
        "tabletotext", "deletetable"};
    std::vector<std::vector<uint32_t>> tables = Tables();
    if (tables.empty() || rng_.Percent(20)) {
      if (rng_.Percent(80)) {
        e.format = "table=" + std::to_string(rng_.Between(1, 4)) + "x" + std::to_string(rng_.Between(1, 4));
        e.target = "new table";
      } else {
        e.to = Later(e.at, 3);
        e.format = "texttotable";
      }
      return;
    }
    const std::vector<uint32_t>& cells = rng_.Pick(tables);
    e.at = RandomPos(rng_.Pick(cells));
    e.to = rng_.Percent(50) ? e.at : RandomPos(rng_.Pick(cells));
    if (Before(e.to, e.at)) std::swap(e.at, e.to);
    e.format = rng_.Pick(changes);
#if FW_TORTURE_PAGE
    if (rng_.Percent(10)) {
      std::vector<std::string> styles = fw::doc::edit::TableStyleNames(doc_);
      if (!styles.empty()) e.format = "tablestyle=" + rng_.Pick(styles);
    }
#endif
    e.target = "table";
  }

  void Picture(Edit& e) {
    const uint32_t drawings = static_cast<uint32_t>(doc_.Drawings().size());
    if (!drawings || rng_.Percent(30)) {
      // The 4 x 3 PNG, or a file of each kind Insert takes (and an icon, which it doesn't).
      static const std::vector<std::string> files = {"",        "",         "",        "gray.jpg", "one.gif",
                                                     "tile.png", "two.bmp", "two.tif", "bom.svg",  "tile.ico"};
      const std::string file = rng_.Pick(files);
      e.format = file.empty() ? "picture" : "picture=" + file;
      if (!skip_.contains("save") && rng_.Percent(25)) {
        // Saved, deleted and saved (its media leaves the file), undone and saved (back, and whole).
        Edit save, del, undo;
        save.kind = EditKind::Save;
        del.kind = EditKind::Delete;
        del.at = e.at;
        del.to = Pos{e.at.para, e.at.offset + 1};
        del.target = "the picture just inserted";
        undo.kind = EditKind::Undo;
        pending_ = {save, del, save, undo, save};
      }
      return;
    }
    const std::string d = std::to_string(rng_.Below(drawings));
    static const std::vector<std::string> changes = {
        ":50", ":150", ":90", ":45", ":10", ":square", ":tight", ":topbottom", ":behind", ":front", ":inline", ":72:144", "", ""};
    static const std::vector<std::string> names = {
        "picsize", "picsize", "picrotate", "picrotate", "piccrop", "picwrap", "picwrap", "picwrap", "picwrap", "picwrap", "picwrap", "picmove", "picalt", "picreset"};
    const uint32_t i = rng_.Below(static_cast<uint32_t>(names.size()));
    e.format = names[i] + "=" + d + changes[i];
    e.target = "drawing " + d;
  }

  void Inline(Edit& e) {
    const uint32_t roll = rng_.Below(100);
    if (roll < 25) {
      static const std::vector<std::string> fields = {"PAGE", "NUMPAGES", "DATE", "AUTHOR"};
      e.format = "field=" + rng_.Pick(fields);
    } else if (roll < 50) {
      e.to = Later(e.at, rng_.Percent(80) ? 0 : 1);
      std::vector<fw::doc::edit::BookmarkInfo> marks = fw::doc::edit::Bookmarks(doc_);
      e.format = "link=" + (marks.empty() || rng_.Percent(60) ? std::string("https://example.com/torture")
                                                             : "#" + rng_.Pick(marks).name);
    } else if (roll < 65) {
      // A link that's there: in one of its runs.
      std::vector<Span> links;
      for (Span& s : Spans())
        if (s.what == "link") links.push_back(std::move(s));
      if (!links.empty()) {
        const Span& l = rng_.Pick(links);
        e.at = e.to = Pos{l.a.para, rng_.Between(l.a.offset, l.b.offset)};
        e.target = "link";
      }
      e.format = rng_.Percent(50) ? "editlink" : "unlink";
    } else if (roll < 82) {
      e.to = Later(e.at, rng_.Percent(80) ? 0 : 2);
      e.format = "bookmark=Torture" + std::to_string(rng_.Below(1000));
      // Often around or inside a field or a link, or across one end of it.
      std::vector<Span> spans;
      for (Span& s : Spans())
        if (s.what.starts_with("field") || s.what.starts_with("simple field") || s.what == "link" ||
            s.what.starts_with("bookmark") || s.what == "content control")
          spans.push_back(std::move(s));
      if (!spans.empty() && rng_.Percent(50)) {
        const Span& s = rng_.Pick(spans);
        auto inside = [&] {
          if (s.a.para == s.b.para) return Pos{s.a.para, rng_.Between(s.a.offset, s.b.offset)};
          return Pos{s.a.para, rng_.Between(s.a.offset, Len(s.a.para))};
        };
        switch (rng_.Below(3)) {
          case 0: e.at = s.a, e.to = s.b; break;      // around it
          case 1: e.at = inside(), e.to = inside(); break;
          default: e.at = Pos{s.a.para, rng_.Between(0, s.a.offset)}, e.to = inside(); break;  // across its start
        }
        if (Before(e.to, e.at)) std::swap(e.at, e.to);
        e.target = s.what;
      }
    } else if (roll < 92) {
      std::vector<fw::doc::edit::BookmarkInfo> marks = fw::doc::edit::Bookmarks(doc_);
      if (marks.empty()) return PlainDelete(e);
      e.format = "delbookmark=" + rng_.Pick(marks).name;
    } else {
      e.format = "parabookmark";
    }
  }

  void HeaderFooter(Edit& e) {
    const std::string s = std::to_string(rng_.Below(std::max<uint32_t>(1, static_cast<uint32_t>(doc_.Sections().size()))));
    const std::string type = std::to_string(rng_.Below(3));
    uint32_t roll = rng_.Below(100);
    if (roll >= 75 && skip_.contains("pagenumber")) roll = rng_.Below(75);  // FW_TORTURE_SKIP=pagenumber
    if (roll < 30) e.format = (rng_.Percent(50) ? "header=" : "footer=") + s + ":" + type;
    else if (roll < 45) e.format = "linkprev=" + s + ":" + std::to_string(rng_.Below(2)) + ":" + type + ":" + std::to_string(rng_.Below(2));
    else if (roll < 55) e.format = "firstpage=" + s + ":" + std::to_string(rng_.Below(2));
    else if (roll < 62) e.format = rng_.Percent(50) ? "oddeven=on" : "oddeven=off";
    else if (roll < 75) e.format = "pagenumbering=" + s + (rng_.Percent(50) ? ":lowerRoman:1" : ":decimal");
    else e.format = "pagenumber=" + s + ":" + std::to_string(rng_.Below(2)) + ":" + type + ":" + std::to_string(rng_.Below(3)) + ":" + std::to_string(rng_.Below(2));
    e.target = "section " + s;
  }
#endif

#if FW_TORTURE_D4
  void D4(Edit& e) {
    e.kind = EditKind::Command;
    e.at = RandomPos(RandomPara());
    e.to = e.at;
    const uint32_t roll = rng_.Below(100);
    if (roll < 30) Forms(e);
    else if (roll < 47) Comments(e);
    else if (roll < 59) Notes(e);
    else if (roll < 77) Review(e);
    else if (roll < 93) Shape(e);
    else Watermark(e);
  }

  // Content controls and legacy form fields filled in as their kind is (text typed over, boxes
  // ticked, items and dates picked), a new control, or Tab to the next one.
  void Forms(Edit& e) {
    namespace ed = fw::doc::edit;
    std::vector<ed::ContentControl> controls;
    std::vector<ed::FormField> fields;
    for (uint32_t s : stories_) {
      for (ed::ContentControl& c : ed::ContentControls(doc_, s))
        if (Known(c.content.from) && Known(c.content.to)) controls.push_back(std::move(c));
      for (ed::FormField& f : ed::FormFields(doc_, s))
        if (Known(f.start)) fields.push_back(std::move(f));
    }
    const uint32_t roll = rng_.Below(100);
    if (roll < 20 || (controls.empty() && fields.empty())) {
      static const std::vector<std::string> kinds = {"richtext", "plaintext", "checkbox", "dropdown:Red:Green:Blue",
                                                     "combobox:One:Two",   "date",      "dropdown"};
      e.format = "control=" + rng_.Pick(kinds);
      e.target = "new content control";
      return;
    }
    if (roll < 30) {
      e.format = rng_.Percent(75) ? "tabfill" : "tabfill=back";
      e.target = "the next fill-in";
      return;
    }
    static const std::vector<std::string> texts = {"Filled in", "Ann Lee", "x", "", "Caf\xC3\xA9 \xE2\x9C\x93"};
    if (!fields.empty() && (controls.empty() || rng_.Percent(35))) {
      const ed::FormField& f = rng_.Pick(fields);
      e.at = e.to = f.hasResult ? f.result.from : f.start;
      e.target = "form field " + ToUtf8(f.name);
      if (f.kind == ed::FormFieldKind::CheckBox) {
        e.format = f.checked ? "formcheck=0" : "formcheck=1";
      } else if (f.kind == ed::FormFieldKind::DropDown) {
        e.format = "formentry=" + std::to_string(rng_.Below(static_cast<uint32_t>(f.entries.size()) + 1));
      } else if (f.hasResult && Known(f.result.to)) {  // typed over, as in a protected form
        e.kind = EditKind::Replace;
        e.to = f.result.to;
        e.text = ToUtf16(rng_.Pick(texts));
        if (e.text.empty()) e.kind = EditKind::Delete;
      } else {
        e.format = "tabfill";
      }
      return;
    }
    const ed::ContentControl& c = rng_.Pick(controls);
    e.at = e.to = rng_.Percent(80) ? c.content.from : c.content.to;
    static const char* kinds[] = {"rich text", "plain text", "check box", "drop-down list", "combo box", "date", "picture", "other"};
    e.target = std::string(kinds[static_cast<int>(c.kind)]) + " content control";
    switch (c.kind) {
      case ed::ControlKind::CheckBox: e.format = c.checked ? "check=0" : "check=1"; break;
      case ed::ControlKind::DropDown:
      case ed::ControlKind::ComboBox:
        e.format = rng_.Percent(80) ? "pickitem=" + std::to_string(rng_.Below(static_cast<uint32_t>(c.items.size()) + 2)) : "placeholder";
        break;
      case ed::ControlKind::Date:
        e.format = rng_.Percent(80) ? "pickdate=" + std::to_string(rng_.Between(1999, 2030)) + ":" + std::to_string(rng_.Between(1, 12)) +
                                          ":" + std::to_string(rng_.Between(1, 31))
                                    : "placeholder";
        break;
      default:
        e.format = rng_.Percent(75) ? "fill=" + rng_.Pick(texts) : "placeholder";
        break;
    }
  }

  // A new comment on a range or at a point, or a reply to one, its text changed, it resolved or deleted.
  void Comments(Edit& e) {
    std::vector<CommentInfo> comments = doc_.Comments();
    if (comments.empty() || rng_.Percent(40)) {
      e.to = rng_.Percent(15) ? e.at : Later(e.at, rng_.Percent(75) ? 0 : 1);
      static const std::vector<std::string> texts = {"A comment from the torture test", "Two paragraphs\nin one comment",
                                                     "Caf\xC3\xA9 \xE2\x9C\x93"};
      e.format = "comment=" + rng_.Pick(texts);
      e.target = "new comment";
      return;
    }
    const std::string id(rng_.Pick(comments).id);
    const uint32_t roll = rng_.Below(100);
    if (roll < 30) e.format = "reply=" + id;
    else if (roll < 55) e.format = "editcomment=" + id;
    else if (roll < 80) e.format = "delcomment=" + id;
    else e.format = "resolve=" + id + ":" + (rng_.Percent(70) ? "1" : "0");
    e.target = "comment " + id;
  }

  // A footnote or endnote at e.at (refused outside the main text), or one deleted: one the text
  // refers to, as a person reaches it (not a separator).
  void Notes(Edit& e) {
    std::vector<std::string> notes;
    for (uint32_t i = 0; i < doc_.Stories().size(); ++i) {
      const Story& s = doc_.Stories()[i];
      if ((s.kind == StoryKind::Footnote || s.kind == StoryKind::Endnote) && !s.id.empty() &&
          fw::doc::edit::NoteReference(doc_, i))
        notes.push_back(std::string(s.kind == StoryKind::Endnote ? "e:" : "f:") + std::string(s.id));
    }
    if (notes.empty() || rng_.Percent(60)) {
      e.format = rng_.Percent(70) ? "footnote" : "endnote";
      e.target = "new note";
      return;
    }
    const std::string note = rng_.Pick(notes);
    e.format = "delnote=" + note;
    e.target = "note " + note;
  }

  // Track Changes on or off; the changes in a range, or all of them, accepted or rejected.
  void Review(Edit& e) {
    const uint32_t roll = rng_.Below(100);
    if (roll < 40) {
      e.format = rng_.Percent(75) ? "track=on" : "track=off";
      e.target = "Track Changes";
      return;
    }
    if (roll < 85) {
      std::vector<Span> changes;
      for (Span& s : Spans())
        if (s.what == "tracked change") changes.push_back(std::move(s));
      if (!changes.empty() && rng_.Percent(80)) {
        const Span& s = rng_.Pick(changes);
        e.at = s.a;
        e.to = rng_.Percent(70) ? s.b : s.a;
        e.target = "tracked change";
      } else {
        e.to = Later(e.at, 1);
      }
      e.format = rng_.Percent(50) ? "review=accept" : "review=reject";
      return;
    }
    e.format = rng_.Percent(50) ? "reviewall=accept" : "reviewall=reject";
    e.target = "every tracked change";
  }

  // A new shape at the start of a paragraph, or a drawing resized, turned, moved, wrapped,
  // filled, outlined or given text (refused for a drawing that isn't a shape).
  void Shape(Edit& e) {
    const uint32_t drawings = static_cast<uint32_t>(doc_.Drawings().size());
    if (!drawings || rng_.Percent(35)) {
      static const std::vector<std::string> kinds = {"textbox", "rect", "roundrect", "oval", "line", "arrow", "callout"};
      e.at.offset = 0;
      e.to = e.at;
      e.format = "shape=" + rng_.Pick(kinds);
      e.target = "new shape";
      return;
    }
    const std::string d = std::to_string(rng_.Below(drawings));
    static const std::vector<std::string> changes = {
        "shapesize=:50", "shapesize=:150", "shaperotate=:30", "shaperotate=:90", "shapemove=:72:144", "shapewrap=:square",
        "shapewrap=:tight", "shapewrap=:topbottom", "shapewrap=:behind", "shapewrap=:front", "shapewrap=:inline",
        "shapefill=:4472C4", "shapefill=:none", "shapeline=:C00000:2", "shapeline=:none", "shapetext="};
    std::string change = rng_.Pick(changes);
    change.insert(change.find('=') + 1, d);
    e.format = change;
    e.target = "drawing " + d;
  }

  void Watermark(Edit& e) {
    static const std::vector<std::string> marks = {"watermark=text", "watermark=CONFIDENTIAL", "watermark=picture", "watermark=none"};
    e.format = rng_.Pick(marks);
    e.target = "every header";
  }
#endif

  // The format painter: from a character anywhere editable to a range, half the time whole paragraphs.
  void Paint(Edit& e) {
    e.kind = EditKind::Paint;
    e.from = RandomPos(RandomPara());
    e.at = RandomPos(RandomPara());
    e.to = Later(e.at, rng_.Percent(70) ? 0 : 2);
    if (rng_.Percent(50)) {
      e.at.offset = 0;
      e.to.offset = Len(e.to.para);
    }
  }

  // Everything the edits can aim at in the document as it is now.
  std::vector<Span> Spans() {
    std::vector<Span> spans;
    auto add = [&](std::string what, uint32_t story, Pos a, Pos b) {
      if (!Known(a) || !Known(b) || order_[a.para].first != order_[b.para].first) return;
      if (Before(b, a)) std::swap(a, b);
      spans.push_back({std::move(what), story, a, b});
    };
    for (uint32_t s : stories_) {
      for (const Field& f : doc_.Fields(s))
        if (f.complete) add("field " + ToUtf8(f.Type()), s, f.begin, Pos{f.end.para, f.end.offset + 1});
      for (const Revision& r : doc_.Revisions(s))
        if (r.kind <= RevisionKind::MoveTo) add("tracked change", s, r.start, r.end);
      // Inline content controls, links and simple fields: the text of their runs; block content
      // controls: their paragraphs.
      for (uint32_t p : paras_[s]) {
        auto runsOf = [&](const char* what, Node TextRun::*member) {
          Node at;
          uint32_t start = 0, end = 0;
          for (const TextRun& r : doc_.Runs(p)) {
            if (r.*member && r.*member == at) {
              end = r.end;
              continue;
            }
            if (at) add(what, s, Pos{p, start}, Pos{p, end});
            at = r.*member, start = r.start, end = r.end;
          }
          if (at) add(what, s, Pos{p, start}, Pos{p, end});
        };
        runsOf("content control", &TextRun::contentControl);
        runsOf("link", &TextRun::hyperlink);
        runsOf("simple field", &TextRun::simpleField);
      }
      for (const Block& b : doc_.Blocks(s)) {
        if (b.kind == BlockKind::ContentControl) {
          std::vector<Block> inner = doc_.Blocks(b.node);
          uint32_t first = kNone, last = kNone;
          for (const Block& i : inner) {
            if (i.kind != BlockKind::Paragraph) continue;
            if (first == kNone) first = i.para;
            last = i.para;
          }
          if (first != kNone) add("block content control", s, Pos{first, 0}, Pos{last, Len(last)});
        } else if (b.kind == BlockKind::Table) {
          std::vector<uint32_t> cellParas;
          for (Node tr : doc_.TableRows(b.node))
            for (Node tc : doc_.RowCells(tr))
              for (const Block& i : doc_.Blocks(tc))
                if (i.kind == BlockKind::Paragraph) cellParas.push_back(i.para);
          if (cellParas.empty()) continue;
          add("table", s, Pos{cellParas.front(), 0}, Pos{cellParas.back(), Len(cellParas.back())});
          uint32_t c = rng_.Below(static_cast<uint32_t>(cellParas.size()));
          add("table cell", s, Pos{cellParas[c], 0}, Pos{cellParas[c], Len(cellParas[c])});
        }
      }
    }
    for (const PairInfo& pair : doc_.Pairs()) {
      const char* what = pair.kind == fw::pkg::RefKind::BookmarkId         ? "bookmark"
                         : pair.kind == fw::pkg::RefKind::CommentId        ? "comment range"
                         : pair.kind == fw::pkg::RefKind::PermissionId     ? "permission range"
                         : pair.kind == fw::pkg::RefKind::MoveRangeId      ? "move range"
                         : pair.kind == fw::pkg::RefKind::CustomXmlRangeId ? "custom XML range"
                                                                           : nullptr;
      if (what) add(std::string(what) + " " + std::string(pair.id), pair.story, pair.openPos, pair.closePos);
    }
    for (const CommentInfo& c : doc_.Comments())
      if (c.reference.para != kNone)
        add("comment " + std::string(c.id) + " reference", c.story, c.reference, Pos{c.reference.para, c.reference.offset + 1});
    return spans;
  }

  // Deletes across one end of a structure, around it or inside it; or types or splits inside it.
  void Structural(Edit& e) {
    std::vector<Span> spans = Spans();
    if (spans.empty()) return PlainDelete(e);
    const Span& s = rng_.Pick(spans);
    e.target = s.what;
    auto before = [&] { return Pos{s.a.para, rng_.Between(0, s.a.offset)}; };
    auto after = [&] { return Pos{s.b.para, rng_.Between(std::min(s.b.offset, Len(s.b.para)), Len(s.b.para))}; };
    auto inside = [&] {
      if (s.a.para == s.b.para) return Pos{s.a.para, rng_.Between(s.a.offset, s.b.offset)};
      return rng_.Percent(50) ? Pos{s.a.para, rng_.Between(s.a.offset, Len(s.a.para))} : Pos{s.b.para, rng_.Between(0, s.b.offset)};
    };
    switch (rng_.Below(6)) {
      case 0: e.kind = EditKind::Delete, e.at = before(), e.to = inside(); break;
      case 1: e.kind = EditKind::Delete, e.at = inside(), e.to = after(); break;
      case 2: e.kind = EditKind::Delete, e.at = before(), e.to = after(); break;
      case 3: e.kind = EditKind::Delete, e.at = inside(), e.to = inside(); break;
      case 4: e.kind = EditKind::Type, e.at = inside(), e.text = RandomText(); break;
      default: e.kind = EditKind::Split, e.at = inside(); break;
    }
    if (e.kind == EditKind::Delete && Before(e.to, e.at)) std::swap(e.at, e.to);
  }

  const Document& doc_;
  Rng& rng_;
  std::vector<Edit> pending_;  // edits that must come next
  std::set<std::string> skip_;  // FW_TORTURE_SKIP
  bool started_ = false;        // the round's first edit is made
  std::vector<uint32_t> stories_;
  std::unordered_map<uint32_t, std::vector<uint32_t>> paras_;               // story -> paragraphs
  std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> order_;       // paragraph -> story, index
};

// One paragraph as the digest has it: its style, alignment, where the formatting changes (so run
// splits don't matter) and its text.
std::string ParaLine(const Document& doc, uint32_t p) {
  const ParaProps& pp = doc.ResolvedPara(p);
  std::string out = std::string(pp.style) + " " + std::to_string(static_cast<int>(pp.jc)) + " | ";
  std::string last;
  for (const TextRun& r : doc.Runs(p)) {
    const RunProps& rp = doc.ResolvedRun(p, r.run);
    std::string sig = std::string(rp.bold ? "b" : "") + (rp.italic ? "i" : "") + (rp.strike ? "s" : "") + "u" +
                      std::to_string(static_cast<int>(rp.underline)) + "z" + std::to_string(rp.size.Value());
    if (sig != last) out += "{" + std::to_string(r.start) + ":" + sig + "}";
    last = sig;
  }
  // Its drawings as the layout sees them: size, in line or floating, and wrap.
  for (const InlineItem& item : doc.Items(p)) {
    if (item.kind != ItemKind::Drawing || item.drawing >= doc.Drawings().size()) continue;
    const Drawing& d = doc.Drawings()[item.drawing];
    out += "[d" + std::to_string(d.cx.Value()) + "x" + std::to_string(d.cy.Value()) + (d.anchored ? " a" : " i") +
           std::to_string(static_cast<int>(d.wrap)) + "]";
  }
  return out + " " + ToUtf8(doc.Para(p).text);
}

// Tables as Word needs them, the editor's guarantees (as tests/unit/test_tables.cpp's GridOk
// checks them): rows with cells, cells ending with a paragraph, a grid (w:tblGrid) as wide as the
// widest row (gridBefore, the spans, gridAfter), and vertical merges that continue a cell above
// with the same column and span. Counted by kind over every story: files from elsewhere break
// some of these already, so a round fails only when the saved file has more than the original.
std::map<std::string, int> GridProblems(const Document& doc) {
  std::map<std::string, int> problems;
  std::function<void(const std::vector<Block>&)> walk = [&](const std::vector<Block>& blocks) {
    for (const Block& b : blocks) {
      if (b.kind == BlockKind::ContentControl) walk(doc.Blocks(b.node));
      if (b.kind != BlockKind::Table) continue;
      const std::vector<Node> rows = doc.TableRows(b.node);
      if (rows.empty()) {
        ++problems["a table without rows"];
        continue;
      }
      int32_t width = 0;
      std::vector<std::vector<std::pair<int32_t, CellProps>>> grid;
      for (Node tr : rows) {
        const RowProps rp = doc.ResolvedRow(tr);
        int32_t col = rp.gridBefore;
        grid.emplace_back();
        const std::vector<Node> cells = doc.RowCells(tr);
        if (cells.empty()) ++problems["a row without cells"];
        for (Node tc : cells) {
          const CellProps cp = doc.ResolvedCell(tc);
          grid.back().emplace_back(col, cp);
          col += std::max(1, cp.gridSpan);
          const std::vector<Block> inner = doc.Blocks(tc);
          if (inner.empty() || inner.back().kind != BlockKind::Paragraph) ++problems["a cell that doesn't end with a paragraph"];
          walk(inner);
        }
        width = std::max(width, col + rp.gridAfter);
      }
      if (static_cast<int32_t>(doc.TableGrid(b.node).size()) != width) ++problems["a grid not as wide as its widest row"];
      for (size_t r = 0; r < grid.size(); ++r)
        for (const auto& [col, cp] : grid[r]) {
          if (cp.vMerge != VMerge::Continue) continue;
          bool above = false;
          if (r > 0)
            for (const auto& [c2, p2] : grid[r - 1])
              above = above || (c2 == col && p2.gridSpan == cp.gridSpan && p2.vMerge != VMerge::None);
          if (!above) ++problems["a vertically merged cell that continues nothing above it"];
        }
    }
  };
  for (uint32_t s = 0; s < doc.Stories().size(); ++s) walk(doc.Blocks(s));
  return problems;
}

// The shape of the tables among `blocks`, nested ones too: for each, its rows, each row's cells
// and each cell's paragraphs ("table {1,1,2}{1,1}").
void TableShapes(const Document& doc, const std::vector<Block>& blocks, std::string& out) {
  for (const Block& b : blocks) {
    if (b.kind == BlockKind::ContentControl) {
      TableShapes(doc, doc.Blocks(b.node), out);
      continue;
    }
    if (b.kind != BlockKind::Table) continue;
    std::string shape = "table ";
    std::vector<std::vector<Block>> inner;
    for (Node tr : doc.TableRows(b.node)) {
      shape += "{";
      bool first = true;
      for (Node tc : doc.RowCells(tr)) {
        std::vector<Block> cell = doc.Blocks(tc);
        uint32_t paras = 0;
        for (const Block& c : cell) paras += c.kind == BlockKind::Paragraph;
        shape += (first ? "" : ",") + std::to_string(paras);
        first = false;
        inner.push_back(std::move(cell));
      }
      shape += "}";
    }
    out += shape + "\n";
    for (const auto& cell : inner) TableShapes(doc, cell, out);
  }
}

// Every paragraph's line, by paragraph index: for checking that an edit changed only its own.
std::vector<std::string> ParaLines(const Document& doc) {
  std::vector<std::string> lines(doc.Paragraphs().size());
  for (uint32_t p = 0; p < lines.size(); ++p) lines[p] = ParaLine(doc, p);
  return lines;
}

#if FW_TORTURE_D4
// What's wrong with the document saved as RTF and as text (Export.h), or nothing: the RTF is
// ASCII (other characters as \uN), one group from {\rtf1 to its last }, every group closed;
// the UTF-8 and UTF-16 text files start with their byte order marks and say the same.
std::string ExportProblem(const Document& doc) {
  const std::string rtf = fw::doc::edit::RtfFile(doc);
  if (!rtf.starts_with("{\\rtf1")) return "the RTF doesn't start with {\\rtf1";
  int depth = 0;
  for (size_t i = 0; i < rtf.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(rtf[i]);
    if (c >= 0x80) return "the RTF has a byte above 127 at " + std::to_string(i);
    if (depth == 0 && i > 0 && c != '\r' && c != '\n' && c != ' ' && c != 0)
      return "the RTF goes on after its last } (byte " + std::to_string(i) + ")";
    if (c == '\\') ++i;  // an escaped character, or a control word's first letter
    else if (c == '{') ++depth;
    else if (c == '}' && --depth < 0) return "the RTF closes a group it didn't open, at byte " + std::to_string(i);
  }
  if (depth != 0) return "the RTF leaves " + std::to_string(depth) + " groups open";
  const std::string utf8 = fw::doc::edit::TextFile(doc, fw::doc::edit::TextEncoding::Utf8);
  const std::string utf16 = fw::doc::edit::TextFile(doc, fw::doc::edit::TextEncoding::Utf16);
  if (!utf8.starts_with("\xEF\xBB\xBF")) return "the UTF-8 text file has no byte order mark";
  if (!utf16.starts_with("\xFF\xFE") || utf16.size() % 2 != 0) return "the UTF-16 text file has no byte order mark, or an odd length";
  std::u16string wide;
  for (size_t i = 2; i + 1 < utf16.size(); i += 2)
    wide += static_cast<char16_t>(static_cast<unsigned char>(utf16[i]) | static_cast<unsigned char>(utf16[i + 1]) << 8);
  std::string again;
  fw::doc::AppendUtf8(again, wide);
  if (again != utf8.substr(3)) {
    size_t at = 0;
    while (at < again.size() && at < utf8.size() - 3 && again[at] == utf8[at + 3]) ++at;
    return "the UTF-8 and UTF-16 text files differ at byte " + std::to_string(at) + " of the text";
  }
  return {};
}
#endif

// What a document says, for comparing the edited model with the saved file read back: each story's
// paragraphs with their style, text, and where the formatting changes (so run splits don't matter).
std::string Digest(const Document& doc) {
  static const char* kinds[] = {"main", "header", "footer", "footnote", "endnote", "comment", "text box"};
  // A text box counts while its drawing is still in a story that counts: one whose drawing was
  // deleted is gone from the file, though the edited model may keep its story.
  // A header or footer counts while a section refers to it: saving leaves out one nothing refers
  // to (D3: a new one undone, a section break deleted), though the edited model keeps its story.
  std::vector<bool> live(doc.Stories().size()), shown(doc.Stories().size());
  for (const Section& section : doc.Sections()) {
    for (uint32_t h : section.headers)
      if (h < shown.size()) shown[h] = true;
    for (uint32_t f : section.footers)
      if (f < shown.size()) shown[f] = true;
  }
  // A comment counts while the text refers to it: one added and undone keeps a story in the model
  // (D4), which saving leaves out.
  std::vector<bool> referred(doc.Stories().size());
  for (const CommentInfo& c : doc.Comments())
    if (c.story < referred.size() && (c.reference.para != kNone || c.start.para != kNone)) referred[c.story] = true;
  for (uint32_t s = 0; s < live.size(); ++s) {
    const StoryKind kind = doc.Stories()[s].kind;
    live[s] = kind != StoryKind::TextBox && ((kind != StoryKind::Header && kind != StoryKind::Footer) || shown[s]) &&
              (kind != StoryKind::Comment || referred[s]);
  }
  for (bool grew = true; grew;) {
    grew = false;
    for (uint32_t s = 0; s < live.size(); ++s) {
      if (!live[s]) continue;
      for (uint32_t p : doc.StoryParagraphs(s))
        for (const InlineItem& item : doc.Items(p)) {
          if (item.kind != ItemKind::Drawing || item.drawing >= doc.Drawings().size()) continue;
          for (uint32_t box : doc.Drawings()[item.drawing].textBoxes)
            if (box < live.size() && !live[box]) live[box] = grew = true;
        }
    }
  }
  std::string main;
  std::vector<std::string> stories;
  for (uint32_t s = 0; s < doc.Stories().size(); ++s) {
    if (!live[s]) continue;
    // A note or comment added and undone (D4) leaves its story in the model without paragraphs;
    // saving leaves it out.
    if (s != doc.MainStory() && doc.StoryParagraphs(s).empty()) continue;
    const Story& story = doc.Stories()[s];
    // Named so that the same story sorts to the same place on both sides, whatever its text: notes
    // and comments by id, headers and footers by their part. Text boxes go by their text: a new
    // shape's story comes last in the edited model but in document order when read (D4).
    std::string name = story.kind == StoryKind::TextBox ? std::string()
                       : !story.id.empty()             ? story.id
                       : story.part < doc.Parts().size() ? doc.Parts()[story.part].name
                                                         : std::string();
    std::string out = std::string("[") + kinds[static_cast<int>(story.kind)] + " " + name + "]\n";
    for (uint32_t p : doc.StoryParagraphs(s)) out += ParaLine(doc, p) + "\n";
    TableShapes(doc, doc.Blocks(s), out);
    if (s == doc.MainStory()) main = std::move(out);
    else stories.push_back(std::move(out));
  }
  std::sort(stories.begin(), stories.end());  // after the main story, by kind and id
  for (const auto& s : stories) main += s;
  return main;
}

// The first line where two digests differ, both sides.
std::string FirstDifference(const std::string& a, const std::string& b) {
  size_t i = 0, line = 0;
  while (i < a.size() && i < b.size() && a[i] == b[i]) {
    if (a[i] == '\n') line = i + 1;
    ++i;
  }
  auto lineAt = [](const std::string& s, size_t from) {
    size_t end = s.find('\n', from);
    std::string l = s.substr(from, end == std::string::npos ? std::string::npos : end - from);
    return l.size() > 160 ? l.substr(0, 160) + "..." : l;
  };
  // The story it's in: the last "[kind id]" line at or before it.
  std::string story = "main";
  for (size_t at = line;; at = a.rfind('\n', at - 2) + 1) {
    if (a.compare(at, 1, "[") == 0) {
      story = lineAt(a, at);
      break;
    }
    if (at < 2) break;
  }
  return "in " + story + ": edited \"" + lineAt(a, line) + "\", read back \"" + lineAt(b, line) + "\"";
}

// The first paragraph whose line differs, for people; empty when none does.
std::string LinesDiffer(const std::vector<std::string>& was, const std::vector<std::string>& now) {
  auto cut = [](const std::string& s) { return s.size() > 120 ? s.substr(0, 120) + "..." : s; };
  for (size_t p = 0; p < std::max(was.size(), now.size()); ++p)
    if (p >= was.size() || p >= now.size() || was[p] != now[p])
      return "paragraph " + std::to_string(p) + ": expected \"" + (p < was.size() ? cut(was[p]) : "(none)") + "\", got \"" +
             (p < now.size() ? cut(now[p]) : "(none)") + "\"";
  return {};
}

// Pictures a file keeps that nothing shows: an image relationship its part doesn't mention, and a
// media part no relationship points to. Saving leaves out a deleted picture's (docx::Serialize), so
// a saved file has no more of these than the original.
std::map<std::string, int> MediaProblems(const fw::pkg::Package& pkg) {
  std::map<std::string, int> out;
  std::set<std::string> targets;
  for (const fw::pkg::RelationshipSet& set : pkg.Relationships()) {
    std::string xml;
    bool read = false;
    for (const fw::pkg::Relationship& r : set.items) {
      if (r.external) continue;
      targets.insert(fw::pkg::PartKey(r.targetPart));
      if (!r.type.ends_with("/image") || set.source == "/") continue;
      if (!read) {
        std::vector<uint8_t> bytes;
        if (pkg.ReadPart(set.source, bytes).Ok()) xml.assign(bytes.begin(), bytes.end());
        read = true;
      }
      if (!xml.empty() && xml.find("\"" + r.id + "\"") == std::string::npos && xml.find("'" + r.id + "'") == std::string::npos)
        ++out["an image relationship its part doesn't use"];
    }
  }
  for (const fw::pkg::Part& p : pkg.Parts()) {
    const std::string key = fw::pkg::PartKey(p.name);
    if (!p.deleted && key.starts_with("/word/media/") && !targets.contains(key)) ++out["a picture part nothing refers to"];
  }
  return out;
}

// Runs fn, catching crashes (access violations and the like) and C++ exceptions alike: a crash in
// the editor is a finding with a reproducer, not the end of the run.
using Thunk = void (*)(void*);
DWORD Guard(Thunk fn, void* context) {
  __try {
    fn(context);
    return 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return GetExceptionCode();
  }
}
template <class F> DWORD Guarded(F& fn) {
  return Guard([](void* c) { (*static_cast<F*>(c))(); }, &fn);
}
std::string CrashText(DWORD code) {
  char buf[64];
  std::snprintf(buf, sizeof buf, code == 0xE06D7363 ? "a C++ exception" : "exception 0x%08lX", code);
  return buf;
}

struct Settings {
  uint64_t seed = 1;
  uint32_t edits = 40, rounds = 3;
  fs::path out;
  std::chrono::seconds minimizeBudget{90};
};

struct Failure {
  std::string category;  // crash, spill (an edit changed what it shouldn't), save, open, check, reopen, differs
  std::string message;
};

struct RoundResult {
  std::optional<Failure> failure;
  std::vector<Edit> applied;  // every edit tried, in order (refused ones too: they're part of the replay)
  uint32_t done = 0, refused = 0;
  std::vector<uint8_t> saved;
  std::vector<std::string> written;  // the parts the save wrote
  std::string edited, readBack;      // the digests, when they differ
};

std::string Hex(const fw::pkg::Sha256& h) {
  static const char* digits = "0123456789abcdef";
  std::string s;
  for (uint8_t b : h) s += digits[b >> 4], s += digits[b & 15];
  return s;
}

// One round: open, apply the edits (generated, or replayed from `replay`), save and check.
RoundResult RunRound(const fs::path& file, uint64_t seed, uint32_t edits, const std::vector<Edit>* replay,
                     const fs::path& log) {
  RoundResult r;
  auto fail = [&](std::string category, std::string message) {
    if (!r.failure) r.failure = Failure{std::move(category), std::move(message)};
  };
  fw::pkg::Status st;
  auto pkg = OpenPackage(file, st);
  std::unique_ptr<Document> doc = pkg ? docx::Read(std::move(pkg), st) : nullptr;
  if (!doc) {
    fail("open", "the model refuses the original: " + st.message);
    return r;
  }
  std::vector<std::string> known = fw::pkg::CheckPackage(doc->Package()).errors;
  const std::map<std::string, int> knownGrid = GridProblems(*doc);
  const std::map<std::string, int> knownMedia = MediaProblems(doc->Package());
  std::unique_ptr<EditTarget> target = torture::OpenTarget(std::move(doc), st);
  if (!target) {
    fail("open", "the editor refuses it: " + st.message);
    return r;
  }
  // Saves, then checks the saved file: it opens, is consistent, and reads back as the edited
  // document. `when` names a save before the last ("after edit 12: "). False when it failed.
  auto saveAndCheck = [&](const std::string& when) {
    std::string before;
    auto save = [&] {
      before = Digest(target->Doc());
      st = target->Save(r.saved);
      r.written = target->written;
    };
    if (DWORD crash = Guarded(save)) return fail("crash", when + CrashText(crash) + " while saving"), false;
    if (!st.Ok()) return fail("save", when + st.message), false;
    auto again = fw::pkg::Package::Open(fw::pkg::Snapshot::FromBytes(r.saved), fw::pkg::Limits::Default(), st);
    if (again) st = again->Verify();
    if (!st.Ok()) return fail("open", when + "the saved file doesn't open: " + st.message), false;
    for (const std::string& error : fw::pkg::CheckPackage(*again).errors)
      if (std::find(known.begin(), known.end(), error) == known.end()) return fail("check", when + error), false;
    // Only where saving leaves out pictures nothing uses (D3's pictures, docx::Serialize); D2's Save
    // keeps a deleted picture's relationship, which does no harm.
    if constexpr (FW_TORTURE_D3 != 0) {
      for (const auto& [problem, count] : MediaProblems(*again)) {
        const auto had = knownMedia.find(problem);
        const int original = had == knownMedia.end() ? 0 : had->second;
        if (count > original)
          return fail("media", when + problem + " (" + std::to_string(count) + " in the saved file, " +
                                   std::to_string(original) + " in the original)"),
                 false;
      }
    }
#if FW_TORTURE_D4
    // Save as RTF and as plain text, from the edited model.
    std::string exported;
    auto exportIt = [&] { exported = ExportProblem(target->Doc()); };
    if (DWORD crash = Guarded(exportIt)) return fail("crash", when + CrashText(crash) + " saving as RTF or text"), false;
    if (!exported.empty()) return fail("export", when + exported), false;
#endif
    std::unique_ptr<Document> reread;
    auto reopen = [&] { reread = docx::Read(std::move(again), st); };
    if (DWORD crash = Guarded(reopen)) return fail("crash", when + CrashText(crash) + " reading the saved file"), false;
    if (!reread) return fail("reopen", when + st.message), false;
    std::map<std::string, int> grid;
    auto tables = [&] { grid = GridProblems(*reread); };
    if (DWORD crash = Guarded(tables)) return fail("crash", when + CrashText(crash) + " reading the saved file's tables"), false;
    for (const auto& [problem, count] : grid) {
      const auto had = knownGrid.find(problem);
      const int original = had == knownGrid.end() ? 0 : had->second;
      if (count > original)
        return fail("grid", when + problem + " (" + std::to_string(count) + " in the saved file, " + std::to_string(original) +
                                " in the original)"),
               false;
    }
    std::string after;
    auto digest = [&] { after = Digest(*reread); };
    if (DWORD crash = Guarded(digest)) return fail("crash", when + CrashText(crash) + " reading the saved file's text"), false;
    if (before != after) {
      fail("differs", when + FirstDifference(before, after));
      r.edited = std::move(before);
      r.readBack = std::move(after);
      return false;
    }
    return true;
  };

  // The edits go to the log before they're applied, so that a crash that ends fwtool still leaves them.
  std::ofstream logFile;
  if (!log.empty()) logFile.open(log, std::ios::trunc);
  Rng rng(seed);
  Generator gen(target->Doc(), rng);
  uint32_t undoable = 0, undone = 0;
  uint32_t groupLeft = 0;     // edits left in an open group
  bool groupCounted = false;  // and whether its undo step is counted yet
  size_t count = replay ? replay->size() : edits;
  for (size_t i = 0; i < count && !r.failure; ++i) {
    Edit e;
    DWORD crash = 0;
    if (replay) {
      e = (*replay)[i];
    } else {
      auto next = [&] { e = gen.Next(undoable, undone); };
      if ((crash = Guarded(next)) != 0) {
        fail("crash", "generating an edit: " + CrashText(crash));
        break;
      }
    }
    if (logFile) logFile << torture::FormatEdit(e) << std::endl;
    r.applied.push_back(e);
    if (e.kind == EditKind::Save) {  // as Ctrl+S: saved and checked, and the edits go on from there
      ++r.done;
      if (!saveAndCheck("after edit " + std::to_string(i) + ": ")) break;
      target->Saved();
      groupLeft = 0;  // saving closed an open group
      continue;
    }
    Applied applied = Applied::Refused;
    std::string why;
    // An edit changes only the paragraphs it's applied to (a delete, those it spans; a join, the two):
    // the others must read the same after, those after it moved by the paragraphs it added or
    // removed. Reading every paragraph first fills the model's caches, which would hide a stale cache
    // from the read-back comparison, so half the rounds (by seed) check this and the other half that.
    // Formatting commands are local too, except changing a style, which changes every paragraph that
    // uses it. (List numbers aren't in the paragraph lines.)
    // D3's operations (tables, pictures, fields, headers) aren't checked for it: they add paragraphs
    // and parts by design.
    static const std::set<std::string> formatting = {
        "bullets", "numbering", "multilevel", "nolist", "restart", "continue", "indent", "outdent", "apply",
        "toggle", "grow", "shrink", "grow1", "shrink1", "clearchars", "clearpara", "clearall", "font",
        "spacing", "spacebefore", "border", "tabs", "para", "case", "newstyle", "themefont", "themecolor"};
    const bool styleChange = e.kind == EditKind::Command && (e.format == "update" || e.format.starts_with("modify="));
    const bool formattingCommand = e.kind == EditKind::Command && formatting.contains(e.format.substr(0, e.format.find('=')));
    bool local = e.kind == EditKind::Type || e.kind == EditKind::Delete || e.kind == EditKind::Replace ||
                 e.kind == EditKind::Split || e.kind == EditKind::Join || e.kind == EditKind::Format ||
                 e.kind == EditKind::Paint || (formattingCommand && !styleChange);
    bool locality = local && (seed & 1);
    // A refused edit leaves the document as it was: always its State(), and on the same rounds every
    // paragraph's line. Inside a group the state changes when the group's last edit closes it, so
    // there only the lines are compared.
    const bool refusable = e.kind != EditKind::Group;
    const bool compareState = refusable && groupLeft == 0;
    const bool readAll = (seed & 1) && (locality || (refusable && e.kind != EditKind::Undo && e.kind != EditKind::Redo));
    std::vector<std::string> linesBefore, linesAfter;
    std::string stateBefore;
    bool refusalChanged = false;
    std::string refusalWhat;
    const size_t storiesBefore = target->Doc().Stories().size();
    // Deleting a note's reference deletes the note (D4): another story's paragraphs go, and the
    // paragraphs no longer line up by index, so such an edit isn't checked for locality.
    auto others = [&](uint32_t story) {
      size_t n = 0;
      for (uint32_t p = 0; p < target->Doc().Paragraphs().size(); ++p) n += target->Doc().Para(p).story != story;
      return n;
    };
    auto apply = [&] {
      if (compareState) stateBefore = target->State();
      if (readAll) linesBefore = ParaLines(target->Doc());
      const uint32_t own = locality && e.at.para < target->Doc().Paragraphs().size() ? target->Doc().Para(e.at.para).story : kNone;
      const size_t othersBefore = own == kNone ? 0 : others(own);
      applied = target->Apply(e, why);
      if (locality && applied == Applied::Done && (own == kNone || others(own) == othersBefore)) linesAfter = ParaLines(target->Doc());
      if (refusable && applied == Applied::Refused) {
        const std::string stateAfter = compareState ? target->State() : std::string();
        if (stateAfter != stateBefore) {
          refusalChanged = true;
          refusalWhat = "before: " + stateBefore + "; after: " + stateAfter;
        } else if (readAll) {
          std::vector<std::string> now = ParaLines(target->Doc());
          for (size_t p = 0; p < std::max(now.size(), linesBefore.size()) && !refusalChanged; ++p)
            if (p >= now.size() || p >= linesBefore.size() || now[p] != linesBefore[p]) {
              refusalChanged = true;
              refusalWhat = "paragraph " + std::to_string(p) + ": before \"" +
                            (p < linesBefore.size() ? linesBefore[p].substr(0, 120) : "(none)") + "\", after \"" +
                            (p < now.size() ? now[p].substr(0, 120) : "(none)") + "\"";
            }
        }
      }
    };
    if ((crash = Guarded(apply)) != 0) {
      fail("crash", CrashText(crash) + " in: " + torture::FormatEdit(e));
      break;
    }
    // A group: the next `count` edits, done or refused, are one undo step (an undo or redo ends it).
    bool inGroup = false;
    if (e.kind == EditKind::Group) {
      if (applied == Applied::Done) groupLeft = e.count, groupCounted = false;
    } else if (groupLeft) {
      if (e.kind == EditKind::Undo || e.kind == EditKind::Redo) groupLeft = 0;
      else inGroup = true, --groupLeft;
    }
    if (applied == Applied::Refused) {
      ++r.refused;
      if (why.starts_with("wrong: ")) {  // the model contradicts itself (D4's Tab in a form)
        fail("wrong", "\"" + torture::FormatEdit(e) + "\": " + why.substr(7));
        break;
      }
      if (refusalChanged) {
        fail("refused", "\"" + torture::FormatEdit(e) + "\" was refused (" + why + ") but changed the document: " + refusalWhat);
        break;
      }
      continue;
    }
    ++r.done;
    if (!linesAfter.empty()) {
      uint32_t lo = std::min(e.at.para, e.to.para), hi = std::max(e.at.para, e.to.para);
      if (e.kind == EditKind::Type || e.kind == EditKind::Split) lo = hi = e.at.para;
      if (e.kind == EditKind::Join) lo = e.at.para, hi = e.at.para + 1;
      int64_t shift = static_cast<int64_t>(linesAfter.size()) - static_cast<int64_t>(linesBefore.size());
      if ((e.kind == EditKind::Format || e.kind == EditKind::Command || e.kind == EditKind::Paint) && shift != 0) {
        fail("spill", "formatting changed the number of paragraphs: " + torture::FormatEdit(e));
        break;
      }
      auto cut = [](const std::string& s) { return s.size() > 120 ? s.substr(0, 120) + "..." : s; };
      // A paragraph without w:pStyle takes the default paragraph style. In a document that has
      // none, a command that adds one (Normal, as List Paragraph's base) changes its style's name
      // from nothing to that, and nothing else: not a spill.
      auto same = [](const std::string& before, const std::string& after) {
        if (after == before) return true;
        const size_t space = after.find(' ');
        return before.starts_with(" ") && space != std::string::npos && after.compare(space, std::string::npos, before) == 0;
      };
      for (uint32_t p = 0; p < linesBefore.size(); ++p) {
        if (p >= lo && p <= hi) continue;
        int64_t q = p > hi ? static_cast<int64_t>(p) + shift : static_cast<int64_t>(p);
        if (q >= 0 && q < static_cast<int64_t>(linesAfter.size()) && same(linesBefore[p], linesAfter[static_cast<size_t>(q)])) continue;
        const Story& story = target->Doc().Stories()[target->Doc().Para(static_cast<uint32_t>(std::max<int64_t>(0, std::min<int64_t>(q, static_cast<int64_t>(linesAfter.size()) - 1)))).story];
        std::string where = story.part < target->Doc().Parts().size() ? target->Doc().Parts()[story.part].name : "?";
        std::string after = q >= 0 && q < static_cast<int64_t>(linesAfter.size()) ? cut(linesAfter[static_cast<size_t>(q)]) : "(gone)";
        fail("spill", "\"" + torture::FormatEdit(e) + "\" also changed paragraph " + std::to_string(p) + " (" + where +
                          "): before \"" + cut(linesBefore[p]) + "\", after \"" + after + "\"");
        break;
      }
      if (r.failure) break;
    }
    // Undo puts back exactly what a command changed, and redo does it again: the formatting
    // commands, D3's operations and paste, on the rounds that read every paragraph, outside groups.
    // A command that changed no paragraph's line is left alone (it may have recorded no step).
    if (e.kind == EditKind::Command && readAll && !inGroup && groupLeft == 0) {
      std::string problem;
      auto check = [&] {
        const std::vector<std::string> after = linesAfter.empty() ? ParaLines(target->Doc()) : linesAfter;
        if (after == linesBefore) return;
        Edit undo, redo;
        undo.kind = EditKind::Undo;
        redo.kind = EditKind::Redo;
        std::string whyNot;
        if (target->Apply(undo, whyNot) != Applied::Done) return void(problem = "couldn't be undone (" + whyNot + ")");
        // A header or footer part a command made stays when it's undone, unreferenced (D3 headers:
        // EnsureHeaderFooter's parts are made outside undo), so its paragraphs aren't compared.
        std::vector<std::string> undone;
        const std::vector<std::string> now = ParaLines(target->Doc());
        for (uint32_t p = 0; p < now.size(); ++p)
          if (target->Doc().Para(p).story < storiesBefore) undone.push_back(now[p]);
        if (std::string d = LinesDiffer(linesBefore, undone); !d.empty()) return void(problem = "wasn't undone exactly: " + d);
        if (target->Apply(redo, whyNot) != Applied::Done) return void(problem = "couldn't be redone (" + whyNot + ")");
        if (std::string d = LinesDiffer(after, ParaLines(target->Doc())); !d.empty()) problem = "wasn't redone exactly: " + d;
      };
      if ((crash = Guarded(check)) != 0) {
        fail("crash", CrashText(crash) + " undoing or redoing: " + torture::FormatEdit(e));
        break;
      }
      if (!problem.empty()) {
        fail("undo", "\"" + torture::FormatEdit(e) + "\" " + problem);
        break;
      }
    }
    if (e.kind == EditKind::Undo) undone += e.count, undoable -= std::min(undoable, e.count);
    else if (e.kind == EditKind::Redo) undoable += e.count, undone -= std::min(undone, e.count);
    else if (e.kind == EditKind::Group) continue;
    else if (inGroup && groupCounted) undone = 0;  // part of a step already counted
    else ++undoable, undone = 0, groupCounted = inGroup;
  }
  if (r.failure) return r;
  saveAndCheck("");
  return r;
}

// Delta debugging (Zeller's ddmin): the fewest edits that still fail the same way, within a time budget.
std::vector<Edit> Minimize(std::vector<Edit> edits, const std::function<bool(const std::vector<Edit>&)>& fails,
                           std::chrono::seconds budget) {
  auto deadline = std::chrono::steady_clock::now() + budget;
  size_t n = 2;
  while (edits.size() >= 2 && std::chrono::steady_clock::now() < deadline) {
    size_t chunk = (edits.size() + n - 1) / n;
    bool reduced = false;
    for (size_t start = 0; start < edits.size() && std::chrono::steady_clock::now() < deadline; start += chunk) {
      // Without this chunk.
      std::vector<Edit> rest(edits.begin(), edits.begin() + start);
      rest.insert(rest.end(), edits.begin() + std::min(start + chunk, edits.size()), edits.end());
      if (!rest.empty() && fails(rest)) {
        edits = std::move(rest);
        n = std::max<size_t>(n - 1, 2);
        reduced = true;
        break;
      }
    }
    if (!reduced) {
      if (n >= edits.size()) break;
      n = std::min(n * 2, edits.size());
    }
  }
  return edits;
}

// A file's name for what the run writes about it: its path, flattened, without the extension.
std::string SafeName(const fs::path& file) {
  std::string name = Display(fs::path(file).replace_extension());
  for (char& c : name)
    if (c == '/' || c == '\\' || c == ':' || c == ' ') c = '_';
  while (!name.empty() && (name.front() == '.' || name.front() == '_')) name.erase(name.begin());
  return name;
}

void WriteBytes(const fs::path& path, const std::vector<uint8_t>& bytes) {
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

std::string JsonText(std::string_view s) {
  std::string out = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') out += '\\', out += c;
    else if (static_cast<unsigned char>(c) < 0x20) {
      char buf[8];
      std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
      out += buf;
    } else out += c;
  }
  return out + "\"";
}

// A failure, shrunk and kept: the edits as text (fwtool torture --replay reads them back) and the
// saved file they make.
// A list of edits as fwtool torture --replay reads it: where it comes from, then one edit per line.
void WriteEdits(const fs::path& path, const fs::path& file, uint64_t seed, uint32_t round, const std::string& failure,
                const std::vector<Edit>& edits, const std::string& count) {
  std::string sha = "unknown";
  if (std::ifstream in{file, std::ios::binary}) {
    std::vector<uint8_t> original(std::istreambuf_iterator<char>(in), {});
    sha = Hex(fw::pkg::HashBytes(original));
  }
  std::ofstream txt(path, std::ios::trunc);
  txt << "# fwtool torture reproducer: fwtool torture --replay " << fw::WideToUtf8(path.generic_wstring()) << "\n";
  txt << "file: " << fw::WideToUtf8(fs::absolute(file).generic_wstring()) << "\n";
  txt << "sha256: " << sha << "\n";
  txt << "seed: " << seed << "\nround: " << round << "\n";
  txt << "failure: " << failure << "\n";
  txt << "edits: " << count << "\n";
  for (const Edit& e : edits) txt << torture::FormatEdit(e) << "\n";
}

std::string KeepReproducer(const fs::path& file, uint64_t seed, uint32_t round, const RoundResult& failed,
                           const Settings& set, size_t& kept) {
  const Failure& f = *failed.failure;
  auto same = [&](const std::vector<Edit>& edits) {
    RoundResult r = RunRound(file, seed, 0, &edits, {});
    return r.failure && r.failure->category == f.category;
  };
  std::vector<Edit> fewest = Minimize(failed.applied, same, set.minimizeBudget);
  RoundResult last = RunRound(file, seed, 0, &fewest, {});
  if (!last.failure) {  // shrinking lost it (a flaky failure): keep them all
    last = failed;
    fewest = failed.applied;
  }
  kept = fewest.size();
  char stem[64];
  std::snprintf(stem, sizeof stem, "-seed%016llx", static_cast<unsigned long long>(seed));
  fs::path base = set.out / "repro" / (SafeName(file) + stem);
  fs::path txtPath = base, docxPath = base;  // names keep their dots: extensions are added, not replaced
  txtPath += ".txt";
  docxPath += ".docx";
  std::error_code ec;
  fs::create_directories(base.parent_path(), ec);
  WriteEdits(txtPath, file, seed, round, last.failure->category + ": " + last.failure->message, fewest,
             std::to_string(fewest.size()) + " of " + std::to_string(failed.applied.size()));
  if (!last.saved.empty()) WriteBytes(docxPath, last.saved);
  return fw::WideToUtf8(txtPath.generic_wstring());
}

bool IsWordFile(const fs::path& file) {
  std::wstring ext = fw::ToLower(file.extension().wstring());
  return ext == L".docx" || ext == L".docm" || ext == L".dotx" || ext == L".dotm";
}

// A reproducer names its file by the path it had where it was made. Elsewhere (another checkout),
// the same file is found by the longest tail of that path that exists from the current folder.
fs::path FindReproFile(const fs::path& recorded) {
  std::error_code ec;
  if (fs::exists(recorded, ec)) return recorded;
  const fs::path relative = recorded.relative_path();
  std::vector<fs::path> parts(relative.begin(), relative.end());
  for (size_t first = 1; first < parts.size(); ++first) {
    fs::path tail;
    for (size_t i = first; i < parts.size(); ++i) tail /= parts[i];
    if (fs::exists(tail, ec)) return tail;
  }
  return recorded;
}

// Replays a reproducer. The file it makes goes next to it (<name>-replay.docx), or into --out.
int Replay(const fs::path& repro, const fs::path& outDir) {
  std::ifstream in(repro);
  if (!in) {
    std::fprintf(stderr, "Can't read %s\n", Display(repro).c_str());
    return 2;
  }
  fs::path file;
  uint64_t seed = 0;
  std::string sha;
  std::vector<Edit> edits;
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    if (line.starts_with("file: ")) file = fs::path(fw::Utf8ToWide(line.substr(6)));
    else if (line.starts_with("seed: ")) seed = std::strtoull(line.c_str() + 6, nullptr, 10);
    else if (line.starts_with("sha256: ")) sha = line.substr(8);
    else if (line.starts_with("round: ") || line.starts_with("failure: ") || line.starts_with("edits: ")) continue;
    else {
      Edit e;
      if (!torture::ParseEdit(line, e)) {
        std::fprintf(stderr, "Can't read the edit \"%s\"\n", line.c_str());
        return 2;
      }
      edits.push_back(std::move(e));
    }
  }
  file = FindReproFile(file);
  if (std::ifstream original{file, std::ios::binary}; original && !sha.empty() && sha != "unknown") {
    std::vector<uint8_t> bytes(std::istreambuf_iterator<char>(original), {});
    if (Hex(fw::pkg::HashBytes(bytes)) != sha)
      std::fprintf(stderr, "Note: %s isn't the file this reproducer was made from (its SHA-256 differs).\n",
                   Display(file).c_str());
  }
  RoundResult r = RunRound(file, seed, 0, &edits, {});
  if (!r.saved.empty()) {
    fs::path made = outDir.empty() ? repro.parent_path() / repro.stem() : outDir / repro.stem();
    made += "-replay.docx";
    if (!outDir.empty()) {
      std::error_code ec;
      fs::create_directories(outDir, ec);
    }
    WriteBytes(made, r.saved);
    if (!r.edited.empty()) {  // what differs, in full: the edited model's text and the saved file's
      fs::path base = made;
      base.replace_extension();
      for (auto [suffix, text] : {std::pair{"-edited.txt", &r.edited}, std::pair{"-readback.txt", &r.readBack}}) {
        fs::path p = base;
        p += suffix;
        WriteBytes(p, std::vector<uint8_t>(text->begin(), text->end()));
      }
    }
  }
  std::string written;
  for (const std::string& part : r.written) written += (written.empty() ? "" : ", ") + part;
  if (r.failure) {
    std::printf("[FAIL] %s: %zu edits (%u done, %u refused): %s: %s\n", Display(file).c_str(), edits.size(), r.done,
                r.refused, r.failure->category.c_str(), r.failure->message.c_str());
  } else {
    std::printf("[ ok ] %s: %zu edits (%u done, %u refused)\n", Display(file).c_str(), edits.size(), r.done, r.refused);
  }
  std::printf("       parts written: %s\n", written.empty() ? "none" : written.c_str());
  return r.failure ? 1 : 0;
}

}  // namespace

int RunTorture(const Options& options) {
  // As the app's UI thread: inserting a BMP or SVG picture encodes a PNG with WIC.
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  Settings set;
  set.seed = options.seed ? options.seed : 1;
  set.edits = static_cast<uint32_t>(options.edits);
  set.rounds = options.rounds;
  if (options.replay) return Replay(options.inputs.front(), options.outDir);
  set.out = options.outDir.empty() ? fs::path("build") / "torture" : options.outDir;

  std::vector<fs::path> files;
  for (const auto& f : CollectFiles(options.inputs))
    if (IsWordFile(f)) files.push_back(f);
  if (files.empty()) {
    std::fprintf(stderr, "No Word documents found.\n");
    return 1;
  }
  std::error_code ec;
  fs::create_directories(set.out / "saved", ec);
  std::ofstream pairs(set.out / "pairs.tsv", std::ios::trunc);  // original, saved copy: for the validator and Word
  std::string json = "{\n  \"seed\": " + std::to_string(set.seed) + ", \"edits\": " + std::to_string(set.edits) +
                     ", \"rounds\": " + std::to_string(set.rounds) + ",\n  \"files\": [";
  int failedFiles = 0, refusedFiles = 0;
  uint64_t totalDone = 0, totalRefused = 0;
  const char* targetName = nullptr;
  for (size_t fi = 0; fi < files.size(); ++fi) {
    const fs::path& file = files[fi];
    std::string name = Display(file);
    std::string fileJson = "\n    {\"file\": " + JsonText(fw::WideToUtf8(fs::absolute(file).generic_wstring())) + ", \"failures\": [";
    uint32_t done = 0, refused = 0, failures = 0;
    bool unreadable = false;
    std::string note, savedPath;
    for (uint32_t round = 0; round < set.rounds; ++round) {
      uint64_t seed = RoundSeed(set.seed, fw::WideToUtf8(file.filename().wstring()), round);
      RoundResult r = RunRound(file, seed, set.edits, nullptr, set.out / "current-edits.txt");
      done += r.done, refused += r.refused;
      if (r.failure && r.failure->category == "open" && r.applied.empty()) {
        unreadable = true;  // the model doesn't read the original: D1's, not an editing failure
        note = r.failure->message;
        break;
      }
      if (!r.saved.empty() && savedPath.empty()) {
        fs::path saved = set.out / "saved" / (SafeName(file) + ".docx");
        WriteBytes(saved, r.saved);
        // Its edits, so that what the validator or Word finds in it can be replayed and shrunk
        // (scripts/torture-minimize.ps1). Not a .docx name: Word and the validator take folders.
        fs::path edits = set.out / "saved" / (SafeName(file) + ".edits.txt");
        WriteEdits(edits, file, seed, round, r.failure ? r.failure->category + ": " + r.failure->message : "none",
                   r.applied, std::to_string(r.applied.size()));
        savedPath = fw::WideToUtf8(fs::absolute(saved).generic_wstring());
        pairs << fw::WideToUtf8(fs::absolute(file).generic_wstring()) << "\t" << savedPath << "\n";
      }
      if (!r.failure) continue;
      size_t kept = 0;
      std::string repro = KeepReproducer(file, seed, round, r, set, kept);
      std::printf("[FAIL] %s: round %u: %s: %s\n       reproducer (%zu of %zu edits): %s\n", name.c_str(), round,
                  r.failure->category.c_str(), r.failure->message.c_str(), kept, r.applied.size(), repro.c_str());
      fileJson += std::string(failures ? "," : "") + "\n      {\"round\": " + std::to_string(round) + ", \"seed\": " +
                  std::to_string(seed) + ", \"category\": " + JsonText(r.failure->category) + ", \"message\": " +
                  JsonText(r.failure->message) + ", \"edits\": " + std::to_string(kept) + ", \"reproducer\": " +
                  JsonText(repro) + "}";
      ++failures;
    }
    totalDone += done, totalRefused += refused;
    if (unreadable) {
      ++refusedFiles;
      std::printf("[skip] %s: %s\n", name.c_str(), note.c_str());
    } else if (!failures) {
      std::printf("[ ok ] %s: %u rounds, %u edits done, %u refused\n", name.c_str(), set.rounds, done, refused);
    } else {
      ++failedFiles;
    }
    fileJson += std::string(failures ? "\n    " : "") + "], \"done\": " + std::to_string(done) + ", \"refused\": " +
                std::to_string(refused) + ", \"skipped\": " + (unreadable ? JsonText(note) : "null") +
                ", \"saved\": " + (savedPath.empty() ? "null" : JsonText(savedPath)) + "}";
    json += fileJson + (fi + 1 < files.size() ? "," : "");
    if (!targetName) {
      fw::pkg::Status st;
      auto pkg = OpenPackage(file, st);
      if (auto doc = pkg ? docx::Read(std::move(pkg), st) : nullptr) targetName = torture::OpenTarget(std::move(doc), st)->Name();
    }
  }
  json += "\n  ]\n}\n";
  if (!options.jsonOut.empty()) std::ofstream(options.jsonOut, std::ios::trunc) << json;
  fs::remove(set.out / "current-edits.txt", ec);
  std::printf("\n%zu documents, %d failed, %d not read by the model; %llu edits done, %llu refused (editor: %s)\n",
              files.size(), failedFiles, refusedFiles, static_cast<unsigned long long>(totalDone),
              static_cast<unsigned long long>(totalRefused), targetName ? targetName : "?");
  return failedFiles == 0 ? 0 : 1;
}

}  // namespace fwtool
