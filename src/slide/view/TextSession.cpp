#include "slide/view/TextSession.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>

#include "draw/Xml.h"
#include "slide/Presentation.h"
#include "text/Fonts.h"

namespace fw::slide::view {

using namespace draw::xml;

namespace {

bool WordChar(wchar_t c) { return c == L'_' || c == L'\'' || c == 0x2019 || IsCharAlphaNumericW(c); }
bool Space(wchar_t c) { return c == L' ' || c == L'\t' || c == 0x00A0 || c == 0x3000 || c == L'\u000B'; }

// PowerPoint's shrink-on-overflow steps (fontScale, lnSpcReduction), as its files record them:
// line spacing first, then the font in 7.5 % steps.
struct FitStep {
  int scale, reduction;  // 1/1000 %
};
constexpr FitStep kFitSteps[] = {{100000, 0},     {100000, 10000}, {100000, 20000}, {92500, 10000}, {92500, 20000},
                                 {85000, 20000},  {77500, 20000},  {70000, 20000},  {62500, 20000}, {55000, 20000},
                                 {47500, 20000},  {40000, 20000},  {32500, 20000},  {25000, 20000}};

}  // namespace

TextSession::TextSession(TextModel& model, TextTarget target, text::Fonts& fonts)
    : model_(model), target_(target), fonts_(fonts) {}

TextSession::~TextSession() = default;

bool TextSession::Start(const TextFrame& frame, const render::TextExtras& extras) {
  if (!model_.Part(target_)) return false;
  TextModel* model = &model_;
  const TextTarget target = target_;
  doc_ = std::make_unique<TextDoc>(
      model_.Body(target_), [model, target](pugi::xml_node body) { return model->Resolve(target, body); },
      target_.Cell() ? "a:txBody" : "p:txBody");
  frame_ = frame;
  extras_ = extras;
  const bool shrink = frame.box.autofit == draw::TextBox::Autofit::ShrinkText;
  fontScale_ = shrink ? frame.box.fontScale : 1;
  lineReduction_ = shrink ? frame.box.lineSpacingReduction : 0;
  caret_ = anchor_ = {};
  Relayout();
  return true;
}

void TextSession::SetFrame(const TextFrame& frame) {
  frame_ = frame;
  Relayout();
}

void TextSession::ModelChanged() {
  if (!model_.Part(target_)) return;
  doc_->Reload(model_.Body(target_));
  caret_ = doc_->Clamp(caret_);
  anchor_ = doc_->Clamp(anchor_);
  pending_.reset();
  typing_ = false;
  // An undone shrink comes back with the text it was for.
  if (frame_.box.autofit == draw::TextBox::Autofit::ShrinkText)
    if (pugi::xml_node fit = A(A(doc_->Body(), "bodyPr"), "normAutofit")) {
      fontScale_ = std::clamp(static_cast<float>(Number(fit, "fontScale", 100000) / 100000), 0.01f, 1.f);
      lineReduction_ = std::clamp(static_cast<float>(Number(fit, "lnSpcReduction", 0) / 100000), 0.f, 1.f);
    }
  Relayout();
}

draw::Matrix TextSession::TextToSlide() const { return block_.ToSlide(frame_.toSlide); }

bool TextSession::TakeGeometryChanged() {
  const bool g = geometryChanged_;
  geometryChanged_ = false;
  return g;
}

std::wstring TextSession::TakeError() {
  std::wstring e;
  e.swap(error_);
  return e;
}

void TextSession::Relayout() {
  const auto t0 = std::chrono::steady_clock::now();
  draw::TextBox box = frame_.box;
  if (box.autofit == draw::TextBox::Autofit::ShrinkText) {
    box.fontScale = fontScale_;
    box.lineSpacingReduction = lineReduction_;
  }
  render::TextExtras extras = extras_;
  extras.bulletEmptyParagraph = doc_->Length(caret_.para) == 0 ? static_cast<int>(caret_.para) : -1;
  block_.Build(fonts_, box, doc_->Resolved(), extras);
  layoutMs_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// ---------------------------------------------------------------------------
// Autofit (plan §8.3): the scale PowerPoint stored holds until the text changes; then Featherweight
// fits the text itself, in PowerPoint's steps, or grows the shape to its text.

void TextSession::Fit() {
  const draw::TextBox& box = frame_.box;
  const bool vertical = box.flow != draw::TextBox::Flow::Horizontal;
  const float room = vertical ? box.rect.Width() : box.rect.Height();
  if (box.autofit == draw::TextBox::Autofit::ShrinkText && !target_.Cell()) {
    render::TextExtras extras = extras_;
    extras.bulletEmptyParagraph = doc_->Length(caret_.para) == 0 ? static_cast<int>(caret_.para) : -1;
    auto fits = [&](const FitStep& s) {
      draw::TextBox b = box;
      b.fontScale = s.scale / 100000.f;
      b.lineSpacingReduction = s.reduction / 100000.f;
      render::TextBlock trial;
      trial.Build(fonts_, b, doc_->Resolved(), extras);
      return trial.ContentHeight() <= room + 0.5f;
    };
    // The first step that fits (they shrink in order): a binary search.
    size_t lo = 0, hi = std::size(kFitSteps) - 1;
    if (fits(kFitSteps[0])) {
      hi = 0;
    } else {
      while (lo + 1 < hi) {
        const size_t mid = (lo + hi) / 2;
        if (fits(kFitSteps[mid])) hi = mid;
        else lo = mid;
      }
    }
    const FitStep& s = kFitSteps[hi];
    const float scale = s.scale / 100000.f, reduction = s.reduction / 100000.f;
    if (scale != fontScale_ || reduction != lineReduction_) {
      fontScale_ = scale;
      lineReduction_ = reduction;
      doc_->EditBody([&](pugi::xml_node bodyPr) {
        pugi::xml_node fit = A(bodyPr, "normAutofit");
        if (!fit) {
          // The autofit choice sits before scene3d, sp3d, flatTx and extLst (CT_TextBodyProperties).
          pugi::xml_node before;
          for (pugi::xml_node c = bodyPr.first_child(); c && !before; c = c.next_sibling()) {
            const std::string_view k = LocalNameOf(c);
            if (k == "scene3d" || k == "sp3d" || k == "flatTx" || k == "extLst") before = c;
          }
          const std::string name = doc_->Prefix() + "normAutofit";
          fit = before ? bodyPr.insert_child_before(name.c_str(), before) : bodyPr.append_child(name.c_str());
        }
        fit.remove_attribute("fontScale");
        fit.remove_attribute("lnSpcReduction");
        if (s.scale != 100000) fit.append_attribute("fontScale") = std::to_string(s.scale).c_str();
        if (s.reduction != 0) fit.append_attribute("lnSpcReduction") = std::to_string(s.reduction).c_str();
      });
    }
  }
}

bool TextSession::Commit(std::u16string_view step, bool coalesce) {
  Fit();
  Relayout();
  const draw::TextBox& box = frame_.box;
  float newHeight = 0;
  if (box.autofit == draw::TextBox::Autofit::ResizeShape && !target_.Cell() && box.flow == draw::TextBox::Flow::Horizontal) {
    // The shape as tall as its text and insets.
    const float now = box.rect.Height() + box.insets.top + box.insets.bottom;
    const float want = block_.ContentHeight() + box.insets.top + box.insets.bottom;
    if (std::fabs(want - now) > 0.5f) newHeight = want;
  }
  std::wstring error;
  bool ok;
  committing_ = true;
  if (newHeight > 0) {
    model_.BeginGroup(step);
    ok = model_.SetText(target_, doc_->Body(), step, false, error);
    if (ok && model_.SetShapeHeight(target_, newHeight)) geometryChanged_ = true;
    model_.EndGroup();
    coalesce = false;
  } else {
    ok = model_.SetText(target_, doc_->Body(), step, coalesce, error);
  }
  committing_ = false;
  if (!ok) {
    error_ = error.empty() ? L"This text can't be changed." : error;
    ModelChanged();
    return false;
  }
  // A cell's text taller or shorter: its row may change.
  if (target_.Cell() && std::fabs(block_.ContentHeight() - frame_.contentHeight) > 0.01f) geometryChanged_ = true;
  typing_ = coalesce || step == u"Typing";
  return true;
}

// ---------------------------------------------------------------------------
// Caret and selection

void TextSession::SetCaret(TextPos p, bool extend, bool lineEnd) {
  p = doc_->Clamp(p);
  uint32_t s, e;
  if (doc_->InField(p, s, e)) p.offset = p.offset - s < e - p.offset ? s : e;
  const bool emptyBefore = doc_->Length(caret_.para) == 0;
  const uint32_t before = caret_.para;
  caret_ = p;
  if (!extend) anchor_ = p;
  lineEnd_ = lineEnd;
  typing_ = false;
  if (pending_ && pendingAt_ != caret_) pending_.reset();
  // An empty paragraph's bullet shows only with the caret in it.
  if ((emptyBefore || doc_->Length(caret_.para) == 0) && before != caret_.para) Relayout();
}

void TextSession::Select(TextPos anchor, TextPos caret) {
  SetCaret(anchor, false);
  SetCaret(caret, true);
  goalX_.reset();
}

void TextSession::SelectAll() { Select({0, 0}, doc_->End()); }

TextPos TextSession::Step(TextPos p, bool forward) const {
  const uint32_t len = doc_->Length(p.para);
  uint32_t s, e;
  if (forward) {
    if (p.offset >= len) return p.para + 1 < doc_->ParagraphCount() ? TextPos{p.para + 1, 0} : p;
    uint32_t o = p.offset + 1;
    while (o < len && !block_.IsStop(p.para, o)) ++o;
    if (doc_->InField({p.para, o}, s, e)) o = e;
    return {p.para, o};
  }
  if (p.offset == 0) return p.para > 0 ? TextPos{p.para - 1, doc_->Length(p.para - 1)} : p;
  uint32_t o = p.offset - 1;
  while (o > 0 && !block_.IsStop(p.para, o)) --o;
  if (doc_->InField({p.para, o}, s, e)) o = s;
  return {p.para, o};
}

TextPos TextSession::StepWord(TextPos p, bool forward) const {
  const std::wstring& t = doc_->Text(p.para);
  const uint32_t n = static_cast<uint32_t>(t.size());
  uint32_t j = p.offset;
  if (forward) {
    if (j >= n) return Step(p, true);
    // Over the word (or a punctuation mark), then the spaces after it: the next word's start.
    if (WordChar(t[j])) {
      while (j < n && WordChar(t[j])) ++j;
    } else if (!Space(t[j])) {
      ++j;
    }
    while (j < n && Space(t[j])) ++j;
    return {p.para, j};
  }
  if (j == 0) return Step(p, false);
  --j;
  while (j > 0 && Space(t[j])) --j;
  if (WordChar(t[j]))
    while (j > 0 && WordChar(t[j - 1])) --j;
  return {p.para, j};
}

void TextSession::WordAt(TextPos p, TextPos& from, TextPos& to) const {
  const std::wstring& t = doc_->Text(p.para);
  const uint32_t n = static_cast<uint32_t>(t.size());
  uint32_t a = std::min(p.offset, n), b = a;
  if (a < n && WordChar(t[a])) {
    while (a > 0 && WordChar(t[a - 1])) --a;
    while (b < n && WordChar(t[b])) ++b;
  } else if (a > 0 && WordChar(t[a - 1])) {
    // Just after a word: that word.
    while (a > 0 && WordChar(t[a - 1])) --a;
  } else if (a < n) {
    ++b;  // a space or a mark on its own
  }
  // The spaces after it go with it, as PowerPoint selects a word.
  while (b < n && t[b] == L' ') ++b;
  from = {p.para, a};
  to = {p.para, b};
}

void TextSession::MoveCaret(Move m, bool extend) {
  const TextPos from = SelFrom(), to = SelTo();
  if (m != Move::Up && m != Move::Down) goalX_.reset();
  switch (m) {
    case Move::Left:
      if (HasSelection() && !extend) return SetCaret(from, false);
      return SetCaret(Step(caret_, false), extend);
    case Move::Right:
      if (HasSelection() && !extend) return SetCaret(to, false);
      return SetCaret(Step(caret_, true), extend);
    case Move::WordLeft: return SetCaret(StepWord(caret_, false), extend);
    case Move::WordRight: return SetCaret(StepWord(caret_, true), extend);
    case Move::Up:
    case Move::Down: {
      const int dir = m == Move::Up ? -1 : 1;
      const TextPos at = HasSelection() && !extend ? (dir < 0 ? from : to) : caret_;
      if (!goalX_) goalX_ = block_.CaretAt(at.para, at.offset, lineEnd_).x;
      render::TextBlock::Hit hit;
      const float goal = *goalX_;
      if (block_.Vertical(at.para, at.offset, lineEnd_, dir, goal, hit)) {
        SetCaret({static_cast<uint32_t>(hit.para), hit.offset}, extend, hit.lineEnd);
      } else {
        // Past the first or last line: to the text's start or end.
        SetCaret(dir < 0 ? TextPos{0, 0} : doc_->End(), extend);
      }
      goalX_ = goal;
      return;
    }
    case Move::LineStart:
    case Move::LineEnd: {
      const auto [s, e] = block_.LineRange(caret_.para, caret_.offset, lineEnd_);
      if (m == Move::LineStart) return SetCaret({caret_.para, s}, extend);
      // The end of a wrapped line shows there, not at the next line's start.
      return SetCaret({caret_.para, e}, extend, e < doc_->Length(caret_.para));
    }
    case Move::ParaUp:
      if (caret_.offset > 0) return SetCaret({caret_.para, 0}, extend);
      return SetCaret({caret_.para > 0 ? caret_.para - 1 : 0, 0}, extend);
    case Move::ParaDown:
      if (caret_.para + 1 < doc_->ParagraphCount()) return SetCaret({caret_.para + 1, 0}, extend);
      return SetCaret(doc_->End(), extend);
    case Move::Start: return SetCaret({0, 0}, extend);
    case Move::End: return SetCaret(doc_->End(), extend);
  }
}

// ---------------------------------------------------------------------------
// Mouse

bool TextSession::Contains(draw::Point slide) const {
  const draw::Point p = frame_.toSlide.Inverse().Apply(slide);
  const draw::TextBox& b = frame_.box;
  return p.x >= b.rect.left - b.insets.left && p.x <= b.rect.right + b.insets.right && p.y >= b.rect.top - b.insets.top &&
         p.y <= b.rect.bottom + b.insets.bottom;
}

TextPos TextSession::PosAt(draw::Point slide) const {
  const draw::Point t = TextToSlide().Inverse().Apply(slide);
  const render::TextBlock::Hit h = block_.HitTest(t);
  TextPos p{static_cast<uint32_t>(h.para), h.offset};
  p = doc_->Clamp(p);
  uint32_t s, e;
  if (doc_->InField(p, s, e)) p.offset = p.offset - s < e - p.offset ? s : e;
  return p;
}

void TextSession::PointerDown(draw::Point slide, int clicks, bool extend) {
  const draw::Point t = TextToSlide().Inverse().Apply(slide);
  const render::TextBlock::Hit h = block_.HitTest(t);
  const TextPos p = doc_->Clamp({static_cast<uint32_t>(h.para), h.offset});
  goalX_.reset();
  clickMode_ = std::clamp(clicks, 1, 3);
  if (clickMode_ == 1) {
    SetCaret(p, extend, h.lineEnd);
    clickFrom_ = clickTo_ = caret_;
    return;
  }
  if (clickMode_ == 2) {
    WordAt(p, clickFrom_, clickTo_);
  } else {
    clickFrom_ = {p.para, 0};
    clickTo_ = {p.para, doc_->Length(p.para)};
  }
  Select(clickFrom_, clickTo_);
}

void TextSession::PointerDrag(draw::Point slide) {
  const TextPos p = PosAt(slide);
  if (clickMode_ == 1) {
    const draw::Point t = TextToSlide().Inverse().Apply(slide);
    SetCaret(p, true, block_.HitTest(t).lineEnd);
    return;
  }
  // After a double- or triple-click, by words or paragraphs.
  TextPos a, b;
  if (clickMode_ == 2) {
    WordAt(p, a, b);
  } else {
    a = {p.para, 0};
    b = {p.para, doc_->Length(p.para)};
  }
  if (p < clickFrom_) {
    SetCaret(clickTo_, false);
    SetCaret(a, true);
  } else {
    SetCaret(clickFrom_, false);
    SetCaret(std::max(b, clickTo_), true);
  }
}

// ---------------------------------------------------------------------------
// Editing

bool TextSession::Type(std::wstring_view text) {
  if (text.empty()) return false;
  const bool coalesce = typing_ && !HasSelection();
  // The look typed text takes: what was set for it (Ctrl+B with no selection), else over a
  // selection its first character's.
  pugi::xml_document hold;
  pugi::xml_node props;
  if (pending_ && pendingAt_ == caret_) {
    props = hold.append_copy(pending_.first_child());
  } else if (HasSelection()) {
    const TextPos from = SelFrom();
    if (pugi::xml_node first = doc_->PropsAt({from.para, from.offset + (from.offset < doc_->Length(from.para) ? 1u : 0u)}))
      props = hold.append_copy(first);
  }
  TextPos at = caret_;
  if (HasSelection()) at = doc_->Delete(SelFrom(), SelTo());
  const TextPos end = doc_->Insert(at, text, props);
  pending_.reset();
  caret_ = anchor_ = end;
  lineEnd_ = false;
  goalX_.reset();
  return Commit(u"Typing", coalesce);
}

bool TextSession::Backspace(bool word) {
  const bool coalesce = typing_ && !HasSelection();
  if (HasSelection()) {
    caret_ = anchor_ = doc_->Delete(SelFrom(), SelTo());
    return Commit(u"Typing", false);
  }
  if (caret_.offset == 0) {
    // At a paragraph's start: an indented one comes back a level, as PowerPoint's lists do; else
    // it joins the paragraph before.
    const int level = caret_.para < doc_->Resolved().paragraphs.size() ? doc_->Resolved().paragraphs[caret_.para].props.level : 0;
    if (level > 0 && doc_->Resolved().paragraphs[caret_.para].props.bullet.kind != Bullet::Kind::None) {
      doc_->EditParagraphs(caret_.para, caret_.para, [&](pugi::xml_node pPr) {
        pPr.remove_attribute("marL");
        pPr.remove_attribute("indent");
        if (level - 1 == 0) pPr.remove_attribute("lvl");
        else (pPr.attribute("lvl") ? pPr.attribute("lvl") : pPr.append_attribute("lvl")) = level - 1;
      });
      return Commit(u"Promote", false);
    }
    if (caret_.para == 0) return false;
  }
  const TextPos from = word ? StepWord(caret_, false) : Step(caret_, false);
  if (from == caret_) return false;
  caret_ = anchor_ = doc_->Delete(from, caret_);
  lineEnd_ = false;
  goalX_.reset();
  return Commit(u"Typing", coalesce);
}

bool TextSession::DeleteForward(bool word) {
  const bool coalesce = typing_ && !HasSelection();
  if (HasSelection()) {
    caret_ = anchor_ = doc_->Delete(SelFrom(), SelTo());
    return Commit(u"Typing", false);
  }
  const TextPos to = word ? StepWord(caret_, true) : Step(caret_, true);
  if (to == caret_) return false;
  caret_ = anchor_ = doc_->Delete(caret_, to);
  lineEnd_ = false;
  goalX_.reset();
  return Commit(u"Typing", coalesce);
}

bool TextSession::Enter() {
  const bool coalesce = typing_ && !HasSelection();
  TextPos at = caret_;
  if (HasSelection()) at = doc_->Delete(SelFrom(), SelTo());
  caret_ = anchor_ = doc_->Split(at);
  lineEnd_ = false;
  goalX_.reset();
  pending_.reset();
  return Commit(u"Typing", coalesce);
}

bool TextSession::LineBreak() { return Type(L"\u000B"); }

bool TextSession::Tab(bool shift) {
  const TextPos from = SelFrom(), to = SelTo();
  const bool levels = from.para != to.para || caret_.offset == 0 || (HasSelection() && from.offset == 0);
  if (!levels) {
    if (shift) return false;
    return Type(L"\t");
  }
  // A level down (Tab) or up (Shift+Tab) for each paragraph: the master's text styles for that
  // level apply, so the paragraph's own indents go.
  bool any = false;
  for (uint32_t p = from.para; p <= to.para && p < doc_->Resolved().paragraphs.size(); ++p) {
    const int level = doc_->Resolved().paragraphs[p].props.level;
    if (shift ? level > 0 : level < 8) any = true;
  }
  if (!any) return false;
  std::vector<int> levels0;
  for (uint32_t p = from.para; p <= to.para && p < doc_->Resolved().paragraphs.size(); ++p)
    levels0.push_back(doc_->Resolved().paragraphs[p].props.level);
  size_t i = 0;
  doc_->EditParagraphs(from.para, to.para, [&](pugi::xml_node pPr) {
    const int level = i < levels0.size() ? levels0[i++] : 0;
    const int next = std::clamp(level + (shift ? -1 : 1), 0, 8);
    pPr.remove_attribute("marL");
    pPr.remove_attribute("indent");
    if (next == 0) pPr.remove_attribute("lvl");
    else (pPr.attribute("lvl") ? pPr.attribute("lvl") : pPr.append_attribute("lvl")) = next;
  });
  return Commit(shift ? u"Promote" : u"Demote", false);
}

bool TextSession::ReplaceSelection(std::wstring_view text, std::u16string_view step) {
  pugi::xml_document hold;
  pugi::xml_node props;
  if (HasSelection()) {
    const TextPos from = SelFrom();
    if (pugi::xml_node first = doc_->PropsAt({from.para, from.offset + (from.offset < doc_->Length(from.para) ? 1u : 0u)}))
      props = hold.append_copy(first);
  }
  TextPos at = caret_;
  if (HasSelection()) at = doc_->Delete(SelFrom(), SelTo());
  caret_ = anchor_ = text.empty() ? at : doc_->Insert(at, text, props);
  goalX_.reset();
  pending_.reset();
  return Commit(step, false);
}

bool TextSession::ReplaceWithParagraphs(const std::vector<pugi::xml_node>& paragraphs, std::u16string_view step) {
  caret_ = anchor_ = doc_->InsertParagraphs(SelFrom(), SelTo(), paragraphs);
  goalX_.reset();
  pending_.reset();
  return Commit(step, false);
}

bool TextSession::FormatRuns(std::u16string_view step, const std::function<void(TextDoc&, pugi::xml_node rPr)>& edit) {
  TextPos from = SelFrom(), to = SelTo();
  if (!HasSelection()) {
    // Inside a word, the word (PowerPoint); in an empty paragraph, its mark; else what's typed next.
    TextPos a, b;
    WordAt(caret_, a, b);
    const std::wstring& t = doc_->Text(caret_.para);
    while (b.offset > a.offset && t[b.offset - 1] == L' ') --b.offset;  // not the spaces after it
    if (a.offset < caret_.offset && caret_.offset < b.offset && WordChar(t[caret_.offset]) && WordChar(t[caret_.offset - 1])) {
      from = a;
      to = b;
    } else if (doc_->Length(caret_.para) == 0) {
      doc_->EditMark(caret_.para, [&](pugi::xml_node rPr) { edit(*doc_, rPr); });
      return Commit(step, false);
    } else {
      if (!pending_ || pendingAt_ != caret_) {
        pending_.reset();
        pugi::xml_node rPr = pending_.append_child((doc_->Prefix() + "rPr").c_str());
        if (pugi::xml_node now = doc_->PropsAt(caret_)) {
          for (pugi::xml_attribute at : now.attributes()) rPr.append_attribute(at.name()) = at.value();
          for (pugi::xml_node c : now.children()) rPr.append_copy(c);
        }
        pendingAt_ = caret_;
      }
      edit(*doc_, pending_.first_child());
      typing_ = false;
      return true;
    }
  }
  if (!doc_->EditRuns(from, to, [&](pugi::xml_node rPr) { edit(*doc_, rPr); })) return false;
  return Commit(step, false);
}

bool TextSession::FormatParagraphs(std::u16string_view step, const std::function<void(TextDoc&, pugi::xml_node pPr)>& edit) {
  doc_->EditParagraphs(SelFrom().para, SelTo().para, [&](pugi::xml_node pPr) { edit(*doc_, pPr); });
  return Commit(step, false);
}

bool TextSession::Edit(std::u16string_view step, const std::function<bool(TextDoc&)>& edit) {
  if (!edit(*doc_)) return false;
  caret_ = doc_->Clamp(caret_);
  anchor_ = doc_->Clamp(anchor_);
  return Commit(step, false);
}

// ---------------------------------------------------------------------------
// Drawing

TextSession::CaretLine TextSession::CaretGeometry() const {
  const render::TextBlock::Caret c = block_.CaretAt(caret_.para, caret_.offset, lineEnd_);
  const draw::Matrix m = TextToSlide();
  return {m.Apply({c.x, c.top}), m.Apply({c.x, c.bottom})};
}

std::vector<std::array<draw::Point, 4>> TextSession::SelectionQuads() const {
  std::vector<std::array<draw::Point, 4>> out;
  if (!HasSelection()) return out;
  const TextPos a = SelFrom(), b = SelTo();
  const draw::Matrix m = TextToSlide();
  for (const draw::Rect& r : block_.SelectionRects(a.para, a.offset, b.para, b.offset))
    out.push_back({m.Apply({r.left, r.top}), m.Apply({r.right, r.top}), m.Apply({r.right, r.bottom}), m.Apply({r.left, r.bottom})});
  return out;
}

// ---------------------------------------------------------------------------

std::optional<TextTarget> TextTargetAt(render::SlideRender& slide, size_t index, bool notes, draw::Point point) {
  const std::vector<render::ShapeInfo>& shapes = slide.Shapes();
  for (size_t i = shapes.size(); i-- > 0;) {
    const render::ShapeInfo& s = shapes[i];
    if (s.fromMaster || s.id == 0 || s.group || !s.shape) continue;
    if (point.x < s.bounds.left || point.x > s.bounds.right || point.y < s.bounds.top || point.y > s.bounds.bottom) continue;
    TextTarget t;
    t.slide = index;
    t.notes = notes;
    t.shape = s.id;
    if (s.shape->kind == Shape::Kind::Table) {
      if (slide.CellAt(s.id, point, t.row, t.column)) return t;
      continue;
    }
    if (s.shape->kind != Shape::Kind::Shape || (!s.shape->textNode && !s.shape->placeholder)) continue;
    render::SlideRender::TextFrame f;
    if (!slide.FindText(s.id, -1, -1, f)) continue;
    const draw::Point p = f.toSlide.Inverse().Apply(point);
    const draw::TextBox& b = f.box;
    if (p.x >= b.rect.left - b.insets.left && p.x <= b.rect.right + b.insets.right && p.y >= b.rect.top - b.insets.top &&
        p.y <= b.rect.bottom + b.insets.bottom)
      return t;
  }
  return std::nullopt;
}

}  // namespace fw::slide::view
