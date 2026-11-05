// fwtool paste-check: copy and paste with formatting (src/doc/edit/Clip.h) on real documents, for
// the never-damage bar. Each Word document's main story is copied as RTF and as HTML and read back;
// the HTML (Featherweight's own) is pasted over the whole story, the RTF at its end, and every
// clipboard sample in --samples (what Word, LibreOffice and browsers copy) at its start. Each result
// is saved and must pass check when the original does; one undo must restore the story. With
// --out, the saved copies go to DIR/html, DIR/rtf and DIR/samples, named as the originals, for
// fwvalidate (tools/validate).
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "Common.h"
#include "Reference.h"
#include "core/Strings.h"
#include "doc/docx/Docx.h"
#include "doc/edit/Clip.h"
#include "doc/edit/Editor.h"
#include "pkg/Check.h"
#include "pkg/Snapshot.h"

namespace fwtool {
namespace {

using namespace fw::doc;
using Clock = std::chrono::steady_clock;

double Ms(Clock::time_point since) {
  return std::chrono::duration<double, std::milli>(Clock::now() - since).count();
}

std::unique_ptr<Document> OpenDoc(const std::filesystem::path& file, bool& checked, std::string& error) {
  fw::pkg::Status st;
  auto pkg = OpenPackage(file, st);
  if (!pkg) {
    error = st.message;
    return nullptr;
  }
  checked = fw::pkg::CheckPackage(*pkg).Ok();
  auto doc = docx::Read(std::move(pkg), st);
  if (!doc) error = st.message;
  return doc;
}

std::string MainXml(const Document& doc) {
  const Part* main = doc.FindPart(PartKind::Document);
  std::string out;
  if (main) docx::WritePart(doc, static_cast<uint32_t>(main - doc.Parts().data()), out);
  return out;
}

// Saves the edited document as the Word tab does, checks it, and writes it to `out` (if given).
// Empty when it's fine.
std::string SaveAndCheck(Document& doc, const edit::Editor& ed, bool checked, const std::filesystem::path& out) {
  fw::pkg::Package& pkg = doc.Package();
  for (uint32_t part : ed.DirtyParts()) {
    std::string xml;
    if (!docx::WritePart(doc, part, xml).Ok()) return "a changed part couldn't be written";
    pkg.SetPart(doc.Parts()[part].name, std::vector<uint8_t>(xml.begin(), xml.end()));
  }
  fw::pkg::MemorySink sink;
  if (!pkg.Save(sink).Ok()) return "couldn't be saved";
  if (!out.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(out.parent_path(), ec);
    std::ofstream(out, std::ios::binary).write(reinterpret_cast<const char*>(sink.bytes.data()),
                                               static_cast<std::streamsize>(sink.bytes.size()));
  }
  fw::pkg::Status st;
  auto back = fw::pkg::Package::Open(fw::pkg::Snapshot::FromBytes(std::move(sink.bytes)), fw::pkg::Limits::Default(), st);
  if (!back) return "the saved copy doesn't open: " + st.message;
  if (checked) {
    const fw::pkg::CheckReport rep = fw::pkg::CheckPackage(*back);
    if (!rep.Ok()) return "the saved copy fails check: " + rep.errors.front();
  }
  auto reread = docx::Read(std::move(back), st);
  if (!reread) return "the saved copy isn't read: " + st.message;
  const auto a = doc.StoryParagraphs(doc.MainStory()), b = reread->StoryParagraphs(reread->MainStory());
  if (a.size() != b.size()) return "the saved copy has other paragraphs";
  for (size_t i = 0; i < a.size(); ++i)
    if (doc.Para(a[i]).text != reread->Para(b[i]).text) return "a paragraph reads back differently";
  return {};
}

std::string ReadFile(const std::filesystem::path& file) {
  std::ifstream in(file, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), {});
}

}  // namespace

