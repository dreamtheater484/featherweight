#pragma once

#include <string>

#include "slide/view/TextSession.h"

// Slide text on the clipboard (P2, p2-text; plan §6.3 "Clipboard", which slides follow): copying
// puts plain text, RTF and HTML there (the documents' converters, doc::edit::Clip), and inside the
// HTML Featherweight's own copy, the DrawingML paragraphs themselves, so text pasted into a slide
// keeps everything it had. Pasting takes Featherweight's own copy first, then RTF (Word), then HTML
// (browsers), then plain text. Text from elsewhere takes the look of where it goes, keeping its
// emphasis (bold, italic, underline, strikethrough, raised and lowered) and web links, as Word's
// "Merge formatting" does.
namespace fw::slide::view {

struct TextClipboard {
  std::wstring text;  // CR LF between paragraphs
  std::string rtf, html;
};

// The selection's text for the clipboard; empty with no selection.
TextClipboard CopySelection(TextSession& s, TextModel& model);

// Pastes over the selection, as one undo step ("Paste"). `textOnly`: the text alone, in the look
// of where it goes (Paste as text). False when the clipboard had nothing to paste.
bool Paste(TextSession& s, TextModel& model, const std::string& html, const std::string& rtf, const std::wstring& text,
           bool textOnly);

}  // namespace fw::slide::view
