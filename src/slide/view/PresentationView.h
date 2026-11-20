#pragma once

#include <windows.h>

#include <d2d1_1.h>
#include <wrl/client.h>

#include <memory>
#include <string>
#include <vector>

#include "TabContent.h"
#include "slide/view/Deck.h"
#include "slide/view/SlideLayout.h"

namespace fw::text {
class Fonts;
}

namespace fw::slide::view {

// A presentation in a tab (milestone P1, plan §5.4 and §8): the thumbnails pane on the left, the
// slide fitted to the window (or zoomed) in the middle, the speaker notes below it, and "Slide n
// of m" in the status bar. Read-only until P2. Page Up/Down, Home and End go through the slides;
// find looks in the slides' text and the notes; copy takes the slide's text and picture. F5 and
// Shift+F5 start the slide show (p1-show), and a .ppsx starts in it.
//
// Printing and Export PDF lay out slides, handouts (1, 2, 3, 4, 6 or 9 per page) or notes pages,
// as chosen in the toolbar's Print layout menu, through the shared print dialog and PrintJob.
class PresentationView : public TabContent {
 public:
  PresentationView(ViewHost& host, std::wstring path);
  // A view of a deck that's already open (tests).
  PresentationView(ViewHost& host, std::wstring path, std::unique_ptr<Deck> deck);
  ~PresentationView() override;

  // ---- TabContent ----
  TabKind Kind() const override { return TabKind::Presentation; }
  OpenResult Load(const std::wstring& password, std::wstring& error) override;
  bool Ready() const override { return deck_ != nullptr; }
  const std::wstring& Path() const override { return path_; }
  std::wstring Title() const override;
  bool Modified() const override { return false; }

  void SetViewport(const RECT& rc, float ui) override;
  void Draw(gfx::Canvas& c, ID2D1DeviceContext* dc) override;
  bool HasIdleWork() const override;
  bool IdleWork(ID2D1DeviceContext* dc, double deadlineMs) override;
  void ReleaseDeviceResources() override;
  void OnDeactivated() override;

  void OnMouseDown(int x, int y, int button, UINT mods, bool doubleClick) override;
  void OnMouseMove(int x, int y, UINT mods) override;
  void OnMouseUp(int x, int y, int button, UINT mods) override;
  void OnMouseLeave() override;
  void OnWheel(int delta, bool horizontal, UINT mods, int x, int y) override;
  bool OnKeyDown(UINT vk, UINT mods) override;
  bool OnChar(wchar_t ch, UINT mods) override;
  HCURSOR CursorAt(int x, int y) override;
  std::wstring TooltipAt(int x, int y) override;
  bool Dragging() const override { return drag_ != Drag::None; }
  bool HasKeyFocus() const override { return false; }

  int ShortcutFor(UINT vk, UINT mods) const override;
  CommandState QueryCommand(int cmd) override;
  std::vector<int> Commands() override;
  bool Execute(int cmd) override;
  void BuildToolbar(ToolbarModel& m, const ToolbarContext& ctx) override;
  void FillMenu(int which, HMENU menu) override;
  void FillContextMenu(HMENU menu, int x, int y) override;
  bool HasStatusBar() const override { return Ready(); }
  void BuildStatusBar(StatusBarModel& m) override;

  void StartSearch(const std::wstring& query, bool matchCase, bool wholeWords) override;
  void ClearSearch() override;
  void SearchStep(bool forward) override;
  int SearchCount() const override { return static_cast<int>(matches_.size()); }
  int SearchCurrent() const override { return match_; }
  bool SearchDone() const override { return true; }
  std::wstring SelectedText() override;

  // Read-only in P1: Save As writes a copy of the file as it was opened.
  bool Save(const std::wstring& path, std::wstring& error) override;
  SaveFormat SaveAsFormat() const override;
  bool CanPrint() const override { return Ready() && deck_->SlideCount() > 0; }
  PrintOutcome Print(HWND owner, const std::function<void(int, int)>& progress, std::wstring& message) override;
  bool CanExportPdf() const override { return CanPrint(); }
  bool ExportPdf(const std::wstring& path, std::wstring& message) override;
  FileState State() const override;
  void ApplyState(const FileState& s) override;
  bool ScriptOp(const std::wstring& op, const std::wstring& arg) override;

  // ---- The deck ----
  size_t SlideCount() const { return deck_ ? deck_->SlideCount() : 0; }
  size_t CurrentSlide() const { return current_; }
  void GoToSlide(size_t slide);
  Deck* GetDeck() { return deck_.get(); }
  const PrintSettings& Printing() const { return print_; }
  void SetPrinting(const PrintSettings& s) { print_ = s; }

