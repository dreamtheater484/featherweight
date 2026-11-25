// fwtool pdf-torture (F1, f1-gate): the never-damage test for the PDF page tools. Each PDF gets R
// rounds of N seeded random operations: rotate, delete, move, insert pages from another PDF (merge),
// a drawn stamp (as a drawn signature is placed), extract and split into new files. Each round's
// result is saved (whole, or incrementally every other round), opened again, and checked page by
// page against what the operations should have made: the page count, and each page's text, size,
// rotation and number of annotations, as it was in the PDF it came from. Every file written must
// also pass pdf-valid's structure check. Saving without edits is tried once per file (an
// incremental save with nothing changed) and whether it gives the same bytes is recorded.
//
// The operations go through PDFium's own functions (fpdf_ppo.h, fpdf_edit.h, fpdf_save.h), which the
// page tools use (docs/f1-plan.md). Until f1-pdf's src/pdf/tools API is there, they are called here
// directly; then this calls that API instead, with OCR as one more operation.
#include <windows.h>

#include <fpdf_annot.h>
#include <fpdf_edit.h>
#include <fpdf_ppo.h>
#include <fpdf_save.h>
#include <fpdf_signature.h>
#include <fpdf_text.h>
#include <fpdfview.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "Common.h"
#include "Reference.h"
#include "core/Strings.h"

namespace fs = std::filesystem;

