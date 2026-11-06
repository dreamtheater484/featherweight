// The hook for D1's layout: fwtool opens a Word document as the app does and lays out every page
// with fwdoclayout (src/doc/layout/Layout.h), marking each stage in OpenTimings.
#include "LayoutHook.h"

#include <windows.h>
#include <psapi.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <random>

#include "Common.h"
#include "core/Strings.h"
#include "doc/msdoc/MsDoc.h"
#include "doc/odf/Odt.h"
#include "doc/docx/Docx.h"
#include "doc/edit/Editor.h"
#include "doc/edit/Format.h"
#include "doc/edit/Inline.h"
#include "doc/edit/Lists.h"
#include "doc/edit/Page.h"
#include "doc/edit/Styles.h"
#include "doc/edit/TreeEdit.h"
#include "doc/layout/Layout.h"
#include "draw/D2DDevice.h"
#include "draw/D2DPainter.h"
#include "pkg/CompoundFile.h"
#include "pkg/OfficeCrypto.h"
#include "pkg/Package.h"
#include "pkg/Snapshot.h"
#include "text/Fonts.h"

namespace fwtool {

namespace {

// The document being laid out, for font substitution from its font table (panose and family).
const fw::doc::Document* g_document = nullptr;

std::wstring Substitute(std::wstring_view family) {
  return g_document ? fw::doc::layout::SubstituteFamily(*g_document, family) : std::wstring();
}

// One Fonts for the process: faces stay cached from one document to the next, as in the app.
fw::text::Fonts& SharedFonts() {
  static fw::text::Fonts fonts(Substitute);
  return fonts;
}

// A document the app imports or decrypts first, as the .docx bytes it then reads: a Word 97-2003
// .doc, an .odt, or a password-protected .docx or .doc opened with `password`. Empty without
// `error` for a plain package, which is read directly.
std::vector<uint8_t> ImportedDocx(const std::filesystem::path& document, std::wstring_view password, std::string& error) {
  namespace crypto = fw::pkg::crypto;
  const std::wstring ext = fw::ToLower(document.extension().wstring());
  std::ifstream in(document, std::ios::binary);
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  fw::pkg::Status status;
  std::vector<uint8_t> docx;
  if (auto cf = fw::pkg::CompoundFile::Open(bytes)) {
    if (crypto::IsEncryptedPackage(*cf)) {
      crypto::Key key;
      if (crypto::Decrypt(*cf, password, docx, key) != crypto::Result::Ok) error = "the password doesn't open it";
      return docx;
    }
    if (crypto::IsEncryptedDoc(*cf)) {
      crypto::DocStreams s;
      if (crypto::DecryptDoc(*cf, password, s) != crypto::Result::Ok) {
        error = "the password doesn't open it";
        return docx;
      }
      docx = fw::doc::msdoc::ToDocx(
          [&](std::string_view name) -> std::optional<std::vector<uint8_t>> {
            if (name == "WordDocument") return s.wordDocument;
            if (name == s.table) return s.tableStream;
            if (name == "Data") return s.data.empty() ? std::nullopt : std::optional<std::vector<uint8_t>>(s.data);
            return cf->Stream(name);
          },
          status, true);
    } else {
      docx = fw::doc::msdoc::ToDocx([&](std::string_view name) { return cf->Stream(name); }, status);
    }
  } else if (ext == L".odt") {
    status = fw::doc::odf::OdtToDocx(bytes, docx);
    if (!status.Ok()) docx.clear();
  } else {
    return docx;
  }
  if (docx.empty()) error = status.message.empty() ? "can't be imported" : status.message;
  return docx;
}

// Reads a document into the model as the app does.
std::unique_ptr<fw::doc::Document> OpenModel(const std::filesystem::path& document, fw::OpenTimings* timings,
                                             std::string& error, std::wstring_view password = {}) {
  fw::pkg::Status status;
  std::unique_ptr<fw::pkg::Package> package;
  error.clear();  // its emptiness below says whether importing failed
  std::vector<uint8_t> imported = ImportedDocx(document, password, error);
  if (!error.empty()) return nullptr;
  if (!imported.empty()) {
    package = fw::pkg::Package::Open(fw::pkg::Snapshot::FromBytes(std::move(imported)), fw::pkg::Limits::Default(), status);
    if (timings) timings->Mark(fw::OpenTimings::Read);
  } else {
    fw::pkg::Snapshot::Options options;
    std::error_code ec;
    options.recoveryDir = (std::filesystem::temp_directory_path(ec) / L"fwtool-recovery").wstring();
    auto snapshot = fw::pkg::Snapshot::Open(document.wstring(), options, status);
    if (!snapshot) {
      error = status.message.empty() ? "can't read the file" : status.message;
      return nullptr;
    }
    if (timings) timings->Mark(fw::OpenTimings::Read);
    package = fw::pkg::Package::Open(std::move(snapshot), fw::pkg::Limits::Default(), status);
  }
  if (!package) {
    error = status.message.empty() ? "can't open the package" : status.message;
    return nullptr;
  }
  if (timings) timings->Mark(fw::OpenTimings::Package);
  auto doc = fw::doc::docx::Read(std::move(package), status);
  if (!doc) {
    error = status.message.empty() ? "can't read the document" : status.message;
    return nullptr;
  }
  if (timings) timings->Mark(fw::OpenTimings::Model);
  return doc;
}

// Opens a document as the app does and lays out every page, then hands the layout to `use`.
bool OpenAndLayOut(const std::filesystem::path& document, fw::OpenTimings* timings, std::string& error,
                   const std::function<void(const fw::doc::layout::Layout&)>& use, std::wstring_view password = {}) {
  std::unique_ptr<fw::doc::Document> doc = OpenModel(document, timings, error, password);
  if (!doc) return false;
  g_document = doc.get();
  fw::text::Fonts& fonts = SharedFonts();
  fonts.ClearSubstitutions();
  fonts.UseDocumentFonts(fw::doc::layout::EmbeddedFonts(*doc));
  {
    fw::doc::layout::Layout layout(*doc, fonts, {}, timings);
    layout.LayOutAll();
    use(layout);
  }
  // FWTOOL_FONTS=1: the fonts this document asked for that aren't installed, and what was drawn.
  wchar_t flag[4] = {};
  if (GetEnvironmentVariableW(L"FWTOOL_FONTS", flag, 4) > 0)
    for (const auto& s : fonts.Substitutions())
      std::fprintf(stderr, "font: %s: %s -> %s\n", fw::WideToUtf8(document.filename().wstring()).c_str(),
                   fw::WideToUtf8(s.requested).c_str(), fw::WideToUtf8(s.used).c_str());
  g_document = nullptr;
  return true;
}

}  // namespace

bool HaveLayout() { return true; }

bool LayOutDocument(const std::filesystem::path& document, fw::LayoutDump* dump, fw::OpenTimings* timings,
                    std::string& error, std::wstring_view password) {
  return OpenAndLayOut(
      document, timings, error,
      [&](const fw::doc::layout::Layout& layout) {
        if (dump) *dump = layout.Dump(fw::WideToUtf8(document.filename().wstring()));
      },
      password);
}

bool WithLayout(const std::filesystem::path& document, std::string& error,
                const std::function<void(const fw::doc::Document&, const fw::doc::layout::Layout&)>& use) {
  return OpenAndLayOut(document, nullptr, error,
                       [&](const fw::doc::layout::Layout& layout) { use(*g_document, layout); });
}

void DumpLayout(const std::filesystem::path& document, const std::filesystem::path& out) {
  std::string error;
  bool ok = OpenAndLayOut(document, nullptr, error, [&](const fw::doc::layout::Layout& layout) {
    if (!out.empty()) {
      std::error_code ec;
      std::filesystem::create_directories(out, ec);
      std::filesystem::path file = out / (document.filename().wstring() + L".layout.json");
      std::string json = fw::WriteLayoutDump(layout.Dump(fw::WideToUtf8(document.filename().wstring())));
      FILE* f = nullptr;
      if (_wfopen_s(&f, file.c_str(), L"wb") == 0 && f) {
        std::fwrite(json.data(), 1, json.size(), f);
        std::fclose(f);
      }
      std::printf("%s: %zu pages\n", fw::WideToUtf8(file.wstring()).c_str(), layout.PageCount());
      return;
    }
    std::printf("%s: %zu pages, %zu KB of layout\n", fw::WideToUtf8(document.wstring()).c_str(), layout.PageCount(),
                layout.MemoryBytes() / 1024);
    std::printf("  story      top  height baseline     x  asc/desc/gap  para | text\n");
    for (size_t p = 0; p < layout.PageCount(); ++p) {
      const fw::doc::layout::Page& page = layout.GetPage(p);
      std::printf("page %zu (%.0f x %.0f), number %d, text area %.1f-%.1f\n", p + 1, page.width, page.height, page.number,
                  page.body.top, page.body.bottom);
      for (const fw::doc::layout::LineBox& l : page.lines)
        std::printf("  %-7s %7.2f %6.2f %7.2f %6.2f %5.2f/%4.2f/%4.2f %5u | %s\n", std::string(fw::StoryName(l.story)).c_str(),
                    l.top, l.height, l.baseline, l.x + l.line.contentLeft, l.line.ascent, l.line.descent, l.line.lineGap,
                    l.para, fw::WideToUtf8(std::wstring(l.text.begin(), l.text.end())).c_str());
    }
  });
  if (!ok) std::fprintf(stderr, "%s: %s\n", fw::WideToUtf8(document.wstring()).c_str(), error.c_str());
}


// ---- fwtool bench --typing

namespace {

double Since(std::chrono::steady_clock::time_point t) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

// Paints pages on the graphics adapter, as the document tab does, at 100 % on a 96 dpi screen.
class PagePainter {
 public:
  bool Init() {
    D2D1_FACTORY_OPTIONS opts{};
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &opts,
                                 reinterpret_cast<void**>(factory_.GetAddressOf()))))
      return false;
    if (FAILED(draw::CreateD2DDevice(factory_.Get(), false, d3d_, d2d_)) &&
        FAILED(draw::CreateD2DDevice(factory_.Get(), true, d3d_, d2d_)))
      return false;
    return SUCCEEDED(d2d_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc_));
  }
  void Paint(const fw::doc::layout::Layout& layout, size_t page) {
    const fw::doc::layout::Page& p = layout.GetPage(page);
    const float scale = 96.0f / 72.0f;
    const D2D1_SIZE_U size{static_cast<UINT32>(p.width * scale + 1), static_cast<UINT32>(p.height * scale + 1)};
    if (!target_ || target_->GetPixelSize().width != size.width || target_->GetPixelSize().height != size.height) {
      target_.Reset();
      const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
          D2D1_BITMAP_OPTIONS_TARGET, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
      if (FAILED(dc_->CreateBitmap(size, nullptr, 0, props, &target_))) return;
      dc_->SetTarget(target_.Get());
    }
    dc_->BeginDraw();
    dc_->Clear(D2D1::ColorF(1, 1, 1, 1));
    {
      draw::D2DPainter painter(dc_.Get());
      painter.Transform(draw::Matrix::Scale(scale, scale));
      layout.Paint(painter, page);
    }
    dc_->EndDraw();
  }

 private:
  Microsoft::WRL::ComPtr<ID2D1Factory1> factory_;
  Microsoft::WRL::ComPtr<ID3D11Device> d3d_;
  Microsoft::WRL::ComPtr<ID2D1Device> d2d_;
  Microsoft::WRL::ComPtr<ID2D1DeviceContext> dc_;
  Microsoft::WRL::ComPtr<ID2D1Bitmap1> target_;
};

