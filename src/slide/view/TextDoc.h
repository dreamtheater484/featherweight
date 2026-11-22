#pragma once

#include <pugixml.hpp>

#include <compare>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "slide/Text.h"

// Text being edited on a slide (P2, p2-text): a working copy of one text body (a shape's p:txBody or
// a table cell's a:txBody), the model's text resolved from it, and the edits typing and formatting
// make to it. The copy goes back to the model through the edit layer (TextModel.h); nothing here
// touches the presentation.
//
// Edits keep the XML as PowerPoint writes it: CT_TextBody's and CT_TextCharacterProperties'
// element order, no empty runs, neighbouring runs with the same properties joined, a line break as
// a:br, fields (slide number, date) kept whole.
namespace fw::slide::view {

// A place in the text: a paragraph and an offset in it. Offsets count the paragraph's runs as the
// model resolves them: a run's text (UTF-16), 1 for a line break, a field's text as shown.
struct TextPos {
  uint32_t para = 0, offset = 0;
  friend bool operator==(const TextPos&, const TextPos&) = default;
  friend std::strong_ordering operator<=>(const TextPos&, const TextPos&) = default;
};

class TextDoc {
 public:
  // Resolves the working copy as the model would (Presentation::ResolveTextBody, ResolveCellText).
  using Resolver = std::function<std::unique_ptr<TextBody>(pugi::xml_node txBody)>;

  // A copy of `txBody`, with the namespaces it uses declared on its root. Null: an empty body
  // (`element`, "p:txBody" for shapes, "a:txBody" for table cells).
  TextDoc(pugi::xml_node txBody, Resolver resolve, const char* element = "p:txBody");
  ~TextDoc();
  TextDoc(const TextDoc&) = delete;
  TextDoc& operator=(const TextDoc&) = delete;

  pugi::xml_node Body() const { return body_; }
  const TextBody& Resolved() const { return *resolved_; }
  // Bumped by every edit (and Reload), for caches.
  uint32_t Version() const { return version_; }
  // The model's text changed under it (undo, another edit): start again from `txBody`.
  void Reload(pugi::xml_node txBody);

  uint32_t ParagraphCount() const { return static_cast<uint32_t>(segs_.size()); }
  uint32_t Length(uint32_t para) const;
  TextPos End() const;
  TextPos Clamp(TextPos p) const;
  // A paragraph's text as laid out: a line break is U+000B, a field its text as shown.
  const std::wstring& Text(uint32_t para) const;
  // [from, to) with '\n' between paragraphs.
  std::wstring Text(TextPos from, TextPos to) const;
  bool Empty() const;  // one paragraph, no text
  // The field (slide number, date) around an offset: false when it isn't strictly inside one.
  bool InField(TextPos at, uint32_t& start, uint32_t& end) const;
  // Fields and runs the editor can't change (inside mc:AlternateContent) are kept whole: a range
  // touching one grows to cover it.
  void Widen(TextPos& from, TextPos& to) const;

