// Text operations (P2, p2-model; for p2-text): typing, deleting, replacing, character, paragraph and
// text box formatting, links. Offsets are UTF-16 units into a paragraph's text (Edit.h).
#include <algorithm>
#include <cmath>
#include <sstream>

#include "pkg/XmlWriter.h"
#include "slide/edit/EditInternal.h"
#include "slide/edit/ShapeXml.h"
#include "slide/edit/SlideCopy.h"

namespace fw::slide::edit {

namespace {

// ---- Finding the text body

std::string TargetPart(const Editor::Impl& m, const TextTarget& t) {
  std::string slide = m.SlidePart(t.slide);
  if (!t.notes || slide.empty()) return slide;
  return TargetOfType(m.Rels(slide), "notesSlide");
}

pugi::xml_node CellOf(pugi::xml_node frame, int row, int column) {
  pugi::xml_node tbl = A(A(A(frame, "graphic"), "graphicData"), "tbl");
  int r = 0;
  for (pugi::xml_node tr = tbl.first_child(); tr; tr = tr.next_sibling()) {
    if (LocalNameOf(tr) != "tr") continue;
    if (r++ != row) continue;
    int c = 0;
    for (pugi::xml_node tc = tr.first_child(); tc; tc = tc.next_sibling()) {
      if (LocalNameOf(tc) == "tc" && c++ == column) return tc;
    }
  }
  return {};
}

// The text body of a target; when `edit`, in the part's copy being edited (made when missing).
pugi::xml_node BodyOf(Editor::Impl& m, const TextTarget& t, bool edit, std::string* partOut = nullptr) {
  std::string part = TargetPart(m, t);
  if (part.empty()) return {};
  if (partOut) *partOut = part;
  pugi::xml_node root = edit ? m.Edit(part) : m.Root(part);
  pugi::xml_node shape = Editor::Impl::FindShape(root, t.shape);
  if (!shape) return {};
  if (edit) shape = Editor::Impl::Unwrap(shape);
  if (t.row >= 0 || t.column >= 0) {
    pugi::xml_node tc = CellOf(shape, t.row, t.column);
    if (!tc) return {};
    pugi::xml_node body = A(tc, "txBody");
    if (!body && edit) {
      std::string a = PrefixFor(tc, kA, "a");
      body = tc.prepend_child((a + ":txBody").c_str());
      body.append_child((a + ":bodyPr").c_str());
      body.append_child((a + ":lstStyle").c_str());
      body.append_child((a + ":p").c_str());
    }
    return body;
  }
  pugi::xml_node body = P(shape, "txBody");
  if (!body && edit && LocalNameOf(shape) == "sp") {
    std::string p = PrefixFor(shape, kP, "p"), a = PrefixFor(shape, kA, "a");
    body = AddChildOrdered(shape, p + ":txBody", {"extLst"});
    body.append_child((a + ":bodyPr").c_str());
    body.append_child((a + ":lstStyle").c_str());
    body.append_child((a + ":p").c_str()).append_child((a + ":endParaRPr").c_str()).append_attribute("lang") = "en-US";
  }
  return body;
}

std::vector<pugi::xml_node> Paragraphs(pugi::xml_node body) {
  std::vector<pugi::xml_node> out;
  for (pugi::xml_node p = body.first_child(); p; p = p.next_sibling()) {
    if (p.type() == pugi::node_element && Is(p, kA, "p")) out.push_back(p);
  }
  return out;
}

// ---- Pieces of a paragraph: runs, breaks and fields

enum class Kind { Run, Break, Field };
struct Piece {
  pugi::xml_node node;
  Kind kind;
  size_t start = 0, len = 0;
};

std::wstring RunText(pugi::xml_node n) {
  if (LocalNameOf(n) == "br") return L"\v";
  return Wide(A(n, "t").text().get());
}

std::vector<Piece> Pieces(pugi::xml_node p) {
  std::vector<Piece> out;
  size_t at = 0;
  for (pugi::xml_node c = p.first_child(); c; c = c.next_sibling()) {
    if (c.type() != pugi::node_element || !Is(c, kA, LocalNameOf(c))) continue;
    std::string_view k = LocalNameOf(c);
    Kind kind;
    if (k == "r") kind = Kind::Run;
    else if (k == "br") kind = Kind::Break;
    else if (k == "fld") kind = Kind::Field;
    else continue;
    size_t len = RunText(c).size();
    out.push_back({c, kind, at, len});
    at += len;
  }
  return out;
}

size_t Length(pugi::xml_node p) {
  std::vector<Piece> pieces = Pieces(p);
  return pieces.empty() ? 0 : pieces.back().start + pieces.back().len;
}

void SetRunText(pugi::xml_node r, std::wstring_view text) {
  pugi::xml_node t = A(r, "t");
  if (!t) t = r.append_child((PrefixFor(r, kA, "a") + ":t").c_str());
  std::string utf8 = Utf8(text);
  t.text().set(utf8.c_str());
}

// Makes a piece boundary at `offset` (splitting a run); returns the first node at or after it
// (null at the end). A field or break isn't split: inside one, the boundary is after it.
pugi::xml_node SplitAt(pugi::xml_node p, size_t offset) {
  for (const Piece& pc : Pieces(p)) {
    if (offset <= pc.start) return pc.node;
    if (offset >= pc.start + pc.len) continue;
    if (pc.kind != Kind::Run) {
      pugi::xml_node next = pc.node.next_sibling();
      while (next && next.type() != pugi::node_element) next = next.next_sibling();
      return next && LocalNameOf(next) != "endParaRPr" ? next : pugi::xml_node();
    }
    std::wstring text = RunText(pc.node);
    size_t cut = offset - pc.start;
    pugi::xml_node tail = p.insert_copy_after(pc.node, pc.node);
    SetRunText(pc.node, std::wstring_view(text).substr(0, cut));
    SetRunText(tail, std::wstring_view(text).substr(cut));
    return tail;
  }
  return {};
}

std::string Serialized(pugi::xml_node n) {
  if (!n) return {};
  std::ostringstream out;
  n.print(out, "", pugi::format_raw);
  return out.str();
}

// Adjacent runs with the same properties become one (typing and formatting leave fewer runs).
void MergeRuns(pugi::xml_node p) {
  pugi::xml_node prev;
  std::string prevProps;
  for (pugi::xml_node c = p.first_child(); c;) {
    pugi::xml_node next = c.next_sibling();
    if (c.type() != pugi::node_element) {
      c = next;
      continue;
    }
    if (Is(c, kA, "r")) {
      std::string props = Serialized(A(c, "rPr"));
      if (prev && props == prevProps) {
        SetRunText(prev, RunText(prev) + RunText(c));
        p.remove_child(c);
        c = next;
        continue;
      }
      if (RunText(c).empty() && Pieces(p).size() > 1) {
        p.remove_child(c);
        c = next;
        continue;
      }
      prev = c;
      prevProps = std::move(props);
    } else {
      prev = {};
    }
    c = next;
  }
}

// Run properties to give new text at `offset`: the text before it, else after it, else the
// paragraph's end properties. A copy, named a:rPr; null when there are none.
pugi::xml_node PropsAt(pugi::xml_node p, size_t offset, pugi::xml_document& holder) {
  std::vector<Piece> pieces = Pieces(p);
  pugi::xml_node from;
  for (const Piece& pc : pieces) {
    if (offset > pc.start && offset <= pc.start + pc.len) from = A(pc.node, "rPr");
  }
  if (!from && !pieces.empty() && offset == 0) from = A(pieces.front().node, "rPr");
  if (!from && pieces.empty()) from = A(p, "endParaRPr");
  if (!from) return {};
  pugi::xml_node copy = holder.append_copy(from);
  copy.set_name((PrefixFor(p, kA, "a") + ":rPr").c_str());
  return copy;
}

void InsertInParagraph(pugi::xml_node p, size_t offset, std::wstring_view line) {
  if (line.empty()) return;
  // Typing's usual case: into the run before the caret.
  if (line.find(L'\v') == std::wstring_view::npos) {
    for (const Piece& pc : Pieces(p)) {
      if (pc.kind == Kind::Run && offset > pc.start && offset <= pc.start + pc.len) {
        std::wstring text = RunText(pc.node);
        text.insert(offset - pc.start, line);
        SetRunText(pc.node, text);
        return;
      }
    }
  }
  pugi::xml_document holder;
  pugi::xml_node props = PropsAt(p, offset, holder);
  pugi::xml_node before = SplitAt(p, offset);
  if (!before) before = A(p, "endParaRPr");
  std::string a = PrefixFor(p, kA, "a");
  auto add = [&](const char* local) {
    std::string name = a + ":" + local;
    pugi::xml_node n = before ? p.insert_child_before(name.c_str(), before) : p.append_child(name.c_str());
    if (props) n.append_copy(props);
    return n;
  };
  size_t i = 0;
  while (i <= line.size()) {
    size_t br = line.find(L'\v', i);
    std::wstring_view piece = line.substr(i, br == std::wstring_view::npos ? std::wstring_view::npos : br - i);
    if (!piece.empty()) SetRunText(add("r"), piece);
    if (br == std::wstring_view::npos) break;
    add("br");
    i = br + 1;
  }
  MergeRuns(p);
}

// Splits paragraph `index` at `offset`: what follows goes into a new paragraph after it.
void SplitParagraph(pugi::xml_node body, pugi::xml_node p, size_t offset) {
  pugi::xml_document holder;
  pugi::xml_node caret = PropsAt(p, offset, holder);
  pugi::xml_node boundary = SplitAt(p, offset);
  pugi::xml_node np = body.insert_child_after(p.name(), p);
  if (pugi::xml_node pPr = A(p, "pPr")) np.append_copy(pPr);
  pugi::xml_node endProps = A(p, "endParaRPr");
  for (pugi::xml_node c = boundary; c && c != endProps;) {
    pugi::xml_node next = c.next_sibling();
    if (!(c.type() == pugi::node_element && LocalNameOf(c) == "pPr")) np.append_move(c);
    c = next;
  }
  if (endProps) {
    np.append_copy(endProps);
  } else if (caret) {
    np.append_copy(caret).set_name((PrefixFor(p, kA, "a") + ":endParaRPr").c_str());
  }
  // An emptied paragraph keeps the formatting it had at its end.
  if (Pieces(p).empty() && caret && !endProps) {
    p.append_copy(caret).set_name((PrefixFor(p, kA, "a") + ":endParaRPr").c_str());
  }
}

void DeleteInParagraph(pugi::xml_node p, size_t from, size_t to) {
  if (to <= from) return;
  SplitAt(p, from);
  SplitAt(p, to);
  pugi::xml_node firstProps;
  pugi::xml_document holder;
  for (const Piece& pc : Pieces(p)) {
    bool overlaps = pc.start < to && pc.start + pc.len > from;
    if (!overlaps) continue;
    if (!firstProps) {
      if (pugi::xml_node rp = A(pc.node, "rPr")) firstProps = holder.append_copy(rp);
    }
    p.remove_child(pc.node);
  }
  if (Pieces(p).empty() && firstProps) {
    // The caret keeps the deleted text's formatting.
    pugi::xml_node end = A(p, "endParaRPr");
    if (end) p.remove_child(end);
    p.append_copy(firstProps).set_name((PrefixFor(p, kA, "a") + ":endParaRPr").c_str());
  }
  MergeRuns(p);
}

Result CheckPos(const std::vector<pugi::xml_node>& paras, TextPos pos) {
  if (pos.paragraph >= paras.size() || pos.offset > Length(paras[pos.paragraph])) {
    return Result::Fail(u"That place isn't in the text.");
  }
  return {};
}

std::wstring Normalize(std::wstring_view text) {
  std::wstring out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == L'\r') {
      out += L'\n';
      if (i + 1 < text.size() && text[i + 1] == L'\n') ++i;
    } else {
      out += text[i];
    }
  }
  return out;
}