// A paragraph of the body with some text on or after `page`, to type into; in a table, the last
// cell on the page with some, not one of the header rows repeated on each page.
uint32_t ParagraphToType(const fw::doc::Document& doc, const fw::doc::layout::Layout& layout, size_t page,
                         fw::LayoutStory story) {
  for (size_t p = page; p < layout.PageCount(); ++p) {
    uint32_t last = fw::doc::kNone;
    for (const fw::doc::layout::LineBox& l : layout.GetPage(p).lines)
      if (l.story == story && l.para != fw::doc::kNone && doc.Para(l.para).text.size() >= 20) {
        if (story == fw::LayoutStory::Body) return l.para;
        last = l.para;
      }
    if (last != fw::doc::kNone) return last;
  }
  return fw::doc::kNone;
}

}  // namespace

namespace {

double PrivateMB() {
  PROCESS_MEMORY_COUNTERS_EX pmc{};
  pmc.cb = sizeof pmc;
  K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof pmc);
  return pmc.PrivateUsage / 1048576.0;
}

double PeakWorkingSetMB() {
  PROCESS_MEMORY_COUNTERS pmc{};
  K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc);
  return pmc.PeakWorkingSetSize / 1048576.0;
}

}  // namespace

bool BenchScrolling(const std::filesystem::path& document, ScrollResult& result, std::string& error) {
  PagePainter painter;
  if (!painter.Init()) {
    error = "can't paint: no Direct2D device";
    return false;
  }
  result.memoryOpen = PrivateMB();
  const auto start = std::chrono::steady_clock::now();
  std::unique_ptr<fw::doc::Document> doc = OpenModel(document, nullptr, error);
  if (!doc) return false;
  g_document = doc.get();
  fw::text::Fonts& fonts = SharedFonts();
  fonts.ClearSubstitutions();
  {
    fw::doc::layout::Layout layout(*doc, fonts);
    layout.LayOutAll();
    result.layOutMs = Since(start);
    result.pages = layout.PageCount();
    result.memoryLaidOut = PrivateMB();
    for (size_t page = 0; page < layout.PageCount(); ++page) {
      const auto t = std::chrono::steady_clock::now();
      painter.Paint(layout, page);
      result.paint.push_back(Since(t));
    }
    result.memoryScrolled = PrivateMB();
  }
  g_document = nullptr;
  result.peakWorkingSet = PeakWorkingSetMB();
  return true;
}

