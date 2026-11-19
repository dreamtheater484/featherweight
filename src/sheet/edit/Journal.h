// Crash recovery from the edit journal (plan §4.3 rule 6): every edit an Editor makes, as bytes
// that make it again on the same workbook opened afresh.
//
//   edit::Journal journal_;                  // one per workbook tab, next to its Editor
//   editor.SetJournal(&journal_);            // at open, before the first edit
//   // every few seconds while there are unsaved edits (the tab's autosave tick):
//   if (file.WriteJournal(<the file's bytes as opened>, meta, journal_.All(), journal_.Unwritten()))
//     journal_.Written();
//   // after a crash: pkg::recovery::ReadJournal; open the workbook from its document, attach the
//   // engine (OnChange) as for editing and a new journal, then editor.Replay(records).
//
// Records are written when an edit succeeds, by the outermost Editor call only (an edit made of
// others records once). Formats added (AddXf...) always record: they find or append, so replaying
// one twice changes nothing. View changes in a row (selection, scrolling, zoom) keep only the last.
// What isn't an edit isn't recorded: values the engine calculates (it recalculates while
// replaying), saving, the locale (SetLocale: replay with the same one).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "sheet/edit/Edit.h"

namespace fw::sheet::edit {

// The records' kinds. Numbers never change and are never reused: journals outlive the exe that
// wrote them (the updater can replace it between a crash and the next start).
enum class JournalOp : uint8_t {
  Version = 1,
  SetCells = 2,
  ClearContents = 3,
  MapStyles = 4,
  AddXf = 5,
  AddFont = 6,
  AddFill = 7,
  AddBorder = 8,
  AddNumFmt = 9,
  SetRowHeight = 10,
  HideRows = 11,
  SetRowOutline = 12,
  SetColumnWidth = 13,
  HideColumns = 14,
  SetColumnOutline = 15,
  ShiftCells = 16,
  SortLines = 17,
  Merge = 18,
  Unmerge = 19,
  AddSheet = 20,
  RenameSheet = 21,
  MoveSheet = 22,
  DeleteSheet = 23,
  SetTabColor = 24,
  SetSheetState = 25,
  FreezePanes = 26,
  SetActiveSheet = 27,
  SetSelection = 28,
  SetTopLeft = 29,
  SetZoom = 30,
  SetName = 31,
  DeleteName = 32,
  BeginGroup = 33,
  EndGroup = 34,
  CancelGroup = 35,
  Undo = 36,
  Redo = 37,
  DropHistory = 38,  // a save that removed sheets ended the undo history
  AddCellStyle = 39,
  SetFilter = 40,
  MoveCells = 41,
  InsertChart = 50,  // s3-charts: 50-59
  MoveObject = 51,
  DeleteObjects = 52,
  EditChart = 53,
  // s3-data's operations: 60-69.
  SetSheetRules = 60,
  AddDxf = 61,
  AddTable = 62,
  SetTable = 63,
  RemoveTable = 64,
  SetTableFilter = 65,
  InsertPicture = 66,
  InsertShape = 67,
  SetObjectXml = 68,
  // S3: each piece numbers its own (s3-review from 90), so branches don't clash.
  SetSheetProtection = 90,
  SetWorkbookProtection = 91,
  SetHyperlink = 92,
  SetNote = 93,
  SetThread = 94,
  SetPageSetup = 95,
  SetPageBreaks = 96,
  SetViewMode = 97,
};

// One record being written: its kind, then its fields in the order Replay reads them.
class JournalRecord {
 public:
  explicit JournalRecord(JournalOp op) : op_(op) {}
  JournalOp Op() const { return op_; }
  const std::string& Bytes() const { return bytes_; }

  JournalRecord& U(uint64_t v);
  JournalRecord& I(int64_t v);
  JournalRecord& D(double v);
  JournalRecord& B(bool v) { return U(v ? 1 : 0); }
  JournalRecord& S(std::string_view s);
  JournalRecord& S16(std::u16string_view s);
  JournalRecord& R(const Range& r) { return U(r.firstRow).U(r.firstCol).U(r.lastRow).U(r.lastCol); }
  JournalRecord& A(CellAddr a) { return U(a.row).U(a.col); }
  JournalRecord& C(const Color& c);
  JournalRecord& Input(const CellInput& in);
  JournalRecord& Filter(const std::optional<SheetFilter>& f);
  // A view change: replaces the record just before it when that one has the same kind and key.
  JournalRecord& Key(int key) {
    key_ = key;
    return *this;
  }
  int CoalesceKey() const { return key_; }

 private:
  JournalOp op_;
  std::string bytes_;
  int key_ = -1;
};

// Every record since it started (the first: the journal's version). A save removes the recovery
// file, so the next one starts again from the file as opened and all of them.
class Journal {
 public:
  static constexpr uint32_t kVersion = 1;  // Replay reads every earlier version

  Journal();
  std::string_view All() const { return bytes_; }
  // The records the recovery file doesn't have yet, whole; Written() once they're appended.
  std::string_view Unwritten() const { return std::string_view(bytes_).substr(written_); }
  void Written() { written_ = bytes_.size(); }
  size_t Size() const { return bytes_.size(); }

  void Add(const JournalRecord& r);

 private:
  std::string bytes_;
  size_t written_ = 0;
  size_t lastAt_ = std::string::npos;  // where the last record starts in bytes_, for coalescing
  JournalOp lastOp_ = JournalOp::Version;
  int lastKey_ = -1;
};

}  // namespace fw::sheet::edit
