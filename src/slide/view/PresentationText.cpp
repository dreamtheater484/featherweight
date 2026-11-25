// Text editing in the presentation tab (P2, p2-text): clicks, keys and commands into TextSession,
// and the text drawn live over the slide while it's edited.

#include "slide/view/PresentationText.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "Commands.h"
#include "Util.h"
#include "core/Lang.h"
#include "core/Strings.h"
#include "draw/D2DPainter.h"
#include "slide/view/Deck.h"
#include "slide/view/TextClip.h"
#include "slide/view/TextFind.h"
#include "slide/edit/Comments.h"
#include "slide/edit/Edit.h"
#include "slide/view/TextFormat.h"
#include "slide/view/TextModelEdit.h"
#include "slide/view/TextSpell.h"
#include "text/SpellCheck.h"
#include "ui/Anim.h"
#include "ui/Gfx.h"
#include "ui/Icons.h"
#include "ui/Menu.h"
#include "ui/PanelButton.h"
#include "ui/SidePanel.h"
#include "ui/Theme.h"
#include "ui/Toolbar.h"
#include "word/DocFormat.h"

namespace fw::slide::view {

struct TextEditing::Hide : render::DrawFilter {
  uint32_t shape = 0;
  int first = 0, count = 0;
  bool Paragraph(uint32_t id, int index) override { return !(id == shape && index >= first && index < first + count); }
};

// Ctrl+K: a link over the selection (or the link at the caret) to a web or e-mail address, a file,
// or a slide of this deck, with the text it shows and its ScreenTip, as PowerPoint's dialog has them.
class TextEditing::LinkPanel : public PanelContent {
 public:
  explicit LinkPanel(TextEditing& t);
  std::wstring Title() const override { return editing_ ? T(L"Edit link") : T(L"Insert link"); }
  std::vector<Control*> Layout(const D2D1_RECT_F& body, float ui) override;
  void Draw(gfx::Canvas& c) override;
  void Open();