TextPos Insert(pugi::xml_node body, TextPos at, std::wstring_view text) {
  std::wstring norm = Normalize(text);
  std::wstring_view rest = norm;
  TextPos pos = at;
  bool first = true;
  while (true) {
    size_t nl = rest.find(L'\n');
    std::wstring_view line = rest.substr(0, nl);
    std::vector<pugi::xml_node> paras = Paragraphs(body);
    if (!first) {
      SplitParagraph(body, paras[pos.paragraph], pos.offset);
      pos = {pos.paragraph + 1, 0};
      paras = Paragraphs(body);
    }
    first = false;
    InsertInParagraph(paras[pos.paragraph], pos.offset, line);
    pos.offset += line.size();
    if (nl == std::wstring_view::npos) break;
    rest.remove_prefix(nl + 1);
  }
  return pos;
}

void Delete(pugi::xml_node body, TextRange r) {
  std::vector<pugi::xml_node> paras = Paragraphs(body);
  if (r.from.paragraph == r.to.paragraph) {
    DeleteInParagraph(paras[r.from.paragraph], r.from.offset, r.to.offset);
    return;
  }
  pugi::xml_node first = paras[r.from.paragraph], last = paras[r.to.paragraph];
  DeleteInParagraph(first, r.from.offset, Length(first));
  DeleteInParagraph(last, 0, r.to.offset);
  for (size_t i = r.from.paragraph + 1; i < r.to.paragraph; ++i) body.remove_child(paras[i]);
  // The last paragraph's rest joins the first.
  pugi::xml_node end = A(first, "endParaRPr");
  bool lastHasText = !Pieces(last).empty();
  for (pugi::xml_node c = last.first_child(); c;) {
    pugi::xml_node next = c.next_sibling();
    std::string_view k = LocalNameOf(c);
    if (c.type() == pugi::node_element && k != "pPr" && k != "endParaRPr") {
      end ? first.insert_move_before(c, end) : first.append_move(c);
    }
    c = next;
  }
  if (lastHasText && end) {
    // The joined paragraph ends as the last one did.
    if (pugi::xml_node lastEnd = A(last, "endParaRPr")) {
      first.insert_copy_after(lastEnd, end);
      first.remove_child(end);
    }
  }
  body.remove_child(last);
  MergeRuns(first);
}

