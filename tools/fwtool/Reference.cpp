// Word's reference renders: the manifest, and the lines in Word's PDFs (plan §10).
#include "Reference.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <unordered_map>

#include <fpdf_edit.h>
#include <fpdf_structtree.h>
#include <fpdf_text.h>
#include <fpdfview.h>

#include "core/Json.h"
#include "core/Strings.h"

namespace fwtool {
namespace {

using fw::LayoutStory;

bool ReadFile(const std::filesystem::path& path, std::string& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return !in.bad();
}


std::wstring ElementType(FPDF_STRUCTELEMENT e, bool raw) {
  wchar_t buf[64] = {};
  unsigned long n = raw ? FPDF_StructElement_GetRawType(e, reinterpret_cast<FPDF_WCHAR*>(buf), sizeof(buf))
                        : FPDF_StructElement_GetType(e, buf, sizeof(buf));
  return n > 2 && n <= sizeof(buf) ? std::wstring(buf) : std::wstring();
}

// What the structure tree says about a marked-content id: which container its text is in (lines
// never cross one) and which story. Word tags table cells TD and TH, notes Footnote and Endnote,
// and text boxes Textbox, role-mapped to Note and Sect; equations Formula.
struct Marked {
  int container = 0;  // 0: the page itself
  LayoutStory story = LayoutStory::Body;
  // In an equation: left out, as Featherweight's layout has an equation as one object without
  // lines, while Word's PDF puts each script on its own baseline (they read as lines of their own).
  bool formula = false;
};

void Walk(FPDF_STRUCTELEMENT e, Marked m, int& nextContainer, std::unordered_map<int, Marked>& out, int depth) {
  if (depth > 64) return;
  std::wstring raw = ElementType(e, true), type = ElementType(e, false);
  if (raw == L"Footnote" || raw == L"Endnote" || type == L"Note") {
    m = {nextContainer++, raw == L"Endnote" ? LayoutStory::Endnote : LayoutStory::Footnote};
  } else if (raw == L"Textbox" || type == L"Sect" || type == L"Div" || type == L"Aside") {
    m.container = nextContainer++;
    if (m.story != LayoutStory::Footnote && m.story != LayoutStory::Endnote) m.story = LayoutStory::TextBox;
  } else if (type == L"TD" || type == L"TH") {
    m.container = nextContainer++;
    if (m.story == LayoutStory::Body) m.story = LayoutStory::Cell;
  } else if (type == L"Formula") {
    m.formula = true;
  }
  for (int i = 0, n = FPDF_StructElement_GetMarkedContentIdCount(e); i < n; ++i) {
    int mcid = FPDF_StructElement_GetMarkedContentIdAtIndex(e, i);
    if (mcid >= 0) out[mcid] = m;
  }
  for (int i = 0, n = FPDF_StructElement_CountChildren(e); i < n; ++i)
    if (FPDF_STRUCTELEMENT child = FPDF_StructElement_GetChildAtIndex(e, i))
      Walk(child, m, nextContainer, out, depth + 1);
}

struct ObjectInfo {
  bool artifact = false;  // headers and footers: Word marks them as pagination artifacts
  bool plain = false;     // an artifact without properties: Word's line numbers (w:lnNumType)
  int mcid = -1;
};

ObjectInfo InfoOf(FPDF_PAGEOBJECT obj) {
  ObjectInfo info;
  if (!obj) return info;
  info.mcid = FPDFPageObj_GetMarkedContentID(obj);
  for (int i = 0, n = FPDFPageObj_CountMarks(obj); i < n; ++i) {
    wchar_t name[32] = {};
    unsigned long len = 0;
    if (FPDFPageObjMark_GetName(FPDFPageObj_GetMark(obj, i), reinterpret_cast<FPDF_WCHAR*>(name), sizeof(name), &len) &&
        std::wstring_view(name) == L"Artifact") {
      info.artifact = true;
      info.plain = FPDFPageObjMark_CountParams(FPDFPageObj_GetMark(obj, i)) == 0;
    }
  }
  return info;
}

bool IsRtl(unsigned c) { return (c >= 0x0590 && c <= 0x08FF) || (c >= 0xFB1D && c <= 0xFDFF) || (c >= 0xFE70 && c <= 0xFEFF); }

// Builds lines from the characters of one page, in the order Word drew them.
class LineBuilder {
 public:
  LineBuilder(fw::LayoutPage& page) : page_(page) {}

