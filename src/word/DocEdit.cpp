// Editing in the document tab (plan milestone D2): the caret and the selection, typing, deleting,
// Enter and Tab, moving by character, word, line, paragraph and page, undo and redo, and cut and
// paste. The edits are fw::doc::edit's transactions; the layout lays out again from the page an
// edit is on and rejoins the old pages (Layout::Edited), so a keystroke costs a page or two.
#include <algorithm>
#include <cmath>

#include "Commands.h"
#include "Util.h"
#include "doc/edit/Clip.h"
#include "ui/Anim.h"
#include "ui/Controls.h"
#include "ui/Theme.h"
#include "word/DocView.h"

using namespace gfx;
namespace layout = fw::doc::layout;
using fw::doc::edit::Result;

namespace {

fw::doc::Pos ToDoc(layout::TextPos p) { return {p.para, p.offset}; }
layout::TextPos ToText(fw::doc::Pos p) { return {p.para, p.offset}; }

// The caret blinks for this long after the last key or click, then stays on: no work while idle.
constexpr double kBlinkFor = 15000;
constexpr UINT kBlinkMs = 530;  // caret::On's half period

void CALLBACK BlinkProc(HWND hwnd, UINT, UINT_PTR, DWORD) { InvalidateRect(hwnd, nullptr, FALSE); }

bool WordChar(char16_t c) { return IsCharAlphaNumericW(static_cast<wchar_t>(c)) || c == u'\'' || c == 0x2019 || c == u'_'; }
bool Space(char16_t c) { return c == u' ' || c == u'\t' || c == 0x00A0 || c == 0x3000; }

}  // namespace

// ---------------------------------------------------------------------------
// Edits

void DocView::OnEdited(const fw::doc::edit::Change& c) {
  pagination_->Edited(c);
  // Positions kept from before: later paragraphs moved; those in the changed ones are found anew.
  const int32_t shift = c.Shift();
  auto move = [&](TextPos& p) {
    if (p.para >= c.first + c.removed) p.para = static_cast<uint32_t>(static_cast<int64_t>(p.para) + shift);
    else if (p.para >= c.first) return false;
    return true;
  };
  for (auto& cm : comments_) {
    annotationsStale_ |= !move(cm.from) | !move(cm.to);
    cm.measured = false;
  }
  for (auto& ch : changes_) {
    annotationsStale_ |= !move(ch.from) | !move(ch.to);
    ch.measured = false;
  }
  for (auto& l : links_) annotationsStale_ |= !move(l.from) | !move(l.to);
  if (hf_ && !move(bodyCaret_)) bodyCaret_ = {c.first, 0};
  // Matches in changed paragraphs are gone (finding again finds them).
  std::vector<Match> kept;
  for (Match& m : matches_) {
    if (!move(m.from) || !move(m.to)) continue;
    m.measured = false;
    kept.push_back(std::move(m));
  }
  if (kept.size() != matches_.size()) {
    matches_ = std::move(kept);
    current_ = matches_.empty() ? -1 : std::min(current_, static_cast<int>(matches_.size()) - 1);
  }
  ProofEdited(c);  // spelling checked and words counted again for the changed paragraphs
  // The file: modified or back to the saved state, and the parts to write.
  const bool wasModified = file_.Dirty();
  file_.Changed(editor_->Modified(), editor_->DirtyParts());
  if (file_.Dirty() != wasModified) host_.ViewStateChanged();
}

void DocView::SyncFile() {
  if (!editor_) return;
  const bool wasModified = file_.Dirty();
  file_.Changed(editor_->Modified(), editor_->DirtyParts());
  if (file_.Dirty() != wasModified) host_.ViewStateChanged();
}

bool DocView::Apply(const Result& r) {
  SyncFile();
  if (!r) {
    if (!r.message.empty()) host_.ViewMessage(BannerKind::Warning, std::wstring(r.message.begin(), r.message.end()));
    return false;
  }
  anchor_ = focus_ = ToText(r.caret);
  goalX_ = -1;
  // The caret's page, and the next where the new layout meets the old one, now; the rest in idle time.
  if (auto c = pagination_->CaretAt(focus_, CaretPage())) pagination_->LayOutPages(c->page + 2);
  TakeNewPages();
  CaretMoved(true);
  host_.ViewStateChanged();
  return true;
}

