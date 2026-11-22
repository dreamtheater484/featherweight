#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "pkg/Package.h"
#include "slide/Presentation.h"
#include "slide/edit/Parts.h"

// Saving an edited presentation (plan §4.3 rules 1 and 5; P2, p2-model). Untouched parts are copied
// from the file as opened, byte for byte, without decompressing them; the parts edits changed are
// written from their DOMs; parts the edits left unused (a deleted slide's, a picture nothing shows
// any more) are left out; nothing else changes. A deck saved without edits is the file it was.
//
// In three steps, so the writing runs on a worker thread while editing goes on (published parts never
// change, so the plan holds a consistent snapshot for as long as it needs):
//   1. UI thread:  SavePlan plan; PrepareSave(editor, plan, format);
//   2. any thread: WriteSave(plan, sink)   (then FileSession::WriteSave keeps the backup and replaces
//                  the file safely);
//   3. UI thread, once the bytes are in the file: CommitSave(editor, plan).
namespace fw::slide::edit {

class Editor;

struct SavePlan {
  const pkg::Package* package = nullptr;
  std::map<std::string, PartState> parts;  // by PartKey: the store's parts as they were at PrepareSave
  std::map<std::string, RelsPtr> rels;
  TypesPtr types;
  std::string presentationPart;
  std::string mainType;  // the presentation part's content type to write (format change); empty: as it is
  uint64_t state = 0;    // the editor's state saved (Editor::StateId)
  // After WriteSave: the parts it wrote anew and those it left out (the rest were copied).
  std::vector<std::string> written, leftOut;
};

// The content type of the presentation part for a format.
std::string_view MainContentType(FileFormat format);

// Takes the snapshot. Fails when the format can't be written (macros into .pptx: Save As decides).
pkg::Status PrepareSave(const Editor& editor, SavePlan& plan, FileFormat format);
pkg::Status WriteSave(SavePlan& plan, pkg::ByteSink& sink);
// The deck now stands for the file written: Modified() is false unless it was edited meanwhile.
void CommitSave(Editor& editor, const SavePlan& plan);

// All three at once into memory (tools, tests, the torture gate): `written` gets the parts written
// anew or left out (every other part is copied byte for byte).
pkg::Status SaveToBytes(Editor& editor, std::vector<uint8_t>& out, std::vector<std::string>* written = nullptr);

}  // namespace fw::slide::edit