  // ---- Edits. Each resolves the copy again. ----
  // Inserts text at `at`: '\r', '\n' or "\r\n" start a new paragraph, U+000B and U+2028 are a line
  // break, other control characters are left out. The text takes `props` (an a:rPr or
  // a:endParaRPr, copied), or when null what typing there takes (PropsAt). Returns where it ends.
  TextPos Insert(TextPos at, std::wstring_view text, pugi::xml_node props = {});
  // Deletes [from, to) (widened over fields). Returns where the text joins.
  TextPos Delete(TextPos from, TextPos to);
  // A new paragraph at `at`, with the paragraph's properties (Enter).
  TextPos Split(TextPos at);
  // The character properties typing at `at` takes: the run's before it, else after it, else the
  // paragraph mark's (a:endParaRPr). Null when none is written (all inherited). Links end with
  // their run: typing just after a link isn't linked.
  pugi::xml_node PropsAt(TextPos at) const;
  // Calls `edit` with the a:rPr of every run in [from, to) (made where missing; runs split at the
  // ends) and the a:endParaRPr of the paragraphs whose mark the range covers. False when the range
  // is empty.
  bool EditRuns(TextPos from, TextPos to, const std::function<void(pugi::xml_node rPr)>& edit);
  // Calls `edit` with a paragraph's mark (a:endParaRPr, made when missing): an empty paragraph's look.
  void EditMark(uint32_t para, const std::function<void(pugi::xml_node rPr)>& edit);
  // Calls `edit` with the a:pPr of paragraphs first..last (made where missing).
  void EditParagraphs(uint32_t first, uint32_t last, const std::function<void(pugi::xml_node pPr)>& edit);
  // The body's a:bodyPr (made where missing), to change autofit; `edit` then resolves again.
  void EditBody(const std::function<void(pugi::xml_node bodyPr)>& edit);
  // Replaces [from, to) with whole paragraphs (pasting Featherweight's own copies): the first one's
  // runs join the text before, the last one's the text after.
  TextPos InsertParagraphs(TextPos from, TextPos to, const std::vector<pugi::xml_node>& paragraphs);
  // The paragraphs [from, to) covers, as a:p elements in `into` (cut to the range), for copying.
  void CopyParagraphs(TextPos from, TextPos to, pugi::xml_node into) const;

  // The DrawingML prefix this body uses ("a:" or ""), for making elements.
  const std::string& Prefix() const { return a_; }
  // Element helpers in schema order: the child named `local` of an a:rPr / a:endParaRPr, or of an
  // a:pPr, made (in its place) when `make`.
  pugi::xml_node RunChild(pugi::xml_node rPr, std::string_view local, bool make) const;
  pugi::xml_node ParaChild(pugi::xml_node pPr, std::string_view local, bool make) const;
  // Removes the children of rPr that set a fill (noFill, solidFill, gradFill, ...).
  static void RemoveFills(pugi::xml_node rPr);
  static bool SameProps(pugi::xml_node a, pugi::xml_node b);

 private:
  struct Seg {
    pugi::xml_node node;  // a:r, a:br or a:fld
    uint32_t start = 0, length = 0;
    TextRun::Kind kind = TextRun::Kind::Text;
    bool locked = false;  // a field, or a run the editor can't edit (in mc:AlternateContent)
  };
  void Index();  // resolve, then rebuild segs_ and texts_
  pugi::xml_node Para(uint32_t para) const { return paras_[para]; }
  // Splits the Text run at an offset strictly inside it; returns the node now starting there.
  void SplitAt(uint32_t para, uint32_t offset);
  void DeleteInline(uint32_t para, uint32_t from, uint32_t to);
  void InsertInline(uint32_t para, uint32_t offset, std::wstring_view text, pugi::xml_node props);
  void InsertBreak(uint32_t para, uint32_t offset, pugi::xml_node props);
  pugi::xml_node NewRunAt(uint32_t para, uint32_t offset, const char* local);  // a new a:r / a:br there
  void SplitPara(uint32_t para, uint32_t offset);
  void JoinNext(uint32_t para);
  void Tidy(pugi::xml_node p);
  pugi::xml_node NewElement(pugi::xml_node parent, const char* local, pugi::xml_node before = {});
  pugi::xml_node MakeRunProps(pugi::xml_node run);  // the run's a:rPr, made first when missing
  pugi::xml_node EndProps(pugi::xml_node p, bool make);
  void CopyPropsInto(pugi::xml_node to, pugi::xml_node from);  // to's attributes and children := from's

  pugi::xml_document doc_;
  pugi::xml_node body_;
  Resolver resolve_;
  std::unique_ptr<TextBody> resolved_;
  std::string a_ = "a:";
  std::string element_;  // what an empty body is called
  std::vector<pugi::xml_node> paras_;
  std::vector<std::vector<Seg>> segs_;
  std::vector<std::wstring> texts_;
  uint32_t version_ = 0;
};

}  // namespace fw::slide::view