// How long a change of the page may lay out before the window shows it.
constexpr double kPageChangeMs = 30;

bool DocView::ApplyPageChange(const Result& r) {
  SyncFile();
  if (!r) {
    if (!r.message.empty()) host_.ViewMessage(BannerKind::Warning, std::wstring(r.message.begin(), r.message.end()));
    return false;
  }
  // Every page may move: the first ones now, and as many more as a frame allows, the rest in idle
  // time (IdleWork), shown as they were until then (plan §11: the first page fast, the rest in the
  // background). The caret and the selection stay, drawn once their pages are laid out.
  pagination_->LayOutPages(2);
  pagination_->Continue(kPageChangeMs);
  TakeNewPages();
  caretRevision_ = UINT32_MAX;
  host_.ViewStateChanged();
  host_.ViewInvalidate();
  return true;
}

void DocView::TypeText(std::u16string_view text) {
  if (text.empty()) return;
  if (HasSelection() && table_ && table_->ReplaceCells(text)) return;  // cells selected: they're replaced
  const bool typed = !HasSelection();
  if (!Apply(HasSelection() ? editor_->Replace(SelFrom(), SelTo(), text) : editor_->InsertText(ToDoc(focus_), text))) return;
  // "1. " or "* " at the start of a paragraph starts a list (DocFormat::AutoFormat).
  if (typed && format_ && (text == u" " || text == u"	")) format_->AutoFormat(ToDoc(focus_));
}

void DocView::DeleteBy(bool forward, bool word) {
  if (HasSelection() && table_ && table_->ReplaceCells(u"")) return;  // cells selected: they're emptied
  if (HasSelection()) {
    Apply(editor_->Delete(SelFrom(), SelTo()));
    return;
  }
  TextPos from = focus_, to = focus_;
  if (word) {
    (forward ? to : from) = StepWord(focus_, forward);
  } else {
    // The character itself (beside the caret), not hidden text such as a field's code next to it.
    const std::vector<uint32_t> stops = Stops(focus_.para);
    const std::u16string& s = doc_->Para(focus_.para).text;
    auto len = [&](uint32_t i) { return i + 1 < s.size() && s[i] >= 0xD800 && s[i] <= 0xDBFF ? 2u : 1u; };
    if (forward) {
      auto it = std::lower_bound(stops.begin(), stops.end(), focus_.offset);
      if (it != stops.end()) from = {focus_.para, *it}, to = {focus_.para, *it + len(*it)};
      else to = StepChar(focus_, true);  // the paragraph mark: joins the next paragraph
    } else {
      auto it = std::lower_bound(stops.begin(), stops.end(), focus_.offset);
      if (it != stops.begin()) from = {focus_.para, *(it - 1)}, to = {focus_.para, *(it - 1) + len(*(it - 1))};
      else from = StepChar(focus_, false);  // the start: joins the paragraph before
    }
  }
  if (from == to) return;
  Apply(editor_->Delete(ToDoc(from), ToDoc(to)));
}

void DocView::Cut() {
  if (!Editing() || !HasSelection()) return;
  PutOnClipboard();
  if (table_ && table_->SelectedCells()) {
    table_->ReplaceCells(u"");
    return;
  }
  editor_->BeginGroup(u"Cut");
  Result r = editor_->Delete(SelFrom(), SelTo());
  editor_->EndGroup();
  Apply(r);
}

