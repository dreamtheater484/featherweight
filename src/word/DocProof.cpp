// Proofing in the document tab (plan §6.2, milestone D2): spelling checked with Windows' own spell
// checker in idle time, the pages on screen first; red squiggles under the errors on the pages
// shown (not printed); suggestions, Ignore all and Add to dictionary in the context menu; F7 to the
// next error; AutoCorrect as you type; the words and the language in the status bar; the language
// of the selection set from a menu; and the Word count dialog.
#include <windows.h>

#include <commctrl.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <tuple>

#include "Commands.h"
#include "doc/proof/Language.h"
#include "doc/proof/WordCount.h"
#include "ui/Anim.h"
#include "ui/Menu.h"
#include "ui/StatusBar.h"
#include "ui/Theme.h"
#include "word/DocView.h"

using namespace gfx;
namespace layout = fw::doc::layout;
namespace proof = fw::doc::proof;
using Action = fw::text::SpellChecker::Action;

namespace {

// One spell checker for the process: Windows' own, with its dictionaries and the user's word list.
fw::text::SpellChecker& Checker() {
  static fw::text::SpellChecker checker;
  return checker;
}

constexpr size_t kSuggestions = 5;
// Squiggles measured on one page at most: a page of text in a language the PC has no dictionary
// for, or of code, would otherwise be underlined throughout.
constexpr size_t kSquigglesPerPage = 200;

std::wstring Thousands(uint64_t n) {
  std::wstring digits = std::to_wstring(n), out;
  for (size_t i = 0; i < digits.size(); ++i) {
    if (i > 0 && (digits.size() - i) % 3 == 0) out += L',';
    out += digits[i];
  }
  return out;
}

std::wstring Words(uint64_t n) { return Thousands(n) + (n == 1 ? L" word" : L" words"); }

// A menu item's text as it is: & would underline the next letter.
std::wstring MenuText(std::wstring s) {
  for (size_t i = s.find(L'&'); i != std::wstring::npos; i = s.find(L'&', i + 2)) s.insert(i, 1, L'&');
  return s;
}

// The languages Windows can check spelling in, by name.
struct Language {
  std::wstring tag, name;
};
const std::vector<Language>& Installed() {
  static const std::vector<Language> langs = [] {
    std::vector<Language> v;
    for (const std::wstring& tag : Checker().Languages()) v.push_back({tag, proof::LanguageName(tag)});
    std::sort(v.begin(), v.end(), [](const Language& a, const Language& b) { return a.name < b.name; });
    if (v.size() > static_cast<size_t>(CmdLanguageLast - CmdLanguagePick + 1)) v.resize(CmdLanguageLast - CmdLanguagePick + 1);
    return v;
  }();
  return langs;
}

bool SameTag(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }

}  // namespace

// ---------------------------------------------------------------------------
// Checking and counting in idle time