int RunPasteCheck(const Options& options) {
  std::vector<std::filesystem::path> files;
  for (const auto& f : CollectFiles(options.inputs))
    if (IsWordDocument(fw::WideToUtf8(f.filename().wstring()))) files.push_back(f);
  if (files.empty()) {
    std::fprintf(stderr, "No Word documents found.\n");
    return 1;
  }
  std::vector<std::pair<std::string, std::string>> samples;  // name, bytes
  if (!options.samples.empty())
    for (const auto& e : std::filesystem::directory_iterator(options.samples)) {
      const std::wstring ext = e.path().extension().wstring();
      if (ext == L".rtf" || ext == L".html") samples.emplace_back(fw::WideToUtf8(e.path().filename().wstring()), ReadFile(e.path()));
    }
  std::printf("Copying each main story as RTF and HTML, pasting it back, and pasting %zu clipboard samples.\n\n",
              samples.size());
  int failed = 0;
  for (const auto& file : files) {
    const std::string name = Display(file);
    std::vector<std::string> problems;
    auto out = [&](const char* variant) {
      return options.outDir.empty() ? std::filesystem::path() : options.outDir / variant / file.filename();
    };
    bool checked = false;
    std::string error;
    auto doc = OpenDoc(file, checked, error);
    if (!doc) {
      std::printf("%s: not read (%s), skipped\n", name.c_str(), error.c_str());
      continue;
    }
    const auto paras = doc->StoryParagraphs(doc->MainStory());
    if (paras.empty()) continue;
    const Pos from{paras.front(), 0}, to{paras.back(), static_cast<uint32_t>(doc->Para(paras.back()).text.size())};

    auto t = Clock::now();
    const edit::Clip clip = edit::CopyClip(*doc, from, to, 1);
    const std::string rtf = edit::ToRtf(clip), html = edit::ToHtml(clip);
    const double copyMs = Ms(t);
    t = Clock::now();
    const auto fromRtf = edit::FromRtf(rtf);
    const auto fromHtml = edit::FromHtml(html);
    const double readMs = Ms(t);
    if (!fromRtf || !fromHtml) problems.push_back(std::string("its copy doesn't read back as ") + (!fromRtf ? "RTF" : "HTML"));

    // One paste: saved, checked and undone. A refusal (what the editor can't delete yet) isn't damage.
    std::vector<std::string> notes;
    auto paste = [&](Document& d, Pos a, Pos b, const edit::Clip& c, uint64_t target, const char* what,
                     const char* variant) -> double {
      edit::Editor ed(d);
      const std::string before = MainXml(d);
      const auto start = Clock::now();
      const edit::Result r = edit::PasteClip(ed, a, b, c, target);
      const double ms = Ms(start);
      if (!r.ok) {
        notes.push_back(std::string(what) + " was refused: " + fw::WideToUtf8(std::wstring(r.message.begin(), r.message.end())));
        if (MainXml(d) != before) problems.push_back(std::string(what) + ": the refused paste changed the document");
        return -1;
      }
      if (std::string e = SaveAndCheck(d, ed, checked, out(variant)); !e.empty()) problems.push_back(std::string(what) + ": " + e);
      // (An empty copy, of a story that's only a drawing, changes nothing.)
      if (MainXml(d) != before && !(ed.Undo().ok && MainXml(d) == before))
        problems.push_back(std::string(what) + ": undo didn't restore it");
      return ms;
    };
    double htmlMs = 0, rtfMs = 0;
    if (fromHtml) {
      htmlMs = paste(*doc, from, to, *fromHtml, 1, "its HTML over itself", "html");
      if (htmlMs < 0) htmlMs = paste(*doc, to, to, *fromHtml, 1, "its HTML at its end", "html");
    }
    if (fromRtf) {
      auto again = OpenDoc(file, checked, error);
      const Pos end{to.para, static_cast<uint32_t>(again->Para(to.para).text.size())};
      rtfMs = paste(*again, end, end, *fromRtf, 0, "its RTF at its end", "rtf");
    }
    double textMs = 0;  // the same as plain text, as Paste Special pastes it
    {
      auto again = OpenDoc(file, checked, error);
      edit::Editor ed(*again);
      const Pos end{to.para, static_cast<uint32_t>(again->Para(to.para).text.size())};
      const auto start = Clock::now();
      ed.InsertText(end, clip.PlainText());
      textMs = Ms(start);
    }
    if (!samples.empty()) {
      auto again = OpenDoc(file, checked, error);
      edit::Editor ed(*again);
      for (const auto& [sample, bytes] : samples) {
        const auto c = sample.ends_with(".rtf") ? edit::FromRtf(bytes) : edit::FromHtml(bytes);
        if (!c) {
          problems.push_back(sample + " doesn't read");
          continue;
        }
        const auto start = Clock::now();
        const edit::Result r = edit::PasteClip(ed, {paras.front(), 0}, {paras.front(), 0}, *c, 0);
        if (const double ms = Ms(start); ms > 100) notes.push_back(sample + " took " + std::to_string(static_cast<int>(ms)) + " ms");
        if (!r.ok) notes.push_back(sample + " was refused: " + fw::WideToUtf8(std::wstring(r.message.begin(), r.message.end())));
      }
      if (std::string e = SaveAndCheck(*again, ed, checked, out("samples")); !e.empty()) problems.push_back("the samples: " + e);
    }
    std::printf("%s %s: %zu paragraphs, copied in %.0f ms (%zu KB RTF, %zu KB HTML), read in %.0f ms, pasted in %.0f ms "
                "(HTML), %.0f ms (RTF), %.0f ms (text)%s\n",
                problems.empty() ? "[ ok ]" : "[FAIL]", name.c_str(), clip.paras.size(), copyMs, rtf.size() >> 10,
                html.size() >> 10, readMs, htmlMs, rtfMs, textMs, checked ? "" : " (the original fails check)");
    for (const auto& n : notes) std::printf("    note: %s\n", n.c_str());
    for (const auto& p : problems) std::printf("    %s\n", p.c_str());
    if (!problems.empty()) ++failed;
    std::fflush(stdout);
  }
  std::printf("\n%zu documents, %d failed\n", files.size(), failed);
  return failed ? 1 : 0;
}

}  // namespace fwtool
