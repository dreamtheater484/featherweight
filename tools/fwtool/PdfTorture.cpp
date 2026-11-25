// fwtool pdf-torture (F1, f1-gate): the never-damage test for the PDF page tools. Each PDF gets R
// rounds of N seeded random operations: rotate, delete, move, insert pages from another PDF (merge),
// a drawn stamp (as a drawn signature is placed), extract and split into new files. Each round's
// result is saved (whole, or incrementally every other round), opened again, and checked page by
// page against what the operations should have made: the page count, and each page's text, size,
// rotation and number of annotations, as it was in the PDF it came from. Every file written must
// also pass pdf-valid's structure check. Saving without edits is tried once per file (an
// incremental save with nothing changed) and whether it gives the same bytes is recorded.
//
// Built with f1-pdf's fwpdftools, the operations go through pdftools::Editor, the PDF tab's own
// functions and rules, with OCR (Recognize text) as one more operation and, with --pfx, certificate
// signatures every third round (signed, saved, changed, saved again). PDFs the tools must refuse
// (XFA, protected) are checked for that: every tool refuses, a save is the file as it was. Without
// fwpdftools (f1-int before f1-pdf's merge) the operations call PDFium's functions directly.
#include <windows.h>
#include <wincrypt.h>

#include <fpdf_annot.h>
#include <fpdf_edit.h>
#include <fpdf_formfill.h>
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
#ifdef FW_HAVE_PDFTOOLS
#include "pdf/tools/Editor.h"
#endif

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
  int addedAnnots = 0;              // drawn signatures (stamp annotations) and signature fields put on it
  std::shared_ptr<PageSig> text;    // its text since OCR wrote a text layer on it (else its source's)
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
      const PageSig& wantText = expect[i].text ? *expect[i].text : want;
      const int wantAnnots = want.annots + expect[i].addedAnnots;
      const std::string where = what + ", page " + std::to_string(i + 1) + " (" + Display(expect[i].src->path) + " page " +
                                std::to_string(expect[i].index + 1) + ")";
      if (got.textHash != wantText.textHash) failures.push_back({"text", where + ": \"" + got.textStart + "\", expected \"" + wantText.textStart + "\""});
      if (got.rot != expect[i].rot) failures.push_back({"rotation", where + ": " + std::to_string(got.rot * 90) + " degrees, expected " + std::to_string(expect[i].rot * 90)});
      if (std::fabs(got.w - want.w) > 0.5f || std::fabs(got.h - want.h) > 0.5f)
        failures.push_back({"size", where + ": " + std::to_string(static_cast<int>(got.w)) + " x " + std::to_string(static_cast<int>(got.h)) +
                                        ", expected " + std::to_string(static_cast<int>(want.w)) + " x " + std::to_string(static_cast<int>(want.h))});
      if (got.annots != wantAnnots) failures.push_back({"annotations", where + ": " + std::to_string(got.annots) + " annotations, expected " + std::to_string(wantAnnots)});
    }
  }
  FPDF_CloseDocument(doc);
}

// A drawn stamp, as a drawn signature was placed before f1-pdf's tools: a small picture of a scribble,
// on the page's content.
bool RawStamp(FPDF_DOCUMENT doc, int index, std::mt19937& rng) {
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
  return pick < 20 ? "rotate" : pick < 34 ? "delete" : pick < 52 ? "move" : pick < 68 ? "insert" : pick < 78 ? "stamp"
       : pick < 84 ? "ocr" : pick < 92 ? "extract" : "split";
}