namespace fwtool {
namespace {

struct PageSig {
  size_t textHash = 0;
  std::string textStart;  // the first characters, for messages
  int rot = 0;            // 0-3, quarter turns
  float w = 0, h = 0;     // unrotated
  int annots = 0;
};

struct Doc {
  fs::path path;
  std::string bytes;  // PDFium reads from here while the document is open
  std::string password;
  FPDF_DOCUMENT doc = nullptr;
  std::vector<PageSig> sigs;
  ~Doc() {
    if (doc) FPDF_CloseDocument(doc);
  }
};

std::string ReadAll(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

PageSig SigOf(FPDF_DOCUMENT doc, int index) {
  PageSig s;
  FPDF_PAGE page = FPDF_LoadPage(doc, index);
  if (!page) return s;
  s.rot = FPDFPage_GetRotation(page);
  float w = FPDF_GetPageWidthF(page), h = FPDF_GetPageHeightF(page);
  if (s.rot % 2) std::swap(w, h);
  s.w = w, s.h = h;
  s.annots = FPDFPage_GetAnnotCount(page);
  if (FPDF_TEXTPAGE text = FPDFText_LoadPage(page)) {
    const int n = FPDFText_CountChars(text);
    if (n > 0) {
      std::vector<unsigned short> buf(static_cast<size_t>(n) + 1);
      const int got = FPDFText_GetText(text, 0, n, buf.data());
      std::wstring t(reinterpret_cast<const wchar_t*>(buf.data()), got > 0 ? static_cast<size_t>(got - 1) : 0);
      // Its characters, whitespace aside and in any order: PDFium reads a page's text in another order
      // once the page is turned or its content written again (a stamp), and spaces runs differently.
      // A character lost, added or changed still shows.
      std::wstring squeezed;
      for (wchar_t c : t)
        if (!iswspace(c)) squeezed += c;
      std::sort(squeezed.begin(), squeezed.end());
      s.textHash = std::hash<std::wstring>()(squeezed);
      s.textStart = fw::WideToUtf8(t.substr(0, 40));
      for (char& c : s.textStart)
        if (c == '\r' || c == '\n') c = ' ';
    }
    FPDFText_ClosePage(text);
  }
  FPDF_ClosePage(page);
  return s;
}

// Opens a PDF, trying no password first and then each of `passwords`.
std::unique_ptr<Doc> Open(const fs::path& path, std::string bytes, const std::vector<std::string>& passwords, std::string& error) {
  auto d = std::make_unique<Doc>();
  d->path = path;
  d->bytes = std::move(bytes);
  std::vector<std::string> tries{""};
  tries.insert(tries.end(), passwords.begin(), passwords.end());
  for (const auto& pw : tries) {
    d->doc = FPDF_LoadMemDocument64(d->bytes.data(), d->bytes.size(), pw.empty() ? nullptr : pw.c_str());
    if (d->doc) {
      d->password = pw;
      break;
    }
    if (FPDF_GetLastError() != FPDF_ERR_PASSWORD) break;
  }
  if (!d->doc) {
    const unsigned long e = FPDF_GetLastError();
    error = e == FPDF_ERR_PASSWORD ? "needs a password not given" : e == FPDF_ERR_SECURITY ? "unsupported security" : "doesn't open";
    return nullptr;
  }
  const int n = FPDF_GetPageCount(d->doc);
  for (int i = 0; i < n; ++i) d->sigs.push_back(SigOf(d->doc, i));
  return d;
}

struct Writer : FPDF_FILEWRITE {
  std::string out;
  Writer() {
    version = 1;
    WriteBlock = [](FPDF_FILEWRITE* self, const void* data, unsigned long size) -> int {
      static_cast<Writer*>(self)->out.append(static_cast<const char*>(data), size);
      return 1;
    };
  }
};

std::string Save(FPDF_DOCUMENT doc, bool incremental, bool& ok) {
  Writer w;
  ok = FPDF_SaveAsCopy(doc, &w, incremental ? FPDF_INCREMENTAL : FPDF_NO_INCREMENTAL) != 0;
  return w.out;
}

// Where a page of the result comes from.
struct PageRef {
  const Doc* src = nullptr;
  int index = 0;
  int rot = 0;
};

struct Failure {
  std::string category, message;
};

// The saved bytes opened again and checked against what the pages should be.
void Verify(const std::string& bytes, const std::string& password, const std::vector<PageRef>& expect, const std::string& what,
            std::vector<Failure>& failures, int inputStructureErrors) {
  for (const auto& e : PdfStructureErrors(bytes)) {
    // A damaged original saved incrementally keeps its damaged table: the app rewrites such files whole.
    failures.push_back({"structure", what + ": " + e + (inputStructureErrors ? " (the original's structure was damaged too)" : "")});
    break;
  }
  FPDF_DOCUMENT doc = FPDF_LoadMemDocument64(bytes.data(), bytes.size(), password.empty() ? nullptr : password.c_str());
  if (!doc) {
    failures.push_back({"reopen", what + ": PDFium can't open it (error " + std::to_string(FPDF_GetLastError()) + ")"});
    return;
  }
  const int n = FPDF_GetPageCount(doc);
  if (n != static_cast<int>(expect.size())) {
    failures.push_back({"page count", what + ": " + std::to_string(n) + " pages, " + std::to_string(expect.size()) + " expected"});
  } else {
    for (int i = 0; i < n; ++i) {
      const PageSig got = SigOf(doc, i);
      const PageSig& want = expect[i].src->sigs[static_cast<size_t>(expect[i].index)];
      const std::string where = what + ", page " + std::to_string(i + 1) + " (" + Display(expect[i].src->path) + " page " +
                                std::to_string(expect[i].index + 1) + ")";
      if (got.textHash != want.textHash) failures.push_back({"text", where + ": \"" + got.textStart + "\", expected \"" + want.textStart + "\""});
      if (got.rot != expect[i].rot) failures.push_back({"rotation", where + ": " + std::to_string(got.rot * 90) + " degrees, expected " + std::to_string(expect[i].rot * 90)});
      if (std::fabs(got.w - want.w) > 0.5f || std::fabs(got.h - want.h) > 0.5f)
        failures.push_back({"size", where + ": " + std::to_string(static_cast<int>(got.w)) + " x " + std::to_string(static_cast<int>(got.h)) +
                                        ", expected " + std::to_string(static_cast<int>(want.w)) + " x " + std::to_string(static_cast<int>(want.h))});
      if (got.annots != want.annots) failures.push_back({"annotations", where + ": " + std::to_string(got.annots) + " annotations, expected " + std::to_string(want.annots)});
    }
  }
  FPDF_CloseDocument(doc);
}

// A drawn stamp, as a drawn signature is placed: a small picture of a scribble, on the page's content.
bool Stamp(FPDF_DOCUMENT doc, int index, std::mt19937& rng) {
  FPDF_PAGE page = FPDF_LoadPage(doc, index);
  if (!page) return false;
  const int bw = 120, bh = 48;
  FPDF_BITMAP bmp = FPDFBitmap_Create(bw, bh, 1);
  FPDFBitmap_FillRect(bmp, 0, 0, bw, bh, 0x00FFFFFF);
  int y = bh / 2;
  for (int x = 4; x < bw - 4; ++x) {
    y = std::clamp(y + static_cast<int>(rng() % 5) - 2, 6, bh - 6);
    FPDFBitmap_FillRect(bmp, x, y, 2, 2, 0xFF1A237E);
  }
  FPDF_PAGEOBJECT img = FPDFPageObj_NewImageObj(doc);
  bool ok = img && FPDFImageObj_SetBitmap(&page, 1, img, bmp);
  if (ok) {
    const float pw = FPDF_GetPageWidthF(page), ph = FPDF_GetPageHeightF(page);
    FPDFImageObj_SetMatrix(img, 120, 0, 0, 48, std::max(10.f, pw - 160), 30.f + (ph > 200 ? 20.f : 0.f));
    FPDFPage_InsertObject(page, img);
    ok = FPDFPage_GenerateContent(page) != 0;
  } else if (img) {
    FPDFPageObj_Destroy(img);
  }
  FPDFBitmap_Destroy(bmp);
  FPDF_ClosePage(page);
  return ok;
}

// The operation a random number 0-99 picks (the shares are the loop's in RunPdfTorture).
std::string KindOf(int pick) {
  return pick < 22 ? "rotate" : pick < 38 ? "delete" : pick < 58 ? "move" : pick < 76 ? "insert" : pick < 86 ? "stamp" : pick < 94 ? "extract" : "split";
}

std::string Json(const std::string& s) {
  std::string out = "\"";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') out += '\\', out += static_cast<char>(c);
    else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); out += b; }
    else out += static_cast<char>(c);
  }
  return out + "\"";
}