// ---- Formatting

const char* UnderlineName(Underline u) {
  static const char* kNames[] = {"none", "sng", "dbl", "heavy", "dotted", "dottedHeavy", "dash", "dashHeavy", "dashLong",
                                 "dashLongHeavy", "dotDash", "dotDashHeavy", "dotDotDash", "dotDotDashHeavy", "wavy",
                                 "wavyHeavy", "wavyDbl", "words"};
  size_t i = static_cast<size_t>(u);
  return i < std::size(kNames) ? kNames[i] : "none";
}

int RunPrGroup(std::string_view k) {
  if (k == "ln") return 0;
  if (k == "noFill" || k == "solidFill" || k == "gradFill" || k == "blipFill" || k == "pattFill" || k == "grpFill") return 1;
  if (k == "effectLst" || k == "effectDag") return 2;
  if (k == "highlight") return 3;
  if (k == "uLnTx" || k == "uLn") return 4;
  if (k == "uFillTx" || k == "uFill") return 5;
  if (k == "latin") return 6;
  if (k == "ea") return 7;
  if (k == "cs") return 8;
  if (k == "sym") return 9;
  if (k == "hlinkClick") return 10;
  if (k == "hlinkMouseOver") return 11;
  if (k == "rtl") return 12;
  return -1;
}

