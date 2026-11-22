#include "slide/view/TextDoc.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <sstream>

#include "core/Strings.h"
#include "draw/Xml.h"

namespace fw::slide::view {

using namespace draw::xml;

namespace {

// CT_TextCharacterProperties' children, in order (a fill and an effect are each one of a group).
constexpr std::array<std::string_view, 22> kRunOrder = {
    "ln",      "noFill", "solidFill", "gradFill", "blipFill", "pattFill", "grpFill", "effectLst",
    "effectDag", "highlight", "uLnTx", "uLn", "uFillTx", "uFill", "latin", "ea", "cs", "sym",
    "hlinkClick", "hlinkMouseOver", "rtl", "extLst"};
// CT_TextParagraphProperties' children.
constexpr std::array<std::string_view, 17> kParaOrder = {
    "lnSpc", "spcBef", "spcAft", "buClrTx", "buClr", "buSzTx", "buSzPct", "buSzPts", "buFontTx",
    "buFont", "buNone", "buAutoNum", "buChar", "buBlip", "tabLst", "defRPr", "extLst"};

template <size_t N>
int Rank(const std::array<std::string_view, N>& order, std::string_view local) {
  for (size_t i = 0; i < order.size(); ++i)
    if (order[i] == local) return static_cast<int>(i);
  return static_cast<int>(N) - 1;  // unknown: with the extensions, last
}

std::string Serialize(pugi::xml_node n) {
  std::ostringstream os;
  n.print(os, "", pugi::format_raw);
  return os.str();
}

// Whether an element is the editor's to change: in the paragraph itself, not inside
// mc:AlternateContent.
bool Editable(pugi::xml_node run, pugi::xml_node para) { return run.parent() == para; }

bool IsRunLevel(pugi::xml_node n) {
  const std::string_view k = LocalNameOf(n);
  return n.type() == pugi::node_element && k != "pPr" && k != "endParaRPr";
}

}  // namespace

TextDoc::TextDoc(pugi::xml_node txBody, Resolver resolve, const char* element)
    : resolve_(std::move(resolve)), element_(element) {
  Reload(txBody);
}

TextDoc::~TextDoc() = default;

void TextDoc::Reload(pugi::xml_node txBody) {
  doc_.reset();
  if (!txBody) {
    // An empty body, as PowerPoint writes one.
    body_ = doc_.append_child(element_.c_str());
    body_.append_attribute("xmlns:a") = std::string(kA).c_str();
    if (element_.starts_with("p:")) body_.append_attribute("xmlns:p") = std::string(kP).c_str();
    a_ = "a:";
    body_.append_child("a:bodyPr");
    body_.append_child("a:lstStyle");
    body_.append_child("a:p");
    Index();
    return;
  }
  body_ = doc_.append_copy(txBody);
  // The namespaces it uses, declared where they were (the part's root, usually), now on the copy.
  for (pugi::xml_node up = txBody.parent(); up; up = up.parent())
    for (pugi::xml_attribute at : up.attributes()) {
      const std::string_view name = at.name();
      if ((name == "xmlns" || name.starts_with("xmlns:")) && !body_.attribute(at.name())) body_.append_attribute(at.name()) = at.value();
    }
  // The DrawingML prefix: the one bodyPr or a paragraph has.
  a_ = "a:";
  for (pugi::xml_node c = body_.first_child(); c; c = c.next_sibling())
    if (c.type() == pugi::node_element && NamespaceOf(c) == kA) {
      const std::string_view n = c.name();
      const size_t colon = n.find(':');
      a_ = colon == std::string_view::npos ? std::string() : std::string(n.substr(0, colon + 1));
      break;
    }
  // At least one paragraph (CT_TextBody needs one).
  if (!A(body_, "p")) NewElement(body_, "p");
  Index();
}

void TextDoc::Index() {
  ++version_;
  resolved_ = resolve_ ? resolve_(body_) : nullptr;
  if (!resolved_) resolved_ = std::make_unique<TextBody>();
  paras_.clear();
  segs_.clear();
  texts_.clear();
  for (const Paragraph& p : resolved_->paragraphs) {
    paras_.push_back(p.node);
    std::vector<Seg>& segs = segs_.emplace_back();
    std::wstring& text = texts_.emplace_back();
    for (const TextRun& r : p.runs) {
      Seg s;
      s.node = r.node;
      s.kind = r.kind;
      s.start = static_cast<uint32_t>(text.size());
      if (r.kind == TextRun::Kind::Break) {
        text += L'\u000B';
      } else {
        for (wchar_t c : r.text) text += c == L'\n' || c == L'\r' ? L'\u000B' : c;
      }
      s.length = static_cast<uint32_t>(text.size()) - s.start;
      s.locked = r.kind == TextRun::Kind::Field || !Editable(r.node, p.node);
      if (s.length > 0 || r.kind != TextRun::Kind::Text) segs.push_back(s);
    }
  }
  if (paras_.empty()) {
    // A body the resolver didn't read (no resolver, in tests of the XML alone): its paragraphs as written.
    for (pugi::xml_node p = A(body_, "p"); p; p = p.next_sibling())
      if (NamespaceOf(p) == kA && LocalNameOf(p) == "p") {
        paras_.push_back(p);
        segs_.emplace_back();
        texts_.emplace_back();
      }
  }
}

uint32_t TextDoc::Length(uint32_t para) const {
  return para < texts_.size() ? static_cast<uint32_t>(texts_[para].size()) : 0;
}

TextPos TextDoc::End() const {
  if (texts_.empty()) return {};
  const uint32_t last = static_cast<uint32_t>(texts_.size()) - 1;
  return {last, Length(last)};
}

TextPos TextDoc::Clamp(TextPos p) const {
  if (texts_.empty()) return {};
  p.para = std::min<uint32_t>(p.para, static_cast<uint32_t>(texts_.size()) - 1);
  p.offset = std::min(p.offset, Length(p.para));
  return p;
}

const std::wstring& TextDoc::Text(uint32_t para) const {
  static const std::wstring none;
  return para < texts_.size() ? texts_[para] : none;
}

std::wstring TextDoc::Text(TextPos from, TextPos to) const {
  from = Clamp(from);
  to = Clamp(to);
  if (to < from) std::swap(from, to);
  std::wstring out;
  for (uint32_t p = from.para; p <= to.para; ++p) {
    const std::wstring& t = texts_[p];
    const uint32_t a = p == from.para ? from.offset : 0, b = p == to.para ? to.offset : static_cast<uint32_t>(t.size());
    if (p > from.para) out += L'\n';
    out.append(t, a, b - a);
  }
  return out;
}

bool TextDoc::Empty() const { return texts_.size() <= 1 && (texts_.empty() || texts_[0].empty()); }

bool TextDoc::InField(TextPos at, uint32_t& start, uint32_t& end) const {
  if (at.para >= segs_.size()) return false;
  for (const Seg& s : segs_[at.para])
    if (s.locked && at.offset > s.start && at.offset < s.start + s.length) {
      start = s.start;
      end = s.start + s.length;
      return true;
    }
  return false;
}

void TextDoc::Widen(TextPos& from, TextPos& to) const {
  uint32_t s, e;
  if (InField(from, s, e)) from.offset = s;
  if (InField(to, s, e)) to.offset = e;
}

std::string TextDoc::RelPrefix() {
  for (pugi::xml_attribute at : body_.attributes()) {
    const std::string_view n = at.name();
    if (n.starts_with("xmlns:") && std::string_view(at.value()) == kR) return std::string(n.substr(6)) + ":";
  }
  if (!body_.attribute("xmlns:r")) body_.append_attribute("xmlns:r") = std::string(kR).c_str();
  return "r:";
}

// ---------------------------------------------------------------------------
// Elements

pugi::xml_node TextDoc::NewElement(pugi::xml_node parent, const char* local, pugi::xml_node before) {
  const std::string name = a_ + local;
  return before ? parent.insert_child_before(name.c_str(), before) : parent.append_child(name.c_str());
}

namespace {

template <size_t N>
pugi::xml_node OrderedChild(pugi::xml_node parent, std::string_view local, bool make, const std::string& prefix,
                            const std::array<std::string_view, N>& order) {
  if (!parent) return {};
  const int want = Rank(order, local);
  pugi::xml_node after;
  for (pugi::xml_node c = parent.first_child(); c; c = c.next_sibling()) {
    if (c.type() != pugi::node_element) continue;
    const std::string_view k = LocalNameOf(c);
    if (k == local && NamespaceOf(c) == kA) return c;
    if (Rank(order, k) <= want) after = c;
  }
  if (!make) return {};
  const std::string name = prefix + std::string(local);
  return after ? parent.insert_child_after(name.c_str(), after) : parent.prepend_child(name.c_str());
}

}  // namespace

pugi::xml_node TextDoc::RunChild(pugi::xml_node rPr, std::string_view local, bool make) const {
  return OrderedChild(rPr, local, make, a_, kRunOrder);
}

pugi::xml_node TextDoc::ParaChild(pugi::xml_node pPr, std::string_view local, bool make) const {
  return OrderedChild(pPr, local, make, a_, kParaOrder);
}

void TextDoc::RemoveFills(pugi::xml_node rPr) {
  for (pugi::xml_node c = rPr.first_child(); c;) {
    pugi::xml_node next = c.next_sibling();
    const std::string_view k = LocalNameOf(c);
    if (k == "noFill" || k == "solidFill" || k == "gradFill" || k == "blipFill" || k == "pattFill" || k == "grpFill")
      rPr.remove_child(c);
    c = next;
  }
}

bool TextDoc::SameProps(pugi::xml_node a, pugi::xml_node b) {
  // Element names aside (rPr against endParaRPr), the same attributes (in any order; spelling and
  // smart tag marks don't count) and children.
  auto attrs = [](pugi::xml_node n) {
    std::vector<std::pair<std::string_view, std::string_view>> v;
    if (n)
      for (pugi::xml_attribute at : n.attributes()) {
        const std::string_view k = at.name();
        if (k == "err" || k == "dirty" || k == "smtClean" || k == "smtId") continue;
        v.emplace_back(k, at.value());
      }
    std::sort(v.begin(), v.end());
    return v;
  };
  if (attrs(a) != attrs(b)) return false;
  std::string ca, cb;
  if (a)
    for (pugi::xml_node c : a.children()) ca += Serialize(c);
  if (b)
    for (pugi::xml_node c : b.children()) cb += Serialize(c);
  return ca == cb;
}

namespace {

// The paragraph's own child holding `n` (n itself, or the mc:AlternateContent around it).
pugi::xml_node Top(pugi::xml_node n, pugi::xml_node p) {
  while (n && n.parent() != p) n = n.parent();
  return n;
}

}  // namespace

pugi::xml_node TextDoc::MakeRunProps(pugi::xml_node run) {
  pugi::xml_node rPr = A(run, "rPr");
  if (rPr && rPr.parent() == run) return rPr;
  return run.prepend_child((a_ + "rPr").c_str());
}

pugi::xml_node TextDoc::EndProps(pugi::xml_node p, bool make) {
  for (pugi::xml_node c = p.last_child(); c; c = c.previous_sibling())
    if (c.type() == pugi::node_element && LocalNameOf(c) == "endParaRPr") return c;
  return make ? NewElement(p, "endParaRPr") : pugi::xml_node();
}

void TextDoc::CopyPropsInto(pugi::xml_node to, pugi::xml_node from) {
  while (to.first_attribute()) to.remove_attribute(to.first_attribute());
  while (to.first_child()) to.remove_child(to.first_child());
  if (!from) return;
  for (pugi::xml_attribute at : from.attributes()) {
    // A spelling mark belongs to the text it was found in.
    if (std::string_view(at.name()) == "err") continue;
    to.append_attribute(at.name()) = at.value();
  }
  for (pugi::xml_node c : from.children()) to.append_copy(c);
}

// ---------------------------------------------------------------------------
// Positions to runs

pugi::xml_node TextDoc::PropsAt(TextPos at) const {
  at = Clamp(at);
  if (at.para >= segs_.size()) return {};
  const std::vector<Seg>& segs = segs_[at.para];
  const Seg* before = nullptr;
  const Seg* after = nullptr;
  for (const Seg& s : segs) {
    if (s.start < at.offset && at.offset <= s.start + s.length) before = &s;
    if (!after && s.start >= at.offset && s.length > 0) after = &s;
  }
  if (before) return A(before->node, "rPr");
  if (after) return A(after->node, "rPr");
  for (pugi::xml_node c = paras_[at.para].last_child(); c; c = c.previous_sibling())
    if (c.type() == pugi::node_element && LocalNameOf(c) == "endParaRPr") return c;
  return {};
}

void TextDoc::SplitAt(uint32_t para, uint32_t offset) {
  for (const Seg& s : segs_[para]) {
    if (offset <= s.start || offset >= s.start + s.length || s.kind != TextRun::Kind::Text || s.locked) continue;
    pugi::xml_node t = A(s.node, "t");
    const std::wstring text = Utf8ToWide(t.text().get());
    const uint32_t cut = std::min<uint32_t>(offset - s.start, static_cast<uint32_t>(text.size()));
    pugi::xml_node second = s.node.parent().insert_copy_after(s.node, s.node);
    t.text().set(WideToUtf8(text.substr(0, cut)).c_str());
    A(second, "t").text().set(WideToUtf8(text.substr(cut)).c_str());
    Index();
    return;
  }
}

// ---------------------------------------------------------------------------
// Inserting

namespace {

// Typed text without what XML can't hold or DrawingML writes otherwise.
std::wstring Clean(std::wstring_view s) {
  std::wstring out;
  out.reserve(s.size());
  for (wchar_t c : s) {
    if (c == L'\t' || (c >= 0x20 && c != 0xFFFE && c != 0xFFFF)) out += c;
  }
  return out;
}

}  // namespace

TextPos TextDoc::Insert(TextPos at, std::wstring_view text, pugi::xml_node props) {
  at = Clamp(at);
  uint32_t s, e;
  if (InField(at, s, e)) at.offset = e;
  // The properties typed text takes, copied now: the edits below move and remove runs.
  pugi::xml_document hold;
  pugi::xml_node style;
  pugi::xml_node from = props ? props : PropsAt(at);
  if (from) {
    style = hold.append_child("rPr");
    CopyPropsInto(style, from);
    // Typing just after a link isn't linked (Word and PowerPoint end a link with its text).
    if (!props) {
      bool afterLink = false;
      for (const Seg& sg : segs_[at.para])
        if (sg.start + sg.length == at.offset && sg.length > 0 && A(A(sg.node, "rPr"), "hlinkClick")) afterLink = true;
      bool insideLink = false;
      for (const Seg& sg : segs_[at.para])
        if (sg.start < at.offset && at.offset < sg.start + sg.length && A(A(sg.node, "rPr"), "hlinkClick")) insideLink = true;
      if (afterLink && !insideLink) {
        for (const char* k : {"hlinkClick", "hlinkMouseOver"})
          if (pugi::xml_node n = A(style, k)) style.remove_child(n);
      }
    }
  }
  size_t i = 0;
  while (i <= text.size()) {
    size_t j = i;
    while (j < text.size() && text[j] != L'\r' && text[j] != L'\n' && text[j] != L'\u000B' && text[j] != L' ') ++j;
    const std::wstring piece = Clean(text.substr(i, j - i));
    if (!piece.empty()) {
      InsertInline(at.para, at.offset, piece, style);
      at.offset += static_cast<uint32_t>(piece.size());
    }
    if (j >= text.size()) break;
    const wchar_t c = text[j];
    if (c == L'\u000B' || c == L' ') {
      InsertBreak(at.para, at.offset, style);
      at.offset += 1;
    } else {
      SplitPara(at.para, at.offset);
      // The new paragraph's mark keeps the typed text's look too.
      ++at.para;
      at.offset = 0;
      if (c == L'\r' && j + 1 < text.size() && text[j + 1] == L'\n') ++j;
    }
    i = j + 1;
  }
  return at;
}

void TextDoc::InsertInline(uint32_t para, uint32_t offset, std::wstring_view text, pugi::xml_node props) {
  pugi::xml_node p = paras_[para];
  const std::vector<Seg>& segs = segs_[para];
  // Into a run with the same properties: the one the offset is in, ends at, or starts at.
  for (const Seg& s : segs) {
    if (s.kind != TextRun::Kind::Text || s.locked) continue;
    if (offset < s.start || offset > s.start + s.length) continue;
    if (!SameProps(A(s.node, "rPr"), props)) continue;
    pugi::xml_node t = A(s.node, "t");
    if (!t) t = pugi::xml_node(s.node).append_child((a_ + "t").c_str());
    std::wstring now = Utf8ToWide(t.text().get());
    now.insert(std::min<size_t>(offset - s.start, now.size()), text);
    t.text().set(WideToUtf8(now).c_str());
    Index();
    return;
  }
  // A run of its own, between the runs there.
  pugi::xml_node run = NewRunAt(para, offset, "r");
  if (props) CopyPropsInto(run.append_child((a_ + "rPr").c_str()), props);
  run.append_child((a_ + "t").c_str()).text().set(WideToUtf8(text).c_str());
  Index();
}

pugi::xml_node TextDoc::NewRunAt(uint32_t para, uint32_t offset, const char* local) {
  SplitAt(para, offset);
  pugi::xml_node p = paras_[para];
  // After the last run ending at or before the offset, else before the first one after it, else
  // (an empty paragraph) before the mark.
  pugi::xml_node before, next;
  for (const Seg& s : segs_[para]) {
    if (s.start < offset && s.start + s.length <= offset) before = Top(s.node, p);
    else if (!next && s.start >= offset) next = Top(s.node, p);
  }
  const std::string name = a_ + local;
  if (before) return p.insert_child_after(name.c_str(), before);
  if (next) return p.insert_child_before(name.c_str(), next);
  if (pugi::xml_node end = EndProps(p, false)) return p.insert_child_before(name.c_str(), end);
  return p.append_child(name.c_str());
}

void TextDoc::InsertBreak(uint32_t para, uint32_t offset, pugi::xml_node props) {
  pugi::xml_node br = NewRunAt(para, offset, "br");
  if (props) CopyPropsInto(br.append_child((a_ + "rPr").c_str()), props);
  Index();
}

TextPos TextDoc::Split(TextPos at) {
  at = Clamp(at);
  uint32_t s, e;
  if (InField(at, s, e)) at.offset = e;
  SplitPara(at.para, at.offset);
  return {at.para + 1, 0};
}

void TextDoc::SplitPara(uint32_t para, uint32_t offset) {
  SplitAt(para, offset);
  pugi::xml_node p = paras_[para];
  // The last run before the split (its look is the first paragraph's mark's) and the first that
  // moves to the new paragraph.
  pugi::xml_node lastBefore, firstAfter;
  for (const Seg& s : segs_[para]) {
    if (s.start < offset && s.start + s.length <= offset) lastBefore = s.node;
    else if (!firstAfter && s.start >= offset) firstAfter = Top(s.node, p);
  }
  pugi::xml_node np = p.parent().insert_child_after(p.name(), p);
  if (pugi::xml_node pPr = A(p, "pPr"); pPr && pPr.parent() == p) np.append_copy(pPr);
  // Everything from the split on moves (runs, breaks, fields, anything else), and the mark.
  pugi::xml_node end = EndProps(p, false);
  std::vector<pugi::xml_node> moving;
  if (firstAfter)
    for (pugi::xml_node c = firstAfter; c; c = c.next_sibling())
      if (c != end && IsRunLevel(c)) moving.push_back(c);
  for (pugi::xml_node c : moving) {
    np.append_move(c);
  }
  if (end) {
    np.append_move(end);
  }
  // The first paragraph's mark.
  pugi::xml_node look = lastBefore ? A(lastBefore, "rPr") : pugi::xml_node();
  if (lastBefore && look) {
    CopyPropsInto(NewElement(p, "endParaRPr"), look);
  } else if (pugi::xml_node moved = EndProps(np, false)) {
    pugi::xml_node copy = NewElement(p, "endParaRPr");
    CopyPropsInto(copy, moved);
  }
  Index();
}

// ---------------------------------------------------------------------------
// Deleting

void TextDoc::DeleteInline(uint32_t para, uint32_t from, uint32_t to) {
  if (from >= to) return;
  pugi::xml_node p = paras_[para];
  SplitAt(para, to);
  SplitAt(para, from);
  pugi::xml_node firstGone, lastGone;
  pugi::xml_node look;  // the first deleted run's look, for an emptied paragraph's mark
  std::vector<pugi::xml_node> gone;
  for (const Seg& s : segs_[para]) {
    if (s.start < from || s.start + s.length > to) continue;
    if (s.length == 0 && (s.start == from || s.start == to)) continue;
    pugi::xml_node n = s.node;
    while (n.parent() != p && n.parent()) n = n.parent();
    if (!look) look = A(s.node, "rPr");
    if (std::find(gone.begin(), gone.end(), n) == gone.end()) gone.push_back(n);
  }
  if (gone.empty()) return;
  // Anything between the first and the last goes with them.
  firstGone = gone.front();
  lastGone = gone.back();
  std::vector<pugi::xml_node> remove;
  for (pugi::xml_node c = firstGone; c; c = c.next_sibling()) {
    if (IsRunLevel(c)) remove.push_back(c);
    if (c == lastGone) break;
  }
  pugi::xml_document hold;
  pugi::xml_node kept;
  if (look) {
    kept = hold.append_copy(look);
  }
  for (pugi::xml_node c : remove) p.remove_child(c);
  // A paragraph left without text keeps the deleted text's look in its mark, for typing.
  bool anyRun = false;
  for (pugi::xml_node c = p.first_child(); c; c = c.next_sibling())
    if (IsRunLevel(c)) anyRun = true;
  if (!anyRun && kept) CopyPropsInto(EndProps(p, true), kept);
  Index();
}

void TextDoc::JoinNext(uint32_t para) {
  if (para + 1 >= paras_.size()) return;
  pugi::xml_node p = paras_[para], q = paras_[para + 1];
  pugi::xml_node end = EndProps(p, false);
  pugi::xml_node qEnd = EndProps(q, false);
  bool firstHasText = false;
  for (pugi::xml_node c = p.first_child(); c; c = c.next_sibling())
    if (IsRunLevel(c)) firstHasText = true;
  std::vector<pugi::xml_node> moving;
  for (pugi::xml_node c = q.first_child(); c; c = c.next_sibling())
    if (IsRunLevel(c)) moving.push_back(c);
  for (pugi::xml_node c : moving) {
    if (end) p.insert_move_before(c, end);
    else p.append_move(c);
  }
  // The joined paragraph's mark is the second one's, as PowerPoint keeps it; an emptied first
  // paragraph keeps its own (the deleted text's look).
  if (qEnd && (firstHasText || !end)) {
    if (end) p.remove_child(end);
    p.append_move(qEnd);
  }
  q.parent().remove_child(q);
  Index();
}

TextPos TextDoc::Delete(TextPos from, TextPos to) {
  from = Clamp(from);
  to = Clamp(to);
  if (to < from) std::swap(from, to);
  Widen(from, to);
  if (from == to) return from;
  if (from.para == to.para) {
    DeleteInline(from.para, from.offset, to.offset);
    Tidy(paras_[from.para]);
    return from;
  }
  DeleteInline(to.para, 0, to.offset);
  for (uint32_t p = to.para - 1; p > from.para; --p) {
    paras_[p].parent().remove_child(paras_[p]);
    Index();
  }
  DeleteInline(from.para, from.offset, Length(from.para));
  JoinNext(from.para);
  Tidy(paras_[from.para]);
  return from;
}

// ---------------------------------------------------------------------------
// Tidying: no empty runs, neighbours with the same look joined.

void TextDoc::Tidy(pugi::xml_node p) {
  bool changed = false;
  for (pugi::xml_node c = p.first_child(); c;) {
    pugi::xml_node next = c.next_sibling();
    if (c.type() == pugi::node_element && LocalNameOf(c) == "r") {
      pugi::xml_node t = A(c, "t");
      if (!t || !*t.text().get()) {
        // An emptied run: its look goes to the mark when the paragraph has no other text.
        p.remove_child(c);
        changed = true;
        c = next;
        continue;
      }
      // Join the runs after it with the same look.
      while (next && next.type() == pugi::node_element && LocalNameOf(next) == "r" &&
             SameProps(A(c, "rPr"), A(next, "rPr")) && A(next, "t")) {
        const std::string joined = std::string(t.text().get()) + A(next, "t").text().get();
        t.text().set(joined.c_str());
        pugi::xml_node after = next.next_sibling();
        p.remove_child(next);
        next = after;
        changed = true;
      }
    }
    c = next;
  }
  if (changed) Index();
}

// ---------------------------------------------------------------------------
// Formatting

bool TextDoc::EditRuns(TextPos from, TextPos to, const std::function<void(pugi::xml_node rPr)>& edit) {
  return EditRuns(from, to, [&](pugi::xml_node rPr, const RunProps&) { edit(rPr); });
}

bool TextDoc::EditRuns(TextPos from, TextPos to, const std::function<void(pugi::xml_node rPr, const RunProps& now)>& edit) {
  from = Clamp(from);
  to = Clamp(to);
  if (to < from) std::swap(from, to);
  if (from == to) return false;
  Widen(from, to);
  for (uint32_t p = from.para; p <= to.para; ++p) {
    const uint32_t a = p == from.para ? from.offset : 0, b = p == to.para ? to.offset : Length(p);
    SplitAt(p, b);
    SplitAt(p, a);
    // Each run with what it looks like now (copied: the edits don't re-resolve until the end).
    std::vector<std::pair<pugi::xml_node, RunProps>> runs;
    const Paragraph& rp = resolved_->paragraphs[p];
    for (const Seg& s : segs_[p])
      if (s.start >= a && s.start + s.length <= b && (s.length > 0 || (s.start > a && s.start < b)) && Editable(s.node, paras_[p]))
        for (const TextRun& r : rp.runs)
          if (r.node == s.node) runs.emplace_back(s.node, r.props);
    const RunProps mark = rp.endProps;
    for (auto& [r, props] : runs) edit(MakeRunProps(r), props);
    // The mark, when the range goes past it, or the paragraph has no text.
    if (p < to.para || Length(p) == 0) edit(EndProps(paras_[p], true), mark);
  }
  Index();
  for (uint32_t p = from.para; p <= to.para && p < paras_.size(); ++p) Tidy(paras_[p]);
  return true;
}

void TextDoc::EditMark(uint32_t para, const std::function<void(pugi::xml_node rPr)>& edit) {
  if (para >= paras_.size()) return;
  edit(EndProps(paras_[para], true));
  Index();
}

bool TextDoc::ReplaceChars(TextPos from, TextPos to, std::wstring_view with) {
  from = Clamp(from);
  to = Clamp(to);
  if (to < from) std::swap(from, to);
  if (from.para != to.para || to.offset - from.offset != with.size()) return false;
  const uint32_t p = from.para;
  SplitAt(p, to.offset);
  SplitAt(p, from.offset);
  bool changed = false;
  for (const Seg& s : segs_[p]) {
    if (s.kind != TextRun::Kind::Text || s.locked || s.start < from.offset || s.start + s.length > to.offset) continue;
    pugi::xml_node t = A(s.node, "t");
    const std::wstring now = Utf8ToWide(t.text().get());
    const std::wstring next(with.substr(s.start - from.offset, s.length));
    if (now == next) continue;
    t.text().set(WideToUtf8(next).c_str());
    changed = true;
  }
  Index();
  Tidy(paras_[p]);
  return changed;
}

void TextDoc::EditParagraphs(uint32_t first, uint32_t last, const std::function<void(pugi::xml_node pPr)>& edit) {
  last = std::min<uint32_t>(last, static_cast<uint32_t>(paras_.size()) - 1);
  for (uint32_t p = first; p <= last; ++p) {
    pugi::xml_node para = paras_[p];
    pugi::xml_node pPr = A(para, "pPr");
    if (!pPr || pPr.parent() != para) pPr = para.prepend_child((a_ + "pPr").c_str());
    edit(pPr);
  }
  Index();
}

void TextDoc::EditBody(const std::function<void(pugi::xml_node bodyPr)>& edit) {
  pugi::xml_node bodyPr = A(body_, "bodyPr");
  if (!bodyPr) bodyPr = body_.prepend_child((a_ + "bodyPr").c_str());
  edit(bodyPr);
  Index();
}

// ---------------------------------------------------------------------------
// Whole paragraphs: copying and pasting Featherweight's own text

void TextDoc::CopyParagraphs(TextPos from, TextPos to, pugi::xml_node into) const {
  from = Clamp(from);
  to = Clamp(to);
  if (to < from) std::swap(from, to);
  for (uint32_t p = from.para; p <= to.para; ++p) {
    pugi::xml_node copy = into.append_copy(paras_[p]);
    const uint32_t a = p == from.para ? from.offset : 0, b = p == to.para ? to.offset : Length(p);
    // Cut the copy to [a, b): runs outside go, runs across an end are trimmed.
    std::vector<pugi::xml_node> drop;
    std::vector<std::pair<pugi::xml_node, std::pair<uint32_t, uint32_t>>> trims;
    // The copy's children match the original's one to one.
    std::vector<pugi::xml_node> orig, mine;
    for (pugi::xml_node c = paras_[p].first_child(); c; c = c.next_sibling()) orig.push_back(c);
    for (pugi::xml_node c = copy.first_child(); c; c = c.next_sibling()) mine.push_back(c);
    for (const Seg& s : segs_[p]) {
      pugi::xml_node top = s.node;
      while (top.parent() != paras_[p] && top.parent()) top = top.parent();
      const auto it = std::find(orig.begin(), orig.end(), top);
      if (it == orig.end()) continue;
      pugi::xml_node m = mine[static_cast<size_t>(it - orig.begin())];
      const uint32_t s0 = s.start, s1 = s.start + s.length;
      if (s1 <= a || s0 >= b) {
        if (!(s.length == 0 && s0 > a && s0 < b)) drop.push_back(m);
      } else if ((s0 < a || s1 > b) && s.kind == TextRun::Kind::Text && !s.locked) {
        trims.push_back({m, {std::max(a, s0) - s0, std::min(b, s1) - s0}});
      }
    }
    for (auto& [n, range] : trims) {
      pugi::xml_node t = A(n, "t");
      const std::wstring text = Utf8ToWide(t.text().get());
      t.text().set(WideToUtf8(text.substr(range.first, range.second - range.first)).c_str());
    }
    for (pugi::xml_node n : drop) copy.remove_child(n);
  }
}

TextPos TextDoc::InsertParagraphs(TextPos from, TextPos to, const std::vector<pugi::xml_node>& paragraphs) {
  TextPos at = Delete(from, to);
  if (paragraphs.empty()) return at;
  // Split where it goes; the pasted paragraphs go between the two halves, the first and last
  // joining them.
  SplitPara(at.para, at.offset);
  pugi::xml_node after = paras_[at.para];
  const uint32_t first = at.para + 1;
  for (pugi::xml_node src : paragraphs) {
    after = after.parent().insert_copy_after(src, after);
    // Pasted elements take this body's prefix when theirs differs (Featherweight's copies are "a:").
    if (a_ != "a:") {
      std::function<void(pugi::xml_node)> rename = [&](pugi::xml_node n) {
        std::string_view name = n.name();
        if (name.starts_with("a:")) n.set_name((a_ + std::string(name.substr(2))).c_str());
        for (pugi::xml_node c : n.children())
          if (c.type() == pugi::node_element) rename(c);
      };
      rename(after);
    }
  }
  Index();
  const uint32_t last = first + static_cast<uint32_t>(paragraphs.size()) - 1;
  const uint32_t endOffset = Length(last);
  // The last pasted paragraph joins the text after; the first joins the text before.
  JoinNext(last);
  JoinNext(at.para);
  Tidy(paras_[at.para]);
  const uint32_t endPara = last - 1;
  if (endPara < paras_.size()) Tidy(paras_[endPara]);
  return {endPara, endPara == at.para ? at.offset + endOffset : endOffset};
}

}  // namespace fw::slide::view
