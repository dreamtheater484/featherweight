// Fuzz target: inserting a picture into a Word document (src/doc/edit/Pictures.cpp, milestone D3),
// and the changes made to it afterwards. The input is the picture file: PNG, JPEG, GIF, BMP, TIFF,
// SVG, or anything else. It's inserted into a new document with the app's limits, changed in ways
// a hash of the input picks (size, rotation, crop, wrap, place, alt text, reset, a second copy),
// saved as the app saves, read back, and every change undone and redone.
// Seeds: tests/fuzz/seeds/picture, a tiny picture in each format (make_seeds.py).
//
// It aborts when:
// - a refused insert changed the document or left an undo step;
// - an inserted picture isn't a picture the tools change, or has no size;
// - the saved file doesn't open, has package errors, or doesn't have the pictures shown;
// - a picture read back isn't one the tools change;
// - undoing everything doesn't give the new document back (no picture, no media part), or redoing
//   it doesn't give the pictures back.
#include <windows.h>
// After windows.h:
#include <objbase.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "doc/docx/Blank.h"
#include "doc/docx/Docx.h"
#include "doc/docx/Save.h"
#include "doc/edit/Editor.h"
#include "doc/edit/Pictures.h"
#include "pkg/Check.h"
#include "pkg/Package.h"
#include "pkg/Snapshot.h"

using namespace fw::doc;
using fw::Emu;

namespace {

void Check(bool ok) {
  if (!ok) std::abort();
}

// FW_FUZZ_TRACE=1 prints each change and how it went, for replaying a failure.
bool Trace() {
  static const bool on = GetEnvironmentVariableW(L"FW_FUZZ_TRACE", nullptr, 0) > 0;
  return on;
}

std::unique_ptr<Document> Open(std::vector<uint8_t> bytes) {
  fw::pkg::Status st;
  auto pkg = fw::pkg::Package::Open(fw::pkg::Snapshot::FromBytes(std::move(bytes)), fw::pkg::Limits{}, st);
  return pkg ? docx::Read(std::move(pkg), st) : nullptr;
}

const std::vector<uint8_t>& BlankBytes() {
  static const std::vector<uint8_t> bytes = docx::BlankDocx({"en-US", false, "Fuzz", 1});
  return bytes;
}

size_t LiveParts(const fw::pkg::Package& p) {
  size_t n = 0;
  for (const fw::pkg::Part& part : p.Parts()) n += part.deleted ? 0 : 1;
  return n;
}

// The drawings in the text, by index into Document::Drawings().
std::vector<uint32_t> Shown(const Document& doc) {
  std::vector<uint32_t> out;
  for (uint32_t i = 0; i < doc.Drawings().size(); ++i)
    if (doc.Drawings()[i].pos.para != kNone) out.push_back(i);
  return out;
}

// Choices from the input's hash (FNV-1a, then a mixer for each draw).
struct Choices {
  uint64_t h = 1469598103934665603ull;
  Choices(const uint8_t* data, size_t size) {
    for (size_t i = 0; i < size; ++i) h = (h ^ data[i]) * 1099511628211ull;
  }
  uint32_t Next(uint32_t n) {
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33;
    return static_cast<uint32_t>(h % n);
  }
  double Fraction() { return Next(10001) / 10000.0; }
};

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  static const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  (void)com;
  auto doc = Open(BlankBytes());
  Check(doc != nullptr);
  edit::Editor ed(*doc);
  edit::Pictures pics(ed);
  const size_t parts = LiveParts(doc->Package());
  const std::vector<uint8_t> bytes(data, data + size);
  Choices pick(data, size);

  // In a 6.5-inch column, or at its own size.
  edit::Result r = pics.Insert(Pos{0, 0}, bytes, Emu(pick.Next(2) ? 5943600 : 0), u"fuzz.png");
  if (!r) {
    Check(!r.message.empty() && !ed.CanUndo() && LiveParts(doc->Package()) == parts && Shown(*doc).empty());
    return 0;
  }
  Check(Shown(*doc).size() == 1);
  {
    const edit::PictureInfo info = pics.Info(Shown(*doc)[0]);
    Check(info.picture && info.cx.Value() > 0 && info.cy.Value() > 0 && info.wrap == edit::Wrap::Inline);
  }

  // Changes; each is made or refused with a reason.
  for (int k = 0; k < 6; ++k) {
    const std::vector<uint32_t> shown = Shown(*doc);
    Check(!shown.empty());
    const uint32_t d = shown[pick.Next(static_cast<uint32_t>(shown.size()))];
    const uint32_t op = pick.Next(9);
    switch (op) {
      case 0: r = pics.Resize(d, Emu(pick.Next(30000000)), Emu(pick.Next(30000000))); break;
      case 1: r = pics.Rotate(d, pick.Fraction() * 1440 - 720); break;
      case 2:
        r = pics.SetCrop(d, {pick.Fraction() * 1.4 - 0.4, pick.Fraction() * 1.4 - 0.4, pick.Fraction() - 0.2,
                             pick.Fraction() - 0.2});
        break;
      case 3:
        r = pics.SetWrap(d, static_cast<edit::Wrap>(pick.Next(6)), Emu(pick.Next(9000000)), Emu(pick.Next(11000000)));
        break;
      case 4:
        if (pics.Info(d).wrap == edit::Wrap::Inline) r = pics.MoveInline(d, Pos{0, pick.Next(3)});
        else r = pics.MoveFloating(d, Emu(pick.Next(9000000)), Emu(pick.Next(11000000)), Pos{0, 0});
        break;
      case 5: r = pics.SetAltText(d, pick.Next(2) ? u"A <b>&amp; \"quoted\" 'alt' texté" : u""); break;
      case 6: r = pics.ResetSize(d); break;
      case 7: r = pics.Insert(Pos{0, 0}, bytes); break;  // the same picture again: it shares the part
      case 8:  // undo, though not the first picture
        r = ed.Undo();
        if (Shown(*doc).empty()) r = ed.Redo();
        break;
    }
    Check(r.ok || !r.message.empty());
    if (Trace()) std::fprintf(stderr, "op %u on %u: %s, %zu shown\n", op, d, r.ok ? "made" : "refused", Shown(*doc).size());
  }

  // What was undone, redone; then saved as the app saves, and read back.
  while (ed.CanRedo()) Check(ed.Redo().ok);
  const size_t shownNow = Shown(*doc).size();
  docx::SaveOptions o;
  o.changedParts = ed.DirtyParts();
  o.edited = true;
  o.now = 1;
  std::vector<uint8_t> out;
  Check(docx::Serialize(*doc, o, out).Ok());
  {
    auto back = Open(out);
    Check(back != nullptr);
    Check(fw::pkg::CheckPackage(back->Package()).Ok());
    Check(Shown(*back).size() == shownNow);
    edit::Editor ed2(*back);
    edit::Pictures pics2(ed2);
    for (uint32_t d : Shown(*back)) Check(pics2.Info(d).picture);
  }

  // Undone: the new document; redone: the pictures again.
  while (ed.CanUndo()) Check(ed.Undo().ok);
  Check(Shown(*doc).empty() && LiveParts(doc->Package()) == parts);
  while (ed.CanRedo()) Check(ed.Redo().ok);
  Check(Shown(*doc).size() == shownNow);
  return 0;
}