int ParaPrGroup(std::string_view k) {
  if (k == "lnSpc") return 0;
  if (k == "spcBef") return 1;
  if (k == "spcAft") return 2;
  if (k == "buClrTx" || k == "buClr") return 3;
  if (k == "buSzTx" || k == "buSzPct" || k == "buSzPts") return 4;
  if (k == "buFontTx" || k == "buFont") return 5;
  if (k == "buNone" || k == "buAutoNum" || k == "buChar" || k == "buBlip") return 6;
  if (k == "tabLst") return 7;
  if (k == "defRPr") return 8;
  return -1;
}

int BodyPrGroup(std::string_view k) {
  if (k == "prstTxWarp") return 0;
  if (k == "noAutofit" || k == "normAutofit" || k == "spAutoFit") return 1;
  if (k == "scene3d") return 2;
  if (k == "sp3d" || k == "flatTx") return 3;
  return -1;
}

constexpr std::string_view kA_ = " xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\"";

void SetAttr(pugi::xml_node n, const char* name, const std::string& v) {
  (n.attribute(name) ? n.attribute(name) : n.append_attribute(name)).set_value(v.c_str());
}

std::string Attr(std::string_view s) {
  std::string out;
  pkg::AppendEscapedAttribute(out, s);
  return out;
}

void ApplyRunPatch(pugi::xml_node rPr, const RunPatch& p) {
  if (p.bold) SetAttr(rPr, "b", *p.bold ? "1" : "0");
  if (p.italic) SetAttr(rPr, "i", *p.italic ? "1" : "0");
  if (p.underline) SetAttr(rPr, "u", UnderlineName(*p.underline));
  if (p.strike) SetAttr(rPr, "strike", *p.strike == Strike::None ? "noStrike" : *p.strike == Strike::Single ? "sngStrike" : "dblStrike");
  if (p.caps) SetAttr(rPr, "cap", *p.caps == Caps::None ? "none" : *p.caps == Caps::Small ? "small" : "all");
  if (p.size) SetAttr(rPr, "sz", std::to_string(std::clamp(std::lround(*p.size * 100), 100L, 400000L)));
  if (p.baseline) {
    long v = std::lround(*p.baseline * 100000);
    if (v == 0) rPr.remove_attribute("baseline");
    else SetAttr(rPr, "baseline", std::to_string(v));
  }
  if (p.spacing) {
    long v = std::lround(*p.spacing * 100);
    if (v == 0) rPr.remove_attribute("spc");
    else SetAttr(rPr, "spc", std::to_string(v));
  }
  if (p.lang) SetAttr(rPr, "lang", *p.lang);
  auto font = [&](const std::optional<std::wstring>& face, const char* local, int group) {
    if (!face) return;
    std::string xml;
    if (!face->empty()) xml = std::string("<a:") + local + std::string(kA_) + " typeface=\"" + Attr(Utf8(*face)) + "\"/>";
    ReplaceGroup(rPr, xml, RunPrGroup, group);
  };
  font(p.latin, "latin", 6);
  font(p.eastAsian, "ea", 7);
  font(p.complexScript, "cs", 8);
  if (p.color) ReplaceGroup(rPr, ColorFillXml(*p.color), RunPrGroup, 1);
  if (p.highlight) {
    std::string xml;
    std::string fill = ColorFillXml(*p.highlight);
    if (p.highlight->kind == ColorSpec::Kind::Rgb || p.highlight->kind == ColorSpec::Kind::Scheme) {
      // a:highlight holds the color itself, not a fill.
      pugi::xml_document doc;
      if (ParseFragment(fill, doc)) {
        std::ostringstream out;
        doc.document_element().first_child().print(out, "", pugi::format_raw);
        std::string color = out.str();
        size_t space = color.find(' ');
        if (space != std::string::npos && color.find(" xmlns:a=") == std::string::npos) color.insert(space, kA_);
        xml = "<a:highlight" + std::string(kA_) + ">" + color + "</a:highlight>";
      }
    }
    ReplaceGroup(rPr, xml, RunPrGroup, 3);
  }
}