  // A gap this wide, in ems, is a space: narrower than any font's space, wider than kerning.
  static constexpr double kSpace = 0.18;

  void Gap() {
    if (open_) space_ = true;
  }

  // `rotation`: the character's quarter turns clockwise on the page, as PDFium measures them (text
  // boxes and table cells with vertical text). Its line is found in its own direction: the coordinates are turned back first.
  void Add(unsigned c, int container, bool artifact, LayoutStory story, double pageX, double pageY, double size,
           const FS_RECTF& pageBox, int rotation) {
    double x = pageX, y = pageY;
    FS_RECTF box = pageBox;
    switch (rotation) {
      case 1: x = -pageY, y = pageX, box.left = -pageBox.top, box.right = -pageBox.bottom; break;
      case 2: x = -pageX, y = -pageY, box.left = -pageBox.right, box.right = -pageBox.left; break;
      case 3: x = pageY, y = -pageX, box.left = pageBox.bottom, box.right = pageBox.top; break;
      default: break;
    }
    // The same line: same container and direction, the same baseline give or take a superscript,
    // and not back to the left (unless the text runs right to left).
    double tolerance = 0.5 * std::max(size, size_);
    bool same = open_ && container == container_ && artifact == artifact_ && rotation == rotation_ &&
                std::fabs(y - y_) <= tolerance && (x >= lastX_ - tolerance || IsRtl(c) || rtl_);
    if (!same) {
      Flush();
      open_ = true;
      container_ = container;
      artifact_ = artifact;
      story_ = story;
      rotation_ = rotation;
      pageX_ = pageX;
      pageY_ = pageY;
      y_ = y;
      size_ = size;
      left_ = box.left;
      right_ = box.right;
    } else if ((space_ || x - right_ > kSpace * std::max(size, lastSize_)) && text_.back() != L' ' && c != L' ') {
      // A gap: a tab, or a space Word didn't draw (after a superscript note mark, PDFium reports
      // a line break, not a space).
      text_ += L' ';
    }
    space_ = false;
    lastSize_ = size;
    if (size > size_ + 0.01) {  // the baseline is the largest text's, not a superscript's
      size_ = size;
      y_ = y;
    }
    rtl_ = IsRtl(c);
    lastX_ = x;
    text_ += c == 2 ? L'-' : static_cast<wchar_t>(c);  // 2: PDFium's hyphen at a line end
    if (c != L' ') {
      left_ = std::min(left_, static_cast<double>(box.left));
      right_ = std::max(right_, static_cast<double>(box.right));
    }
  }

  void Flush() {
    if (!open_) return;
    open_ = false;
    std::wstring text = fw::Trim(text_);
    text_.clear();
    space_ = false;
    if (text.empty()) return;
    fw::LayoutLine line;
    // Word doesn't say which pagination artifacts are headers and which footers: top or bottom.
    const double y = rotation_ ? pageY_ : y_;
    line.story = artifact_ && story_ != LayoutStory::LineNumber ? (y > page_.height / 2 ? LayoutStory::Header : LayoutStory::Footer)
                                                                 : story_;
    // A turned line is placed where it starts.
    line.x = rotation_ ? pageX_ : left_;
    line.baseline = page_.height - y;
    line.width = std::max(0.0, right_ - left_);
    line.size = size_;
    line.text = std::move(text);
    page_.lines.push_back(std::move(line));
  }