namespace {

// The paragraphs with lines on pages [first, last], in order.
std::vector<uint32_t> ParagraphsOn(const layout::Layout& pages, int first, int last) {
  std::vector<uint32_t> out;
  for (int pg = std::max(first, 0); pg <= last && pg < static_cast<int>(pages.PageCount()); ++pg)
    for (const layout::LineBox& lb : pages.GetPage(pg).lines)
      if (lb.para != fw::doc::kNone && (out.empty() || out.back() != lb.para)) out.push_back(lb.para);
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

}  // namespace

// The pages on screen and the next: their paragraphs are checked in idle time, so squiggles are
// there when you scroll on. Nothing else is checked until it's shown or searched.
bool DocView::HasProofWork() const {
  if (spellingWanted_ || !wordsCounted_) return true;
  if (!spelling_ || placeholder_) return false;
  int first, last;
  VisiblePages(first, last);
  for (int pg = std::max(first, 0); pg <= last + 1 && pg < static_cast<int>(pagination_->PageCount()); ++pg)
    for (const layout::LineBox& lb : pagination_->GetPage(pg).lines)
      if (lb.para != fw::doc::kNone && !spelling_->Checked(lb.para)) return true;
  return false;
}

bool DocView::ProofIdle(double deadlineMs) {
  bool repaint = false;
  if (spellingWanted_) {
    // The checker's first use loads Windows' spell checking: this tick's work.
    spellingWanted_ = false;
    if (host_.ViewSpelling() && !proof::Spelling::HiddenByDocument(*doc_) && Checker().Available())
      spelling_ = std::make_unique<proof::Spelling>(*doc_, Checker());
    return false;
  }
  if (spelling_ && !placeholder_) {
    // The pages on screen first, then the next.
    int first, last;
    VisiblePages(first, last);
    auto ms = [&] { return std::clamp(deadlineMs - anim::Now(), 1.0, 8.0); };
    const uint32_t before = spelling_->Revision();
    if (!spelling_->Check(ParagraphsOn(*pagination_, first, last), ms()))
      spelling_->Check(ParagraphsOn(*pagination_, last + 1, last + 1), ms());
    repaint |= spelling_->Revision() != before;
  }
  // Words, for the status bar, once the pages are laid out (a 300-page document takes about 7 ms).
  if (!wordsCounted_ && pagination_->Done()) {
    const uint32_t n = static_cast<uint32_t>(doc_->Paragraphs().size()), main = doc_->MainStory();
    const double until = anim::Now() + 4;
    while (wordsNext_ < n && anim::Now() < until) {
      for (int k = 0; k < 50 && wordsNext_ < n; ++k, ++wordsNext_) {
        const uint32_t w =
            doc_->Para(wordsNext_).story == main ? static_cast<uint32_t>(proof::CountParagraph(*doc_, wordsNext_).words) : 0;
        paraWords_.push_back(w);
        words_ += w;
      }
    }
    if (wordsNext_ >= n) {
      wordsCounted_ = true;
      host_.ViewStateChanged();
    }
  }
  return repaint;
}

void DocView::ProofEdited(const fw::doc::edit::Change& c) {
  ++edits_;
  menuError_.reset();
  if (spelling_) spelling_->Changed(c);
  // Words: the replaced paragraphs' counts out, the new ones' in.
  if (c.first == fw::doc::kNone || wordsNext_ <= c.first) return;  // not counted yet
  const uint32_t end = c.first + c.removed;
  if (wordsNext_ < end || end > paraWords_.size()) {  // counting stopped inside them: on from there
    for (uint32_t i = c.first; i < paraWords_.size(); ++i) words_ -= paraWords_[i];
    paraWords_.resize(c.first);
    wordsNext_ = c.first;
    wordsCounted_ = false;
    return;
  }
  for (uint32_t i = c.first; i < end; ++i) words_ -= paraWords_[i];
  std::vector<uint32_t> fresh(c.added, 0);
  if (c.story == doc_->MainStory()) {
    for (uint32_t i = 0; i < c.added; ++i) {
      fresh[i] = static_cast<uint32_t>(proof::CountParagraph(*doc_, c.first + i).words);
      words_ += fresh[i];
    }
  }
  paraWords_.erase(paraWords_.begin() + c.first, paraWords_.begin() + end);
  paraWords_.insert(paraWords_.begin() + c.first, fresh.begin(), fresh.end());
  wordsNext_ = wordsNext_ - c.removed + c.added;
  wordsCounted_ = wordsNext_ >= doc_->Paragraphs().size();
}

void DocView::SettingsChanged() {
  if (!Ready()) return;
  const bool on = host_.ViewSpelling();
  if (on && !spelling_) {
    spellingWanted_ = true;  // made in idle time
  } else if (!on) {
    spellingWanted_ = false;
    spelling_.reset();
    squiggles_.clear();
    menuError_.reset();
  }
  host_.ViewInvalidate();
}

// ---------------------------------------------------------------------------
// Squiggles

void DocView::DrawSquiggles(Canvas& c, int first, int last) {
  if (!spelling_ || presentation_ || placeholder_) return;
  const int pages = static_cast<int>(pagination_->PageCount());
  squiggles_.resize(pages);
  const D2D1_COLOR_F red = Rgb(0xE81123);
  const float u = std::max(1.0f, ui_);
  for (int pg = std::max(first, 0); pg <= last && pg < pages; ++pg) {
    if (pg >= static_cast<int>(pagination_->PageCount())) break;  // fewer once an edit's layout is done
    PageSquiggles& ps = squiggles_[pg];
    if (ps.layout != pagination_->Revision() || ps.spelling != spelling_->Revision() || ps.edits != edits_) {
      // Measured again: the errors of the paragraphs with lines on the page.
      const uint32_t revision = pagination_->Revision();
      ps.layout = revision;
      ps.spelling = spelling_->Revision();
      ps.edits = edits_;
      ps.lines.clear();
      std::vector<uint32_t> paras;
      for (const layout::LineBox& lb : pagination_->GetPage(pg).lines)
        if (lb.para != fw::doc::kNone && (paras.empty() || paras.back() != lb.para)) paras.push_back(lb.para);
      std::sort(paras.begin(), paras.end());
      paras.erase(std::unique(paras.begin(), paras.end()), paras.end());
      for (uint32_t p : paras) {
        const auto* errors = spelling_->Errors(p);
        if (!errors) continue;
        for (const proof::Misspelling& m : *errors) {
          if (ps.lines.size() >= kSquigglesPerPage) break;
          // Ranges lays out pages that aren't yet, and after an edit that replaces the old page
          // shown here: the page is looked up again after it, never held across it.
          for (const auto& [rp, r] : pagination_->Ranges({p, m.from}, {p, m.to})) {
            if (static_cast<int>(rp) != pg || rp >= pagination_->PageCount()) continue;
            // Just below the baseline, by the size of the text it's under.
            float y = r.bottom - 1;
            for (const layout::LineBox& lb : pagination_->GetPage(pg).lines) {
              if (lb.para != p || lb.turn != 0 || std::abs(lb.top - r.top) > 0.01f) continue;
              float em = 0;
              for (const auto& piece : lb.line.pieces)
                if (lb.x + piece.x < r.right && lb.x + piece.x + piece.width > r.left) em = std::max(em, piece.emSize);
              y = std::min(lb.top + lb.height - 1, lb.baseline + std::max(1.0f, 0.15f * em));
              break;
            }
            ps.lines.push_back({p, m.from, m.to, r.left, r.right, y});
          }
        }
      }
      // Pages laid out while measuring: this frame painted the old ones, so it's painted again
      // (and the squiggles measured again, as the revision no longer matches).
      if (pagination_->Revision() != revision) host_.ViewInvalidate();
    }
    for (const Squiggle& s : ps.lines) {
      // The word being typed isn't marked until it's finished, as Word does it.
      if (typed_ && !HasSelection() && focus_ == typedAt_ && s.para == focus_.para && s.from <= focus_.offset &&
          focus_.offset <= s.to)
        continue;
      const D2D1_RECT_F r = PageRectToScreen(pg, {s.x1, s.y, s.x2, s.y});
      if (r.right < vp_.left || r.left > vp_.right || r.top > vp_.bottom || r.top < vp_.top - 4 * u) continue;
      // A wave: down and up every 2 px.
      const float top = std::round(r.top) + 0.5f, step = 2 * u;
      bool down = true;
      for (float x = r.left; x < r.right; x += step, down = !down) {
        const float x2 = std::min(x + step, r.right);
        c.Line(x, down ? top : top + 1.5f * u, x2, down ? top + 1.5f * u : top, red, u);
      }
    }
  }
}

// ---------------------------------------------------------------------------
// The context menu, F7 and Ignore, Add, Replace

void DocView::FillSpellingMenu(HMENU m, int x, int y) {
  using menu::AppendItem;
  menuError_.reset();
  if (!spelling_) return;
  const auto pos = PosAt(x, y, false);
  if (!pos) return;
  const proof::Misspelling* e = spelling_->At({pos->para, pos->offset});
  if (!e) return;
  MenuError t;
  t.para = pos->para;
  t.error = *e;
  t.word = spelling_->Word(pos->para, *e);
  if (e->action == Action::Replace && !e->replacement.empty()) t.suggestions = {e->replacement};
  else if (e->action == Action::Suggest) t.suggestions = Checker().Suggest(spelling_->Language(*e), t.word, kSuggestions);
  const bool can = Editing();
  if (e->action == Action::Delete) {
    AppendItem(m, CmdSpellDelete, L"Delete repeated word", can);
  } else if (t.suggestions.empty()) {
    AppendItem(m, CmdSpellSuggest, L"(No spelling suggestions)", false);
  }
  for (size_t i = 0; i < t.suggestions.size(); ++i)
    AppendItem(m, CmdSpellSuggest + static_cast<UINT>(i), MenuText(t.suggestions[i]), can);
  menu::AppendSeparator(m);
  AppendItem(m, CmdSpellIgnore, L"Ignore all");
  if (e->action != Action::Delete) AppendItem(m, CmdSpellAdd, L"Add to dictionary");
  menu::AppendSeparator(m);
  menuError_ = std::move(t);
}

void DocView::FillLanguageMenu(HMENU m) {
  const TextPos a = std::min(anchor_, focus_), b = std::max(anchor_, focus_);
  std::optional<std::wstring> current;
  if (a.para < doc_->Paragraphs().size())
    current = HasSelection() ? proof::LanguageOfRange(*doc_, {a.para, a.offset}, {b.para, b.offset})
                             : proof::LanguageAt(*doc_, {a.para, a.offset});
  const auto& langs = Installed();
  bool listed = false;
  for (size_t i = 0; i < langs.size(); ++i) {
    const bool on = current && SameTag(*current, langs[i].tag);
    listed |= on;
    menu::AppendItem(m, CmdLanguagePick + static_cast<UINT>(i), MenuText(langs[i].name), Editing(), on, true);
  }
  // The text's own language, when Windows has no dictionary for it.
  if (current && !listed && !current->empty()) {
    if (!langs.empty()) menu::AppendSeparator(m);
    menu::AppendItem(m, 0, MenuText(proof::LanguageName(*current)) + L" (no spelling dictionary)", false, true, true);
  }
}

bool DocView::ExecuteProof(int cmd) {
  if (cmd >= CmdSpellSuggest && cmd < CmdLanguagePick) {
    if (!menuError_ || !Editing()) return true;
    const size_t i = static_cast<size_t>(cmd - CmdSpellSuggest);
    if (i >= menuError_->suggestions.size()) return true;
    const std::wstring& s = menuError_->suggestions[i];
    const MenuError e = *menuError_;
    editor_->BreakTyping();
    editor_->BeginGroup(u"Spelling");
    const auto r = editor_->Replace({e.para, e.error.from}, {e.para, e.error.to}, std::u16string(s.begin(), s.end()));
    editor_->EndGroup();
    editor_->BreakTyping();
    Apply(r);
    return true;
  }
  if (cmd >= CmdLanguagePick && cmd <= CmdLanguageLast) {
    const size_t i = static_cast<size_t>(cmd - CmdLanguagePick);
    if (i < Installed().size()) SetLanguage(Installed()[i].tag);
    return true;
  }
  switch (cmd) {
    case CmdSpelling: NextMisspelling(); return true;
    case CmdWordCount: ShowWordCount(); return true;
    case CmdLanguage: ChooseLanguage(); return true;
    case CmdSpellIgnore:
    case CmdSpellAdd: {
      if (!menuError_ || !spelling_) return true;
      const std::wstring lang = spelling_->Language(menuError_->error), word = menuError_->word;
      // Ignore all lasts until Featherweight closes; Add goes into Windows' own word list.
      if (cmd == CmdSpellIgnore) Checker().Ignore(lang, word);
      else if (!Checker().Add(lang, word)) host_.ViewMessage(BannerKind::Warning, L"The word couldn't be added to the dictionary.");
      spelling_->Recheck(word);
      menuError_.reset();
      host_.ViewInvalidate();
      return true;
    }
    case CmdSpellDelete: {
      if (!menuError_ || !Editing()) return true;
      const MenuError e = *menuError_;
      // The repeated word with the space before it.
      const std::u16string& text = doc_->Para(e.para).text;
      uint32_t from = e.error.from;
      while (from > 0 && from <= text.size() && text[from - 1] == u' ') --from;
      editor_->BreakTyping();
      Apply(editor_->Delete({e.para, from}, {e.para, e.error.to}));
      return true;
    }
  }
  return false;
}

void DocView::NextMisspelling() {
  if (!spelling_ && spellingWanted_) ProofIdle(anim::Now() + 8);  // made now
  if (!spelling_) {
    host_.ViewMessage(BannerKind::Info, !host_.ViewSpelling()
                                            ? L"Spelling isn't checked: turn on “Check spelling as you type” in the ⋯ menu."
                                        : !Checker().Available() ? L"Windows' spell checker isn't available on this PC."
                                                                 : L"This document asks not to show spelling errors.");
    return;
  }
  TextPos from = HasSelection() ? std::max(anchor_, focus_) : focus_;
  if (from.para >= doc_->Paragraphs().size()) from = {0, 0};
  // Checking the paragraphs on the way, for up to two seconds (a 300-page document takes 1.6 s).
  bool complete = true;
  auto next = spelling_->FindNext({from.para, from.offset}, 2000, complete);
  if (!next) {
    host_.ViewMessage(complete ? BannerKind::Success : BannerKind::Info,
                      complete ? L"No spelling errors." : L"Still checking the spelling: press F7 again to go on.");
    host_.ViewInvalidate();
    return;
  }
  const auto& [para, m] = *next;
  if (editor_) editor_->BreakTyping();
  anchor_ = {para, m.from};
  focus_ = {para, m.to};
  typed_ = false;
  ScrollToPos(anchor_, focus_);
  CaretMoved(false);
  host_.ViewStateChanged();
  // Its suggestions, under the word.
  auto rects = pagination_->Ranges(anchor_, focus_);
  if (rects.empty() || static_cast<int>(rects.front().first) >= PageCount()) return;
  const D2D1_RECT_F r = PageRectToScreen(static_cast<int>(rects.front().first), rects.front().second);
  HMENU menu = CreatePopupMenu();
  FillSpellingMenu(menu, static_cast<int>((r.left + r.right) / 2), static_cast<int>((r.top + r.bottom) / 2));
  if (GetMenuItemCount(menu) > 0) {
    POINT pt{static_cast<LONG>(r.left), static_cast<LONG>(r.bottom + 2)};
    ClientToScreen(host_.ViewHwnd(), &pt);
    theme::ApplyToMenus();
    host_.ViewInvalidate();
    const int cmd = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y,
                                     host_.ViewHwnd(), nullptr);
    if (cmd) ExecuteProof(cmd);
  }
  DestroyMenu(menu);
  host_.ViewInvalidate();
}