pugi::xml_node RunProps(pugi::xml_node piece) {
  pugi::xml_node rPr = A(piece, "rPr");
  if (!rPr) rPr = piece.prepend_child((PrefixFor(piece, kA, "a") + ":rPr").c_str());
  return rPr;
}

pugi::xml_node EndProps(pugi::xml_node p) {
  pugi::xml_node e = A(p, "endParaRPr");
  if (!e) e = p.append_child((PrefixFor(p, kA, "a") + ":endParaRPr").c_str());
  return e;
}

// Calls `fn` for every piece in the range (runs split at its ends) and for each end-of-paragraph
// properties the range covers.
void ForRange(pugi::xml_node body, TextRange r, const std::function<void(pugi::xml_node)>& fn) {
  std::vector<pugi::xml_node> paras = Paragraphs(body);
  for (size_t i = r.from.paragraph; i <= r.to.paragraph && i < paras.size(); ++i) {
    pugi::xml_node p = paras[i];
    size_t len = Length(p);
    size_t a = i == r.from.paragraph ? r.from.offset : 0;
    size_t b = i == r.to.paragraph ? r.to.offset : len;
    if (b > a) {
      SplitAt(p, a);
      SplitAt(p, b);
      for (const Piece& pc : Pieces(p)) {
        if (pc.start >= a && pc.start + pc.len <= b && pc.len > 0) fn(pc.node);
      }
    }
    if (i < r.to.paragraph || len == 0 || (a == 0 && b == len)) fn(EndProps(p));
  }
}

}  // namespace

// ---- Editor: text

size_t Editor::ParagraphCount(const TextTarget& t) const {
  return Paragraphs(BodyOf(*impl_, t, false)).size();
}

std::wstring Editor::ParagraphText(const TextTarget& t, size_t paragraph) const {
  std::vector<pugi::xml_node> paras = Paragraphs(BodyOf(*impl_, t, false));
  if (paragraph >= paras.size()) return {};
  std::wstring out;
  for (const Piece& pc : Pieces(paras[paragraph])) out += RunText(pc.node);
  return out;
}

bool Editor::Impl::ContinueTyping(const TextTarget& t) {
  if (!typingOpen || undo.empty() || !redo.empty() || !undo.back().typing) return false;
  const TextTarget& o = undo.back().target;
  if (o.slide != t.slide || o.shape != t.shape || o.row != t.row || o.column != t.column || o.notes != t.notes) return false;
  changes = Changes{};
  open.push_back(std::move(undo.back()));
  undo.pop_back();
  return true;
}

Result Editor::InsertText(const TextTarget& t, TextPos at, std::wstring_view text, TextPos* end) {
  Call call(*this, u"Typing", &t);
  Impl& m = *impl_;
  std::string part;
  pugi::xml_node body = BodyOf(m, t, true, &part);
  if (!body) return call.End(Result::Fail(u"That shape has no text."));
  std::vector<pugi::xml_node> paras = Paragraphs(body);
  if (paras.empty()) {
    body.append_child((PrefixFor(body, kA, "a") + ":p").c_str());
    paras = Paragraphs(body);
  }
  if (Result r = CheckPos(paras, at); !r) return call.End(r);
  TextPos e = Insert(body, at, text);
  if (end) *end = e;
  m.Touched(part, t.shape);
  return call.End({});
}

Result Editor::DeleteText(const TextTarget& t, TextRange r) {
  Call call(*this, u"Typing", &t);
  Impl& m = *impl_;
  if (r.to < r.from) std::swap(r.from, r.to);
  std::string part;
  pugi::xml_node body = BodyOf(m, t, true, &part);
  if (!body) return call.End(Result::Fail(u"That shape has no text."));
  std::vector<pugi::xml_node> paras = Paragraphs(body);
  if (Result c = CheckPos(paras, r.from); !c) return call.End(c);
  if (Result c = CheckPos(paras, r.to); !c) return call.End(c);
  Delete(body, r);
  m.Touched(part, t.shape);
  return call.End({});
}

