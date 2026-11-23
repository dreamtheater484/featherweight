#pragma once

#include <windows.h>

#include <d2d1_1.h>
#include <wrl/client.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "TabContent.h"
#include "slide/view/Deck.h"
#include "slide/view/SlideLayout.h"
#include "slide/view/SlideDesign.h"
#include "slide/view/SlideHeaderFooter.h"
#include "slide/view/SlideOrder.h"

namespace fw::text {
class Fonts;
}
namespace fw::slide::show {
class SlideRenderer;
}
namespace fw::slide::edit {
class Editor;
struct Changes;
struct Result;
}
class PresentationFile;

namespace fw::slide::view {

class TextEditing;
class ObjectTool;
class NotesEdit;
class SlideGallery;

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
  // A new, untitled presentation ("Presentation1"): PowerPoint's blank 16:9 deck (P2).
  static std::unique_ptr<PresentationView> NewPresentation(ViewHost& host, int number);
  // A presentation from memory: a recovered one (`unsaved`), titled `title`, Save As starting at
  // `suggested` (P2, p2-model).
  static std::unique_ptr<PresentationView> FromBytes(ViewHost& host, std::vector<uint8_t> bytes, std::wstring title,
                                                     std::wstring suggested, bool unsaved);

  // ---- TabContent ----
  TabKind Kind() const override { return TabKind::Presentation; }
  OpenResult Load(const std::wstring& password, std::wstring& error) override;
  bool Ready() const override { return deck_ != nullptr; }
  const std::wstring& Path() const override { return path_; }
  std::wstring Title() const override;
  bool Modified() const override;

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
  bool Dragging() const override;
  bool HasKeyFocus() const override;
  std::optional<RECT> CaretRect() override;

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
  // Replace (p2-text): in the text of slides and notes, through the edit layer.
  bool CanReplace() const override;
  bool ReplaceCurrent(const std::wstring& with) override;
  int ReplaceAll(const std::wstring& with) override;

  // Saving (P2, p2-model: PresentationSave.cpp, through PresentationFile): Ctrl+S and Save As write
  // on a worker thread; closing saves at once.
  bool Save(const std::wstring& path, std::wstring& error) override;
  SaveStart StartSave(const std::wstring& path, std::wstring& error) override;
  void SaveDone() override;
  SaveFormat SaveAsFormat() const override;
  std::vector<SaveFormat> OtherSaveFormats() const override;
  std::wstring SuggestedSavePath() const override;
  void Autosave() override;
  bool ReplayEdits(std::string_view records, std::wstring& error) override;
  FileChange CheckFileOnDisk() override;
  bool CanPrint() const override { return Ready() && deck_->SlideCount() > 0; }
  PrintOutcome Print(HWND owner, const std::function<void(int, int)>& progress, std::wstring& message) override;
  bool CanExportPdf() const override { return CanPrint(); }
  bool ExportPdf(const std::wstring& path, std::wstring& message) override;
  FileState State() const override;
  void ApplyState(const FileState& s) override;
  bool ScriptOp(const std::wstring& op, const std::wstring& arg) override;
  bool TakesPictures(const std::vector<std::wstring>& files) const override;
  void DropPictures(const std::vector<std::wstring>& files, int x, int y) override;

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
  // The pages as they print and export, each a PNG (page-1.png...) in `dir` at `dpi`; how many were
  // written. A page that fails is reported on stderr, counted in `failed` and skipped.
  int RenderPagesForTesting(const std::wstring& dir, float dpi, int* failed = nullptr);
  // The slide fitted in the view, client pixels (for tests and the snapshot scripts).
  D2D1_RECT_F SlideRectForTesting() const { return SlideRect(); }

 private:
  // Text being edited (p2-text): TextEditing takes clicks, keys and commands first while it edits.
  TextEditing* Text() { return text_.get(); }
  draw::Point SlidePoint(int x, int y) const;  // client pixels -> slide points

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
  // `editing`: as the editor shows it (the text being typed left out, empty placeholders' prompts).
  Microsoft::WRL::ComPtr<ID2D1Bitmap1> RenderBitmap(ID2D1DeviceContext* dc, size_t slide, UINT w, UINT h, bool editing = false);
  // Anything drawn in slide points into such a bitmap (P2: the layout gallery's pictures).
  Microsoft::WRL::ComPtr<ID2D1Bitmap1> RenderBitmapWith(ID2D1DeviceContext* dc, UINT w, UINT h,
                                                        const std::function<void(draw::Painter&)>& paint);

