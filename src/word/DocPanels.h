#pragma once

// The Word document's side panels (D3): inserting and editing links (Ctrl+K), bookmarks, page
// numbers, and the navigation pane (headings and search results). Each is a DocView member and
// works through its editor; DocHeaders.cpp opens them.

#include <functional>
#include <string>
#include <vector>

#include "doc/edit/Inline.h"
#include "ui/Controls.h"
#include "ui/SidePanel.h"
#include "word/DocView.h"

// A plain push button: the panels' actions.
class DocView::PushButton : public Control {
 public:
  std::wstring text;
  bool primary = false;  // the panel's main action: in the accent color
  std::function<void()> onInvoke;

  void Draw(gfx::Canvas& c) override;
  bool OnMouseMove(float x, float y) override;
  void OnMouseLeave() override { hover_ = false; }
  void OnMouseDown(float x, float y, bool dbl, bool shift) override;
  void OnMouseUp(float x, float y) override;
  bool OnKeyDown(UINT vk, bool ctrl, bool shift, bool alt) override;
  void Describe(uia::Tree& t, int parent) override;

 private:
  void Invoke();
  bool hover_ = false, pressed_ = false;
};

// A panel laid out as rows: a label over each control, buttons side by side.
class DocPanelRows {
 protected:
  struct Row {
    std::wstring label;  // drawn above the controls
    std::vector<Control*> controls;
    float height = 32;   // DIPs
  };
  std::vector<Control*> LayOutRows(const std::vector<Row>& rows, const D2D1_RECT_F& body, float ui);
  void DrawLabels(gfx::Canvas& c) const;
  std::vector<std::pair<std::wstring, D2D1_RECT_F>> labels_;
  std::wstring note_;  // a line of help under the rows
  D2D1_RECT_F noteRc_{};
  float ui_ = 1;
};

class DocView::LinkPanel : public PanelContent, DocPanelRows {
 public:
  explicit LinkPanel(DocView& view);
  std::wstring Title() const override { return editing_ ? L"Edit link" : L"Insert link"; }
  std::vector<Control*> Layout(const D2D1_RECT_F& body, float ui) override;
  void Draw(gfx::Canvas& c) override { DrawLabels(c); }
  // Fills the boxes from the link at the caret, or the selection.
  void Open();

 private:
  struct Place {
    uint32_t para = fw::doc::kNone;  // a heading
    std::string bookmark;            // or a bookmark
  };
  void Apply();
  void Remove();
  DocView& v_;
  bool editing_ = false;
  TextPos at_;  // the link being edited
  std::vector<Place> places_;  // the address list's places in the document, after its first item
  ComboBox address_, text_;
  PushButton apply_, remove_;
};

class DocView::BookmarkPanel : public PanelContent, DocPanelRows {
 public:
  explicit BookmarkPanel(DocView& view);
  std::wstring Title() const override { return L"Bookmarks"; }
  std::vector<Control*> Layout(const D2D1_RECT_F& body, float ui) override;
  void Draw(gfx::Canvas& c) override { DrawLabels(c); }
  void Open();

 private:
  void Refresh();
  void Add();
  void GoTo();
  void Delete();
  DocView& v_;
  std::vector<fw::doc::edit::BookmarkInfo> marks_;
  ComboBox name_;
  ListBox list_;
  PushButton add_, goTo_, delete_;
};

class DocView::PageNumberPanel : public PanelContent, DocPanelRows {
 public:
  explicit PageNumberPanel(DocView& view);
  std::wstring Title() const override { return L"Page numbers"; }
  std::vector<Control*> Layout(const D2D1_RECT_F& body, float ui) override;
  void Draw(gfx::Canvas& c) override { DrawLabels(c); }
  void Open();

 private:
  uint32_t Section() const;
  void Insert();
  void ApplyFormat();
  DocView& v_;
  Dropdown position_, align_, style_, format_, numbering_;
  SpinBox start_;
  PushButton insert_, apply_;
};

// The navigation pane: the document's headings (the one the caret is under marked, and kept in
// view), or the search's results. Drawn and handled here, without controls.
class DocView::NavPanel : public PanelContent {
 public:
  explicit NavPanel(DocView& view) : v_(view) {}
  std::wstring Title() const override { return L"Navigation"; }
  std::vector<Control*> Layout(const D2D1_RECT_F& body, float ui) override;
  void Draw(gfx::Canvas& c) override;
  bool TakesFocus() const override { return true; }
  void SetFocus(bool has, bool byKeyboard) override;
  bool OnMouseDown(float x, float y, bool dbl) override;
  bool OnMouseMove(float x, float y) override;
  void OnMouseLeave() override;
  bool OnWheel(int delta, float x, float y) override;
  bool OnKeyDown(UINT vk, bool ctrl, bool shift) override;
  void Describe(uia::Tree& t, int parent, const std::function<void()>& takeFocus) override;
  void ShowResults(bool on) { results_ = on; }

 private:
  void Refresh();
  size_t Count() const;
  int RowAt(float x, float y) const;
  void Activate(int row);
  int Current() const;  // the heading the caret is under, or the current match
  DocView& v_;
  bool results_ = false;
  std::vector<Heading> headings_;
  uint32_t edits_ = UINT32_MAX;
  D2D1_RECT_F body_{}, tabs_[2]{};
  float ui_ = 1, scroll_ = 0;
  int hover_ = -1, selected_ = -1, followed_ = -1;
  bool focused_ = false;
};