Result Editor::ReplaceText(const TextTarget& t, TextRange r, std::wstring_view text, TextPos* end) {
  Call call(*this, u"Replace");
  Impl& m = *impl_;
  if (r.to < r.from) std::swap(r.from, r.to);
  std::string part;
  pugi::xml_node body = BodyOf(m, t, true, &part);
  if (!body) return call.End(Result::Fail(u"That shape has no text."));
  std::vector<pugi::xml_node> paras = Paragraphs(body);
  if (Result c = CheckPos(paras, r.from); !c) return call.End(c);
  if (Result c = CheckPos(paras, r.to); !c) return call.End(c);
  // The new text takes the replaced text's formatting.
  pugi::xml_document holder;
  pugi::xml_node props;
  if (!(r.from == r.to)) {
    for (const Piece& pc : Pieces(paras[r.from.paragraph])) {
      if (pc.start <= r.from.offset && r.from.offset < pc.start + pc.len) {
        if (pugi::xml_node rp = A(pc.node, "rPr")) props = holder.append_copy(rp);
        break;
      }
    }
  }
  Delete(body, r);
  TextPos e = Insert(body, r.from, text);
  if (props) {
    ForRange(body, {r.from, e}, [&](pugi::xml_node n) {
      if (LocalNameOf(n) == "endParaRPr") return;
      if (pugi::xml_node old = A(n, "rPr")) n.remove_child(old);
      n.prepend_copy(props);
    });
    std::vector<pugi::xml_node> now = Paragraphs(body);
    for (size_t i = r.from.paragraph; i <= e.paragraph && i < now.size(); ++i) MergeRuns(now[i]);
  }
  if (end) *end = e;
  m.Touched(part, t.shape);
  return call.End({});
}

Result Editor::SetText(const TextTarget& t, std::wstring_view text) {
  Call call(*this, u"Typing");
  Impl& m = *impl_;
  std::string part;
  pugi::xml_node body = BodyOf(m, t, true, &part);
  if (!body) return call.End(Result::Fail(u"That shape has no text."));
  SetPlainText(body, Normalize(text));
  m.Touched(part, t.shape);
  return call.End({});
}

Result Editor::SetRunProps(const TextTarget& t, TextRange r, const RunPatch& patch) {
  Call call(*this, u"Font");
  Impl& m = *impl_;
  if (r.to < r.from) std::swap(r.from, r.to);
  std::string part;
  pugi::xml_node body = BodyOf(m, t, true, &part);
  if (!body) return call.End(Result::Fail(u"That shape has no text."));
  std::vector<pugi::xml_node> paras = Paragraphs(body);
  if (Result c = CheckPos(paras, r.from); !c) return call.End(c);
  if (Result c = CheckPos(paras, r.to); !c) return call.End(c);
  ForRange(body, r, [&](pugi::xml_node n) {
    ApplyRunPatch(LocalNameOf(n) == "endParaRPr" ? n : RunProps(n), patch);
  });
  for (size_t i = r.from.paragraph; i <= r.to.paragraph; ++i) MergeRuns(Paragraphs(body)[i]);
  m.Touched(part, t.shape);
  return call.End({});
}

Result Editor::SetParagraphProps(const TextTarget& t, size_t first, size_t last, const ParagraphPatch& patch) {
  Call call(*this, u"Paragraph");
  Impl& m = *impl_;
  std::string part;
  pugi::xml_node body = BodyOf(m, t, true, &part);
  if (!body) return call.End(Result::Fail(u"That shape has no text."));
  std::vector<pugi::xml_node> paras = Paragraphs(body);
  if (first > last || last >= paras.size()) return call.End(Result::Fail(u"Those paragraphs aren't in the text."));
  auto spacing = [](const char* local, const Spacing& s) {
    std::string inner = s.percent ? "<a:spcPct val=\"" + std::to_string(std::lround(s.value * 100000)) + "\"/>"
                                  : "<a:spcPts val=\"" + std::to_string(std::lround(s.value * 100)) + "\"/>";
    return std::string("<a:") + local + std::string(kA_) + ">" + inner + "</a:" + local + ">";
  };
  for (size_t i = first; i <= last; ++i) {
    pugi::xml_node p = paras[i];
    pugi::xml_node pPr = A(p, "pPr");
    if (!pPr) pPr = p.prepend_child((PrefixFor(p, kA, "a") + ":pPr").c_str());
    if (patch.align) {
      static const char* kAlign[] = {"l", "ctr", "r", "just", "justLow", "dist", "thaiDist"};
      SetAttr(pPr, "algn", kAlign[static_cast<size_t>(*patch.align) % 7]);
    }
    if (patch.level) {
      int lvl = std::clamp(*patch.level, 0, 8);
      if (lvl == 0) pPr.remove_attribute("lvl");
      else SetAttr(pPr, "lvl", std::to_string(lvl));
    }
    if (patch.marginLeft) SetAttr(pPr, "marL", std::to_string(std::llround(*patch.marginLeft * 12700.0)));
    if (patch.indent) SetAttr(pPr, "indent", std::to_string(std::llround(*patch.indent * 12700.0)));
    if (patch.rtl) SetAttr(pPr, "rtl", *patch.rtl ? "1" : "0");
    if (patch.lineSpacing) ReplaceGroup(pPr, spacing("lnSpc", *patch.lineSpacing), ParaPrGroup, 0);
    if (patch.spaceBefore) ReplaceGroup(pPr, spacing("spcBef", *patch.spaceBefore), ParaPrGroup, 1);
    if (patch.spaceAfter) ReplaceGroup(pPr, spacing("spcAft", *patch.spaceAfter), ParaPrGroup, 2);
    if (patch.bullet) {
      const BulletSpec& b = *patch.bullet;
      switch (b.kind) {
        case BulletSpec::Kind::Inherit:
          for (int g = 3; g <= 6; ++g) ReplaceGroup(pPr, {}, ParaPrGroup, g);
          break;
        case BulletSpec::Kind::None:
          ReplaceGroup(pPr, {}, ParaPrGroup, 5);
          ReplaceGroup(pPr, "<a:buNone" + std::string(kA_) + "/>", ParaPrGroup, 6);
          break;
        case BulletSpec::Kind::Char:
          ReplaceGroup(pPr, b.font.empty() ? std::string() : "<a:buFont" + std::string(kA_) + " typeface=\"" + Attr(Utf8(b.font)) + "\"/>",
                       ParaPrGroup, 5);
          ReplaceGroup(pPr, "<a:buChar" + std::string(kA_) + " char=\"" + Attr(Utf8(b.character.empty() ? L"\x2022" : b.character)) + "\"/>",
                       ParaPrGroup, 6);
          break;
        case BulletSpec::Kind::AutoNumber:
          ReplaceGroup(pPr, "<a:buAutoNum" + std::string(kA_) + " type=\"" + Attr(b.scheme) + "\"" +
                                (b.startAt > 1 ? " startAt=\"" + std::to_string(std::min(b.startAt, 32767)) + "\"" : std::string()) + "/>",
                       ParaPrGroup, 6);
          break;
      }
    }
  }
  m.Touched(part, t.shape);
  return call.End({});
}