bool BenchTyping(const std::filesystem::path& document, std::vector<TypingPlace>& places, size_t& pages,
                 std::string& error) {
  std::unique_ptr<fw::doc::Document> doc = OpenModel(document, nullptr, error);
  if (!doc) return false;
  PagePainter painter;
  if (!painter.Init()) {
    error = "can't paint: no Direct2D device";
    return false;
  }
  g_document = doc.get();
  fw::text::Fonts& fonts = SharedFonts();
  fonts.ClearSubstitutions();
  fonts.UseDocumentFonts(fw::doc::layout::EmbeddedFonts(*doc));
  {
    fw::doc::layout::Layout layout(*doc, fonts);
    layout.LayOutAll();
    pages = layout.PageCount();
    fw::doc::edit::Editor editor(*doc);
    editor.OnChange([&](const fw::doc::edit::Change& c) { layout.Edited(c); });
    // Each keystroke as the document tab handles it: the edit, the pages on screen laid out again
    // (the caret's and the next), the caret placed, the caret's page painted. Then the rest is
    // laid out again in idle time, untimed.
    auto key = [&](fw::doc::Pos& caret, const std::function<fw::doc::edit::Result()>& edit) {
      const auto start = std::chrono::steady_clock::now();
      fw::doc::edit::Result r = edit();
      if (!r) return -1.0;
      caret = r.caret;
      const auto at = layout.CaretAt({caret.para, caret.offset});
      const size_t page = at ? at->page : 0;
      layout.LayOutPages(page + 2);
      painter.Paint(layout, page);
      const double ms = Since(start);
      while (layout.Continue(50)) {
      }
      return ms;
    };
    struct Where {
      const char* name;
      size_t page;
      fw::LayoutStory story;
    };
    const Where where[] = {{"start", 0, fw::LayoutStory::Body},
                           {"middle", pages / 2, fw::LayoutStory::Body},
                           {"end", pages > 2 ? pages - 2 : 0, fw::LayoutStory::Body},
                           {"table", pages / 2, fw::LayoutStory::Cell}};
    for (const Where& w : where) {
      TypingPlace place;
      place.where = w.name;
      uint32_t para = ParagraphToType(*doc, layout, w.page, w.story);
      if (para == fw::doc::kNone && w.story == fw::LayoutStory::Cell) para = ParagraphToType(*doc, layout, 0, w.story);
      if (para == fw::doc::kNone) continue;
      fw::doc::Pos caret{para, 10};
      // The page is on screen before the typing starts.
      if (const auto at = layout.CaretAt({caret.para, caret.offset})) painter.Paint(layout, at->page);
      for (char16_t c : std::u16string_view(u"the quick brown fox jumps over the lazy dog ")) {
        const double ms = key(caret, [&] { return editor.InsertText(caret, std::u16string(1, c)); });
        if (ms >= 0) place.typing.push_back(ms);
      }
      for (int i = 0; i < 10; ++i) {
        const double ms = key(caret, [&] { return editor.Delete({caret.para, caret.offset - 1}, caret); });
        if (ms >= 0) place.backspace.push_back(ms);
      }
      // Enter, then Backspace joining the paragraphs again.
      double ms = key(caret, [&] { return editor.SplitParagraph(caret); });
      if (ms >= 0) place.enter.push_back(ms);
      ms = key(caret, [&] { return editor.JoinParagraphs(editor.PrevPara(caret.para)); });
      if (ms >= 0) place.enter.push_back(ms);
      // Formatting there, each undone: a word bold (Ctrl+B), the paragraph centered, Heading 1,
      // bullets. Styles and lists may lay out the rest of the story again (TreeEdit::Local).
      namespace edit = fw::doc::edit;
      const edit::Range word{{caret.para, 0}, {caret.para, std::min<uint32_t>(4, editor.ParaLength(caret.para))}};
      const edit::Range here{caret, caret};
      edit::ParaFormat center;
      center.jc = fw::doc::Justification::Center;
      const std::function<void(edit::TreeEdit&)> commands[] = {
          [&](edit::TreeEdit& te) { edit::ToggleRuns(te, word, edit::Toggle::Bold); },
          [&](edit::TreeEdit& te) { edit::FormatParagraphs(te, here, center); },
          [&](edit::TreeEdit& te) { edit::ApplyStyle(te, here, "heading 1"); },
          [&](edit::TreeEdit& te) { edit::ToggleList(te, here, edit::ListKind::Bullet); },
      };
      for (const auto& command : commands) {
        ms = key(caret, [&] {
          edit::TreeEdit te(editor, caret);
          command(te);
          edit::Result r;
          r.caret = caret;
          return r;
        });
        if (ms >= 0) place.format.push_back(ms);
        ms = key(caret, [&] { return editor.Undo(); });
        if (ms >= 0) place.format.push_back(ms);
      }
      // Page setup: every section's left and right margins a quarter inch wider, then undone. As
      // the document tab does it (DocView::ApplyPageChange): the first pages and 30 ms more, the
      // page on screen painted (as it was, when not laid out again yet); the rest in idle time.
      for (int step = 0; step < 2; ++step) {
        const auto shown = layout.CaretAt({caret.para, caret.offset});
        const size_t onScreen = shown ? shown->page : 0;
        const auto start = std::chrono::steady_clock::now();
        edit::Result r;
        if (step == 0) {
          const fw::doc::SectionProps& first = doc->Sections().front().props;
          edit::PageSetup wider;
          wider.left = fw::Twips(first.marginStart.Value() + 360);
          wider.right = fw::Twips(first.marginEnd.Value() + 360);
          r = edit::SetPageSetup(editor, 0, static_cast<uint32_t>(doc->Sections().size() - 1), wider);
        } else {
          r = editor.Undo();
        }
        if (!r) continue;
        layout.LayOutPages(2);
        layout.Continue(30);
        painter.Paint(layout, std::min(onScreen, layout.PageCount() - 1));
        place.page.push_back(Since(start));
        while (layout.Continue(50)) {
        }
        place.pageAll.push_back(Since(start));
      }
      places.push_back(std::move(place));
    }
  }
  g_document = nullptr;
  return true;
}