 private:
  class Box : public ComboBox {
   public:
    using ComboBox::TakeFocus;
  };
  void Apply();
  void Remove();
  void Close();
  TextEditing& t_;
  Box text_, address_, tip_;
  PanelButton apply_, remove_;
  bool editing_ = false, focusWanted_ = false;
  std::vector<std::pair<std::wstring, D2D1_RECT_F>> labels_;
  std::wstring note_;
  D2D1_RECT_F noteRc_{};
  float ui_ = 1;
};

namespace {

constexpr int kCommands[] = {CmdBold, CmdItalic, CmdUnderline, CmdStrike, CmdSubscript, CmdSuperscript, CmdGrowFont,
                             CmdShrinkFont, CmdGrowFontPoint, CmdShrinkFontPoint, CmdClearCharFormat, CmdFontColor,
                             CmdHighlightColor, CmdCopyFormat, CmdPasteFormat, CmdFormatPainter, CmdAlignLeft, CmdAlignCenter, CmdAlignRight,
                             CmdAlignJustify, CmdSpacing1, CmdSpacing15, CmdSpacing2, CmdBullets, CmdNumbering, CmdIndent,
                             CmdOutdent, CmdChangeCase, CmdRemoveLink, CmdCut, CmdCopy, CmdPaste, CmdPasteMerge,
                             CmdSelectAll, CmdSpellIgnore, CmdSpellAdd};

draw::Color FromHex(const std::wstring& hex) {
  const unsigned v = static_cast<unsigned>(std::wcstoul(hex.c_str(), nullptr, 16));
  return {((v >> 16) & 255) / 255.f, ((v >> 8) & 255) / 255.f, (v & 255) / 255.f, 1};
}

}  // namespace

TextEditing::TextEditing(ViewHost& host) : host_(host), hide_(std::make_unique<Hide>()) {}
TextEditing::~TextEditing() = default;

void TextEditing::SetModel(Deck* deck, TextModel* model) {
  End();
  deck_ = deck;
  model_ = model;
}

void TextEditing::Attach(Deck* deck, edit::Editor* editor) {
  Presentation* p = deck ? deck->Model() : nullptr;
  if (editor == editor_ && deck == deck_ && (editor || !model_)) return;
  End();
  ownModel_.reset();
  deck_ = deck;
  model_ = nullptr;
  editor_ = editor;
  if (!editor || !p || !editor->CanEdit()) return;  // shown, not edited: read-only
  ownModel_ = std::make_unique<EditTextModel>(*p, *editor);
  model_ = ownModel_.get();
  // The editor lives with the deck, which the tab keeps no longer than this object.
  editor->OnChange([this, editor](const edit::Changes& c) {
    if (editor == editor_) Edited(c);
  });
}

void TextEditing::Edited(const edit::Changes& c) {
  if (!deck_ || edit::CommentsOnly(c)) return;  // p3-review: comments don't change the slide's text
  // What the slides were prepared from is gone (the editor read the touched slides again): those
  // are prepared again when next drawn.
  if (c.slideList || c.allSlides || c.slideSize) {
    for (size_t i = 0; i < deck_->SlideCount(); ++i) deck_->Invalidate(i);
  } else {
    for (const edit::SlideChange& s : c.slides) {
      if (s.index >= 0) deck_->Invalidate(static_cast<size_t>(s.index));
    }
  }
  if (!session_) return;
  if (session_->Committing()) return;  // the session's own edit: the text is drawn live
  if (c.slideList || c.allSlides) {
    // Slides came, went or moved, or the design changed under the text: editing ends.
    End();
    return;
  }
  ModelChanged();
}

uint32_t TextEditing::EditingShape(size_t slide) const {
  return session_ && !session_->Target().notes && session_->Target().slide == slide && !session_->Target().Cell()
             ? session_->Target().shape
             : 0;
}

bool TextEditing::StartAt(size_t slide, const TextTarget& t) {
  if (!deck_ || !model_ || !deck_->LayoutFonts()) return false;
  render::SlideRender* r = deck_->Rendered(slide);
  render::SlideRender::TextFrame frame;
  if (!r || !r->FindText(t.shape, t.row, t.column, frame)) return false;
  auto s = std::make_unique<TextSession>(*model_, t, *deck_->LayoutFonts());
  // The text's colors resolve against the slide's theme and color map, kept here: each edit replaces
  // the slide's model object (its pointers die), and the text is laid out again after each key.
  render::TextExtras extras;
  if (const SlideBase* part = model_->Part(t)) {
    theme_ = part->theme ? part->theme : draw::Theme::Default();
    colorMap_ = part->colorMap;
    extras.colors.theme = theme_.get();
    extras.colors.colorMap = &colorMap_;
  }
  if (!s->Start(frame, extras)) return false;
  session_ = std::move(s);
  hide_->shape = t.shape;
  hide_->first = frame.firstParagraph;
  hide_->count = std::max(frame.paragraphs, 1 << 20);
  if (t.Cell()) hide_->count = frame.paragraphs;
  slideChanged_ = true;  // the bitmap goes without this text
  return true;
}

bool TextEditing::Begin(const TextTarget& t, std::optional<draw::Point> at) {
  End();
  if (!StartAt(t.slide, t)) return false;
  if (at) session_->PointerDown(*at, 1, false);
  else session_->MoveCaret(TextSession::Move::End, false);
  Changed();
  return true;
}

bool TextEditing::BeginTextEdit(size_t slide, uint32_t shape) {
  TextTarget t;
  t.slide = slide;
  t.shape = shape;
  if (Begin(t)) return true;
  t.row = t.column = 0;  // a table: its first cell
  return Begin(t);
}

bool TextEditing::TypeInto(size_t slide, uint32_t shape) {
  TextTarget t;
  t.slide = slide;
  t.shape = shape;
  if (!Begin(t)) return false;
  session_->SelectAll();
  return true;
}

uint32_t TextEditing::TakeLeft() {
  const uint32_t id = left_;
  left_ = 0;
  return id;
}

bool TextEditing::NextCell(bool back) {
  const TextTarget t = session_->Target();
  Presentation* p = model_ ? model_->Deck() : nullptr;
  const SlideBase* part = model_ ? model_->Part(t) : nullptr;
  const Shape* sh = part ? part->FindShape(t.shape) : nullptr;
  const Table* table = p && sh ? p->GetTable(*sh, *part) : nullptr;
  if (!table) return false;
  const int rows = static_cast<int>(table->rows.size()), columns = static_cast<int>(table->columns.size());
  int r = t.row, c = t.column;
  for (int guard = rows * columns; guard > 0; --guard) {
    if (back && --c < 0) {
      c = columns - 1;
      if (--r < 0) return false;  // the first cell: nowhere to go
    } else if (!back && ++c >= columns) {
      c = 0;
      if (++r >= rows) return false;  // the last cell (PowerPoint adds a row there: a table op, not text's)
    }
    const TableCell* cell = table->Cell(r, c);
    if (cell && !cell->covered) break;
  }
  TextTarget next = t;
  next.row = r;
  next.column = c;
  if (!Begin(next)) return false;
  session_->SelectAll();
  Changed();
  return true;
}

bool TextEditing::NextMisspelling(size_t slide, const std::function<void(size_t)>& goTo) {
  if (!model_ || !deck_) return false;
  if (!checker_) {
    checker_ = std::make_unique<fw::text::SpellChecker>();
    spelling_ = std::make_unique<SlideSpelling>(*checker_);
  }
  if (!checker_->Available()) {
    host_.ViewMessage(BannerKind::Info, T(L"Spelling can't be checked on this PC."));
    return false;
  }
  // The texts in order, and where to start: the caret's text, else the current slide's first.
  std::vector<TextTarget> texts;
  ForEachText(*model_, false, [&](const TextTarget& t, const TextBody&) {
    texts.push_back(t);
    return true;
  });
  if (texts.empty()) return false;
  size_t start = 0;
  TextPos after{};
  const bool inText = session_ && !session_->Target().notes;
  if (inText) {
    after = session_->SelTo();
    start = static_cast<size_t>(std::find(texts.begin(), texts.end(), session_->Target()) - texts.begin());
    if (start == texts.size()) start = 0;
  } else {
    while (start < texts.size() && texts[start].slide < slide) ++start;
    if (start == texts.size()) start = 0;
  }
  // Round the deck once; the caret's own text twice (after the caret, then before it).
  for (size_t k = 0; k <= texts.size(); ++k) {
    const TextTarget t = texts[(start + k) % texts.size()];
    const std::unique_ptr<TextDoc> doc = OpenText(*model_, t);
    if (!doc) continue;
    for (const SpellError& e : spelling_->Check(doc->Resolved())) {
      const TextPos from{e.para, e.from};
      if (inText && k == 0 && from < after) continue;
      if (inText && k == texts.size() && !(from < after)) continue;
      if (session_ && session_->Target() == t) {
        session_->Select(from, {e.para, e.to});
      } else {
        if (goTo && t.slide != slide) goTo(t.slide);
        if (!Begin(t)) continue;
        session_->Select(from, {e.para, e.to});
      }
      Changed();
      return true;
    }
  }
  host_.ViewMessage(BannerKind::Info, T(L"No spelling errors were found."));
  return false;
}

bool TextEditing::ScriptOp(const std::wstring& op, const std::wstring& arg, size_t slide) {
  auto fail = [&](const char* why) {
    std::fwprintf(stderr, L"%ls:%ls: %hs\n", op.c_str(), arg.c_str(), why);
    std::fflush(stderr);
    return true;
  };
  if (op == L"edit") {
    Presentation* p = model_ ? model_->Deck() : nullptr;
    if (!p || !deck_) return fail(T("text can't be edited in this deck"));
    const Slide* s = slide < p->SlideCount() ? p->GetSlide(slide) : nullptr;
    if (!s) return fail(T("no such slide"));
    const Shape* found = nullptr;
    std::vector<const Shape*> bodies;
    for (const Shape& sh : s->shapes) {
      const PlaceholderType type = sh.placeholder.type;
      if ((arg == L"title" && (type == PlaceholderType::Title || type == PlaceholderType::CenteredTitle)) ||
          (arg == L"subtitle" && type == PlaceholderType::Subtitle)) {
        found = &sh;
        break;
      }
      if (type == PlaceholderType::Body || type == PlaceholderType::Object) bodies.push_back(&sh);
    }
    if (!found && (arg == L"body" || arg == L"body2")) {
      // Content placeholders in their layout's order (idx): Two Content's left one, then its right one.
      std::stable_sort(bodies.begin(), bodies.end(),
                       [](const Shape* a, const Shape* b) { return a->placeholder.index < b->placeholder.index; });
      const size_t want = arg == L"body" ? 0 : 1;
      if (want < bodies.size()) found = bodies[want];
    }
    if (!found) return fail(T("no such placeholder on this slide"));
    if (!BeginTextEdit(slide, found->id)) return fail(T("its text can't be edited"));
    return true;
  }
  if (op == L"fontsize" || op == L"fontcolor") {
    if (!session_) return fail(T("no text is being edited"));
    bool ok = false;
    if (op == L"fontsize") {
      const float points = static_cast<float>(_wtof(arg.c_str()));
      ok = points >= 1 && points <= 4000 && format::Size(*session_, points);
    } else if (arg.size() == 6 && arg.find_first_not_of(L"0123456789abcdefABCDEF") == std::wstring::npos) {
      ok = format::Color(*session_, FromHex(arg));
    }
    Changed();
    if (!ok) return fail("not done");
    return true;
  }
  return false;
}

void TextEditing::End() {
  if (!session_) return;
  session_.reset();
  cellTo_.reset();
  if (linkPanel_ && host_.ViewShownPanel() == linkPanel_.get()) host_.ViewShowPanel(nullptr);
  if (editor_) editor_->EndTyping();  // typing here again later is a new step
  slideChanged_ = true;
  host_.ViewInvalidate();
  host_.ViewStateChanged();
}

void TextEditing::Changed() {
  if (session_) {
    const std::wstring error = session_->TakeError();
    if (!error.empty()) {
      // The edit layer won't change this text (an equation in it, a part it can't edit): said once,
      // and editing ends, so the next keys don't try again. The text is as it was (nothing committed).
      host_.ViewMessage(BannerKind::Warning, error);
      End();
      return;
    }
    if (session_->TakeGeometryChanged()) {
      // The slide around the text changed: it's prepared again and the text laid out in its new place.
      const TextTarget t = session_->Target();
      if (deck_) deck_->Invalidate(t.slide);
      render::SlideRender::TextFrame frame;
      if (deck_ && deck_->Rendered(t.slide) && deck_->Rendered(t.slide)->FindText(t.shape, t.row, t.column, frame)) {
        session_->SetFrame(frame);
        hide_->first = frame.firstParagraph;
        if (t.Cell()) hide_->count = frame.paragraphs;
      }
      slideChanged_ = true;
    }
  }
  host_.ViewInvalidate();
  host_.ViewStateChanged();
}

bool TextEditing::TakeSlideChanged() {
  const bool c = slideChanged_;
  slideChanged_ = false;
  return c;
}

void TextEditing::ModelChanged() {
  if (!session_ || session_->Committing()) return;
  if (!model_ || !model_->Part(session_->Target())) {
    End();
    return;
  }
  // Undo, redo or another edit: the slide is prepared again, the text read again where it is now.
  const TextTarget t = session_->Target();
  render::SlideRender::TextFrame frame;
  if (!deck_ || !deck_->Rendered(t.slide) || !deck_->Rendered(t.slide)->FindText(t.shape, t.row, t.column, frame)) {
    End();
    return;
  }
  session_->SetFrame(frame);
  session_->ModelChanged();
  slideChanged_ = true;
  host_.ViewInvalidate();
}

// ---------------------------------------------------------------------------
// Input

bool TextEditing::MouseDown(size_t slide, draw::Point at, bool doubleClick, UINT mods) {
  if (!deck_ || !model_) return false;
  const double now = anim::Now();
  int clicks = doubleClick ? 2 : 1;
  if (!doubleClick && now - lastDoubleClick_ < GetDoubleClickTime() && std::fabs(at.x - lastDoubleAt_.x) + std::fabs(at.y - lastDoubleAt_.y) < 4)
    clicks = 3;
  if (doubleClick) {
    lastDoubleClick_ = now;
    lastDoubleAt_ = at;
  }
  // Shift+click in another cell of the table being edited: a block of cells.
  int row = -1, column = -1;
  if (session_ && session_->Target().Cell() && session_->Target().slide == slide && (mods & kModShift) &&
      CellUnder(at, row, column) && (row != session_->Target().row || column != session_->Target().column)) {
    cellTo_ = {row, column};
    Changed();
    return true;
  }
  cellTo_.reset();
  // In the text being edited: the caret, a word, a paragraph.
  if (session_ && session_->Target().slide == slide && !session_->Target().notes && session_->Contains(at)) {
    session_->PointerDown(at, clicks, (mods & kModShift) != 0);
    Changed();
    return true;
  }
  render::SlideRender* r = deck_->Rendered(slide);
  const std::optional<TextTarget> t = r ? TextTargetAt(*r, slide, false, at) : std::nullopt;
  if (!t) {
    End();
    return false;
  }
  End();
  if (!StartAt(slide, *t)) return false;
  session_->PointerDown(at, clicks, false);
  Changed();
  return true;
}

void TextEditing::MouseMove(draw::Point at) {
  if (!session_) return;
  // A drag out of the edited cell into another of its table selects the block of cells between.
  int row = -1, column = -1;
  if (session_->Target().Cell() && CellUnder(at, row, column) &&
      (row != session_->Target().row || column != session_->Target().column)) {
    cellTo_ = {row, column};
  } else {
    if (cellTo_) cellTo_.reset();
    session_->PointerDrag(at);
  }
  host_.ViewInvalidate();
  host_.ViewStateChanged();
}

void TextEditing::MouseUp() {
  if (!painter_ || !session_) return;
  painter_ = false;  // the format painter's look onto what was clicked or selected (the word at a click)
  format::ApplyLook(*session_, copiedLook_.first_child());
  Changed();
}

bool TextEditing::Names(int cmd) {
  return cmd == CmdFontBox || cmd == CmdSizeBox || cmd == CmdInsertLink ||
         std::find(std::begin(kCommands), std::end(kCommands), cmd) != std::end(kCommands);
}

std::vector<int> TextEditing::CommandIds() {
  std::vector<int> out(std::begin(kCommands), std::end(kCommands));
  out.erase(std::remove_if(out.begin(), out.end(), [](int c) { return c == CmdSpellIgnore || c == CmdSpellAdd; }), out.end());
  out.push_back(CmdInsertLink);
  return out;
}

bool TextEditing::CellUnder(draw::Point at, int& row, int& column) const {
  if (!session_ || !deck_ || !session_->Target().Cell()) return false;
  render::SlideRender* r = deck_->Rendered(session_->Target().slide);
  return r && r->CellAt(session_->Target().shape, at, row, column);
}

std::optional<std::array<int, 4>> TextEditing::CellRange() const {
  if (!session_ || !session_->Target().Cell()) return std::nullopt;
  const int r0 = session_->Target().row, c0 = session_->Target().column;
  const int r1 = cellTo_ ? cellTo_->first : r0, c1 = cellTo_ ? cellTo_->second : c0;
  return std::array<int, 4>{std::min(r0, r1), std::min(c0, c1), std::max(r0, r1), std::max(c0, c1)};
}

bool TextEditing::OverText(size_t slide, draw::Point at) {
  if (!deck_ || !model_) return false;
  if (session_ && session_->Target().slide == slide && session_->Contains(at)) return true;
  render::SlideRender* r = deck_->Rendered(slide);
  return r && TextTargetAt(*r, slide, false, at).has_value();
}

bool TextEditing::KeyDown(UINT vk, UINT mods) {
  if (!session_) return false;
  if (cellTo_ && vk != VK_SHIFT && vk != VK_CONTROL && vk != VK_MENU) cellTo_.reset();
  TextSession& s = *session_;
  const bool ctrl = (mods & kModCtrl) != 0, shift = (mods & kModShift) != 0, alt = (mods & kModAlt) != 0;
  using M = TextSession::Move;
  bool handled = true;
  if (alt && shift && (vk == VK_RIGHT || vk == VK_LEFT)) {
    format::Level(s, vk == VK_RIGHT);
  } else if (alt) {
    return false;
  } else {
    switch (vk) {
      case VK_LEFT: s.MoveCaret(ctrl ? M::WordLeft : M::Left, shift); break;
      case VK_RIGHT: s.MoveCaret(ctrl ? M::WordRight : M::Right, shift); break;
      case VK_UP: s.MoveCaret(ctrl ? M::ParaUp : M::Up, shift); break;
      case VK_DOWN: s.MoveCaret(ctrl ? M::ParaDown : M::Down, shift); break;
      case VK_HOME: s.MoveCaret(ctrl ? M::Start : M::LineStart, shift); break;
      case VK_END: s.MoveCaret(ctrl ? M::End : M::LineEnd, shift); break;
      case VK_BACK: s.Backspace(ctrl); break;
      case VK_DELETE: s.DeleteForward(ctrl); break;
      case VK_RETURN: shift ? s.LineBreak() : s.Enter(); break;
      case VK_TAB:
        // In a table, Tab goes to the next cell (Shift+Tab the one before), its text selected.
        if (s.Target().Cell()) return NextCell(shift) || true;
        s.Tab(shift);
        break;
      case VK_ESCAPE:
        if (painter_) {  // Esc first puts the format painter down
          painter_ = false;
          break;
        }
        // The text is left; the shape stays selected (the tab asks TakeLeft for it).
        left_ = s.Target().notes ? 0 : s.Target().shape;
        End();
        return true;
      default: handled = false; break;
    }
  }
  if (handled) Changed();
  return handled;
}

bool TextEditing::Char(wchar_t ch, UINT mods) {
  if (!session_) return false;
  if ((mods & kModCtrl) && !(mods & kModAlt)) return false;  // Ctrl+letter: a command, not text
  if (ch < 0x20 || ch == 0x7F) return true;                  // Enter, Tab, Backspace: KeyDown's
  // A character outside the BMP comes as two: the pair goes in together.
  if (ch >= 0xD800 && ch <= 0xDBFF) {
    highSurrogate_ = ch;
    return true;
  }
  std::wstring text;
  if (ch >= 0xDC00 && ch <= 0xDFFF) {
    if (!highSurrogate_) return true;
    text = {highSurrogate_, ch};
  } else {
    text = {ch};
  }
  highSurrogate_ = 0;
  session_->Type(text);
  Changed();
  return true;
}

// ---------------------------------------------------------------------------
// Drawing

render::DrawFilter* TextEditing::Filter(size_t slide) {
  return session_ && session_->Target().slide == slide && !session_->Target().notes ? hide_.get() : nullptr;
}

void TextEditing::Draw(gfx::Canvas& c, ID2D1DeviceContext* dc, size_t slide, const D2D1_RECT_F& rc) {
  if (!session_ || !deck_ || session_->Target().slide != slide || session_->Target().notes) return;
  const draw::Size size = deck_->SlideSize();
  if (size.width <= 0) return;
  const float k = (rc.right - rc.left) / size.width;
  const draw::Matrix toScreen = draw::Matrix::Scale(k, k) * draw::Matrix::Translate(rc.left, rc.top);
  const Palette& pal = theme::P();
  c.PushClip(rc);
  {
    // Transforms go through the painter (it keeps its own, from the device's when it's made): the
    // text block adds its own to the painter's.
    draw::D2DPainter painter(dc);
    painter.UsePdfLikeDrawing();  // as the slide view draws it, so text doesn't change look when editing starts
    // The text box's outline, dashed, as PowerPoint marks the text being edited.
    const render::SlideRender::TextFrame& f = session_->Frame();
    painter.Save();
    painter.Transform(f.toSlide * toScreen);
    const draw::Rect& r = f.box.rect;
    draw::Path box;
    box.MoveTo({r.left - f.box.insets.left, r.top - f.box.insets.top});
    box.LineTo({r.right + f.box.insets.right, r.top - f.box.insets.top});
    box.LineTo({r.right + f.box.insets.right, r.bottom + f.box.insets.bottom});
    box.LineTo({r.left - f.box.insets.left, r.bottom + f.box.insets.bottom});
    box.Close();
    draw::Pen pen;
    pen.brush = draw::Brush::Solid({0.45f, 0.45f, 0.45f, 1});
    pen.width = 1 / std::max(0.01f, k);
    pen.dashes = {4, 3};
    painter.StrokePath(box, pen);
    // The text itself, laid out after the last keystroke.
    session_->Block().Draw(painter, nullptr, session_->Target().shape);
    painter.Restore();
    // The selection over it: a block of cells, or the text's.
    const D2D1_COLOR_F sel = pal.selection;
    if (const auto range = CellRange(); range && cellTo_) {
      render::SlideRender* table = deck_->Rendered(slide);
      for (int row = (*range)[0]; table && row <= (*range)[2]; ++row)
        for (int col = (*range)[1]; col <= (*range)[3]; ++col) {
          render::SlideRender::TextFrame cf;
          if (!table->FindText(session_->Target().shape, row, col, cf)) continue;
          const draw::Rect& b = cf.box.rect;
          draw::Path cell;
          cell.MoveTo({b.left - cf.box.insets.left, b.top - cf.box.insets.top});
          cell.LineTo({b.right + cf.box.insets.right, b.top - cf.box.insets.top});
          cell.LineTo({b.right + cf.box.insets.right, b.bottom + cf.box.insets.bottom});
          cell.LineTo({b.left - cf.box.insets.left, b.bottom + cf.box.insets.bottom});
          cell.Close();
          painter.Save();
          painter.Transform(cf.toSlide * toScreen);
          painter.FillPath(cell, draw::Brush::Solid({sel.r, sel.g, sel.b, 0.4f}));
          painter.Restore();
        }
    }
    painter.Save();
    painter.Transform(toScreen);
    for (const auto& q : cellTo_ ? std::vector<std::array<draw::Point, 4>>() : session_->SelectionQuads()) {
      draw::Path p;
      p.MoveTo(q[0]);
      p.LineTo(q[1]);
      p.LineTo(q[2]);
      p.LineTo(q[3]);
      p.Close();
      painter.FillPath(p, draw::Brush::Solid({sel.r, sel.g, sel.b, 0.4f}));
    }
    painter.Restore();
  }
  // The caret: a thin line, in screen pixels.
  if (!session_->HasSelection() && !cellTo_) {
    const TextSession::CaretLine line = session_->CaretGeometry();
    const draw::Point a = toScreen.Apply(line.top), b = toScreen.Apply(line.bottom);
    c.Line(std::round(a.x) + 0.5f, a.y, std::round(b.x) + 0.5f, b.y, D2D1::ColorF(0, 0, 0, 1), std::max(1.0f, k * 0.75f));
  }
  c.PopClip();
}

void TextEditing::DrawSpelling(gfx::Canvas& c, size_t slide, const D2D1_RECT_F& rc, bool on) {
  // Squiggles under misspelled words in the slide's texts, as the editor shows them (a deck that
  // can't be edited shows none): the one being edited as typed, the others as drawn.
  if (!on || !deck_ || !model_) return;
  render::SlideRender* render = deck_->Rendered(slide);
  if (!render) return;
  if (!checker_) {
    checker_ = std::make_unique<fw::text::SpellChecker>();
    spelling_ = std::make_unique<SlideSpelling>(*checker_);
  }
  if (!checker_->Available()) return;
  const draw::Size size = deck_->SlideSize();
  if (size.width <= 0) return;
  const float k = (rc.right - rc.left) / size.width;
  const draw::Matrix toScreen = draw::Matrix::Scale(k, k) * draw::Matrix::Translate(rc.left, rc.top);
  auto underline = [&](const render::TextBlock& block, const TextBody& body, const draw::Matrix& toSlide) {
    const draw::Matrix m = toSlide * toScreen;
    for (const SpellError& e : spelling_->Check(body)) {
      for (const draw::Rect& r : block.SelectionRects(e.para, e.from, e.para, e.to)) {
        // A wavy red line along the rectangle's bottom.
        const draw::Point a = m.Apply({r.left, r.bottom}), b = m.Apply({r.right, r.bottom});
        const float len = std::hypot(b.x - a.x, b.y - a.y);
        if (len < 1) continue;
        const float step = 2.0f;
        const int n = static_cast<int>(len / step);
        for (int i = 0; i < n; ++i) {
          const float t0 = i * step / len, t1 = (i + 1) * step / len;
          const float y0 = (i % 2) ? -1.f : 1.f, y1 = -y0;
          c.Line(a.x + (b.x - a.x) * t0, a.y - 1 + y0, a.x + (b.x - a.x) * t1, a.y - 1 + y1, D2D1::ColorF(0.85f, 0.1f, 0.1f, 1), 1);
        }
      }
    }
  };
  const uint32_t editing = EditingShape(slide);
  c.PushClip(rc);
  for (const render::SlideRender::TextOnSlide& t : render->Texts())
    if (t.shape != editing) underline(*t.block, *t.body, t.toSlide);
  if (session_ && session_->Target().slide == slide && !session_->Target().notes)
    underline(session_->Block(), session_->Doc().Resolved(), session_->TextToSlide());
  c.PopClip();
}

std::optional<RECT> TextEditing::CaretRect(const D2D1_RECT_F& rc) const {
  if (!session_ || !deck_) return std::nullopt;
  const draw::Size size = deck_->SlideSize();
  if (size.width <= 0) return std::nullopt;
  const float k = (rc.right - rc.left) / size.width;
  const TextSession::CaretLine line = session_->CaretGeometry();
  const draw::Matrix toScreen = draw::Matrix::Scale(k, k) * draw::Matrix::Translate(rc.left, rc.top);
  const draw::Point a = toScreen.Apply(line.top), b = toScreen.Apply(line.bottom);
  return RECT{static_cast<LONG>(std::min(a.x, b.x)), static_cast<LONG>(std::min(a.y, b.y)), static_cast<LONG>(std::max(a.x, b.x)) + 1,
              static_cast<LONG>(std::max(a.y, b.y))};
}

// ---------------------------------------------------------------------------
// Commands

int TextEditing::Shortcut(UINT vk, UINT mods) const {
  if (vk == VK_F7 && !(mods & (kModCtrl | kModShift | kModAlt)) && model_) return CmdSpelling;
  if (!session_) return 0;
  const bool ctrl = (mods & kModCtrl) != 0, shift = (mods & kModShift) != 0, alt = (mods & kModAlt) != 0;
  if (ctrl && !alt && !shift && vk == 'K') return CmdInsertLink;
  // The text shortcuts of plan §6.3, as the documents have them (headings and styles aside).
  const int cmd = DocFormat::Shortcut(vk, mods);
  return Owns(cmd) || cmd == CmdInsertLink ? cmd : 0;
}

bool TextEditing::Owns(int cmd) const {
  if (cmd == CmdSpelling) return model_ != nullptr;
  if (!session_) return false;
  if (cmd == CmdInsertLink) return true;
  if (cmd >= CmdSpellSuggest && cmd < CmdSpellSuggest + 20) return true;
  return std::find(std::begin(kCommands), std::end(kCommands), cmd) != std::end(kCommands);
}

CommandState TextEditing::Query(int cmd) {
  CommandState s;
  if (cmd == CmdSpelling) {
    s.enabled = model_ != nullptr;
    s.label = T(L"Spelling: next error");
    s.shortcut = L"F7";
    return s;
  }
  // Without text being edited each command keeps its name, unavailable (the ribbon shows them).
  const TextLook look = session_ ? LookOf(*session_) : TextLook{};
  s.enabled = session_ != nullptr;
  auto label = [&](const wchar_t* l, const wchar_t* keys) {
    s.label = l;
    s.shortcut = keys;
  };
  switch (cmd) {
    case CmdBold: label(T(L"Bold"), L"Ctrl+B"), s.checked = look.bold.value_or(false); break;
    case CmdItalic: label(T(L"Italic"), L"Ctrl+I"), s.checked = look.italic.value_or(false); break;
    case CmdUnderline: label(T(L"Underline"), L"Ctrl+U"), s.checked = look.underline.value_or(false); break;
    case CmdStrike: label(T(L"Strikethrough"), L""), s.checked = look.strike.value_or(false); break;
    case CmdSuperscript: label(T(L"Superscript"), L"Ctrl+Shift+="), s.checked = look.baseline > 0; break;
    case CmdSubscript: label(T(L"Subscript"), L"Ctrl+="), s.checked = look.baseline < 0; break;
    case CmdGrowFont: label(T(L"Increase font size"), L"Ctrl+Shift+>"); break;
    case CmdShrinkFont: label(T(L"Decrease font size"), L"Ctrl+Shift+<"); break;
    case CmdGrowFontPoint: label(T(L"Increase font size by 1 point"), L"Ctrl+]"); break;
    case CmdShrinkFontPoint: label(T(L"Decrease font size by 1 point"), L"Ctrl+["); break;
    case CmdClearCharFormat: label(T(L"Clear formatting"), L"Ctrl+Space"); break;
    case CmdFontColor: label(T(L"Font color"), L""); break;
    case CmdHighlightColor: label(T(L"Highlight"), L""); break;
    case CmdCopyFormat: label(T(L"Copy formatting"), L"Ctrl+Shift+C"); break;
    case CmdPasteFormat: label(T(L"Paste formatting"), L"Ctrl+Shift+V"), s.enabled = !copiedLook_.first_child().empty(); break;
    case CmdFormatPainter: label(T(L"Format painter"), L""), s.checked = painter_; break;
    case CmdAlignLeft: label(T(L"Align left"), L"Ctrl+L"), s.checked = look.align == TextAlign::Left; break;
    case CmdAlignCenter: label(T(L"Center"), L"Ctrl+E"), s.checked = look.align == TextAlign::Center; break;
    case CmdAlignRight: label(T(L"Align right"), L"Ctrl+R"), s.checked = look.align == TextAlign::Right; break;
    case CmdAlignJustify: label(T(L"Justify"), L"Ctrl+J"), s.checked = look.align == TextAlign::Justify; break;
    case CmdSpacing1: label(T(L"Single line spacing"), L"Ctrl+1"), s.checked = std::fabs(look.lineSpacing - 1) < 0.01f; break;
    case CmdSpacing15: label(T(L"1.5 line spacing"), L"Ctrl+5"), s.checked = std::fabs(look.lineSpacing - 1.5f) < 0.01f; break;
    case CmdSpacing2: label(T(L"Double line spacing"), L"Ctrl+2"), s.checked = std::fabs(look.lineSpacing - 2) < 0.01f; break;
    case CmdBullets: label(T(L"Bullets"), L"Ctrl+Shift+L"), s.checked = look.bullets; break;
    case CmdNumbering: label(T(L"Numbering"), L""), s.checked = look.numbering; break;
    case CmdIndent: label(T(L"Increase list level"), L"Alt+Shift+Right"), s.enabled = look.level < 8; break;
    case CmdOutdent: label(T(L"Decrease list level"), L"Alt+Shift+Left"), s.enabled = look.level > 0; break;
    case CmdChangeCase: label(T(L"Change case"), L"Shift+F3"); break;
    case CmdInsertLink: label(look.link ? T(L"Edit link") : T(L"Link"), L"Ctrl+K"); break;
    case CmdRemoveLink: label(T(L"Remove link"), L""), s.enabled = look.link; break;
    case CmdCut: label(T(L"Cut"), L"Ctrl+X"), s.enabled = session_ && session_->HasSelection(); break;
    case CmdCopy: label(T(L"Copy"), L"Ctrl+C"), s.enabled = session_ && session_->HasSelection(); break;
    case CmdPaste: label(T(L"Paste"), L"Ctrl+V"), s.enabled = IsClipboardFormatAvailable(CF_UNICODETEXT) || util::UsingPrivateClipboard(); break;
    case CmdPasteMerge: label(T(L"Paste as text"), L""), s.enabled = IsClipboardFormatAvailable(CF_UNICODETEXT) || util::UsingPrivateClipboard(); break;
    case CmdSelectAll: label(T(L"Select all"), L"Ctrl+A"); break;
    case CmdFontBox: label(T(L"Font"), L""); break;
    case CmdSizeBox: label(T(L"Font size"), L""); break;
    case CmdSpellIgnore: label(T(L"Ignore all"), L""), s.enabled = menuWord_.has_value(); break;
    case CmdSpellAdd: label(T(L"Add to dictionary"), L""), s.enabled = menuWord_.has_value(); break;
    default:
      if (cmd >= CmdSpellSuggest && cmd < CmdSpellSuggest + static_cast<int>(suggestions_.size()))
        s.label = suggestions_[cmd - CmdSpellSuggest];
      break;
  }
  if (!session_) s.enabled = s.checked = false;
  return s;
}

bool TextEditing::Execute(int cmd) {
  if (!session_) return false;
  TextSession& s = *session_;
  const TextLook look = LookOf(s);
  switch (cmd) {
    case CmdBold: format::Bold(s, !look.bold.value_or(false)); break;
    case CmdItalic: format::Italic(s, !look.italic.value_or(false)); break;
    case CmdUnderline: format::Underline(s, !look.underline.value_or(false)); break;
    case CmdStrike: format::Strike(s, !look.strike.value_or(false)); break;
    case CmdSuperscript: format::Baseline(s, look.baseline > 0 ? 0 : 1); break;
    case CmdSubscript: format::Baseline(s, look.baseline < 0 ? 0 : -1); break;
    case CmdGrowFont: format::Grow(s, true, false); break;
    case CmdShrinkFont: format::Grow(s, false, false); break;
    case CmdGrowFontPoint: format::Grow(s, true, true); break;
    case CmdShrinkFontPoint: format::Grow(s, false, true); break;
    case CmdClearCharFormat: format::ClearCharacter(s); break;
    case CmdFontColor: format::Color(s, FromHex(lastColorHex_)); break;
    case CmdHighlightColor: format::Highlight(s, draw::Color{1, 1, 0, 1}); break;
    case CmdCopyFormat: format::CopyLook(s, copiedLook_); break;
    case CmdPasteFormat: format::ApplyLook(s, copiedLook_.first_child()); break;
    case CmdFormatPainter:  // the look now; the next click or selection in text gets it (MouseUp)
      if (!painter_) format::CopyLook(s, copiedLook_);
      painter_ = !painter_ && !copiedLook_.first_child().empty();
      break;
    case CmdAlignLeft: format::Align(s, TextAlign::Left); break;
    case CmdAlignCenter: format::Align(s, TextAlign::Center); break;
    case CmdAlignRight: format::Align(s, TextAlign::Right); break;
    case CmdAlignJustify: format::Align(s, TextAlign::Justify); break;
    case CmdSpacing1: format::LineSpacing(s, 1); break;
    case CmdSpacing15: format::LineSpacing(s, 1.5f); break;
    case CmdSpacing2: format::LineSpacing(s, 2); break;
    case CmdBullets: format::Bullets(s, !look.bullets); break;
    case CmdNumbering: format::Numbering(s, !look.numbering); break;
    case CmdIndent: format::Level(s, true); break;
    case CmdOutdent: format::Level(s, false); break;
    case CmdChangeCase: format::ChangeCase(s); break;
    case CmdRemoveLink: format::SetLink(s, {}, false, {}); break;
    case CmdInsertLink: OpenLinkPanel(); break;
    case CmdCut: Copy(true); break;
    case CmdCopy: Copy(false); break;
    case CmdPaste: Paste(false); break;
    case CmdPasteMerge: Paste(true); break;
    case CmdSelectAll: s.SelectAll(); break;
    case CmdSpellIgnore:
    case CmdSpellAdd:
      if (menuWord_ && checker_) {
        const std::wstring word = s.Doc().Text(menuWord_->first, menuWord_->second);
        const std::wstring lang = checker_->Resolve(Utf8ToWide(s.LookAt(menuWord_->first).lang));
        if (cmd == CmdSpellIgnore) checker_->Ignore(lang, word);
        else checker_->Add(lang, word);
        spelling_->Forget();
      }
      break;
    default:
      if (cmd >= CmdSpellSuggest && cmd < CmdSpellSuggest + static_cast<int>(suggestions_.size()) && menuWord_) {
        s.Select(menuWord_->first, menuWord_->second);
        s.ReplaceSelection(suggestions_[cmd - CmdSpellSuggest], T(u"Spelling"));
        break;
      }
      return false;
  }
  Changed();
  return true;
}

void TextEditing::Copy(bool cut) {
  if (!session_ || !model_ || !session_->HasSelection()) return;
  const TextClipboard clip = CopySelection(*session_, *model_);
  util::SetClipboardText(clip.text, clip.rtf, clip.html);
  if (cut) session_->ReplaceSelection({}, T(u"Cut"));
}

void TextEditing::Paste(bool textOnly) {
  if (!session_ || !model_) return;
  const std::string html = textOnly ? std::string() : util::GetClipboardBytes(L"HTML Format");
  const std::string rtf = textOnly ? std::string() : util::GetClipboardBytes(L"Rich Text Format");
  fw::slide::view::Paste(*session_, *model_, html, rtf, util::GetClipboardText(), textOnly);
}

ToolItem TextEditing::Tool(int cmd) {
  // Without text being edited the items show, unavailable (the ribbon's Font and Paragraph groups).
  const CommandState q = Query(cmd);
  ToolItem t;
  t.cmd = cmd;
  t.tooltip = q.Tooltip();
  t.enabled = session_ && q.enabled;
  t.checked = q.checked;
  const std::optional<TextLook> look = session_ ? std::optional<TextLook>(LookOf(*session_)) : std::nullopt;
  switch (cmd) {
    case CmdFontBox:
      if (DocFormat::FontsPending()) DocFormat::ReadFonts();
      t.kind = ToolKind::ComboBox;
      t.width = 116;
      t.tooltip = T(L"Font");
      t.enabled = session_ != nullptr;
      t.choices = DocFormat::FontNames();
      t.text = look ? look->font.value_or(L"") : L"";
      t.onChange = [this](const ToolValue& v) {
        if (session_ && !v.text.empty()) format::Font(*session_, v.text);
        Changed();
      };
      return t;
    case CmdSizeBox:
      t.kind = ToolKind::ComboBox;
      t.width = 56;
      t.tooltip = T(L"Font size");
      t.enabled = session_ != nullptr;
      for (int v : {8, 9, 10, 11, 12, 14, 16, 18, 20, 24, 28, 32, 36, 40, 44, 48, 54, 60, 66, 72, 80, 88, 96})
        t.choices.push_back(std::to_wstring(v));
      if (look && look->size) {
        wchar_t buf[16];
        swprintf(buf, 16, L"%g", std::round(*look->size * 10) / 10);
        t.text = buf;
      }
      t.validate = [](std::wstring& text) {
        wchar_t* end = nullptr;
        const double v = std::wcstod(text.c_str(), &end);
        return end != text.c_str() && v >= 1 && v <= 4000;
      };
      t.onChange = [this](const ToolValue& v) {
        if (session_) format::Size(*session_, static_cast<float>(std::wcstod(v.text.c_str(), nullptr)));
        Changed();
      };
      return t;
    case CmdFontColor:
    case CmdHighlightColor: {
      const bool font = cmd == CmdFontColor;
      t.kind = ToolKind::SplitButton;
      t.glyph = font ? icon::FontColor : icon::Highlight;
      t.width = 52;
      t.colors = true;
      const draw::Color last = font ? FromHex(lastColorHex_) : draw::Color{1, 1, 0, 1};
      t.color = {last.r, last.g, last.b, 1};
      t.automaticLabel = font ? T(L"Automatic") : T(L"No color");
      t.automaticColor = font ? D2D1_COLOR_F{0, 0, 0, 1} : D2D1_COLOR_F{0, 0, 0, 0};
      t.onChange = [this, font](const ToolValue& v) {
        if (!session_) return;
        std::optional<draw::Color> c;
        if (!v.automatic) {
          c = draw::Color{v.color.r, v.color.g, v.color.b, 1};
          if (font) {
            wchar_t buf[8];
            swprintf(buf, 8, L"%02X%02X%02X", static_cast<unsigned>(std::lround(v.color.r * 255)),
                     static_cast<unsigned>(std::lround(v.color.g * 255)), static_cast<unsigned>(std::lround(v.color.b * 255)));
            lastColorHex_ = buf;
          }
        }
        if (font) format::Color(*session_, c);
        else format::Highlight(*session_, c);
        Changed();
      };
      return t;
    }
    default: break;
  }
  struct Glyph {
    int cmd;
    wchar_t glyph;
    bool toggle;
  };
  static constexpr Glyph kGlyphs[] = {
      {CmdBold, icon::Bold, true},           {CmdItalic, icon::Italic, true},
      {CmdUnderline, icon::Underline, true}, {CmdStrike, icon::Strikethrough, true},
      {CmdGrowFont, 0xE8E8, false},          {CmdShrinkFont, 0xE8E7, false},  // FontIncrease, FontDecrease
      {CmdClearCharFormat, icon::ClearFormatting, false}, {CmdChangeCase, icon::FontCase, false},
      {CmdFormatPainter, icon::Brush, true}, {CmdAlignLeft, icon::AlignLeft, true},
      {CmdAlignCenter, icon::AlignCenter, true}, {CmdAlignRight, icon::AlignRight, true},
      {CmdAlignJustify, icon::AlignJustify, true}, {CmdBullets, icon::BulletedList, true},
      {CmdNumbering, icon::NumberedList, true},  {CmdOutdent, icon::DecreaseIndent, false},
      {CmdIndent, icon::IncreaseIndent, false},   {CmdInsertLink, icon::Link, false},
  };
  for (const Glyph& g : kGlyphs)
    if (g.cmd == cmd) t.glyph = g.glyph, t.toggle = g.toggle;
  return t;
}

void TextEditing::FillContextMenu(HMENU m, size_t slide, draw::Point at) {
  suggestions_.clear();
  menuWord_.reset();
  if (!session_ || session_->Target().slide != slide) return;
  // A misspelled word under the click: its suggestions first.
  if (spelling_ && checker_ && session_->Contains(at)) {
    const TextPos p = session_->PosAt(at);
    for (const SpellError& e : spelling_->Check(session_->Doc().Resolved()))
      if (e.para == p.para && e.from <= p.offset && p.offset <= e.to) {
        menuWord_ = std::make_pair(TextPos{e.para, e.from}, TextPos{e.para, e.to});
        const std::wstring word = session_->Doc().Text(menuWord_->first, menuWord_->second);
        suggestions_ = e.action == fw::text::SpellChecker::Action::Replace ? std::vector<std::wstring>{e.replacement}
                                                                            : checker_->Suggest(e.lang, word, 5);
        for (size_t i = 0; i < suggestions_.size(); ++i) menu::AppendItem(m, CmdSpellSuggest + static_cast<int>(i), suggestions_[i]);
        menu::AppendItem(m, CmdSpellIgnore, T(L"Ignore all"));
        menu::AppendItem(m, CmdSpellAdd, T(L"Add to dictionary"));
        menu::AppendSeparator(m);
        break;
      }
  }
  for (int cmd : {CmdCut, CmdCopy, CmdPaste}) menu::AppendItem(m, cmd, Query(cmd).label, Query(cmd).enabled);
  menu::AppendSeparator(m);
  menu::AppendItem(m, CmdInsertLink, Query(CmdInsertLink).label);
  if (Query(CmdRemoveLink).enabled) menu::AppendItem(m, CmdRemoveLink, T(L"Remove link"));
}

// ---------------------------------------------------------------------------
// The link panel (Ctrl+K)

namespace {

// What's typed as PowerPoint takes it: an e-mail address gets mailto:, a drive path file:///, and
// anything else without a scheme https://.
std::wstring LinkAddress(std::wstring a) {
  if (a.empty() || a.find(L"://") != std::wstring::npos || a.rfind(L"mailto:", 0) == 0) return a;
  if (a.size() > 2 && a[1] == L':' && (a[2] == L'\\' || a[2] == L'/')) {
    std::replace(a.begin(), a.end(), L'\\', L'/');
    return L"file:///" + a;
  }
  if (a.find(L'@') != std::wstring::npos && a.find(L'/') == std::wstring::npos) return L"mailto:" + a;
  return L"https://" + a;
}

// "Slide 3: Its title", as the address list shows the deck's slides.
std::wstring SlideItem(Deck& deck, size_t i) {
  std::wstring title;
  Presentation* p = deck.Model();
  const Slide* s = p && i < p->SlideCount() ? p->GetSlide(i) : nullptr;
  for (const Shape& sh : s ? s->shapes : std::vector<Shape>{}) {
    if (!sh.placeholder || (sh.placeholder.type != PlaceholderType::Title && sh.placeholder.type != PlaceholderType::CenteredTitle))
      continue;
    if (const TextBody* t = p->Text(sh, *s)) {
      for (const Paragraph& para : t->paragraphs) {
        for (const TextRun& r : para.runs)
          if (r.kind == TextRun::Kind::Text) title += r.text;
        if (!title.empty()) break;
      }
    }
    break;
  }
  return Fmt(T(L"Slide {0}"), {std::to_wstring(i + 1)}) + (title.empty() ? L"" : L": " + title);
}

}  // namespace

void TextEditing::OpenLinkPanel() {
  if (!session_ || !model_) return;
  if (!linkPanel_) linkPanel_ = std::make_unique<LinkPanel>(*this);
  linkPanel_->Open();
  host_.ViewShowPanel(linkPanel_.get());
  host_.ViewInvalidate();
}

TextEditing::LinkPanel::LinkPanel(TextEditing& t) : t_(t) {
  text_.autoComplete = false;
  tip_.autoComplete = false;
  address_.tooltip = T(L"A web or e-mail address, a file, or a slide of this presentation");
  for (Box* b : {&text_, &address_, &tip_}) b->onEnter = [this] { Apply(); };
  address_.onCommit = [this](const std::wstring& item, int index) {
    // Picking a slide shows its title when there's no text yet.
    if (index >= 0 && text_.Text().empty() && !editing_) {
      const size_t colon = item.find(L": ");
      if (colon != std::wstring::npos) text_.SetText(item.substr(colon + 2));
    }
  };
  apply_.onInvoke = [this] { Apply(); };
  remove_.text = T(L"Remove link");
  remove_.onInvoke = [this] { Remove(); };
}

void TextEditing::LinkPanel::Open() {
  note_.clear();
  focusWanted_ = true;
  TextSession& s = *t_.session_;
  std::vector<std::wstring> items;
  for (size_t i = 0; t_.deck_ && i < t_.deck_->SlideCount(); ++i) items.push_back(SlideItem(*t_.deck_, i));
  address_.items = std::move(items);
  TextPos from, to;
  pugi::xml_node h;
  editing_ = format::LinkAt(s, from, to, h);
  std::wstring address, tip;
  if (editing_) {
    // What the link does, from the text as resolved: an address or a slide.
    const std::vector<Paragraph>& paras = s.Doc().Resolved().paragraphs;
    if (from.para < paras.size()) {
      uint32_t at = 0;
      for (const TextRun& r : paras[from.para].runs) {
        const uint32_t len = r.kind == TextRun::Kind::Break ? 1 : static_cast<uint32_t>(r.text.size());
        if (from.offset < at + len || (len == 0 && from.offset == at)) {
          const Hyperlink& l = r.props.click;
          if (l.action == Hyperlink::Action::Slide && l.slide >= 0 && t_.deck_) address = SlideItem(*t_.deck_, static_cast<size_t>(l.slide));
          else address = Utf8ToWide(l.url);
          tip = Utf8ToWide(l.tooltip);
          break;
        }
        at += len;
      }
    }
    text_.SetText(s.Doc().Text(from, to));
  } else {
    text_.SetText(s.HasSelection() && s.SelFrom().para == s.SelTo().para ? s.Doc().Text(s.SelFrom(), s.SelTo()) : L"");
  }
  address_.SetText(address);
  tip_.SetText(tip);
  apply_.text = editing_ ? T(L"Update link") : T(L"Insert link");
  remove_.visible = editing_;
}

std::vector<Control*> TextEditing::LinkPanel::Layout(const D2D1_RECT_F& body, float ui) {
  ui_ = ui;
  const float left = body.left + 16 * ui, right = body.right - 16 * ui;
  float y = body.top + 12 * ui;
  labels_.clear();
  const std::pair<const wchar_t*, Box*> rows[] = {{T(L"Text to display"), &text_},
                                                  {T(L"Address, or a slide in this presentation"), &address_},
                                                  {T(L"ScreenTip"), &tip_}};
  for (const auto& [label, box] : rows) {
    labels_.push_back({label, D2D1::RectF(left, y, right, y + 16 * ui)});
    box->rc = D2D1::RectF(left, y + 18 * ui, right, y + 48 * ui);
    y += 56 * ui;
  }
  apply_.rc = D2D1::RectF(left, y, left + 110 * ui, y + 32 * ui);
  remove_.rc = D2D1::RectF(apply_.rc.right + 8 * ui, y, apply_.rc.right + 8 * ui + 120 * ui, y + 32 * ui);
  noteRc_ = D2D1::RectF(left, y + 44 * ui, right, y + 124 * ui);
  std::vector<Control*> out = {&text_, &address_, &tip_, &apply_};
  if (editing_) out.push_back(&remove_);
  return out;
}

void TextEditing::LinkPanel::Draw(gfx::Canvas& c) {
  const Palette& p = theme::P();
  for (const auto& [text, rc] : labels_) c.Text(text, rc, p.textSecondary, 12 * ui_);
  c.TextWrapped(note_.empty() ? T(L"In the slide show a click on the text follows the link.") : note_, noteRc_,
                note_.empty() ? p.textSecondary : p.text, 12 * ui_, 4);
  if (focusWanted_) {
    address_.TakeFocus();
    focusWanted_ = false;
  }
}

void TextEditing::LinkPanel::Close() {
  t_.host_.ViewShowPanel(nullptr);
  t_.host_.ViewFocusPage();
  t_.host_.ViewInvalidate();
}

void TextEditing::LinkPanel::Apply() {
  TextSession* s = t_.session_.get();
  TextModel* model = t_.model_;
  if (!s || !model) {
    note_ = T(L"Click into the text to link first.");
    t_.host_.ViewInvalidate();
    return;
  }
  const std::wstring typed = Trim(address_.Text());
  if (typed.empty()) {
    note_ = T(L"Type an address, or pick a slide.");
    t_.host_.ViewInvalidate();
    return;
  }
  int slide = -1;
  const auto item = std::find(address_.items.begin(), address_.items.end(), typed);
  if (item != address_.items.end()) slide = static_cast<int>(item - address_.items.begin());
  const std::wstring url = slide >= 0 ? std::wstring() : LinkAddress(typed);
  std::wstring shown = text_.Text();
  if (shown.empty()) shown = typed;
  // The text the link goes over: the link's, the selection, or what's typed as text to display.
  TextPos from = s->SelFrom(), to = s->SelTo();
  pugi::xml_node h;
  if (editing_) format::LinkAt(*s, from, to, h);
  model->BeginGroup(T(u"Hyperlink"));
  bool ok = true;
  if ((from == to || s->Doc().Text(from, to) != shown) && from.para == to.para) {
    s->Select(from, to);
    ok = s->ReplaceSelection(shown, T(u"Hyperlink"));
    to = {from.para, from.offset + static_cast<uint32_t>(shown.size())};
  }
  std::string rel;
  if (ok) rel = model->AddLink(s->Target(), url, slide);
  if (ok && !rel.empty()) {
    s->Select(from, to);
    ok = format::SetLink(*s, rel, slide >= 0, Trim(tip_.Text()));
    s->Select(to, to);
  }
  model->EndGroup();
  t_.Changed();
  if (!ok || rel.empty()) {
    note_ = T(L"That link can't be made here.");
    t_.host_.ViewInvalidate();
    return;
  }
  Close();
}

void TextEditing::LinkPanel::Remove() {
  if (TextSession* s = t_.session_.get()) {
    format::SetLink(*s, {}, false, {});
    t_.Changed();
  }
  Close();
}

}  // namespace fw::slide::view