  // ---- Commands (PresentationCommands.cpp)
  void StartShow(bool fromCurrent);
  void CopySlide(bool text, bool picture);
  void CopyNotes();
  void FollowLink(const SlideLink& link);
  const SlideLink* LinkAt(int x, int y) const;
  std::wstring SlideText(size_t slide) const;
  void ShowMatch();
  void ReadOnlyBanner();

  // ---- Objects on the slide (P2, p2-objects: ObjectTool)
  ObjectTool& Objects() const;
  // Whether the objects get a command: not the clipboard or select all while the keys are in the thumbnails.
  bool ObjectsTake(int cmd) const;
  // ---- Slides (P2, p2-slides: PresentationSlides.cpp): the thumbnails as an editor
  // A click on thumbnail `t` with the modifier keys; true when it was handled.
  void ThumbsMouseDown(int t, UINT mods, int x, int y);
  void ThumbsDragMove(int x, int y);
  void ThumbsDragEnd(bool drop);
  bool ThumbsKeyDown(UINT vk, UINT mods);
  bool SlideScriptOp(const std::wstring& op, const std::wstring& arg);
  void DrawThumbSelection(gfx::Canvas& c, const std::vector<ThumbRow>& rows, size_t i);
  void DrawDropMarker(gfx::Canvas& c);
  int SlideShortcut(UINT vk, UINT mods) const;
  bool SlideCommandState(int cmd, CommandState& s);
  bool SlideCommand(int cmd);
  void SlideContextMenu(HMENU menu, int thumb);
  void AddSlideTools(ToolbarModel& m);
  std::vector<size_t> SelectedSlides() const;
  // Edits through the edit layer (each one undo step). False, with a banner saying why, when the
  // deck can't be edited.
  bool CanEditSlides() const;
  // The deck's editor, which the tab listens to from the first time it's asked for (null: none).
  edit::Editor* SlideEditor() const;
  // Right after the tab first gets the deck's Editor (at load), before any edit: p2-model's
  // PresentationFile hooks the recovery journal in here (SetJournal).
  void OnEditorMade(edit::Editor& ed);
  void OnSlideEdits(const edit::Changes& c);
  bool Done(const edit::Result& r) const;  // a banner saying why when it was refused
  void SelectSlides(const std::vector<size_t>& slides, size_t current);
  int NewSlideLayout() const;  // Ctrl+M's layout: the current slide's (after a title slide, the next one)
  // Notes typed in the notes pane (plain text, NotesEdit): a click there starts it; leaving the
  // pane, Esc, another slide or another tab ends it, and the text goes to the slide's notes.
  bool NotesMouseDown();
  void EndNotesEdit();
  // The notes being typed go to the slide now, the box staying open (before a save or autosave).
  void CommitNotes();
  void PlaceNotesEdit(const D2D1_RECT_F& box);
  std::unique_ptr<NotesEdit> notesEdit_;
  size_t notesSlide_ = 0;
  mutable bool editListening_ = false;
  mutable std::vector<uint32_t> editIds_;  // the slides' ids as the last change left them
  bool InsertSlide(size_t at, int layout);
  bool DuplicateSlides(const std::vector<size_t>& slides);
  bool DeleteSlides(const std::vector<size_t>& slides);
  bool MoveSlides(const std::vector<size_t>& slides, size_t gap);
  bool HideSlides(const std::vector<size_t>& slides, bool hide);
  bool SetSlidesLayout(const std::vector<size_t>& slides, int layout);
  // Copying slides between presentation tabs: the selected slides to the clipboard (cut: then
  // deleted), and the slides copied pasted after the current slide, in this deck's design or
  // keeping their own.
  bool CopySlides(bool cut);
  bool PasteSlides(bool keepSource);
  bool SlidesOnClipboard() const;  // slides copied here are still what the clipboard holds
  // Design (PresentationDesign.cpp): a built-in theme or theme colors on the masters of `slides`
  // (every master when empty), a background on `slides` (every slide with `all`), slide numbers,
  // footer and date, the slide size, the notes.
  bool ApplyTheme(int theme, const std::vector<size_t>& slides);
  bool ApplyColors(int scheme, const std::vector<size_t>& slides);
  bool SetBackground(const std::vector<size_t>& slides, const Background& bg, const std::wstring& picture);
  bool ResetBackground(const std::vector<size_t>& slides);
  bool ApplyHeaderFooter(const HeaderFooterSettings& s, const std::vector<size_t>& slides);
  bool SetSlideSize(int choice, int scale);
  bool SetNotes(size_t slide, const std::wstring& text);
  // The Design and Header & footer panels (PresentationPanels.h).
  class DesignPanel;
  class HeaderFooterPanel;
  std::unique_ptr<DesignPanel> designPanel_;
  std::unique_ptr<HeaderFooterPanel> hfPanel_;
  void OpenDesign(int tab);
  void OpenHeaderFooter();
  bool DesignCommandState(int cmd, CommandState& s);
  bool DesignCommand(int cmd);
  void DesignChanged();  // after an edit or an undo: open panels show the deck as it is now
  // After an edit or an undo: the caches follow the slides (by id), the edited ones redrawn.
  void SlidesChanged(const std::vector<uint32_t>& idsBefore, const std::vector<size_t>& touched, bool all);
  std::vector<uint32_t> SlideIds() const;