// ---- fwtool edit-check

namespace {

// The layout dump, one line of text per line of the document: what's compared.
std::vector<std::string> DumpLines(const fw::doc::layout::Layout& layout) {
  std::vector<std::string> lines;
  const std::string json = fw::WriteLayoutDump(layout.Dump("check"));
  size_t at = 0;
  while (at < json.size()) {
    size_t end = json.find('\n', at);
    if (end == std::string::npos) end = json.size();
    lines.push_back(json.substr(at, end - at));
    at = end + 1;
  }
  return lines;
}

// A random edit of the kind typing makes, mostly in the body; formatting, a field, undo and redo
// too (H3: only the paragraphs they change read differently).
void RandomEdit(fw::doc::edit::Editor& ed, const fw::doc::Document& doc, std::mt19937& rng) {
  const bool body = rng() % 8 != 0;
  std::vector<uint32_t> paras;
  for (uint32_t p = 0; p < doc.Paragraphs().size(); ++p)
    if ((doc.Para(p).story == doc.MainStory()) == body) paras.push_back(p);
  if (paras.empty()) return;
  const uint32_t para = paras[rng() % paras.size()];
  const auto len = static_cast<uint32_t>(doc.Para(para).text.size());
  const fw::doc::Pos at{para, len ? static_cast<uint32_t>(rng() % (len + 1)) : 0};
  static const char16_t* const kTexts[] = {u"x", u" ", u"word ", u"a few more words to wrap ",
                                           u"a long run of text, long enough to take a line or two by itself and push "
                                           u"what follows down the page, and then some more of it "};
  namespace edit = fw::doc::edit;
  const uint32_t section = edit::SectionOf(doc, para), sections = static_cast<uint32_t>(doc.Sections().size());
  auto twips = [&](int32_t from, int32_t span) { return fw::Twips(from + static_cast<int32_t>(rng() % static_cast<uint32_t>(span))); };
  switch (rng() % 18) {
    case 0: case 1: case 2: case 3: ed.InsertText(at, kTexts[rng() % 5]); break;
    case 4: case 5: {
      const uint32_t to = std::min(len, at.offset + 1 + static_cast<uint32_t>(rng() % 60));
      if (to > at.offset) ed.Delete(at, {para, to});
      break;
    }
    case 6: ed.SplitParagraph(at); break;
    case 7: ed.JoinParagraphs(para); break;
    case 8: ed.InsertText(at, u"\r"); break;
    case 9: {
      fw::doc::edit::PropEdit p;
      if (rng() % 2) p.Set("keepNext");
      else p.Remove("keepNext");
      ed.SetParaProps(para, para, p);
      break;
    }
    case 10: {  // page setup (D3): the paragraph's section, or the whole document
      const bool all = rng() % 4 == 0;
      edit::PageSetup p;
      switch (rng() % 4) {
        case 0: p.landscape = rng() % 2 == 0; break;
        case 1: p.left = twips(720, 1440); p.right = twips(720, 1440); break;
        case 2: p.top = twips(720, 1440); p.bottom = twips(720, 1440); break;
        default: p.width = twips(10000, 6000); break;
      }
      edit::SetPageSetup(ed, all ? 0 : section, all ? sections - 1 : section, p);
      break;
    }
    case 11:
      switch (rng() % 3) {
        case 0: {
          edit::ColumnSetup c;
          c.count = 1 + static_cast<int32_t>(rng() % 3);
          c.line = rng() % 2 == 0;
          edit::SetColumns(ed, section, section, c);
          break;
        }
        case 1: edit::SetSectionStart(ed, section, static_cast<fw::doc::SectionStart>(rng() % 4)); break;
        default: {
          edit::Hyphenation h;
          h.automatic = rng() % 2 == 0;
          edit::SetHyphenation(ed, h);
          break;
        }
      }
      break;
    case 12:
      switch (rng() % 3) {
        case 0: edit::InsertSectionBreak(ed, at, static_cast<fw::doc::SectionStart>(rng() % 4)); break;
        case 1: if (sections > 1) edit::DeleteSectionBreak(ed, static_cast<uint32_t>(rng() % (sections - 1))); break;
        default: edit::InsertBreak(ed, at, rng() % 2 == 0); break;
      }
      break;
    case 13: {  // a few letters bold or italic
      const edit::Range range{at, {para, std::min(len, at.offset + 1 + static_cast<uint32_t>(rng() % 20))}};
      edit::TreeEdit te(ed, at);
      edit::ToggleRuns(te, range, rng() % 2 ? edit::Toggle::Bold : edit::Toggle::Italic);
      break;
    }
    case 14: {  // the paragraph centered, or back at the start
      edit::ParaFormat f;
      f.jc = rng() % 2 ? fw::doc::Justification::Center : fw::doc::Justification::Start;
      edit::TreeEdit te(ed, at);
      edit::FormatParagraphs(te, {at, at}, f);
      break;
    }
    case 15: {  // a heading, or the normal style
      edit::TreeEdit te(ed, at);
      edit::ApplyStyle(te, {at, at}, rng() % 2 ? "heading 1" : "Normal");
      break;
    }
    case 16: {  // bullets or numbers, on or off
      edit::TreeEdit te(ed, at);
      edit::ToggleList(te, {at, at}, rng() % 2 ? edit::ListKind::Bullet : edit::ListKind::Number);
      break;
    }
    default:
      switch (rng() % 4) {
        case 0: edit::InsertField(ed, at, " PAGE ", u"1"); break;
        case 1: ed.Redo(); break;
        default: ed.Undo(); break;
      }
      break;
  }
}

// A step that says anything may read differently: the model's caches are made again from nothing,
// so a layout from scratch after it doesn't see what an edit left in them.
struct EverythingAgain : fw::doc::edit::CustomStep {
  void Undo() override {}
  void Redo() override {}
};

}  // namespace