void Write(const fs::path& p, const std::string& bytes) {
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  std::ofstream out(p, std::ios::binary);
  out << bytes;
}

}  // namespace

int RunPdfTorture(const Options& options) {
  std::string error;
  if (!InitPdfium(error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 1;
  }
  const fs::path out = options.outDir.empty() ? fs::path("build") / "pdf-torture" : options.outDir;
  std::vector<std::string> passwords;
  for (size_t a = 0, s = 0; a <= options.password.size(); ++a)
    if (a == options.password.size() || options.password[a] == L',') {
      if (a > s) passwords.push_back(fw::WideToUtf8(options.password.substr(s, a - s)));
      s = a + 1;
    }
  std::vector<fs::path> files;
  auto add = [&](const fs::path& p) {
    std::error_code ec;
    if (fs::is_directory(p, ec)) {
      for (const auto& e : fs::recursive_directory_iterator(p, ec))
        if (e.is_regular_file() && fw::ToLower(e.path().extension().wstring()) == L".pdf") files.push_back(e.path());
    } else files.push_back(p);
  };
  for (const auto& p : options.inputs) add(p);
  if (!options.samples.empty()) {
    std::ifstream list(options.samples, std::ios::binary);
    std::string line;
    while (std::getline(list, line)) {
      while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
      if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF) line.erase(0, 3);
      if (!line.empty()) add(fw::Utf8ToWide(line));
    }
  }
  std::sort(files.begin(), files.end());
  files.erase(std::unique(files.begin(), files.end()), files.end());

  // Every input open at once: the others are where merged pages come from.
  std::vector<std::unique_ptr<Doc>> docs;
  std::vector<std::pair<fs::path, std::string>> skipped;
  for (const auto& f : files) {
    std::string why;
    auto d = Open(f, ReadAll(f), passwords, why);
    if (!d) skipped.push_back({f, why});
    else if (d->sigs.empty()) skipped.push_back({f, "has no pages"});
    else docs.push_back(std::move(d));
  }
  std::vector<const Doc*> donors;
  for (const auto& d : docs)
    if (d->password.empty() && d->sigs.size() <= 400) donors.push_back(d.get());

  const unsigned seed = options.seed ? options.seed : 1;
  const int edits = std::max(1, options.edits);
  const uint32_t rounds = std::max<uint32_t>(1, options.rounds);
  std::set<std::string> allowed;
  for (size_t a = 0, s = 0; a <= options.ops.size(); ++a)
    if (a == options.ops.size() || options.ops[a] == ',') {
      if (a > s) allowed.insert(options.ops.substr(s, a - s));
      s = a + 1;
    }
  long long totalOps = 0, totalFailures = 0, written = 0, uneditedSame = 0, uneditedTried = 0;
  std::string json = "{\n  \"format\": \"featherweight-pdf-torture\",\n  \"through\": \"pdfium\",\n  \"seed\": " + std::to_string(seed) +
                     ",\n  \"edits\": " + std::to_string(edits) + ",\n  \"rounds\": " + std::to_string(rounds) + ",\n  \"files\": [";
  bool firstFile = true;
  for (const auto& sk : skipped) {
    std::printf("[SKIP] %s: %s\n", Display(sk.first).c_str(), sk.second.c_str());
    json += std::string(firstFile ? "\n" : ",\n") + "    {\"file\": " + Json(fw::WideToUtf8(fs::absolute(sk.first).wstring())) +
            ", \"skipped\": " + Json(sk.second) + "}";
    firstFile = false;
  }
  for (const auto& d : docs) {
    const std::string name = fw::WideToUtf8(d->path.stem().wstring());
    const int inputErrors = static_cast<int>(PdfStructureErrors(d->bytes).size());
    std::vector<Failure> failures;
    std::string roundsJson;
    int fileOps = 0;
    // Saved without edits: an incremental save with nothing changed.
    bool uneditedIdentical = false;
    {
      bool ok = false;
      std::string again = Save(d->doc, true, ok);
      ++uneditedTried;
      uneditedIdentical = ok && again == d->bytes;
      if (uneditedIdentical) ++uneditedSame;
    }
    for (uint32_t r = 0; r < rounds; ++r) {
      const size_t failedBefore = failures.size();
      std::mt19937 rng(seed * 2654435761u ^ static_cast<unsigned>(std::hash<std::string>()(name)) ^ (r * 7919u));
      std::string why;
      auto work = Open(d->path, d->bytes, passwords, why);  // each round from the original
      if (!work) { failures.push_back({"open", "round " + std::to_string(r + 1) + ": " + why}); continue; }
      std::vector<PageRef> model;
      for (int i = 0; i < static_cast<int>(work->sigs.size()); ++i) model.push_back({d.get(), i, d->sigs[static_cast<size_t>(i)].rot});
      std::vector<std::string> ops;
      int extracts = 0;
      const std::string tag = name + "-r" + std::to_string(r + 1);
      for (int e = 0; e < edits; ++e) {
        const int n = static_cast<int>(model.size());
        // --ops: only these kinds (rotate, delete, move, insert, stamp, extract, split), to find which one fails.
        int pick = static_cast<int>(rng() % 100);
        for (int tries = 0; !allowed.empty() && !allowed.count(KindOf(pick)) && tries < 200; ++tries) pick = static_cast<int>(rng() % 100);
        if (!allowed.empty() && !allowed.count(KindOf(pick))) break;
        auto randomPage = [&] { return static_cast<int>(rng() % static_cast<unsigned>(n)); };
        if (pick < 22) {  // rotate
          const int i = randomPage(), q = 1 + static_cast<int>(rng() % 3);
          FPDF_PAGE page = FPDF_LoadPage(work->doc, i);
          if (!page) { failures.push_back({"operation", tag + ": page " + std::to_string(i + 1) + " doesn't load to rotate"}); continue; }
          FPDFPage_SetRotation(page, (FPDFPage_GetRotation(page) + q) % 4);
          FPDF_ClosePage(page);
          model[static_cast<size_t>(i)].rot = (model[static_cast<size_t>(i)].rot + q) % 4;
          ops.push_back("rotate " + std::to_string(i + 1) + " by " + std::to_string(q * 90));
        } else if (pick < 38) {  // delete
          if (n < 2) continue;
          const int i = randomPage();
          FPDFPage_Delete(work->doc, i);
          model.erase(model.begin() + i);
          ops.push_back("delete " + std::to_string(i + 1));
        } else if (pick < 58) {  // move
          if (n < 2) continue;
          const int k = 1 + static_cast<int>(rng() % static_cast<unsigned>(std::min(3, n - 1)));
          std::vector<int> idx;
          while (static_cast<int>(idx.size()) < k) {
            const int i = randomPage();
            if (std::find(idx.begin(), idx.end(), i) == idx.end()) idx.push_back(i);
          }
          const int dest = static_cast<int>(rng() % static_cast<unsigned>(n - k + 1));
          if (!FPDF_MovePages(work->doc, idx.data(), static_cast<unsigned long>(idx.size()), dest)) {
            failures.push_back({"operation", tag + ": FPDF_MovePages refused a valid move"});
            continue;
          }
          std::vector<PageRef> moved, rest;
          for (int i : idx) moved.push_back(model[static_cast<size_t>(i)]);
          for (int i = 0; i < n; ++i)
            if (std::find(idx.begin(), idx.end(), i) == idx.end()) rest.push_back(model[static_cast<size_t>(i)]);
          rest.insert(rest.begin() + dest, moved.begin(), moved.end());
          model = rest;
          std::string list;
          for (int i : idx) list += (list.empty() ? "" : ",") + std::to_string(i + 1);
          ops.push_back("move " + list + " to " + std::to_string(dest + 1));
        } else if (pick < 76) {  // insert pages from another PDF (merge)
          if (donors.empty() || n > 600) continue;
          const Doc* from = donors[rng() % donors.size()];
          const int dn = static_cast<int>(from->sigs.size());
          const int k = 1 + static_cast<int>(rng() % static_cast<unsigned>(std::min(3, dn)));
          std::vector<int> idx;
          for (int j = 0; j < k; ++j) idx.push_back(static_cast<int>(rng() % static_cast<unsigned>(dn)));
          const int at = static_cast<int>(rng() % static_cast<unsigned>(n + 1));
          if (!FPDF_ImportPagesByIndex(work->doc, from->doc, idx.data(), static_cast<unsigned long>(idx.size()), at)) {
            failures.push_back({"operation", tag + ": importing pages from " + Display(from->path) + " failed"});
            continue;
          }
          for (int j = 0; j < k; ++j) model.insert(model.begin() + at + j, PageRef{from, idx[static_cast<size_t>(j)], from->sigs[static_cast<size_t>(idx[static_cast<size_t>(j)])].rot});
          ops.push_back("insert " + std::to_string(k) + " pages of " + fw::WideToUtf8(from->path.filename().wstring()) + " at " + std::to_string(at + 1));
        } else if (pick < 86) {  // a drawn stamp
          const int i = randomPage();
          if (!Stamp(work->doc, i, rng)) failures.push_back({"operation", tag + ": the stamp on page " + std::to_string(i + 1) + " failed"});
          ops.push_back("stamp " + std::to_string(i + 1));
        } else {  // extract some pages, or split the whole into parts, into new files
          const bool split = pick >= 94 && n >= 2;
          std::vector<std::vector<int>> parts;
          if (split) {
            const int size = 1 + static_cast<int>(rng() % static_cast<unsigned>(std::max(1, n / 2)));
            for (int s = 0; s < n; s += size) {
              std::vector<int> part;
              for (int i = s; i < std::min(n, s + size); ++i) part.push_back(i);
              parts.push_back(part);
            }
          } else {
            std::vector<int> part;
            const int k = 1 + static_cast<int>(rng() % static_cast<unsigned>(std::min(4, n)));
            for (int j = 0; j < k; ++j) part.push_back(randomPage());
            parts.push_back(part);
          }
          for (const auto& part : parts) {
            FPDF_DOCUMENT nd = FPDF_CreateNewDocument();
            bool ok = FPDF_ImportPagesByIndex(nd, work->doc, part.data(), static_cast<unsigned long>(part.size()), 0) != 0;
            std::string bytes = ok ? Save(nd, false, ok) : std::string();
            FPDF_CloseDocument(nd);
            const std::string label = tag + (split ? "-split" : "-extract") + std::to_string(++extracts);
            if (!ok) { failures.push_back({"operation", label + ": the new file couldn't be made"}); continue; }
            Write(out / "extracted" / (label + ".pdf"), bytes);
            ++written;
            std::vector<PageRef> want;
            for (int i : part) want.push_back(model[static_cast<size_t>(i)]);
            Verify(bytes, "", want, label, failures, 0);
          }
          ops.push_back(std::string(split ? "split into " : "extract into ") + std::to_string(parts.size()) + " files");
        }
        ++fileOps;
      }
      // Saved whole, or incrementally every other round, then opened again and checked.
      // A signed PDF only ever incrementally: saved whole, its signature's /ByteRange no longer frames the
      // bytes it signed (Acrobat saves signed files incrementally too, and shows the changes after signing).
      const bool incremental = r % 2 == 1 || FPDF_GetSignatureCount(d->doc) > 0;
      bool ok = false;
      std::string bytes = Save(work->doc, incremental, ok);
      const std::string label = tag + (incremental ? "-incremental" : "");
      if (!ok) failures.push_back({"save", label + ": FPDF_SaveAsCopy failed"});
      else {
        Write(out / "saved" / (label + ".pdf"), bytes);
        ++written;
        Verify(bytes, work->password, model, label, failures, inputErrors);
      }
      std::string list;
      for (const auto& o : ops) list += (list.empty() ? "" : ", ") + Json(o);
      roundsJson += std::string(roundsJson.empty() ? "" : ", ") + "{\"round\": " + std::to_string(r + 1) + ", \"incremental\": " +
                    (incremental ? "true" : "false") + ", \"pages\": " + std::to_string(model.size()) + ", \"ops\": [" + list + "]}";
      // A reproducer: the operations of a round that failed.
      if (failures.size() > failedBefore) {
        std::string text = "file: " + fw::WideToUtf8(fs::absolute(d->path).wstring()) + "\nseed: " + std::to_string(seed) + "\nround: " +
                           std::to_string(r + 1) + "\n";
        for (const auto& o : ops) text += o + "\n";
        Write(out / "repro" / (tag + ".txt"), text);
      }
    }
    totalOps += fileOps;
    totalFailures += static_cast<long long>(failures.size());
    std::printf("[%s] %s: %d operations in %u rounds%s%s\n", failures.empty() ? "OK" : "FAIL", Display(d->path).c_str(), fileOps, rounds,
                failures.empty() ? "" : ": ", failures.empty() ? "" : (failures.front().category + ": " + failures.front().message).c_str());
    for (size_t k = 1; k < failures.size() && k < 4; ++k) std::printf("    %s: %s\n", failures[k].category.c_str(), failures[k].message.c_str());
    std::string fj;
    for (const auto& f : failures) fj += std::string(fj.empty() ? "" : ", ") + "{\"category\": " + Json(f.category) + ", \"message\": " + Json(f.message) + "}";
    json += std::string(firstFile ? "\n" : ",\n") + "    {\"file\": " + Json(fw::WideToUtf8(fs::absolute(d->path).wstring())) +
            ", \"pages\": " + std::to_string(d->sigs.size()) + ", \"password\": " + (d->password.empty() ? "false" : "true") +
            ", \"inputStructureErrors\": " + std::to_string(inputErrors) + ", \"uneditedIdentical\": " + (uneditedIdentical ? "true" : "false") +
            ",\n     \"rounds\": [" + roundsJson + "],\n     \"failures\": [" + fj + "]}";
    firstFile = false;
  }
  json += "\n  ],\n  \"totals\": {\"files\": " + std::to_string(docs.size()) + ", \"skipped\": " + std::to_string(skipped.size()) +
          ", \"operations\": " + std::to_string(totalOps) + ", \"failures\": " + std::to_string(totalFailures) + ", \"written\": " +
          std::to_string(written) + ", \"uneditedIdentical\": " + std::to_string(uneditedSame) + ", \"uneditedTried\": " +
          std::to_string(uneditedTried) + "}\n}\n";
  std::printf("%zu PDFs (%zu skipped), %lld operations, %lld files written, %lld failures; saved without edits: %lld of %lld the same bytes\n",
              docs.size(), skipped.size(), totalOps, written, totalFailures, uneditedSame, uneditedTried);
  if (!options.jsonOut.empty()) Write(options.jsonOut, json);
  return totalFailures ? 1 : 0;
}

}  // namespace fwtool