// Where the first of `sorted` ends up when they're moved to the gap before page `gap` (0..count of
// the document as it is), counted here apart from the tools' own pdftools::MoveDestination.
int MoveDest(const std::vector<int>& sorted, int gap) {
  int before = 0;
  for (int p : sorted)
    if (p < gap) ++before;
  return gap - before;
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

// The document a round edits. Built with f1-pdf's fwpdftools, through pdftools::Editor: the PDF
// tab's own functions and rules (moves pin inherited attributes, a drawn signature is a stamp
// annotation, an edited PDF is saved whole and a signed one incrementally, through the safe save).
// Without it, through PDFium's functions directly, saved whole or incrementally every other round.
class Work {
 public:
  bool Open(const std::string& bytes, const std::vector<std::string>& passwords, std::string& error) {
    std::vector<std::string> tries{""};
    tries.insert(tries.end(), passwords.begin(), passwords.end());
#ifdef FW_HAVE_PDFTOOLS
    for (const auto& pw : tries) {
      const pdftools::SourceError e = ed_.Open(bytes, pw);
      if (e == pdftools::SourceError::None) { password_ = pw; return true; }
      if (e != pdftools::SourceError::Password) { error = e == pdftools::SourceError::NotPdf ? "isn't a PDF, or is damaged" : "doesn't open"; return false; }
    }
    error = "needs a password not given";
    return false;
#else
    bytes_ = bytes;
    for (const auto& pw : tries) {
      doc_ = FPDF_LoadMemDocument64(bytes_.data(), bytes_.size(), pw.empty() ? nullptr : pw.c_str());
      if (doc_) { password_ = pw; return true; }
      if (FPDF_GetLastError() != FPDF_ERR_PASSWORD) break;
    }
    error = FPDF_GetLastError() == FPDF_ERR_PASSWORD ? "needs a password not given" : "doesn't open";
    return false;
#endif
  }
  ~Work() {
#ifndef FW_HAVE_PDFTOOLS
    if (doc_) FPDF_CloseDocument(doc_);
#endif
  }
  FPDF_DOCUMENT Handle() const {
#ifdef FW_HAVE_PDFTOOLS
    return ed_.Handle();
#else
    return doc_;
#endif
  }
  const std::string& Password() const { return password_; }
  bool ThroughTools() const {
#ifdef FW_HAVE_PDFTOOLS
    return true;
#else
    return false;
#endif
  }
  // Why the page tools must leave this PDF as it is: "xfa", "protected", or "" when they may change it.
  std::string Refuses() const {
#ifdef FW_HAVE_PDFTOOLS
    switch (ed_.PageToolsBlocked()) {
      case pdftools::Block::Xfa: return "xfa";
      case pdftools::Block::Protected: return "protected";
      default: return "";
    }
#else
    const int type = FPDF_GetFormType(doc_);
    if (type == FORMTYPE_XFA_FULL || type == FORMTYPE_XFA_FOREGROUND) return "xfa";
    const unsigned long perms = FPDF_GetDocPermissions(doc_);
    if (FPDF_GetSecurityHandlerRevision(doc_) >= 0 && !(perms & 8) && !(perms & 1024)) return "protected";
    return "";
#endif
  }
  bool Rotate(int page, int quarterTurns) {
#ifdef FW_HAVE_PDFTOOLS
    return ed_.Rotate({page}, quarterTurns);
#else
    FPDF_PAGE p = FPDF_LoadPage(doc_, page);
    if (!p) return false;
    FPDFPage_SetRotation(p, (FPDFPage_GetRotation(p) + quarterTurns) % 4);
    FPDF_ClosePage(p);
    return true;
#endif
  }
  bool Delete(int page) {
#ifdef FW_HAVE_PDFTOOLS
    return ed_.Delete({page});
#else
    FPDFPage_Delete(doc_, page);
    return true;
#endif
  }
  bool Move(const std::vector<int>& sorted, int gap) {
#ifdef FW_HAVE_PDFTOOLS
    return ed_.Move(sorted, gap);
#else
    return FPDF_MovePages(doc_, sorted.data(), static_cast<unsigned long>(sorted.size()), MoveDest(sorted, gap)) != 0;
#endif
  }
  // Every page of `from` before page `at`, as Insert and Merge do. The pages inserted, or 0.
  int Insert(const Doc& from, int at) {
    const int n = static_cast<int>(from.sigs.size());
#ifdef FW_HAVE_PDFTOOLS
    pdftools::Source src;
    if (pdftools::OpenSource(from.bytes, from.password, src) != pdftools::SourceError::None) return 0;
    return ed_.Insert(src, at);
#else
    std::vector<int> idx;
    for (int i = 0; i < n; ++i) idx.push_back(i);
    return FPDF_ImportPagesByIndex(doc_, from.doc, idx.data(), static_cast<unsigned long>(n), at) ? n : 0;
#endif
  }
  // A drawn signature placed on `page`: the annotations it added, or -1 when it failed.
  int Stamp(int page, std::mt19937& rng) {
#ifdef FW_HAVE_PDFTOOLS
    std::vector<std::vector<std::pair<float, float>>> strokes(1 + rng() % 3);
    for (auto& s : strokes) {
      float x = static_cast<float>(rng() % 100) / 100, y = static_cast<float>(rng() % 100) / 100;
      for (int k = 0; k < 24; ++k) {
        x = std::clamp(x + static_cast<float>(static_cast<int>(rng() % 21) - 10) / 100, 0.f, 1.f);
        y = std::clamp(y + static_cast<float>(static_cast<int>(rng() % 21) - 10) / 100, 0.f, 1.f);
        s.push_back({x, y});
      }
    }
    FS_SIZEF size{};
    if (!FPDF_GetPageSizeByIndexF(ed_.Handle(), page, &size)) return -1;
    const float x = size.width * (0.2f + static_cast<float>(rng() % 60) / 100), y = size.height * (0.2f + static_cast<float>(rng() % 60) / 100);
    return ed_.PlaceSignature(page, x, y, 120.f, pdfsign::InkFromPoints(strokes)) ? 1 : -1;
#else
    return RawStamp(doc_, page, rng) ? 0 : -1;
#endif
  }
  // Recognize text on `page` (OCR), as the tab's Recognize text does: the words written (0: the page
  // has text already), -1 when it failed, -2 when there's no OCR here (`note` says why).
  int Ocr(int page, std::string& note) {
#ifdef FW_HAVE_PDFTOOLS
    int words = 0;
    switch (ed_.Recognize({page}, words)) {
      case pdfocr::Status::Ok: return words;
      case pdfocr::Status::NoEngine: note = "Windows' OCR isn't on this PC"; return -2;
      case pdfocr::Status::NoLanguage: note = "no OCR language on this PC"; return -2;
      default: return -1;
    }
#else
    (void)page;
    note = "OCR goes through f1-pdf's tools (fwpdftools)";
    return -2;
#endif
  }
  bool Extract(const std::vector<int>& pages, std::string& out) {
#ifdef FW_HAVE_PDFTOOLS
    return ed_.Extract(pages, out);
#else
    FPDF_DOCUMENT nd = FPDF_CreateNewDocument();
    bool ok = FPDF_ImportPagesByIndex(nd, doc_, pages.data(), static_cast<unsigned long>(pages.size()), 0) != 0;
    if (ok) out = Save(nd, false, ok);
    FPDF_CloseDocument(nd);
    return ok;
#endif
  }
  // Saved to `path` as the app saves it; `incremental` says whether it went after the file.
  bool SaveTo(const fs::path& path, bool oddRound, std::string& bytes, bool& incremental, std::string& error) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    incremental = FPDF_GetSignatureCount(Handle()) > 0;
#ifdef FW_HAVE_PDFTOOLS
    (void)oddRound;
    std::wstring why;
    if (!ed_.SaveTo(path.wstring(), why)) { error = "the save failed: " + fw::WideToUtf8(why); return false; }
    bytes = ReadAll(path);
    return true;
#else
    incremental = incremental || oddRound;
    bool ok = false;
    bytes = Save(doc_, incremental, ok);
    if (!ok) { error = "FPDF_SaveAsCopy failed"; return false; }
    Write(path, bytes);
    return true;
#endif
  }
  // What a save of the PDF as opened writes: the same bytes is right.
  bool UneditedSame(const std::string& original) {
#ifdef FW_HAVE_PDFTOOLS
    std::string out;
    return ed_.Bytes(out) && out == original;
#else
    bool ok = false;
    std::string again = Save(doc_, true, ok);
    return ok && again == original;
#endif
  }
#ifdef FW_HAVE_PDFTOOLS
  pdftools::Editor& Editor() { return ed_; }
#endif

 private:
#ifdef FW_HAVE_PDFTOOLS
  pdftools::Editor ed_;
#else
  std::string bytes_;
  FPDF_DOCUMENT doc_ = nullptr;
#endif
  std::string password_;
};