bool CheckEdits(const std::filesystem::path& document, int edits, unsigned seed, EditCheck& result, std::string& error) {
  std::unique_ptr<fw::doc::Document> doc = OpenModel(document, nullptr, error);
  if (!doc) return false;
  g_document = doc.get();
  fw::text::Fonts& fonts = SharedFonts();
  fonts.ClearSubstitutions();
  fonts.UseDocumentFonts(fw::doc::layout::EmbeddedFonts(*doc));
  {
    fw::doc::layout::Layout layout(*doc, fonts);
    layout.LayOutAll();
    fw::doc::edit::Editor editor(*doc);
    if (!editor.CanEdit()) {
      g_document = nullptr;
      result.skipped = true;
      return true;
    }
    int changes = 0;
    editor.OnChange([&](const fw::doc::edit::Change& c) {
      layout.Edited(c);
      ++changes;
    });
    std::mt19937 rng(seed);
    for (int i = 0; i < edits && result.mismatch.empty(); ++i) {
      RandomEdit(editor, *doc, rng);
      // Laid out a bit at a time, as a view does: the pages on screen, then idle time.
      if (rng() % 2) layout.LayOutPages(1 + rng() % 8);
      else layout.Continue(1);
      if (i % 10 != 9 && i + 1 != edits) continue;
      layout.LayOutAll();
      const std::vector<std::string> a = DumpLines(layout);
      const int counted = changes;
      editor.RecordStep(u"Check", std::make_shared<EverythingAgain>());
      changes = counted;
      fw::doc::layout::Layout fresh(*doc, fonts);
      fresh.LayOutAll();
      ++result.checks;
      const std::vector<std::string> b = DumpLines(fresh);
      if (a == b && layout.PageCount() == fresh.PageCount()) continue;
      size_t k = 0;
      while (k < a.size() && k < b.size() && a[k] == b[k]) ++k;
      result.mismatch = "after edit " + std::to_string(i + 1) + ": " + std::to_string(layout.PageCount()) + " pages, from scratch " +
                        std::to_string(fresh.PageCount()) + "; first difference:\n      " + (k < a.size() ? a[k] : "(end)") +
                        "\n    from scratch:\n      " + (k < b.size() ? b[k] : "(end)");
    }
    result.edits = changes;
  }
  g_document = nullptr;
  return true;
}

}  // namespace fwtool