Result Editor::SetBodyProps(const TextTarget& t, const BodyPatch& patch) {
  Call call(*this, u"Format Text");
  Impl& m = *impl_;
  std::string part;
  pugi::xml_node body = BodyOf(m, t, true, &part);
  if (!body) return call.End(Result::Fail(u"That shape has no text."));
  pugi::xml_node bodyPr = A(body, "bodyPr");
  if (!bodyPr) bodyPr = body.prepend_child((PrefixFor(body, kA, "a") + ":bodyPr").c_str());
  if (patch.anchor) {
    static const char* kAnchor[] = {"t", "ctr", "b", "just", "dist"};
    SetAttr(bodyPr, "anchor", kAnchor[static_cast<size_t>(*patch.anchor) % 5]);
  }
  if (patch.wrap) SetAttr(bodyPr, "wrap", *patch.wrap ? "square" : "none");
  if (patch.insets) {
    const char* names[] = {"lIns", "tIns", "rIns", "bIns"};
    for (int i = 0; i < 4; ++i) SetAttr(bodyPr, names[i], std::to_string(std::llround((*patch.insets)[i] * 12700.0)));
  }
  if (patch.vertical) {
    static const char* kVert[] = {"horz", "vert", "vert270", "wordArtVert", "eaVert", "mongolianVert", "wordArtVertRtl"};
    SetAttr(bodyPr, "vert", kVert[static_cast<size_t>(*patch.vertical) % 7]);
  }
  if (patch.columns) {
    int n = std::clamp(*patch.columns, 1, 16);
    if (n == 1) bodyPr.remove_attribute("numCol");
    else SetAttr(bodyPr, "numCol", std::to_string(n));
  }
  auto scaleAttrs = [&](pugi::xml_node fit) {
    if (patch.fontScale) {
      long v = std::lround(std::clamp(*patch.fontScale, 0.01f, 1.f) * 100000);
      if (v >= 100000) fit.remove_attribute("fontScale");
      else SetAttr(fit, "fontScale", std::to_string(v));
    }
    if (patch.lineSpacingReduction) {
      long v = std::lround(std::clamp(*patch.lineSpacingReduction, 0.f, 0.2f) * 100000);
      if (v <= 0) fit.remove_attribute("lnSpcReduction");
      else SetAttr(fit, "lnSpcReduction", std::to_string(v));
    }
  };
  if (patch.autofit) {
    switch (*patch.autofit) {
      case BodyProps::Autofit::None:
        ReplaceGroup(bodyPr, "<a:noAutofit" + std::string(kA_) + "/>", BodyPrGroup, 1);
        break;
      case BodyProps::Autofit::Shrink:
        ReplaceGroup(bodyPr, "<a:normAutofit" + std::string(kA_) + "/>", BodyPrGroup, 1);
        break;
      case BodyProps::Autofit::ResizeShape:
        ReplaceGroup(bodyPr, "<a:spAutoFit" + std::string(kA_) + "/>", BodyPrGroup, 1);
        break;
    }
  }
  if (patch.fontScale || patch.lineSpacingReduction) {
    pugi::xml_node fit = A(bodyPr, "normAutofit");
    if (!fit) {
      ReplaceGroup(bodyPr, "<a:normAutofit" + std::string(kA_) + "/>", BodyPrGroup, 1);
      fit = A(bodyPr, "normAutofit");
    }
    scaleAttrs(fit);
  }
  m.Touched(part, t.shape);
  return call.End({});
}