  // ---- Tests and tools ----
  // The pages, laid out as chosen, to Microsoft Print to PDF with no dialog (the visual checks).
  PrintOutcome PrintToFile(const std::wstring& path, std::wstring& message);
  // The pages as they print and export, each a PNG (page-1.png...) in `dir` at `dpi`.
  int RenderPagesForTesting(const std::wstring& dir, float dpi);
  // The slide fitted in the view, client pixels (for tests and the snapshot scripts).
  D2D1_RECT_F SlideRectForTesting() const { return SlideRect(); }

 private:
  // ---- Layout (device pixels in the client area)
  void LayoutParts();
  D2D1_RECT_F SlideRect() const;          // the slide on screen at the zoom and scroll
  float FitScale() const;                 // pixels per point that fits the slide in the view
  float Scale() const;                    // pixels per point now
  void ClampScroll();
  void SetZoom(double zoom);              // 0: fit to window; else 1 = 100 % (96 dpi)
  std::vector<ThumbRow> Rows() const;     // the thumbnails, scrolled into the pane
  void ScrollThumbIntoView(size_t slide);
  int ThumbAt(int x, int y) const;        // -1 when none

  // ---- Drawing
  void DrawThumbnails(gfx::Canvas& c, ID2D1DeviceContext* dc);
  void DrawSlide(gfx::Canvas& c, ID2D1DeviceContext* dc);
  void DrawNotes(gfx::Canvas& c);
  ID2D1Bitmap* ThumbBitmap(ID2D1DeviceContext* dc, size_t slide, bool make);
  // The slide drawn into a bitmap `w` x `h` pixels (null when it couldn't be).
  Microsoft::WRL::ComPtr<ID2D1Bitmap1> RenderBitmap(ID2D1DeviceContext* dc, size_t slide, UINT w, UINT h);

  // ---- Commands (PresentationCommands.cpp)
  void StartShow(bool fromCurrent);
  void CopySlide(bool text, bool picture);
  void CopyNotes();
  void FollowLink(const SlideLink& link);
  const SlideLink* LinkAt(int x, int y) const;
  std::wstring SlideText(size_t slide) const;
  void ShowMatch();
  void ReadOnlyBanner();

  // ---- Printing (PresentationPrint.cpp)
  std::vector<size_t> PrintedSlides() const;
  std::vector<PrintPage> Pages(draw::Size slidePaper, draw::Size paper) const;
  text::Fonts& PageFonts();
  PrintOutcome PrintTo(const std::wstring& printer, const DEVMODEW* devmode, const std::vector<int>& pages,
                       IStream* output, const std::function<void(int, int)>& progress, std::wstring& message);

  ViewHost& host_;
  std::wstring path_;
  std::unique_ptr<Deck> deck_;
  std::unique_ptr<text::Fonts> fonts_;  // printing's and Export PDF's text (page numbers, notes)
  size_t current_ = 0;
  bool showPending_ = false;  // a .ppsx: the show starts once the tab is drawn
  bool bannerShown_ = false;

  // Parts.
  RECT vp_{};
  float ui_ = 1;
  D2D1_RECT_F thumbs_{}, view_{}, splitter_{}, notes_{};
  float thumbWidth_ = 196;    // DIPs
  float notesHeight_ = 96;    // DIPs, when shown
  bool notesShown_ = true;
  float thumbScroll_ = 0;     // pixels the list is scrolled up
  double zoom_ = 0;           // 0: fit to window
  float sx_ = 0, sy_ = 0;     // the zoomed slide's scroll, pixels
  float notesScroll_ = 0;

  // Thumbnails, drawn at idle and kept while their size stays.
  struct Thumb {
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> bitmap;
  };
  std::vector<Thumb> thumbCache_;
  UINT thumbW_ = 0, thumbH_ = 0;
  // The current slide as shown, kept while the slide, size and zoom stay.
  Microsoft::WRL::ComPtr<ID2D1Bitmap1> slideBitmap_;
  size_t slideBitmapOf_ = SIZE_MAX;
  UINT slideBitmapW_ = 0, slideBitmapH_ = 0;

  // Mouse.
  enum class Drag { None, Splitter, ThumbBar, Pan };
  Drag drag_ = Drag::None;
  int downX_ = 0, downY_ = 0;
  float downValue_ = 0, downValue2_ = 0;
  int hoverThumb_ = -1;
  bool keysInThumbs_ = false;  // arrow keys move the thumbnails' selection (after a click there)

  // Find.
  struct Match {
    size_t slide = 0;
    bool notes = false;
    size_t block = 0;   // the text block, for slide text
    size_t at = 0, length = 0;
  };
  std::vector<Match> matches_;
  int match_ = -1;

  // The current slide's links, read when the mouse first asks.
  mutable std::vector<SlideLink> links_;
  mutable size_t linksOf_ = SIZE_MAX;

  PrintSettings print_;
};

}  // namespace fw::slide::view