void DocView::Paste(bool formatted) {
  if (!Editing()) return;
  if (formatted && PastePicture()) return;  // only when there's no text
  namespace edit = fw::doc::edit;
  std::optional<edit::Clip> clip;
  if (formatted) {
    // Featherweight's own copies come as HTML; Word's RTF says more than its HTML; then a
    // browser's HTML.
    const std::string html = util::GetClipboardBytes(L"HTML Format");
    if (!html.empty() && edit::IsOwnHtml(html)) clip = edit::FromHtml(html);
    if (!clip || clip->Empty()) {
      const std::string rtf = util::GetClipboardBytes(L"Rich Text Format");
      if (!rtf.empty()) clip = edit::FromRtf(rtf);
    }
    if ((!clip || clip->Empty()) && !html.empty()) clip = edit::FromHtml(html);
    if (clip && clip->Empty()) clip.reset();
  }
  std::u16string t;
  if (!clip) {
    std::wstring text = util::GetClipboardText();
    if (text.empty()) return;
    t.assign(text.begin(), text.end());
  }
  editor_->BeginGroup(u"Paste");
  fw::doc::Pos from = SelFrom(), to = SelTo();
  Result r;
  r.caret = from;
  if (HasSelection() && table_ && table_->SelectedCells()) {
    // Cells selected: they're emptied, and the paste goes in from the first (a table over the
    // cells from there).
    r = editor_->ClearCells(ToDoc(anchor_), ToDoc(focus_));
    from = to = r.caret;
  }
  if (r) r = clip ? edit::PasteClip(*editor_, from, to, *clip, clipSource_)
                  : from == to ? editor_->InsertText(from, t) : editor_->Replace(from, to, t);
  if (r) editor_->EndGroup();
  else editor_->CancelGroup();
  Apply(r);
}

void DocView::Undo() {
  if (Editing() && editor_->CanUndo()) Apply(editor_->Undo());
  CheckHeaderFooter();
}

void DocView::Redo() {
  if (Editing() && editor_->CanRedo()) Apply(editor_->Redo());
  CheckHeaderFooter();
}

// ---------------------------------------------------------------------------
// Keys

bool DocView::OnChar(wchar_t ch, UINT mods) {
  if (!Editing()) return false;
  // Ctrl with a letter makes a control character; AltGr (Ctrl+Alt) makes real ones.
  if ((mods & kModCtrl) && !(mods & kModAlt) && ch < 0x20) return true;
  // Enter, Tab, Backspace and Escape are keys (EditKey), not text.
  if (ch < 0x20 || ch == 0x7F) return true;
  if (ch >= 0xD800 && ch <= 0xDBFF) {
    highSurrogate_ = ch;
    return true;
  }
  std::u16string text;
  if (ch >= 0xDC00 && ch <= 0xDFFF) {
    if (!highSurrogate_) return true;
    text = {static_cast<char16_t>(highSurrogate_), static_cast<char16_t>(ch)};
  } else {
    text = {static_cast<char16_t>(ch)};
  }
  highSurrogate_ = 0;
  TypeText(text);
  AutoCorrectTyped(text.back());
  return true;
}