Result Editor::SetTextLink(const TextTarget& t, TextRange r, const std::optional<LinkTarget>& link) {
  Call call(*this, u"Link");
  Impl& m = *impl_;
  if (r.to < r.from) std::swap(r.from, r.to);
  std::string part;
  pugi::xml_node body = BodyOf(m, t, true, &part);
  if (!body) return call.End(Result::Fail(u"That shape has no text."));
  std::vector<pugi::xml_node> paras = Paragraphs(body);
  if (Result c = CheckPos(paras, r.from); !c) return call.End(c);
  if (Result c = CheckPos(paras, r.to); !c) return call.End(c);
  if (r.from == r.to) return call.End(Result::Fail(u"Select the text to link first."));
  std::set<std::string> old;
  std::string relId, action, tooltip;
  bool made = false;
  bool ok = true;
  ForRange(body, r, [&](pugi::xml_node n) {
    if (LocalNameOf(n) == "endParaRPr") return;
    pugi::xml_node rPr = RunProps(n);
    if (pugi::xml_node h = A(rPr, "hlinkClick")) {
      for (const pkg::FoundRef& f : m.registry.Find(h, pkg::RefKind::RelationshipId)) old.insert(std::string(f.Value()));
      rPr.remove_child(h);
    }
    if (!link) return;
    ReplaceGroup(rPr, "<a:hlinkClick" + std::string(kA_) + "/>", RunPrGroup, 10);
    pugi::xml_node h = A(rPr, "hlinkClick");
    if (!made) {
      made = true;
      ok = WriteLink(m, part, h, *link);
      relId = FindAttribute(h, kRelNs, "id").value();
      action = h.attribute("action").value();
      tooltip = h.attribute("tooltip").value();
      return;
    }
    std::string rp = PrefixFor(h, kRelNs, "r");
    h.append_attribute((rp + ":id").c_str()) = relId.c_str();
    if (!action.empty()) h.append_attribute("action") = action.c_str();
    if (!tooltip.empty()) h.append_attribute("tooltip") = tooltip.c_str();
  });
  if (!ok) return call.End(Result::Fail(u"That link can't be made."));
  // Relationships the old links used that nothing names now.
  if (!old.empty()) {
    pugi::xml_node root = m.Edit(part);
    std::set<std::string> used;
    for (const pkg::FoundRef& f : m.registry.Find(root, pkg::RefKind::RelationshipId)) used.insert(std::string(f.Value()));
    pkg::RelationshipSet& rels = m.EditRels(part);
    std::erase_if(rels.items, [&](const pkg::Relationship& rel) {
      return old.count(rel.id) && !used.count(rel.id) && (rel.type.ends_with("/hyperlink") || rel.type.ends_with("/slide"));
    });
  }
  m.Touched(part, t.shape);
  return call.End({});
}

Result Editor::SetTextBodyXml(const TextTarget& t, std::string_view xml) {
  Call call(*this, u"Paste");
  Impl& m = *impl_;
  pugi::xml_document doc;
  if (!ParseFragment(xml, doc) || LocalNameOf(doc.document_element()) != "txBody") {
    return call.End(Result::Fail(u"That isn't text Featherweight can paste."));
  }
  std::string part;
  pugi::xml_node body = BodyOf(m, t, true, &part);
  if (!body) return call.End(Result::Fail(u"That shape has no text."));
  std::set<std::string> rels;
  for (const pkg::FoundRef& f : m.registry.Find(doc.document_element(), pkg::RefKind::RelationshipId)) rels.insert(std::string(f.Value()));
  const pkg::RelationshipSet* have = m.Rels(part);
  for (const std::string& r : rels) {
    if (!r.empty() && (!have || !have->Find(r))) return call.End(Result::Fail(u"The text names a part the slide doesn't have."));
  }
  for (pugi::xml_node c = body.first_child(); c;) {
    pugi::xml_node next = c.next_sibling();
    body.remove_child(c);
    c = next;
  }
  for (pugi::xml_node c = doc.document_element().first_child(); c; c = c.next_sibling()) InsertCopy(body, c);
  if (Paragraphs(body).empty()) body.append_child((PrefixFor(body, kA, "a") + ":p").c_str());
  m.Touched(part, t.shape);
  return call.End({});
}

}  // namespace fw::slide::edit
