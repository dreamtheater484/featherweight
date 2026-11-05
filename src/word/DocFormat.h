#pragma once

#include <windows.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "TabContent.h"
#include "doc/edit/Editor.h"
#include "doc/edit/Format.h"
#include "doc/edit/Styles.h"
#include "ui/Toolbar.h"

// Formatting in the Word tab (plan §5.2, §6.2): the toolbar's formatting groups, Word's keys,
// command search, the format painter, Insert date and time, and the status bar's language. Each
// command is one undo step of the tab's editor; the tab lays out again when the editor says so.
class DocFormat {
 public:
  // What a command applies to: character formatting to `chars` (the selection, or the word the
  // caret is in), the rest to `sel`. `any` is false when there's neither caret nor selection.
  struct Target {
    fw::doc::edit::Range chars, sel;
    bool any = false;
  };
  // After a command: where the caret goes (Insert date), or why nothing happened.
  struct Done {
    std::optional<fw::doc::Pos> caret;
    std::wstring message;
  };

  DocFormat(fw::doc::edit::Editor& editor, std::function<Target()> target, std::function<void(const Done&)> done);

  // Word's keys (Ctrl+B, Ctrl+Shift+>, Ctrl+1 for single spacing...), or 0. In a document being
  // edited they come before the viewer's own.
  static int Shortcut(UINT vk, UINT mods);
  static bool Owns(int cmd);
  // The commands command search offers.
  static std::vector<int> CommandIds();

  CommandState Query(int cmd);
  void Execute(int cmd);
  // The toolbar's editing groups, after Save and Print.
  void AddToolbar(ToolbarModel& m);

  // The format painter: its button takes the selection's format, and the next selection made
  // gets it (Esc cancels). Ctrl+Shift+C and Ctrl+Shift+V copy and paste a format the same way.
  bool PainterOn() const { return painter_ != Painter::Off; }
  void SelectionMade();
  void CancelPainter() { painter_ = Painter::Off; }
  bool Carrying() const { return carried_.has; }  // a format copied (Ctrl+Shift+C)

  // AutoFormat As You Type: "1. ", "a) " or "* " typed at the start of a paragraph (the caret just
  // after the space or tab) makes it a list item, as one undo step of its own, so Ctrl+Z gives the
  // typed text back. False when the text isn't one of those.
  bool AutoFormat(fw::doc::Pos caret);

  // The installed fonts for the font box, read once in idle time (a few milliseconds).
  static bool FontsPending();
  static void ReadFonts();

 private:
  enum class Painter : uint8_t { Off, Once };
  struct Format {  // what the painter carries
    fw::doc::edit::RunFormat run;
    fw::doc::edit::ParaFormat para;
    std::string style;
    bool has = false;
  };

  const fw::doc::Document& Doc() const { return editor_.Doc(); }
  // The state of the target, cached until it or the document changes (the toolbar asks each frame).
  const fw::doc::edit::FormatState& State();
  const std::vector<fw::doc::edit::StyleEntry>& Gallery();
  bool Editable() const { return editor_.CanEdit(); }
  // Runs one change on the current target, as one undo step.
  void Run(const std::function<void(fw::doc::edit::TreeEdit&, const Target&)>& change);
  void ApplyStyle(const std::wstring& nameOrId);
  void ApplyFont(const std::wstring& name);
  void ApplySize(double points);
  void ApplyColor(D2D1_COLOR_F color, bool automatic, bool highlight);
  void InsertDateTime(size_t index);
  Format Capture();
  void PaintTo(const Format& f, const Target& t);
  // Date and time formats for the document's language, with today's samples (Insert ▾).
  void DateFormats(std::vector<std::wstring>& pictures, std::vector<std::wstring>& samples);

  fw::doc::edit::Editor& editor_;
  std::function<Target()> target_;
  std::function<void(const Done&)> done_;
  uint32_t version_ = 0;  // bumped by every change the editor reports

  // Caches.
  fw::doc::edit::FormatState state_;
  fw::doc::edit::Range stateRange_{};
  uint32_t stateVersion_ = UINT32_MAX;
  std::vector<fw::doc::edit::StyleEntry> gallery_;
  uint32_t galleryVersion_ = UINT32_MAX;

  D2D1_COLOR_F fontColor_{0.753f, 0.0f, 0.0f, 1.0f};  // Word's dark red
  D2D1_COLOR_F highlight_{1.0f, 1.0f, 0.0f, 1.0f};    // yellow
  Painter painter_ = Painter::Off;
  Format carried_;
};