// ---------------------------------------------------------------------------
// AutoCorrect

void DocView::AutoCorrectTyped(char16_t typed) {
  typedAt_ = focus_;
  typed_ = true;
  if (!Editing() || !host_.ViewAutoCorrect() || HasSelection() || focus_.para >= doc_->Paragraphs().size()) return;
  // Only when the character went in (an edit can be refused).
  const std::u16string& text = doc_->Para(focus_.para).text;
  if (focus_.offset == 0 || focus_.offset > text.size() || text[focus_.offset - 1] != typed) return;
  if (auto caret = autoCorrect_.AfterTyping(*editor_, {focus_.para, focus_.offset}, typed)) {
    fw::doc::edit::Result r;
    r.caret = *caret;
    Apply(r);
    typedAt_ = focus_;
  }
}

void DocView::AutoCorrectBreak(uint32_t para) {
  typed_ = false;
  if (!Editing() || !host_.ViewAutoCorrect()) return;
  if (autoCorrect_.AfterParagraphBreak(*editor_, para)) {
    fw::doc::edit::Result r;  // the caret stays at the new paragraph's start
    r.caret = {focus_.para, focus_.offset};
    Apply(r);
  }
}

// ---------------------------------------------------------------------------
// Language

void DocView::ChooseLanguage() {
  HMENU menu = CreatePopupMenu();
  FillLanguageMenu(menu);
  POINT pt;
  GetCursorPos(&pt);
  theme::ApplyToMenus();
  const int cmd = GetMenuItemCount(menu) > 0 ? TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y,
                                                                 host_.ViewHwnd(), nullptr)
                                             : 0;
  DestroyMenu(menu);
  if (cmd) ExecuteProof(cmd);
}