 private:
  fw::LayoutPage& page_;
  bool open_ = false, space_ = false, artifact_ = false, rtl_ = false;
  int container_ = 0, rotation_ = 0;
  LayoutStory story_ = LayoutStory::Body;
  double y_ = 0, size_ = 0, lastSize_ = 0, lastX_ = 0, left_ = 0, right_ = 0, pageX_ = 0, pageY_ = 0;
  std::wstring text_;
};

void ReadPage(FPDF_PAGE page, fw::LayoutPage& out) {
  out.width = FPDF_GetPageWidthF(page);
  out.height = FPDF_GetPageHeightF(page);

  std::unordered_map<int, Marked> marked;
  if (FPDF_STRUCTTREE tree = FPDF_StructTree_GetForPage(page)) {
    int nextContainer = 1;
    for (int i = 0, n = FPDF_StructTree_CountChildren(tree); i < n; ++i)
      if (FPDF_STRUCTELEMENT e = FPDF_StructTree_GetChildAtIndex(tree, i)) Walk(e, {}, nextContainer, marked, 0);
    FPDF_StructTree_Close(tree);
  }

  FPDF_TEXTPAGE text = FPDFText_LoadPage(page);
  if (!text) return;
  std::unordered_map<FPDF_PAGEOBJECT, ObjectInfo> objects;
  LineBuilder lines(out);
  for (int i = 0, n = FPDFText_CountChars(text); i < n; ++i) {
    unsigned c = FPDFText_GetUnicode(text, i);
    if (FPDFText_IsGenerated(text, i) == 1) {
      if (c == L' ') lines.Gap();
      continue;  // PDFium's line breaks: lines are found here
    }
    if (c == L'\r' || c == L'\n' || c == 0) continue;
    FPDF_PAGEOBJECT obj = FPDFText_GetTextObject(text, i);
    auto it = objects.find(obj);
    if (it == objects.end()) it = objects.emplace(obj, InfoOf(obj)).first;
    const ObjectInfo& info = it->second;
    Marked m;
    if (!info.artifact)
      if (auto found = marked.find(info.mcid); found != marked.end()) m = found->second;
    if (m.formula) {
      lines.Gap();
      continue;
    }
    double x = 0, y = 0;
    FPDFText_GetCharOrigin(text, i, &x, &y);
    FS_RECTF box{static_cast<float>(x), static_cast<float>(y), static_cast<float>(x), static_cast<float>(y)};
    FPDFText_GetLooseCharBox(text, i, &box);
    // Quarter turns, from the character's angle (radians; -1 when unknown): at a quarter turn
    // the text runs down the page.
    const float angle = FPDFText_GetCharAngle(text, i);
    const int rotation = angle < 0 ? 0 : static_cast<int>(std::lround(angle / 1.5707963)) % 4;
    if (info.artifact && info.plain) m.story = LayoutStory::LineNumber;
    lines.Add(c, info.artifact ? (info.plain ? -2 : -1) : m.container, info.artifact, m.story, x, y, FPDFText_GetFontSize(text, i), box,
              rotation);
  }
  lines.Flush();
  FPDFText_ClosePage(text);
  // Artifacts without properties: line numbers (w:lnNumType), digits beside the page's text, and
  // anything else (a chart's text, a shape in a header), which is a header or footer as before.
  double left = 1e9, right = -1e9;
  for (const fw::LayoutLine& l : out.lines)
    if (l.story != LayoutStory::LineNumber && l.story != LayoutStory::Header && l.story != LayoutStory::Footer) {
      left = std::min(left, l.x);
      right = std::max(right, l.x + l.width);
    }
  for (fw::LayoutLine& l : out.lines) {
    if (l.story != LayoutStory::LineNumber) continue;
    const bool digits = std::all_of(l.text.begin(), l.text.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; });
    if (!digits || (l.x + l.width > left + 0.5 && l.x < right - 0.5))
      l.story = l.baseline < out.height / 2 ? LayoutStory::Header : LayoutStory::Footer;
  }
}

}  // namespace

// pdfium.dll is delay-loaded, as in the app: only the commands that read PDFs load it, and only
// the copy next to fwtool.exe.
bool InitPdfium(std::string& error) {
  static int state = 0;  // 0 not tried, 1 ready, -1 missing
  if (state == 0) {
    wchar_t exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::filesystem::path dll = std::filesystem::path(std::wstring(exe, n)).parent_path() / L"pdfium.dll";
    if (LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH)) {
      FPDF_LIBRARY_CONFIG config{};
      config.version = 2;
      FPDF_InitLibraryWithConfig(&config);
      state = 1;
    } else {
      state = -1;
    }
  }
  if (state < 0) error = "pdfium.dll isn't next to fwtool.exe";
  return state > 0;
}

