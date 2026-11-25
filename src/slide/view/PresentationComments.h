#pragma once

// The presentation's comments (P3, p3-review; plan §8.2): PowerPoint's Comments pane (a card for each
// thread of the current slide with its replies; a new comment, reply, edit, resolve, delete), markers
// on the slide (a click picks the thread, a drag moves it), Review > Comments and Ctrl+Alt+M. Every
// change is one undo step through src/slide/edit/Comments.h. The view makes this object the first time
// it draws a slide and calls it for the markers, the pointer, the commands and script ops.

#include <string>
#include <utility>
#include <vector>

#include "Commands.h"
#include "slide/edit/Comments.h"
#include "slide/view/PresentationView.h"
#include "ui/Controls.h"
#include "ui/PanelButton.h"
#include "ui/SidePanel.h"

namespace fw::slide::view {

class PresentationView::CommentsPanel : public PanelContent {
 public:
  explicit CommentsPanel(PresentationView& view);
  // ---- The pane
  std::wstring Title() const override { return L"Comments"; }
  std::vector<Control*> Layout(const D2D1_RECT_F& body, float ui) override;
  void Draw(gfx::Canvas& c) override;
  bool OnMouseDown(float x, float y, bool dbl) override;
  bool OnMouseMove(float x, float y) override;
  void OnMouseLeave() override;
  bool OnWheel(int delta, float x, float y) override;
  std::wstring TooltipAt(float x, float y) override;

  // ---- The slide: markers drawn over it (`slide`: the slide on screen), the pointer on them.
  void DrawMarkers(gfx::Canvas& c, const D2D1_RECT_F& slide);
  bool MouseDown(float x, float y);
  bool MouseMove(float x, float y);  // true while a marker is dragged
  bool MouseUp(float x, float y);
  bool Dragging() const { return !drag_.empty(); }
  std::wstring Tooltip(float x, float y);

  // ---- Commands (ids 3880-3909, CmdNewComment, CmdReviewPane, CmdDeleteThread) and script ops.
  static bool Owns(int cmd);
  CommandState Query(int cmd);
  bool Execute(int cmd);
  // comment:new|text, comment:reply|text, comment:edit|text, comment:resolve, comment:delete,
  // comment:next, comment:prev, comment:pane.
  bool ScriptOp(const std::wstring& op, const std::wstring& arg);

 private:
  struct Card {
    std::string id;
    bool resolved = false, legacy = false;
    std::vector<std::pair<std::wstring, std::wstring>> lines;  // "Ann Lee · 5 Nov 2026", text: the first, then replies
    D2D1_RECT_F rc{};
  };
  class Box : public ComboBox {
   public:
    using ComboBox::TakeFocus;
  };
  edit::Editor* Ed() const;
  const std::vector<edit::Comment>& List();  // the current slide's, read again after each edit
  bool AnyInDeck();  // any comment on any slide, counted again after each edit
  draw::Point Anchor(const edit::Comment& c);  // where its marker is, slide points
  D2D1_RECT_F Marker(const edit::Comment& c, const D2D1_RECT_F& slide);
  std::string MarkerAt(float x, float y);
  edit::CommentAuthor Writer() const;
  void Open(bool startNew);  // the pane shown (a new thread: the box takes the keyboard)
  void Step(bool forward);   // Previous / Next across slides
  bool Post(const std::wstring& text);
  bool Reply(const std::wstring& text);
  void Edit();
  void Resolve();
  void Delete();
  bool Done(const edit::Result& r, const wchar_t* ok);
  void Refresh();  // the cards and the buttons, as the deck is now
  void PlaceCards();
  int CardAt(float x, float y) const;
  bool Shown() const;

  PresentationView& v_;
  // The current slide's comments, as of the editor's state.
  std::vector<edit::Comment> list_;
  size_t listSlide_ = SIZE_MAX;
  uint64_t listState_ = UINT64_MAX;
  // Whether the deck has comments, as of the editor's state: counting reads every slide's comments
  // (1.2 ms in a 50-slide deck), and the ribbon asks for Previous and Next twice each time it lays out.
  bool anyInDeck_ = false;
  uint64_t anyState_ = UINT64_MAX;
  std::string picked_;  // the picked thread (its first comment's id)
  bool markers_ = true;  // Show Comments > Show Markup
  // A marker being dragged.
  std::string drag_;
  draw::Point dragFrom_, dragAt_, dragGrab_;
  bool dragMoved_ = false;
  // The pane.
  Box box_, name_;
  Box* focusWanted_ = nullptr;
  PanelButton post_, reply_, edit_, resolve_, delete_;
  std::vector<Card> cards_;
  std::string editing_;   // the comment the box edits; empty: a new thread or a reply
  bool newThread_ = true;  // the box starts a thread at at_ (else it replies to the picked one)
  draw::Point at_;
  std::wstring note_;
  D2D1_RECT_F cardsRc_{}, noteRc_{};
  std::vector<std::pair<std::wstring, D2D1_RECT_F>> labels_;
  float ui_ = 1, scroll_ = 0, height_ = 0;
  int hover_ = -1;
  size_t seenSlide_ = SIZE_MAX;
  uint64_t seenState_ = UINT64_MAX;
};

// File > Info > Protect Presentation > Encrypt with Password (PresentationProtect.cpp): a password set
// (typed twice) or removed; the next save writes the file encrypted with it, or without one.
class PresentationView::PasswordPanel : public PanelContent {
 public:
  explicit PasswordPanel(PresentationView& view);
  std::wstring Title() const override { return L"Encrypt with password"; }
  std::vector<Control*> Layout(const D2D1_RECT_F& body, float ui) override;
  void Draw(gfx::Canvas& c) override;
  void Open();

 private:
  class Box : public ComboBox {
   public:
    using ComboBox::TakeFocus;
  };
  void Encrypt();
  void Remove();
  PresentationView& v_;
  Box password_, confirm_;
  bool focusWanted_ = false;
  PanelButton encrypt_, remove_;
  std::wstring note_;
  std::vector<std::pair<std::wstring, D2D1_RECT_F>> labels_;
  D2D1_RECT_F noteRc_{};
  float ui_ = 1;
};

}  // namespace fw::slide::view