void DocView::SetLanguage(const std::wstring& tag) {
  if (!Editing()) return;
  TextPos a = std::min(anchor_, focus_), b = std::max(anchor_, focus_);
  if (a == b) WordAt(a, a, b);  // no selection: the word at the caret, as Word does
  if (a == b) return;
  const TextPos keepAnchor = anchor_, keepFocus = focus_;
  editor_->BreakTyping();
  if (Apply(proof::SetLanguage(*editor_, {a.para, a.offset}, {b.para, b.offset}, tag))) {
    anchor_ = keepAnchor;  // the text didn't move: the selection stays
    focus_ = keepFocus;
    CaretMoved(false);
  }
}

// ---------------------------------------------------------------------------
// The status bar and the Word count dialog

void DocView::ProofStatus(StatusBarModel& m) {
  if (wordsCounted_) {
    if (HasSelection()) {
      const TextPos a = std::min(anchor_, focus_), b = std::max(anchor_, focus_);
      if (!(a == selWordsFrom_) || !(b == selWordsTo_) || selWordsEdits_ != edits_) {
        selWords_ = static_cast<size_t>(proof::CountRange(*doc_, {a.para, a.offset}, {b.para, b.offset}).words);
        selWordsFrom_ = a;
        selWordsTo_ = b;
        selWordsEdits_ = edits_;
      }
      m.segments.push_back(Thousands(selWords_) + L" of " + Words(words_));
    } else {
      m.segments.push_back(Words(words_));
    }
  }
  // The language at the caret or the selection's start; in a document that isn't edited, its first.
  TextPos at = std::min(anchor_, focus_);
  if (!Editing() && !HasSelection()) {
    const auto& main = doc_->StoryParagraphs(doc_->MainStory());
    at = {main.empty() ? fw::doc::kNone : main.front(), 0};
  }
  if (at.para < doc_->Paragraphs().size()) {
    const std::wstring lang = proof::LanguageAt(*doc_, {at.para, at.offset});
    static std::wstring lastTag, lastName;  // the bar is built every frame
    if (lang != lastTag) {
      lastTag = lang;
      lastName = lang.empty() ? std::wstring() : proof::LanguageName(lang);
    }
    if (!lastName.empty()) m.segments.push_back(lastName);
  }
}