bool IsWordDocument(std::string_view name) {
  std::wstring ext = fw::ToLower(std::filesystem::path(fw::Utf8ToWide(name)).extension().wstring());
  return ext == L".docx" || ext == L".docm" || ext == L".dotx" || ext == L".dotm" || ext == L".doc";
}

std::optional<ReferenceSet> ReadReferences(const std::filesystem::path& folder, std::string& error) {
  ReferenceSet set;
  set.folder = folder.filename() == L"manifest.json" ? folder.parent_path() : folder;
  // The private corpus folder itself (FW_CORPUS) has its renders in reference\.
  std::error_code ec;
  if (!std::filesystem::exists(set.folder / L"manifest.json", ec) &&
      std::filesystem::exists(set.folder / L"reference" / L"manifest.json", ec))
    set.folder /= L"reference";
  std::string text;
  if (!ReadFile(set.folder / L"manifest.json", text)) {
    error = "no manifest.json in " + fw::WideToUtf8(set.folder.wstring());
    return std::nullopt;
  }
  auto manifest = fw::ParseJson(text);
  const fw::JsonValue* files = manifest ? manifest->Find("files") : nullptr;
  if (!files || files->type != fw::JsonValue::Type::Array) {
    error = fw::WideToUtf8((set.folder / L"manifest.json").wstring()) + " isn't a reference manifest";
    return std::nullopt;
  }
  // Sources are relative to the corpus: the one the manifest names, while it's still there, or
  // the reference folder's parent.
  std::filesystem::path corpus = fw::Utf8ToWide(manifest->StringOr("corpus"));
  set.corpus = !corpus.empty() && std::filesystem::is_directory(corpus, ec)
                   ? corpus
                   : std::filesystem::absolute(set.folder).parent_path();
  for (const fw::JsonValue& f : files->array) {
    ReferenceEntry e;
    e.source = f.StringOr("source");
    if (!IsWordDocument(e.source)) continue;
    e.document = set.corpus / fw::Utf8ToWide(e.source);
    std::string pdf = f.StringOr("pdf");
    if (!pdf.empty()) e.pdf = set.folder / fw::Utf8ToWide(pdf);
    if (const fw::JsonValue* pages = f.Find("pages"); pages && pages->type == fw::JsonValue::Type::Number)
      e.pages = static_cast<int>(pages->number);
    e.error = f.StringOr("error");
    if (e.pdf.empty() && e.error.empty()) e.error = "no PDF";
    set.entries.push_back(std::move(e));
  }
  return set;
}

std::filesystem::path DumpPathFor(const std::filesystem::path& dumps, std::string_view source) {
  return dumps / fw::Utf8ToWide(std::string(source) + ".layout.json");
}

std::optional<fw::LayoutDump> ReadPdfLines(const std::filesystem::path& pdf, std::string& error) {
  if (!InitPdfium(error)) return std::nullopt;
  std::string bytes;
  if (!ReadFile(pdf, bytes)) {
    error = "can't read " + fw::WideToUtf8(pdf.wstring());
    return std::nullopt;
  }
  FPDF_DOCUMENT doc = FPDF_LoadMemDocument64(bytes.data(), bytes.size(), nullptr);
  if (!doc) {
    error = "PDFium can't open " + fw::WideToUtf8(pdf.wstring()) + " (error " + std::to_string(FPDF_GetLastError()) + ")";
    return std::nullopt;
  }
  fw::LayoutDump dump;
  dump.producer = "Word, read by fwtool from " + fw::WideToUtf8(pdf.filename().wstring());
  for (int i = 0, n = FPDF_GetPageCount(doc); i < n; ++i) {
    dump.pages.emplace_back();
    if (FPDF_PAGE page = FPDF_LoadPage(doc, i)) {
      ReadPage(page, dump.pages.back());
      FPDF_ClosePage(page);
    }
  }
  FPDF_CloseDocument(doc);
  return dump;
}

}  // namespace fwtool
