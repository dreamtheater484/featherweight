#include "slide/view/TextFind.h"

#include <windows.h>

#include <algorithm>
#include <map>

#include "slide/Presentation.h"

namespace fw::slide::view {

namespace {

std::wstring Folded(std::wstring s, bool matchCase) {
  if (!matchCase && !s.empty()) CharLowerBuffW(s.data(), static_cast<DWORD>(s.size()));
  return s;
}

// A paragraph's text as the edit offsets count it: a line break is one character, a field its text.
std::wstring ParagraphText(const Paragraph& p) {
  std::wstring t;
  for (const TextRun& r : p.runs) t += r.kind == TextRun::Kind::Break ? std::wstring(L"\u000B") : r.text;
  return t;
}

void Search(const TextBody* body, const TextTarget& target, const std::wstring& query, bool matchCase, bool wholeWords,
            std::vector<TextMatch>& out) {
  if (!body || query.empty()) return;
  const std::wstring q = Folded(query, matchCase);
  for (size_t p = 0; p < body->paragraphs.size(); ++p) {
    const std::wstring raw = ParagraphText(body->paragraphs[p]);
    const std::wstring t = Folded(raw, matchCase);
    for (size_t at = t.find(q); at != std::wstring::npos; at = t.find(q, at + q.size())) {
      if (wholeWords) {
        const bool before = at > 0 && IsCharAlphaNumericW(raw[at - 1]);
        const bool after = at + q.size() < raw.size() && IsCharAlphaNumericW(raw[at + q.size()]);
        if (before || after) continue;
      }
      out.push_back({target, {static_cast<uint32_t>(p), static_cast<uint32_t>(at)},
                     {static_cast<uint32_t>(p), static_cast<uint32_t>(at + q.size())}});
    }
  }
}

bool Walk(Presentation& deck, const SlideBase& part, const std::vector<Shape>& shapes, TextTarget base, const TextVisitor& visit) {
  for (const Shape& sh : shapes) {
    if (sh.kind == Shape::Kind::Group) {
      if (!Walk(deck, part, sh.children, base, visit)) return false;
      continue;
    }
    TextTarget t = base;
    t.shape = sh.id;
    if (sh.kind == Shape::Kind::Table) {
      const Table* table = deck.GetTable(sh, part);
      if (!table) continue;
      for (const TableCell& c : table->cells) {
        if (c.covered || !c.text) continue;
        t.row = c.row;
        t.column = c.column;
        if (!visit(t, *c.text)) return false;
      }
      continue;
    }
    if (sh.kind != Shape::Kind::Shape || !sh.textNode) continue;
    // On a notes page, only the notes themselves (not the slide number or header placeholders).
    if (base.notes && sh.placeholder.type != PlaceholderType::Body) continue;
    if (const TextBody* body = deck.Text(sh, part))
      if (!visit(t, *body)) return false;
  }
  return true;
}

}  // namespace

void ForEachText(TextModel& model, bool notes, const TextVisitor& visit) {
  Presentation* deck = model.Deck();
  if (!deck) return;
  for (size_t i = 0; i < deck->SlideCount(); ++i) {
    const Slide* s = deck->GetSlide(i);
    if (!s) continue;
    TextTarget base;
    base.slide = i;
    if (!Walk(*deck, *s, s->shapes, base, visit)) return;
    if (notes && s->notes) {
      base.notes = true;
      if (!Walk(*deck, *s->notes, s->notes->shapes, base, visit)) return;
    }
  }
}

std::vector<TextMatch> FindAll(TextModel& model, std::wstring_view query, bool matchCase, bool wholeWords) {
  std::vector<TextMatch> out;
  if (query.empty()) return out;
  const std::wstring q(query);
  ForEachText(model, true, [&](const TextTarget& t, const TextBody& body) {
    Search(&body, t, q, matchCase, wholeWords, out);
    return true;
  });
  return out;
}

std::unique_ptr<TextDoc> OpenText(TextModel& model, const TextTarget& target) {
  if (!model.Part(target)) return nullptr;
  TextModel* m = &model;
  return std::make_unique<TextDoc>(
      model.Body(target), [m, target](pugi::xml_node body) { return m->Resolve(target, body); },
      target.Cell() ? "a:txBody" : "p:txBody");
}

namespace {

// Replaces [from, to) in a working copy, the new text in the look of its first character.
void ReplaceIn(TextDoc& doc, TextPos from, TextPos to, std::wstring_view with) {
  pugi::xml_document hold;
  pugi::xml_node props;
  if (pugi::xml_node first = doc.PropsAt({from.para, from.offset + (from.offset < doc.Length(from.para) ? 1u : 0u)}))
    props = hold.append_copy(first);
  const TextPos at = doc.Delete(from, to);
  if (!with.empty()) doc.Insert(at, with, props);
}

bool Still(const TextDoc& doc, const TextMatch& m, std::wstring_view query, bool matchCase) {
  if (m.from.para != m.to.para || m.from.para >= doc.ParagraphCount() || m.to.offset > doc.Length(m.from.para)) return false;
  const std::wstring now = doc.Text(m.from, m.to);
  return Folded(now, matchCase) == Folded(std::wstring(query), matchCase);
}

}  // namespace

bool ReplaceMatch(TextModel& model, const TextMatch& match, std::wstring_view query, bool matchCase, std::wstring_view with) {
  std::unique_ptr<TextDoc> doc = OpenText(model, match.target);
  if (!doc || !Still(*doc, match, query, matchCase)) return false;
  ReplaceIn(*doc, match.from, match.to, with);
  std::wstring error;
  return model.SetText(match.target, doc->Body(), u"Replace", false, error);
}

int ReplaceAll(TextModel& model, const std::vector<TextMatch>& matches, std::wstring_view with) {
  // Body by body, each from its last match to its first, so earlier places hold.
  std::vector<std::pair<TextTarget, std::vector<const TextMatch*>>> bodies;
  for (const TextMatch& m : matches) {
    auto it = std::find_if(bodies.begin(), bodies.end(), [&](const auto& b) { return b.first == m.target; });
    if (it == bodies.end()) bodies.push_back({m.target, {&m}});
    else it->second.push_back(&m);
  }
  int replaced = 0;
  model.BeginGroup(u"Replace All");
  for (auto& [target, list] : bodies) {
    std::unique_ptr<TextDoc> doc = OpenText(model, target);
    if (!doc) continue;
    std::sort(list.begin(), list.end(), [](const TextMatch* a, const TextMatch* b) { return b->from < a->from; });
    int here = 0;
    for (const TextMatch* m : list) {
      if (m->to.para >= doc->ParagraphCount() || m->to.offset > doc->Length(m->to.para)) continue;
      ReplaceIn(*doc, m->from, m->to, with);
      ++here;
    }
    std::wstring error;
    if (here && model.SetText(target, doc->Body(), u"Replace All", false, error)) replaced += here;
  }
  model.EndGroup();
  return replaced;
}

}  // namespace fw::slide::view