  // ---- Printing (PresentationPrint.cpp)
  std::vector<size_t> PrintedSlides() const;
  std::vector<PrintPage> Pages(draw::Size slidePaper, draw::Size paper) const;
  text::Fonts& PageFonts();
  PrintOutcome PrintTo(const std::wstring& printer, const DEVMODEW* devmode, const std::vector<int>& pages,
                       IStream* output, const std::function<void(int, int)>& progress, std::wstring& message);

  ViewHost& host_;
  std::wstring path_;
  std::wstring untitled_;  // a new presentation's name, until it's saved (P2)
  bool newDeck_ = false;
  std::unique_ptr<Deck> deck_;
  // The slide show (p1-show): its renderer, the monitor chosen (-1: the window's), and whether a
  // show this tab started is running (the tab outlives it, or stops it when closed).
  std::unique_ptr<fw::slide::show::SlideRenderer> showRenderer_;
  int showMonitor_ = -1;
  bool showing_ = false;
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

  // The selected slides (P2): the thumbnails' selection, the current slide always in it.
  SlideSelection selection_;
  // A drag of the selected thumbnails: armed on the press, moving once the pointer has gone a few
  // pixels; `dropGap_` is the gap it would drop into (SIZE_MAX: none shown).
  bool thumbPress_ = false;
  size_t dropGap_ = SIZE_MAX;
  float dragY_ = 0;
  int pendingCollapse_ = -1;  // a plain click on a slide of a multi-selection: it alone, unless it drags
  // ＋ Slide ▾ and Layout ▾ (their pictures: the deck's layouts as P1's renderer draws them, made
  // when a gallery opens and kept while the deck and the picture size stay).
  std::unique_ptr<SlideGallery> newSlideGallery_, layoutGallery_;
  std::vector<Deck::LayoutChoice> galleryLayouts_;
  std::vector<Microsoft::WRL::ComPtr<ID2D1Bitmap1>> layoutPictures_;  // by the presentation's layout index
  UINT layoutPictureW_ = 0;
  void PaintLayoutPicture(gfx::Canvas& c, size_t layout, const D2D1_RECT_F& r);
  void PrepareGalleries();

  // Mouse.
  enum class Drag { None, Splitter, ThumbBar, Pan, Text, Slides };
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
  std::unique_ptr<TextEditing> text_;  // p2-text
  struct {
    std::wstring query;
    bool matchCase = false, wholeWords = false;
  } search_;  // what StartSearch looked for, for Replace

  mutable std::unique_ptr<ObjectTool> objects_;  // made on first use

  // ---- The file (P2, p2-model: PresentationSave.cpp)
  PresentationFile& File();
  // After the deck opened: what the tab is to its file (opened, new, recovered), the in-use banner.
  void FileOpened();
  // The deck's Editor records into the file's recovery journal and tells it each change (before
  // any edit; once).
  void AttachFile(edit::Editor& ed);
  bool ReadyToSave(std::wstring& error);
  void SavedTo();
  std::vector<uint8_t> bytes_;  // FromBytes: the deck, until it's loaded
  bool fromBytes_ = false;
  bool fileAttached_ = false;
  // Last, so it goes first: a save still writing finishes before the deck and its Editor go.
  std::unique_ptr<PresentationFile> file_;
};

}  // namespace fw::slide::view