// Whether Insert takes this PDF's pages: the tools refuse a file whose permissions don't allow copying
// from it (pdftools::OpenSource's NotAllowed).
bool Insertable(const Doc& d) {
#ifdef FW_HAVE_PDFTOOLS
  pdftools::Source src;
  return pdftools::OpenSource(d.bytes, d.password, src) == pdftools::SourceError::None;
#else
  (void)d;
  return true;
#endif
}

int AnnotsOn(FPDF_DOCUMENT doc, int index) {
  FPDF_PAGE page = FPDF_LoadPage(doc, index);
  if (!page) return -1;
  const int n = FPDFPage_GetAnnotCount(page);
  FPDF_ClosePage(page);
  return n;
}

#ifdef FW_HAVE_PDFTOOLS
// f1-pdf's own check of signature `index` of `bytes`: valid, untrusted, changed, invalid or unreadable.
std::string SignatureVerdict(const std::string& bytes, const std::string& password, int index) {
  FPDF_DOCUMENT doc = FPDF_LoadMemDocument64(bytes.data(), bytes.size(), password.empty() ? nullptr : password.c_str());
  if (!doc) return "the file doesn't open";
  std::string kind = "no signature " + std::to_string(index + 1);
  if (FPDF_SIGNATURE sig = FPDF_GetSignatureObject(doc, index)) {
    const unsigned long n = FPDFSignatureObj_GetByteRange(sig, nullptr, 0);
    std::vector<int> range(n);
    if (n) FPDFSignatureObj_GetByteRange(sig, range.data(), n);
    const unsigned long c = FPDFSignatureObj_GetContents(sig, nullptr, 0);
    std::vector<uint8_t> contents(c);
    if (c) FPDFSignatureObj_GetContents(sig, contents.data(), c);
    static const char* const names[] = {"valid", "untrusted", "changed", "invalid", "unreadable"};
    kind = names[static_cast<int>(pdfsign::Verify(bytes, range, contents).kind)];
  }
  FPDF_CloseDocument(doc);
  return kind;
}
#endif

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
  // --pfx: certificate signatures too (f1-pdf's signing, the test signer loaded in memory only).
  const CERT_CONTEXT* cert = nullptr;
  if (!options.pfx.empty()) {
#ifdef FW_HAVE_PDFTOOLS
    cert = pdfsign::LoadPfx(ReadAll(options.pfx), options.pfxPassword);
    if (!cert) {
      std::fprintf(stderr, "%s doesn't load as a PFX with that password.\n", Display(options.pfx).c_str());
      return 1;
    }
#else
    std::printf("--pfx: signing goes through f1-pdf's tools (fwpdftools), not in this build: no signatures.\n");
#endif
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

  // Every input open at once: the others are where inserted (merged) pages come from.
  std::vector<std::unique_ptr<Doc>> docs;
  std::vector<std::pair<fs::path, std::string>> skipped;
  for (const auto& f : files) {
    std::string why;
    auto d = Open(f, ReadAll(f), passwords, why);
    if (!d) skipped.push_back({f, why});
    else if (d->sigs.empty()) skipped.push_back({f, "has no pages"});
    else docs.push_back(std::move(d));
  }
  // Inserted whole, as Insert and Merge do: the small ones the tools take pages from.
  std::vector<const Doc*> donors;
  for (const auto& d : docs)
    if (d->password.empty() && d->sigs.size() <= 12 && Insertable(*d)) donors.push_back(d.get());

  const unsigned seed = options.seed ? options.seed : 1;
  const int edits = std::max(1, options.edits);
  const uint32_t rounds = std::max<uint32_t>(1, options.rounds);
  std::set<std::string> allowed;
  for (size_t a = 0, s = 0; a <= options.ops.size(); ++a)
    if (a == options.ops.size() || options.ops[a] == ',') {
      if (a > s) allowed.insert(options.ops.substr(s, a - s));
      s = a + 1;
    }
  const bool throughTools = Work().ThroughTools();
  long long totalOps = 0, totalFailures = 0, written = 0, uneditedSame = 0, uneditedTried = 0, ocrWords = 0, ocrPages = 0, signedFiles = 0,
            refusedFiles = 0;
  std::string ocrNote;
  std::string signaturesJson;  // <out>\signed\signatures.json, for pyHanko (tests\pdf\validate-signatures.py)
  std::string json = std::string("{\n  \"format\": \"featherweight-pdf-torture\",\n  \"through\": \"") + (throughTools ? "featherweight" : "pdfium") +
                     "\",\n  \"seed\": " + std::to_string(seed) + ",\n  \"edits\": " + std::to_string(edits) + ",\n  \"rounds\": " +
                     std::to_string(rounds) + ",\n  \"files\": [";
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
    std::string roundsJson, refused;
    int fileOps = 0;
    bool uneditedIdentical = false;
    for (uint32_t r = 0; r < rounds; ++r) {
      const size_t failedBefore = failures.size();
      std::mt19937 rng(seed * 2654435761u ^ static_cast<unsigned>(std::hash<std::string>()(name)) ^ (r * 7919u));
      std::string why;
      Work work;  // each round from the original
      if (!work.Open(d->bytes, passwords, why)) { failures.push_back({"open", "round " + std::to_string(r + 1) + ": " + why}); continue; }
      if (r == 0) {
        // Saved without edits: the same bytes.
        ++uneditedTried;
        uneditedIdentical = work.UneditedSame(d->bytes);
        if (uneditedIdentical) ++uneditedSame;
        refused = work.Refuses();
      }
      const std::string tag = name + "-r" + std::to_string(r + 1);
      if (!refused.empty()) {
        // A PDF the page tools must refuse (an XFA form, or protected against changes): every tool
        // refuses, and a save afterwards is the file as it was.
        if (r == 0) ++refusedFiles;
        if (!work.ThroughTools()) break;  // nothing of the tools' to check without them
        const int n = static_cast<int>(d->sigs.size());
        if (work.Rotate(0, 1)) failures.push_back({"refusal", tag + ": rotate wasn't refused (" + refused + ")"});
        if (n > 1 && work.Delete(n - 1)) failures.push_back({"refusal", tag + ": delete wasn't refused (" + refused + ")"});
        if (n > 1 && work.Move({n - 1}, 0)) failures.push_back({"refusal", tag + ": move wasn't refused (" + refused + ")"});
        if (!donors.empty() && work.Insert(*donors[rng() % donors.size()], 0)) failures.push_back({"refusal", tag + ": insert wasn't refused (" + refused + ")"});
        std::string bytes;
        bool incremental = false;
        if (!work.SaveTo(out / "refused" / (tag + ".pdf"), false, bytes, incremental, why)) failures.push_back({"save", tag + ": " + why});
        else if (bytes != d->bytes) failures.push_back({"refusal", tag + ": saved after the refused tools, it isn't the file as it was"});
        ++written;
        fileOps += 4;
        break;  // one round says it all
      }
      std::vector<PageRef> model;
      for (int i = 0; i < static_cast<int>(d->sigs.size()); ++i) model.push_back({d.get(), i, d->sigs[static_cast<size_t>(i)].rot});
      std::vector<std::string> ops;
      int extracts = 0;
      for (int e = 0; e < edits; ++e) {
        const int n = static_cast<int>(model.size());
        // --ops: only these kinds (rotate, delete, move, insert, stamp, ocr, extract, split), to find which one fails.
        int pick = static_cast<int>(rng() % 100);
        for (int tries = 0; !allowed.empty() && !allowed.count(KindOf(pick)) && tries < 200; ++tries) pick = static_cast<int>(rng() % 100);
        if (!allowed.empty() && !allowed.count(KindOf(pick))) break;
        auto randomPage = [&] { return static_cast<int>(rng() % static_cast<unsigned>(n)); };
        const std::string kind = KindOf(pick);
        if (kind == "rotate") {
          const int i = randomPage(), q = 1 + static_cast<int>(rng() % 3);
          if (!work.Rotate(i, q)) { failures.push_back({"operation", tag + ": rotating page " + std::to_string(i + 1) + " failed"}); continue; }
          model[static_cast<size_t>(i)].rot = (model[static_cast<size_t>(i)].rot + q) % 4;
          ops.push_back("rotate " + std::to_string(i + 1) + " by " + std::to_string(q * 90));
        } else if (kind == "delete") {
          if (n < 2) continue;
          const int i = randomPage();
          if (!work.Delete(i)) { failures.push_back({"operation", tag + ": deleting page " + std::to_string(i + 1) + " failed"}); continue; }
          model.erase(model.begin() + i);
          ops.push_back("delete " + std::to_string(i + 1));
        } else if (kind == "move") {
          if (n < 2) continue;
          const int k = 1 + static_cast<int>(rng() % static_cast<unsigned>(std::min(3, n - 1)));
          std::vector<int> idx;
          while (static_cast<int>(idx.size()) < k) {
            const int i = randomPage();
            if (std::find(idx.begin(), idx.end(), i) == idx.end()) idx.push_back(i);
          }
          std::sort(idx.begin(), idx.end());
          const int gap = static_cast<int>(rng() % static_cast<unsigned>(n + 1));
          const int dest = MoveDest(idx, gap);
          bool same = true;
          for (int j = 0; j < k; ++j) same = same && idx[static_cast<size_t>(j)] == dest + j;
          if (same) continue;  // dropped where they are: nothing to do
          if (!work.Move(idx, gap)) { failures.push_back({"operation", tag + ": a valid move was refused"}); continue; }
          std::vector<PageRef> moved, rest;
          for (int i : idx) moved.push_back(model[static_cast<size_t>(i)]);
          for (int i = 0; i < n; ++i)
            if (std::find(idx.begin(), idx.end(), i) == idx.end()) rest.push_back(model[static_cast<size_t>(i)]);
          rest.insert(rest.begin() + dest, moved.begin(), moved.end());
          model = rest;
          std::string list;
          for (int i : idx) list += (list.empty() ? "" : ",") + std::to_string(i + 1);
          ops.push_back("move " + list + " before " + std::to_string(gap + 1));
        } else if (kind == "insert") {  // another PDF's pages (merge)
          if (donors.empty() || n > 600) continue;
          const Doc* from = donors[rng() % donors.size()];
          const int at = static_cast<int>(rng() % static_cast<unsigned>(n + 1));
          const int got = work.Insert(*from, at);
          if (got != static_cast<int>(from->sigs.size())) {
            failures.push_back({"operation", tag + ": inserting " + Display(from->path) + " gave " + std::to_string(got) + " pages"});
            continue;
          }
          for (int j = 0; j < got; ++j) model.insert(model.begin() + at + j, PageRef{from, j, from->sigs[static_cast<size_t>(j)].rot});
          ops.push_back("insert " + fw::WideToUtf8(from->path.filename().wstring()) + " at " + std::to_string(at + 1));
        } else if (kind == "stamp") {  // a drawn signature
          const int i = randomPage();
          const int added = work.Stamp(i, rng);
          if (added < 0) { failures.push_back({"operation", tag + ": the drawn signature on page " + std::to_string(i + 1) + " failed"}); continue; }
          model[static_cast<size_t>(i)].addedAnnots += added;
          ops.push_back("stamp " + std::to_string(i + 1));
        } else if (kind == "ocr") {  // Recognize text: scanned pages get a text layer
          const int i = randomPage();
          std::string note;
          const int words = work.Ocr(i, note);
          if (words == -2) { ocrNote = note; continue; }
          if (words < 0) { failures.push_back({"operation", tag + ": OCR on page " + std::to_string(i + 1) + " failed"}); continue; }
          if (words > 0) {
            // The page's text is what OCR wrote now: the save must keep it as it is here.
            model[static_cast<size_t>(i)].text = std::make_shared<PageSig>(SigOf(work.Handle(), i));
            ocrWords += words;
            ++ocrPages;
          }
          ops.push_back("ocr " + std::to_string(i + 1) + " (" + std::to_string(words) + " words)");
        } else {  // extract some pages, or split the whole into parts, into new files
          const bool split = kind == "split" && n >= 2;
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
            while (static_cast<int>(part.size()) < k) {
              const int i = randomPage();
              if (std::find(part.begin(), part.end(), i) == part.end()) part.push_back(i);
            }
            std::sort(part.begin(), part.end());  // as the Pages panel hands a selection over
            parts.push_back(part);
          }
          for (const auto& part : parts) {
            std::string bytes;
            const std::string label = tag + (split ? "-split" : "-extract") + std::to_string(++extracts);
            if (!work.Extract(part, bytes)) { failures.push_back({"operation", label + ": the new file couldn't be made"}); continue; }
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
      bool signedRound = false;
#ifdef FW_HAVE_PDFTOOLS
      // Every third round, with --pfx: the result signed (PAdES B-B, the test signer), saved, then one
      // more change and saved again. The signed revision must stay byte for byte at the start of the
      // changed file, and f1-pdf's check must say "changed" there.
      if (cert && r % 3 == 2 && work.Editor().SigningBlocked() == pdfsign::Error::None) {
        signedRound = true;
        const int before = AnnotsOn(work.Handle(), 0);
        pdfsign::Details details;
        details.name = L"Featherweight Test Signer";
        details.reason = L"f1-gate pdf-torture";
        const pdfsign::Error e = work.Editor().Sign(cert, details);
        if (e != pdfsign::Error::None) {
          failures.push_back({"signature", tag + ": signing failed (error " + std::to_string(static_cast<int>(e)) + ")"});
        } else {
          model[0].addedAnnots += AnnotsOn(work.Handle(), 0) - before;
          const int sigIndex = FPDF_GetSignatureCount(work.Handle()) - 1;
          std::string signedBytes, changed;
          bool incremental = false;
          const std::string label = tag + "-signed";
          if (!work.SaveTo(out / "signed" / (label + ".pdf"), false, signedBytes, incremental, why)) failures.push_back({"save", label + ": " + why});
          else {
            ++written, ++signedFiles;
            Verify(signedBytes, work.Password(), model, label, failures, inputErrors);
            const std::string v = SignatureVerdict(signedBytes, work.Password(), sigIndex);
            if (v != "untrusted" && v != "valid") failures.push_back({"signature", label + ": f1-pdf's check says " + v + ", not valid"});
            signaturesJson += std::string(signaturesJson.empty() ? "" : ",\n") + "  " + Json(label + ".pdf") + ": \"valid\"";
            const int p = static_cast<int>(rng() % model.size());
            if (!work.Rotate(p, 1)) failures.push_back({"operation", label + ": rotating page " + std::to_string(p + 1) + " after signing failed"});
            else {
              model[static_cast<size_t>(p)].rot = (model[static_cast<size_t>(p)].rot + 1) % 4;
              ops.push_back("sign, then rotate " + std::to_string(p + 1) + " by 90");
              if (!work.SaveTo(out / "signed" / (label + "-changed.pdf"), false, changed, incremental, why)) failures.push_back({"save", label + "-changed: " + why});
              else {
                ++written;
                if (!incremental || changed.size() <= signedBytes.size() || changed.compare(0, signedBytes.size(), signedBytes) != 0)
                  failures.push_back({"signature", label + "-changed: the signed revision isn't kept byte for byte before the change"});
                Verify(changed, work.Password(), model, label + "-changed", failures, inputErrors);
                const std::string c = SignatureVerdict(changed, work.Password(), sigIndex);
                if (c != "changed") failures.push_back({"signature", label + "-changed: f1-pdf's check says " + c + ", not changed"});
                signaturesJson += ",\n  " + Json(label + "-changed.pdf") + ": \"changed-after-signing\"";
              }
            }
          }
        }
      }
#endif
      bool incremental = false;
      if (!signedRound) {
        // Saved as the app saves (through the tools: whole, or incrementally when signed; without them,
        // incrementally every other round), then opened again and checked.
        std::string bytes;
        const fs::path path = out / "saved" / (tag + ".pdf");
        if (!work.SaveTo(path, r % 2 == 1, bytes, incremental, why)) failures.push_back({"save", tag + ": " + why});
        else {
          if (incremental) {  // named so, for pdf-readers.ps1
            std::error_code ec;
            fs::rename(path, out / "saved" / (tag + "-incremental.pdf"), ec);
          }
          ++written;
          Verify(bytes, work.Password(), model, tag + (incremental ? "-incremental" : ""), failures, inputErrors);
        }
      }
      std::string list;
      for (const auto& o : ops) list += (list.empty() ? "" : ", ") + Json(o);
      roundsJson += std::string(roundsJson.empty() ? "" : ", ") + "{\"round\": " + std::to_string(r + 1) + ", \"incremental\": " +
                    (incremental ? "true" : "false") + ", \"signed\": " + (signedRound ? "true" : "false") + ", \"pages\": " +
                    std::to_string(model.size()) + ", \"ops\": [" + list + "]}";
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
    std::printf("[%s] %s: %d operations%s%s%s\n", failures.empty() ? "OK" : "FAIL", Display(d->path).c_str(), fileOps,
                refused.empty() ? "" : (", refused (" + refused + ")").c_str(), failures.empty() ? "" : ": ",
                failures.empty() ? "" : (failures.front().category + ": " + failures.front().message).c_str());
    for (size_t k = 1; k < failures.size() && k < 4; ++k) std::printf("    %s: %s\n", failures[k].category.c_str(), failures[k].message.c_str());
    std::string fj;
    for (const auto& f : failures) fj += std::string(fj.empty() ? "" : ", ") + "{\"category\": " + Json(f.category) + ", \"message\": " + Json(f.message) + "}";
    json += std::string(firstFile ? "\n" : ",\n") + "    {\"file\": " + Json(fw::WideToUtf8(fs::absolute(d->path).wstring())) +
            ", \"pages\": " + std::to_string(d->sigs.size()) + ", \"password\": " + (d->password.empty() ? "false" : "true") +
            ", \"refused\": " + Json(refused) + ", \"inputStructureErrors\": " + std::to_string(inputErrors) +
            ", \"uneditedIdentical\": " + (uneditedIdentical ? "true" : "false") + ",\n     \"rounds\": [" + roundsJson +
            "],\n     \"failures\": [" + fj + "]}";
    firstFile = false;
  }
  if (!signaturesJson.empty())
    Write(out / "signed" / "signatures.json",
          "{\"generator\": \"fwtool pdf-torture --pfx\", \"trustRoot\": \"test-signer.crt\", \"expect\": {\n" + signaturesJson + "\n}}\n");
#ifdef FW_HAVE_PDFTOOLS
  if (cert) CertFreeCertificateContext(cert);
#endif
  json += "\n  ],\n  \"totals\": {\"files\": " + std::to_string(docs.size()) + ", \"skipped\": " + std::to_string(skipped.size()) +
          ", \"refused\": " + std::to_string(refusedFiles) + ", \"operations\": " + std::to_string(totalOps) + ", \"failures\": " +
          std::to_string(totalFailures) + ", \"written\": " + std::to_string(written) + ", \"signed\": " + std::to_string(signedFiles) +
          ", \"ocrPages\": " + std::to_string(ocrPages) + ", \"ocrWords\": " + std::to_string(ocrWords) + ", \"ocrNote\": " + Json(ocrNote) +
          ", \"uneditedIdentical\": " + std::to_string(uneditedSame) + ", \"uneditedTried\": " + std::to_string(uneditedTried) + "}\n}\n";
  std::printf("%zu PDFs (%zu skipped, %lld refused), %lld operations through %s, %lld files written (%lld signed), %lld failures; "
              "OCR: %lld words on %lld pages%s%s; saved without edits: %lld of %lld the same bytes\n",
              docs.size(), skipped.size(), refusedFiles, totalOps, throughTools ? "f1-pdf's tools" : "PDFium", written, signedFiles, totalFailures,
              ocrWords, ocrPages, ocrNote.empty() ? "" : ", ", ocrNote.c_str(), uneditedSame, uneditedTried);
  if (!options.jsonOut.empty()) Write(options.jsonOut, json);
  return totalFailures ? 1 : 0;
}

// fwtool pdf-ocr --out DIR: Recognize text on every page that needs it, as the PDF tab's Recognize text
// does (pdftools::Editor::Recognize: Windows' OCR, an invisible text layer), each PDF saved into DIR
// under its own name through the tab's safe save. tests\pdf\ocr-accuracy.ps1 reads the text back.
int RunPdfOcr(const Options& options) {
#ifdef FW_HAVE_PDFTOOLS
  std::string error;
  if (!InitPdfium(error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 1;
  }
  if (options.outDir.empty()) {
    std::fprintf(stderr, "pdf-ocr needs --out DIR.\n");
    return 2;
  }
  std::vector<std::string> passwords;
  for (size_t a = 0, s = 0; a <= options.password.size(); ++a)
    if (a == options.password.size() || options.password[a] == L',') {
      if (a > s) passwords.push_back(fw::WideToUtf8(options.password.substr(s, a - s)));
      s = a + 1;
    }
  std::vector<fs::path> files;
  for (const auto& p : options.inputs) {
    std::error_code ec;
    if (fs::is_directory(p, ec)) {
      for (const auto& e : fs::directory_iterator(p, ec))
        if (e.is_regular_file() && fw::ToLower(e.path().extension().wstring()) == L".pdf") files.push_back(e.path());
    } else files.push_back(p);
  }
  std::sort(files.begin(), files.end());
  int failed = 0;
  long long allWords = 0;
  for (const auto& f : files) {
    Work work;
    std::string why;
    if (!work.Open(ReadAll(f), passwords, why)) {
      std::printf("[FAIL] %s: %s\n", Display(f).c_str(), why.c_str());
      ++failed;
      continue;
    }
    int words = 0;
    const pdfocr::Status st = work.Editor().Recognize({}, words);
    if (st != pdfocr::Status::Ok) {
      std::printf("[FAIL] %s: OCR %s\n", Display(f).c_str(), st == pdfocr::Status::NoEngine ? "isn't on this PC" : st == pdfocr::Status::NoLanguage ? "has no language here" : "failed on a page");
      ++failed;
      continue;
    }
    std::string bytes;
    bool incremental = false;
    if (!work.SaveTo(options.outDir / f.filename(), false, bytes, incremental, why)) {
      std::printf("[FAIL] %s: %s\n", Display(f).c_str(), why.c_str());
      ++failed;
      continue;
    }
    allWords += words;
    std::printf("[OK] %s: %d words\n", Display(f).c_str(), words);
  }
  std::printf("%zu PDFs, %lld words written, %d failed\n", files.size(), allWords, failed);
  return failed ? 1 : 0;
#else
  (void)options;
  std::fprintf(stderr, "pdf-ocr goes through f1-pdf's tools (fwpdftools), which this build hasn't.\n");
  return 2;
#endif
}

}  // namespace fwtool
