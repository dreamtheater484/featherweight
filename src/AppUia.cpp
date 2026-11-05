// What the window shows UI Automation: the title bar, the toolbar, the document area with the
// tab's own elements, the floating find panel, message and note card, and the side panel, status
// bar and command search. Elements act through the same code as clicks do.

#include "App.h"

#include "ui/Uia.h"
#if FW_TEST_SCREENS
#include "ui/WidgetGallery.h"
#endif

using namespace gfx;

void App::OpenGallery() {
#if FW_TEST_SCREENS
  tabs_.push_back(std::make_unique<WidgetGallery>(*this));
  tabs_.back()->SetViewport(DocRect(), ui_);
  ActivateTab(static_cast<int>(tabs_.size()) - 1);
#endif
}

void App::DescribeUia(uia::Tree& t) {
  TabContent* v = Active();

  if (!fullscreen_ && !presentation_) {
    std::vector<TabInfo> tabs;
    for (auto& tab : tabs_) tabs.push_back({tab->Title(), tab->Path(), tab->Modified()});
    wchar_t caption[512] = {};
    GetWindowTextW(hwnd_, caption, 512);
    title_.Describe(t, 0, caption, tabs, active_, IsZoomed(hwnd_) != FALSE, [this](TitleBar::Part part, int index) {
      switch (part) {
        case TitleBar::Part::Tab: ActivateTab(index); break;
        case TitleBar::Part::TabClose: CloseTab(index); break;
        // Posted: it opens a dialog or a menu, which can't run inside a UI Automation call.
        case TitleBar::Part::NewTab: PostMessageW(hwnd_, WM_COMMAND, CmdNewTab, 0); break;
        case TitleBar::Part::Minimize: ShowWindow(hwnd_, SW_MINIMIZE); break;
        case TitleBar::Part::Maximize: ShowWindow(hwnd_, IsZoomed(hwnd_) ? SW_RESTORE : SW_MAXIMIZE); break;
        case TitleBar::Part::Close: PostMessageW(hwnd_, WM_CLOSE, 0, 0); break;
        default: break;
      }
    });
  }

  const int toolbarFirst = t.Size();
  if (!presentation_ && (!fullscreen_ || toolbarReveal_.V() > 0.01f)) {
    auto isMenu = [](int cmd) {
      return cmd == CmdZoomMenu || cmd == CmdViewMenu || cmd == CmdSaveMenu || cmd == CmdMoreMenu;
    };
    toolbar_.Describe(t, 0, isMenu, openMenu_, [this](TextBox* box) { FocusBox(box); });
  }

  // The document area. It has the keyboard when nothing else does (arrows scroll the pages).
  int doc = t.Add(0, 5, UIA_PaneControlTypeId, v ? v->Title() : L"Start", RectF(DocRect()));
  if (v) {
    v->DescribeUia(t, doc);
  } else {
    empty_.Describe(t, doc, settings_.recent, [this](EmptyState::Result res) { OnStartScreen(res); });
  }

  // Focus moves the way a click moves it: the window's text box (and, for the tab's own
  // controls, the toolbar) lets go first, and the tab hears where the keyboard went.
  const int docEnd = t.Size();
  for (int k = toolbarFirst; k < docEnd; ++k) {
    if (k == doc || !t[k].focus) continue;
    const bool inTab = k > doc;
    t[k].focus = [this, inTab, f = std::move(t[k].focus)] {
      FocusBox(nullptr);
      if (inTab) toolbar_.Blur();
      panel_.Blur();
      status_.Blur();
      f();
      KeyFocusMoved();
    };
  }

  search_.Describe(t, 0, SearchStatus(), [this](int cmd) { Execute(cmd); }, [this] { FocusBox(&search_.Box()); });
  banner_.Describe(t, 0, [this](int id) {
    banner_.Hide();
    if (id > 0) Execute(CmdBannerAction + id);
  });
  notes_.Describe(t, 0, [this](NoteEditor::Action a) { FinishNote(a); });
  DescribeShell(t);

  if (v) {
    uia::Node& pane = t[doc];
    pane.focusable = true;
    // The tab may say what focusing the document does to its own controls.
    pane.focus = [this, own = std::move(pane.focus)] {
      FocusBox(nullptr);
      toolbar_.Blur();
      panel_.Blur();
      status_.Blur();
      SetFocus(hwnd_);
      if (own) own();
      KeyFocusMoved();
    };
    pane.focused = t.Focused() < 0 && !notes_.IsOpen();
  }
}