bool DocView::EditKey(UINT vk, UINT mods) {
  const bool ctrl = (mods & kModCtrl) != 0, shift = (mods & kModShift) != 0, alt = (mods & kModAlt) != 0;
  if (alt) return false;
  auto collapse = [&](bool toEnd) {
    const TextPos to = toEnd ? std::max(anchor_, focus_) : std::min(anchor_, focus_);
    MoveCaret(to, false);
  };
  switch (vk) {
    case VK_LEFT:
    case VK_RIGHT: {
      const bool fwd = vk == VK_RIGHT;
      if (HasSelection() && !shift && !ctrl) collapse(fwd);
      else MoveCaret(ctrl ? StepWord(focus_, fwd) : StepChar(focus_, fwd), shift);
      return true;
    }
    case VK_UP:
    case VK_DOWN: {
      const bool down = vk == VK_DOWN;
      if (ctrl) {  // the paragraph's start, or the next one's
        TextPos to = focus_;
        if (down) {
          const uint32_t next = editor_->NextPara(focus_.para);
          to = next == fw::doc::kNone ? StoryEdge(focus_, true) : TextPos{next, 0};
        } else if (focus_.offset > 0) {
          to = {focus_.para, 0};
        } else if (const uint32_t prev = editor_->PrevPara(focus_.para); prev != fw::doc::kNone) {
          to = {prev, 0};
        }
        MoveCaret(to, shift);
        return true;
      }
      const auto& c = CaretBox();
      if (goalX_ < 0 && c) goalX_ = c->x;
      const float goal = goalX_;
      auto to = pagination_->LineStep(focus_, down ? 1 : -1, goal);
      MoveCaret(to ? *to : StoryEdge(focus_, down), shift);
      goalX_ = goal;  // kept for the next Up or Down
      return true;
    }
    case VK_PRIOR:
    case VK_NEXT: {
      // A screen up or down: as many lines as fit in the window.
      const auto& c = CaretBox();
      if (!c) return true;
      const D2D1_RECT_F r = PageRectToScreen(static_cast<int>(c->page), {c->x, c->top, c->x, c->bottom});
      const int lines = std::max(1, static_cast<int>(VpH() / std::max(1.0f, r.bottom - r.top)) - 1);
      if (goalX_ < 0) goalX_ = c->x;
      const float goal = goalX_;
      auto to = pagination_->LineStep(focus_, vk == VK_NEXT ? lines : -lines, goal);
      MoveCaret(to ? *to : StoryEdge(focus_, vk == VK_NEXT), shift);
      goalX_ = goal;
      return true;
    }
    case VK_HOME:
    case VK_END: {
      const bool end = vk == VK_END;
      if (ctrl) {
        MoveCaret(StoryEdge(focus_, end), shift);
      } else if (auto line = pagination_->LineAt(focus_)) {
        MoveCaret(end ? line->end : line->start, shift);
      }
      return true;
    }
    case VK_BACK:
      DeleteBy(false, ctrl);
      return true;
    case VK_DELETE:
      if (shift && !ctrl) Cut();
      else DeleteBy(true, ctrl);
      return true;
    case VK_INSERT:
      if (ctrl && !shift) Copy();
      else if (shift && !ctrl) Paste();
      return true;
    case VK_RETURN:
      if (ctrl) return false;
      if (shift) {
        TypeText(u"\v");  // a line break (w:br)
        AutoCorrectTyped(u'\v');
      } else if (HasSelection()) {
        TypeText(u"\r");
      } else if (const uint32_t para = focus_.para; Apply(editor_->SplitParagraph(ToDoc(focus_)))) {
        AutoCorrectBreak(para);
      }
      return true;
    case VK_TAB:
      if (ctrl) return false;  // the window's (next tab)
      TypeText(u"\t");
      AutoCorrectTyped(u'\t');
      return true;
    case VK_ESCAPE:
      if (!HasSelection()) return false;
      MoveCaret(focus_, false);
      return true;
  }
  if (ctrl && !shift) {
    switch (vk) {
      case 'Z': Undo(); return true;
      case 'Y': Redo(); return true;
      case 'X': Cut(); return true;
      case 'C': Copy(); return true;
      case 'V': Paste(); return true;
      case 'A': SelectAll(); return true;
    }
  }
  if (ctrl && shift && vk == 'Z') {
    Redo();
    return true;
  }
  if (ctrl && shift && vk == 'V') {
    Paste(false);  // as plain text
    return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// The caret

void DocView::MoveCaret(TextPos to, bool extend) {
  if (extend && StoryOf(to) != StoryOf(anchor_)) return;  // a selection stays in its story
  if (!extend) anchor_ = to;
  focus_ = to;
  goalX_ = -1;
  editor_->BreakTyping();
  CaretMoved(true);
}

void DocView::CaretMoved(bool show) {
  caretRevision_ = UINT32_MAX;  // measured again
  caretStart_ = anim::Now();
  if (Editing() && keyFocus_ && !blinking_) {
    blinking_ = true;
    SetTimer(host_.ViewHwnd(), reinterpret_cast<UINT_PTR>(this), kBlinkMs, BlinkProc);
  } else if (blinking_) {
    // In step with the new start, so the caret turns on and off on the timer's ticks.
    SetTimer(host_.ViewHwnd(), reinterpret_cast<UINT_PTR>(this), kBlinkMs, BlinkProc);
  }
  if (show) {
    if (const auto& c = CaretBox()) {
      const int page = static_cast<int>(c->page);
      if (page < PageCount()) {
        if (viewMode_ == fw::ViewMode::Single && layout_.rowOfPage[page] != singleRow_) {
          singleRow_ = layout_.rowOfPage[page];
          sy_ = tsy_ = 0;
          Relayout(nullptr);
        }
        ScrollScreenRectIntoView(PageRectToScreen(page, {c->x - 2, c->top - 2, c->x + 2, c->bottom + 2}), false);
      }
    }
  }
  host_.ViewInvalidate();
}

const std::optional<layout::Layout::Caret>& DocView::CaretBox() {
  if (!(caretFor_ == focus_) || caretRevision_ != pagination_->Revision()) {
    caretBox_ = pagination_->CaretAt(focus_, CaretPage());
    caretFor_ = focus_;
    caretRevision_ = pagination_->Revision();
    TakeNewPages();
  }
  return caretBox_;
}

std::optional<RECT> DocView::CaretRect() {
  if (!Editing() || !keyFocus_ || !pagination_->LaidOut(focus_)) return std::nullopt;
  const auto& box = CaretBox();
  if (!box || box->page >= static_cast<size_t>(PageCount())) return std::nullopt;
  const D2D1_RECT_F r = PageRectToScreen(static_cast<int>(box->page), {box->x, box->top, box->x, box->bottom});
  const LONG x = std::lround(r.left);
  return RECT{x, std::lround(r.top), x + 1, std::lround(r.bottom)};
}

void DocView::DrawCaret(Canvas& c, int first, int last) {
  if (!Editing() || !keyFocus_ || HasSelection()) return;
  const bool on = caret::On(caretStart_) || anim::Now() - caretStart_ > kBlinkFor;
  if (blinking_ && anim::Now() - caretStart_ > kBlinkFor) StopBlinking();
  // After a change of the page, the caret shows once idle time has laid out its page again.
  if (!on || !pagination_->LaidOut(focus_)) return;
  const auto& box = CaretBox();
  if (!box) return;
  const int page = static_cast<int>(box->page);
  if (page < first || page > last || page >= PageCount()) return;
  const D2D1_RECT_F r = PageRectToScreen(page, {box->x, box->top, box->x, box->bottom});
  const float w = std::max(1.0f, std::round(1.0f * ui_));
  const float x = std::round(r.left);
  c.Fill(RectF(x, r.top, x + w, r.bottom), host_.ViewInverted() ? Rgb(0xFFFFFF) : Rgb(0x000000));
}

void DocView::StopBlinking() {
  if (!blinking_) return;
  blinking_ = false;
  KillTimer(host_.ViewHwnd(), reinterpret_cast<UINT_PTR>(this));
}

void DocView::SetKeyFocus(bool has) {
  if (keyFocus_ == has) return;
  keyFocus_ = has;
  if (has && Editing()) CaretMoved(false);
  else StopBlinking();
  host_.ViewInvalidate();
}

void DocView::OnDeactivated() {
  drag_ = Drag::None;
  StopBlinking();
}

// ---------------------------------------------------------------------------
// Moving

std::vector<uint32_t> DocView::Stops(uint32_t para) const {
  const std::u16string& s = doc_->Para(para).text;
  std::vector<uint32_t> out;
  for (const auto& run : doc_->Runs(para)) {
    if (run.fieldCode) continue;
    if (run.revision && (run.revision.Is(fw::doc::Name("del")) || run.revision.Is(fw::doc::Name("moveFrom")))) continue;
    if (run.run && doc_->ResolvedRun(para, run.run).vanish) continue;
    for (uint32_t i = run.start; i < run.end && i < s.size(); ++i) {
      const char16_t c = s[i];
      if (c >= 0xDC00 && c <= 0xDFFF) continue;  // the second half of a pair
      if (c == 0x13 || c == 0x14 || c == 0x15 || c == 0x03 || c == 0x04 || c == 0x05 || c == 0x00AD) continue;
      out.push_back(i);
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

DocView::TextPos DocView::StepChar(TextPos p, bool forward) const {
  const std::vector<uint32_t> stops = Stops(p.para);
  const std::u16string& s = doc_->Para(p.para).text;
  auto it = std::lower_bound(stops.begin(), stops.end(), p.offset);
  if (forward) {
    if (it != stops.end()) {
      const uint32_t i = *it;
      return {p.para, i + (i + 1 < s.size() && s[i] >= 0xD800 && s[i] <= 0xDBFF ? 2u : 1u)};
    }
    if (p.offset < s.size()) return {p.para, static_cast<uint32_t>(s.size())};
    const uint32_t next = editor_->NextPara(p.para);
    return next == fw::doc::kNone ? p : TextPos{next, 0};
  }
  if (it != stops.begin()) return {p.para, *(it - 1)};
  if (p.offset > 0) return {p.para, 0};
  const uint32_t prev = editor_->PrevPara(p.para);
  return prev == fw::doc::kNone ? p : TextPos{prev, static_cast<uint32_t>(doc_->Para(prev).text.size())};
}

DocView::TextPos DocView::StepWord(TextPos p, bool forward) const {
  const std::vector<uint32_t> stops = Stops(p.para);
  const std::u16string& s = doc_->Para(p.para).text;
  const size_t n = stops.size();
  size_t j = static_cast<size_t>(std::lower_bound(stops.begin(), stops.end(), p.offset) - stops.begin());
  if (forward) {
    if (j >= n) return StepChar(p, true);  // on to the next paragraph
    // Over the word (or a punctuation mark), then the spaces after it: the next word's start.
    if (WordChar(s[stops[j]])) {
      while (j < n && WordChar(s[stops[j]])) ++j;
    } else if (!Space(s[stops[j]])) {
      ++j;
    }
    while (j < n && Space(s[stops[j]])) ++j;
    return {p.para, j < n ? stops[j] : static_cast<uint32_t>(s.size())};
  }
  if (j == 0) return StepChar(p, false);  // back to the paragraph before
  --j;
  while (j > 0 && Space(s[stops[j]])) --j;
  if (WordChar(s[stops[j]]))
    while (j > 0 && WordChar(s[stops[j - 1]])) --j;
  return {p.para, stops[j]};
}

DocView::TextPos DocView::StoryEdge(TextPos p, bool end) const {
  const std::vector<uint32_t> paras = doc_->StoryParagraphs(StoryOf(p));
  if (paras.empty()) return p;
  return end ? TextPos{paras.back(), static_cast<uint32_t>(doc_->Para(paras.back()).text.size())} : TextPos{paras.front(), 0};
}

// ---------------------------------------------------------------------------
// Replace

bool DocView::ReplaceCurrent(const std::wstring& with) {
  if (!Editing() || current_ < 0 || current_ >= static_cast<int>(matches_.size())) return false;
  const Match m = matches_[current_];
  if (!Apply(editor_->Replace(ToDoc(m.from), ToDoc(m.to), std::u16string(with.begin(), with.end())))) return false;
  // Find again (the text changed) and go on to the next match after the one replaced.
  const TextPos after = focus_;
  StartSearch(query_, matchCase_);
  for (size_t i = 0; i < matches_.size(); ++i)
    if (StoryOf(matches_[i].from) == StoryOf(after) && !(matches_[i].from < after)) {
      current_ = static_cast<int>(i);
      ScrollToPos(matches_[i].from, matches_[i].to);
      break;
    }
  host_.ViewStateChanged();
  return true;
}

int DocView::ReplaceAll(const std::wstring& with) {
  if (!Editing() || matches_.empty()) return 0;
  // From the last to the first, so each match's place holds while the ones after it change.
  std::vector<Match> all = matches_;
  std::stable_sort(all.begin(), all.end(), [](const Match& a, const Match& b) { return b.from < a.from; });
  const std::u16string text(with.begin(), with.end());
  int replaced = 0;
  editor_->BeginGroup(u"Replace all");
  for (const Match& m : all)
    if (editor_->Replace(ToDoc(m.from), ToDoc(m.to), text)) ++replaced;  // one inside a field's code is left
  editor_->EndGroup();
  if (replaced > 0) {
    fw::doc::edit::Result r;
    r.caret = ToDoc(all.back().from);
    Apply(r);
  }
  StartSearch(query_, matchCase_);
  host_.ViewStateChanged();
  return replaced;
}