namespace {

struct WordCountDialog {
  DocView* view = nullptr;
  std::function<std::pair<std::wstring, std::wstring>(bool)> text;  // main instruction, content
  std::wstring main, content;
};

}  // namespace

void DocView::ShowWordCount() {
  static bool notesAndBoxes = false;  // Word's "Include textboxes, footnotes and endnotes", for the session
  pagination_->LayOutAll();
  TakeNewPages();
  const bool selection = HasSelection();
  const TextPos a = std::min(anchor_, focus_), b = std::max(anchor_, focus_);
  WordCountDialog d;
  d.view = this;
  d.text = [this, selection, a, b](bool notes) {
    using fw::doc::StoryKind;
    const proof::Counts n = selection ? proof::CountRange(*doc_, {a.para, a.offset}, {b.para, b.offset})
                                      : proof::CountDocument(*doc_, notes);
    std::wstring content;
    if (!selection) {
      uint64_t lines = 0;
      for (size_t pg = 0; pg < pagination_->PageCount(); ++pg) {
        for (const layout::LineBox& lb : pagination_->GetPage(pg).lines) {
          if (lb.para == fw::doc::kNone) continue;
          const StoryKind k = doc_->Stories()[doc_->Para(lb.para).story].kind;
          lines += k == StoryKind::Main ||
                   (notes && (k == StoryKind::Footnote || k == StoryKind::Endnote || k == StoryKind::TextBox));
        }
      }
      content = L"Pages: " + Thousands(pagination_->PageCount()) + L"\n";
      content += L"Characters (no spaces): " + Thousands(n.characters) + L"\n";
      content += L"Characters (with spaces): " + Thousands(n.charactersSpaces) + L"\n";
      content += L"Paragraphs: " + Thousands(n.paragraphs) + L"\n";
      content += L"Lines: " + Thousands(lines);
    } else {
      content = L"In the selection:\n";
      content += L"Characters (no spaces): " + Thousands(n.characters) + L"\n";
      content += L"Characters (with spaces): " + Thousands(n.charactersSpaces) + L"\n";
      content += L"Paragraphs: " + Thousands(n.paragraphs);
    }
    return std::make_pair(Words(n.words), content);
  };
  std::tie(d.main, d.content) = d.text(notesAndBoxes);

  TASKDIALOGCONFIG cfg{sizeof(cfg)};
  cfg.hwndParent = host_.ViewHwnd();
  cfg.hInstance = GetModuleHandleW(nullptr);
  cfg.dwFlags = TDF_POSITION_RELATIVE_TO_WINDOW | TDF_ALLOW_DIALOG_CANCELLATION |
                (notesAndBoxes ? TDF_VERIFICATION_FLAG_CHECKED : 0);
  cfg.dwCommonButtons = TDCBF_CLOSE_BUTTON;
  cfg.pszWindowTitle = L"Word count";
  cfg.pszMainInstruction = d.main.c_str();
  cfg.pszContent = d.content.c_str();
  if (!selection) cfg.pszVerificationText = L"Include text boxes, footnotes and endnotes";
  cfg.lpCallbackData = reinterpret_cast<LONG_PTR>(&d);
  cfg.pfCallback = [](HWND hwnd, UINT msg, WPARAM wp, LPARAM, LONG_PTR data) -> HRESULT {
    if (msg != TDN_VERIFICATION_CLICKED) return S_OK;
    auto* d = reinterpret_cast<WordCountDialog*>(data);
    notesAndBoxes = wp != 0;
    std::tie(d->main, d->content) = d->text(notesAndBoxes);
    SendMessageW(hwnd, TDM_SET_ELEMENT_TEXT, TDE_MAIN_INSTRUCTION, reinterpret_cast<LPARAM>(d->main.c_str()));
    SendMessageW(hwnd, TDM_SET_ELEMENT_TEXT, TDE_CONTENT, reinterpret_cast<LPARAM>(d->content.c_str()));
    return S_OK;
  };
  TaskDialogIndirect(&cfg, nullptr, nullptr, nullptr);
}
