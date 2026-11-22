// Crash recovery for presentations (plan §4.3 rule 6, §13; P2, p2-model): the edits since the deck
// was opened, as records that bring the deck opened afresh to the same state.
//
//   edit::Journal journal_;                 // one per presentation tab, next to its Editor
//   editor.SetJournal(&journal_);           // at open, before the first edit
//   // every few seconds while there are unsaved edits (the tab's autosave tick):
//   if (journal_.Due() && file.WriteJournal(<the file's bytes as opened>, "pptx",
//                                           journal_.All(editor), journal_.Unwritten(editor)))
//     journal_.Written();
//   // after a crash: pkg::recovery::ReadJournal, open the deck from its document, then
//   // edit::Replay(editor, records) (one undo step, "Recover").
//
// What's recorded is the parts as the edits left them, not the operations: replaying needs no
// knowledge of what each edit did, so any later version replays an older journal, and every edit,
// whatever made it, is covered. Each write records only the parts changed since the last one, as
// they are then (typing in a slide for a minute writes that slide a few times, not each keystroke).
// The records are encrypted by pkg::recovery (DPAPI) like the workbooks' and documents'.
#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <string_view>

#include "slide/edit/Edit.h"

namespace fw::slide::edit {

class PartStore;

// Record kinds. Numbers never change and are never reused (journals outlive the exe).
enum class JournalOp : uint8_t {
  Version = 1,       // u32 version
  Part = 2,          // name, bytes: the part's content now
  PartRemoved = 3,   // name
  PartOriginal = 4,  // name: back to the file's own
  Rels = 5,          // source, the relationships part's XML
  RelsOriginal = 6,  // source
  Types = 7,         // [Content_Types].xml
  TypesOriginal = 8,
  Saved = 9,         // the edits so far were saved (the file has them): nothing to do on replay
};

class Journal {
 public:
  static constexpr uint32_t kVersion = 1;  // Replay reads every earlier version

  // The editor tells the journal which parts its edits changed (Editor::SetJournal).
  void Changed(const PartStore& store, const std::set<std::string>& keys);
  bool Due() const { return !dirty_.empty() || typesDirty_; }

  // Every part the edits changed since opening, as it is now (the recovery file starts with it).
  std::string All(const Editor& editor) const;
  // The parts changed since the last Written(), as they are now.
  std::string Unwritten(const Editor& editor) const;
  void Written();
  // After a save: the file is the deck as saved, so the next journal starts from it.
  void Saved();

 private:
  std::string Records(const PartStore& store, const std::set<std::string>& keys, bool types) const;
  std::set<std::string> dirty_;   // PartKeys
  std::set<std::string> ever_;    // every PartKey changed since opening (or the last save)
  bool typesDirty_ = false, typesEver_ = false;
};

// Applies a journal's records to `editor`'s deck (opened from the journal's document) as one undo
// step. Stops at the first record it can't read (the ones before it stay applied); `applied` counts
// the records applied.
Result Replay(Editor& editor, std::string_view records, size_t* applied = nullptr);

}  // namespace fw::slide::edit
