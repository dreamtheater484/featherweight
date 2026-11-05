#include "word/DocView.h"

#include <commdlg.h>
#include <shlwapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>

#include "Commands.h"
#include "Util.h"
#include "core/Strings.h"
#include "doc/docx/Blank.h"
#include "doc/docx/Docx.h"
#include "doc/edit/Clip.h"
#include "draw/D2DPainter.h"
#include "draw/PrintJob.h"
#include "pkg/Package.h"
#include "pkg/Snapshot.h"
#include "pkg/Zip.h"
#include "ui/Anim.h"
#include "ui/Icons.h"
#include "ui/Menu.h"
#include "ui/StatusBar.h"
#include "ui/SwapChain.h"
#include "ui/Theme.h"
#include "ui/Toolbar.h"
#include "word/PicturePanel.h"

using namespace gfx;
namespace layout = fw::doc::layout;

namespace {

D2D1_RECT_F Intersect(const D2D1_RECT_F& a, const D2D1_RECT_F& b) {
  return {std::max(a.left, b.left), std::max(a.top, b.top), std::min(a.right, b.right), std::min(a.bottom, b.bottom)};
}
bool Empty(const D2D1_RECT_F& r) { return r.right <= r.left || r.bottom <= r.top; }

std::wstring Wide(std::string_view s) { return fw::Utf8ToWide(std::string(s)); }

// Night reading, as PDFs have it: brightness inverted, hues kept, into a comfortable range.
draw::Color Invert(draw::Color c) {
  const float lo = 0x24 / 255.f, hi = 0xE6 / 255.f, k = hi - lo;
  const float H[3][3] = {{-0.574f, 1.430f, 0.144f}, {0.426f, 0.430f, 0.144f}, {0.426f, 1.430f, -0.856f}};
  auto row = [&](int i) { return std::clamp(hi - k * (H[i][0] * c.r + H[i][1] * c.g + H[i][2] * c.b), 0.f, 1.f); };
  return {row(0), row(1), row(2), c.a};
}

// A painter that draws through another with every color inverted (pictures stay as they are).
class InvertingPainter : public draw::Painter {
 public:
  explicit InvertingPainter(draw::Painter& inner) : inner_(inner) {}
  void Save() override { inner_.Save(); }
  void Restore() override { inner_.Restore(); }
  void Transform(const draw::Matrix& m) override { inner_.Transform(m); }
  draw::Matrix CurrentTransform() const override { return inner_.CurrentTransform(); }
  void ClipRect(const draw::Rect& r) override { inner_.ClipRect(r); }
  void ClipPath(const draw::Path& p) override { inner_.ClipPath(p); }
  void BeginGroup(float opacity) override { inner_.BeginGroup(opacity); }
  void FillPath(const draw::Path& p, const draw::Brush& b) override { inner_.FillPath(p, Map(b)); }
  void StrokePath(const draw::Path& p, const draw::Pen& pen) override {
    draw::Pen q = pen;
    q.brush = Map(pen.brush);
    inner_.StrokePath(p, q);
  }
  void FillRect(const draw::Rect& r, const draw::Brush& b) override { inner_.FillRect(r, Map(b)); }
  void DrawGlyphs(draw::Point origin, const draw::GlyphRun& run, const draw::Brush& b) override {
    inner_.DrawGlyphs(origin, run, Map(b));
  }
  void DrawBitmap(const draw::Bitmap& bitmap, const draw::Rect& dest, const draw::Rect* src, float opacity) override {
    inner_.DrawBitmap(bitmap, dest, src, opacity);
  }

 private:
  static draw::Brush Map(draw::Brush b) {
    b.color = Invert(b.color);
    for (auto& s : b.stops) s.color = Invert(s.color);
    return b;
  }
  draw::Painter& inner_;
};

// Characters of the model's text that aren't text the reader sees (doc/model/Document.h).
bool Invisible(char16_t c) {
  return c == 0x13 || c == 0x14 || c == 0x15 || c == 0x02 || c == 0x03 || c == 0x04 || c == 0x05 || c == 0xFFFC ||
         c == 0x00AD;
}

// Text of a deletion (w:del, w:moveFrom): not laid out, so not shown, found or copied.
bool Deleted(fw::doc::Node revision) {
  return revision && (revision.Is(fw::doc::Name("del")) || revision.Is(fw::doc::Name("moveFrom")));
}

// The color tracked changes are marked in, one per author as Word does.
D2D1_COLOR_F ChangeColor(size_t author) {
  static const uint32_t kColors[] = {0xC0392B, 0x2E6FD8, 0x1E8A4C, 0x8E44AD, 0xD35400, 0x16A085, 0xB03A6F, 0x7F6A00};
  return Rgb(kColors[author % (sizeof(kColors) / sizeof(kColors[0]))]);
}

bool WordChar(wchar_t c) { return IsCharAlphaNumericW(c) || c == L'\'' || c == 0x2019 || c == L'_'; }

// The first quoted argument of a field code (HYPERLINK "https://..."), and its \l anchor.
std::wstring HyperlinkFieldTarget(std::u16string_view code) {
  std::wstring c(code.begin(), code.end());
  auto quoted = [&](size_t from) -> std::wstring {
    size_t a = c.find(L'"', from);
    if (a == std::wstring::npos) return {};
    size_t b = c.find(L'"', a + 1);
    return b == std::wstring::npos ? std::wstring() : c.substr(a + 1, b - a - 1);
  };
  size_t l = c.find(L"\\l");
  std::wstring url = quoted(0);
  if (l != std::wstring::npos && (url.empty() || c.find(L'"') > l)) return L"#" + quoted(l);
  if (l != std::wstring::npos) {
    std::wstring anchor = quoted(l);
    if (!anchor.empty() && url.empty()) return L"#" + anchor;
  }
  return url;
}

// A plain message for a file that isn't a Word document Featherweight can read.
std::wstring OpenError(const std::wstring& name, const fw::pkg::Status& st) {
  using Code = fw::pkg::Status::Code;
  switch (st.code) {
    case Code::IoError: return name + L" couldn't be opened — the file couldn't be read.";
    case Code::TooLarge: return name + L" couldn't be opened — it goes beyond the limits Featherweight sets for safety.";
    case Code::Rejected: return name + L" couldn't be opened — it contains something Featherweight refuses for safety.";
    case Code::Unsupported: return name + L" couldn't be opened — it uses something Featherweight doesn't support yet.";
    default: return name + L" couldn't be opened — it seems to be damaged or isn't a Word document.";
  }
}

}  // namespace

DocView::DocView(ViewHost& host, std::wstring path) : PagedView(host), path_(std::move(path)) {}

std::unique_ptr<DocView> DocView::NewDocument(ViewHost& host, int number) {
  auto view = std::make_unique<DocView>(host, L"");
  const DocFile::Setup& setup = DocFile::Config();
  view->bytes_ = fw::doc::docx::BlankDocx(
      {fw::WideToUtf8(setup.language.empty() ? L"en-US" : setup.language), setup.a4, fw::WideToUtf8(setup.user)});
  view->file_.New(number);
  return view;
}

std::unique_ptr<DocView> DocView::FromBytes(ViewHost& host, std::vector<uint8_t> bytes, std::wstring title,
                                            std::wstring suggested, bool unsaved) {
  auto view = std::make_unique<DocView>(host, L"");
  view->bytes_ = std::move(bytes);
  view->file_.Copy(std::move(title), std::move(suggested), unsaved);
  return view;
}

DocView::~DocView() {
  StopBlinking();
  pictures_.reset();
  editor_.reset();
  drawings_.clear();  // pictures read from the document's package
  images_.reset();
  pagination_.reset();
  fonts_.reset();
  doc_.reset();
}

std::wstring DocView::Title() const { return path_.empty() ? file_.Title() : fw::FileNameOf(path_); }

// ---------------------------------------------------------------------------
// Loading

OpenResult DocView::Load(const std::wstring&, std::wstring& error) {
  const std::wstring name = Title();
  // Password-protected Office files aren't zip packages but OLE compound files with the
  // encrypted package inside.
  if (bytes_.empty()) {
    std::string head;
    HANDLE f = CreateFileW(path_.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
      char buf[8] = {};
      DWORD got = 0;
      if (ReadFile(f, buf, sizeof(buf), &got, nullptr)) head.assign(buf, got);
      CloseHandle(f);
    }
    if (head == std::string("\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1", 8)) {
      error = name + L" couldn't be opened — it's protected with a password, and Featherweight doesn't open "
                     L"protected documents yet.";
      return OpenResult::Failed;
    }
  }

  fw::pkg::Status st;
  fw::pkg::Snapshot::Options options;
  wchar_t temp[MAX_PATH + 1] = {};
  if (GetTempPathW(MAX_PATH + 1, temp)) options.recoveryDir = std::wstring(temp) + L"Featherweight";
  auto snapshot = bytes_.empty() ? fw::pkg::Snapshot::Open(path_, options, st)
                                 : fw::pkg::Snapshot::FromBytes(std::move(bytes_));
  bytes_.clear();
  std::unique_ptr<fw::pkg::Package> package =
      snapshot ? fw::pkg::Package::Open(std::move(snapshot), fw::pkg::Limits::Default(), st) : nullptr;
  std::unique_ptr<fw::doc::Document> doc = package ? fw::doc::docx::Read(std::move(package), st) : nullptr;
  if (!doc) {
    error = OpenError(name, st);
    return OpenResult::Failed;
  }
  doc_ = std::move(doc);
  if (!path_.empty()) file_.Opened(path_, doc_->Package().GetSnapshot().GetStamp());

  // Fonts this PC doesn't have: the family Word draws instead, from the document's font table.
  // The document outlives the fonts (members are destroyed in reverse order).
  const fw::doc::Document* model = doc_.get();
  fonts_ = std::make_unique<fw::text::Fonts>(
      [model](std::wstring_view family) { return layout::SubstituteFamily(*model, family); });
  drawings_.resize(doc_->Drawings().size());
  images_ = std::make_shared<draw::ImageCache>();
  if (const fw::doc::Part* main = doc_->FindPart(fw::doc::PartKind::Document)) {
    theme_ = draw::PackageResources::FindTheme(doc_->Package(), main->name);
    colorMap_ = draw::PackageResources::FindColorMap(doc_->Package(), main->name);
  }
  pagination_ = std::make_unique<layout::Layout>(*doc_, *fonts_);
  pagination_->LayOutPages(1);
  revision_ = pagination_->Revision();
  TakeNewPages();
  CollectAnnotations();

  // Editing, when the model can write the document back as it was.
  clipSource_ = GetTickCount64() ^ (static_cast<uint64_t>(GetCurrentProcessId()) << 32) ^
                static_cast<uint64_t>(reinterpret_cast<uintptr_t>(this));
  auto editor = std::make_unique<fw::doc::edit::Editor>(*doc_);
  if (editor->CanEdit()) {
    editor_ = std::move(editor);
    editor_->OnChange([this](const fw::doc::edit::Change& c) { OnEdited(c); });
    format_ = std::make_unique<DocFormat>(
        *editor_, [this] { return FormatTarget(); }, [this](const DocFormat::Done& d) { AfterFormat(d); });
    pictures_ = std::make_unique<fw::doc::edit::Pictures>(*editor_);
    format_->insertPicture = [this] { Execute(CmdInsertPicture); };
    const std::vector<uint32_t> main = doc_->StoryParagraphs(doc_->MainStory());
    if (!main.empty()) anchor_ = focus_ = {main.front(), 0};
  }

  Relayout(nullptr);
  return OpenResult::Ok;
}

void DocView::CollectAnnotations() {
  // Comments, for their highlight and tooltip; hyperlink fields, for clicks.
  comments_.clear();
  links_.clear();
  changes_.clear();
  for (const auto& c : doc_->Comments()) {
    if (c.start.para == fw::doc::kNone) continue;
    Comment cm;
    cm.from = {c.start.para, c.start.offset};
    cm.to = c.end.para != fw::doc::kNone ? TextPos{c.end.para, c.end.offset} : cm.from;
    if (cm.to < cm.from) std::swap(cm.from, cm.to);
    std::wstring text;
    if (c.story != fw::doc::kNone) {
      for (uint32_t p : doc_->StoryParagraphs(c.story)) {
        std::wstring t;
        std::vector<uint32_t> map;
        VisibleText(p, t, map);
        if (!text.empty() && !t.empty()) text += L"\n";
        text += t;
      }
    }
    if (text.size() > 400) text = text.substr(0, 400) + L"…";
    cm.text = (c.author.empty() ? std::wstring() : Wide(c.author) + L": ") + text;
    comments_.push_back(std::move(cm));
  }
  for (uint32_t s = 0; s < doc_->Stories().size(); ++s) {
    if (doc_->Stories()[s].kind == fw::doc::StoryKind::Comment) continue;
    for (const auto& f : doc_->Fields(s)) {
      if (f.Type() != u"HYPERLINK" || f.separator.para == fw::doc::kNone || f.end.para == fw::doc::kNone) continue;
      std::wstring target = HyperlinkFieldTarget(f.code);
      if (!target.empty()) links_.push_back({{f.separator.para, f.separator.offset}, {f.end.para, f.end.offset}, target});
    }
  }

  // Tracked changes in the text (not formatting changes, which show nothing in Simple Markup).
  std::vector<std::string_view> authors;
  for (uint32_t s = 0; s < doc_->Stories().size(); ++s) {
    if (doc_->Stories()[s].kind == fw::doc::StoryKind::Comment) continue;
    for (const auto& r : doc_->Revisions(s)) {
      using Kind = fw::doc::RevisionKind;
      if (r.kind != Kind::Insertion && r.kind != Kind::Deletion && r.kind != Kind::MoveFrom && r.kind != Kind::MoveTo)
        continue;
      if (r.start.para == fw::doc::kNone || r.end.para == fw::doc::kNone) continue;
      Change ch;
      ch.from = {r.start.para, r.start.offset};
      ch.to = {r.end.para, r.end.offset};
      if (ch.to < ch.from) std::swap(ch.from, ch.to);
      if (ch.from == ch.to) continue;
      ch.deletion = r.kind == Kind::Deletion || r.kind == Kind::MoveFrom;
      auto who = std::find(authors.begin(), authors.end(), r.author);
      if (who == authors.end()) who = authors.insert(authors.end(), r.author);
      ch.author = static_cast<size_t>(who - authors.begin());
      std::wstring by = r.author.empty() ? std::wstring() : L" by " + Wide(r.author);
      if (ch.deletion) {
        std::wstring gone;
        for (uint32_t p = ch.from.para; p <= ch.to.para && gone.size() < 200; ++p) {
          const std::u16string& t = doc_->Para(p).text;
          uint32_t a = p == ch.from.para ? ch.from.offset : 0;
          uint32_t b = p == ch.to.para ? ch.to.offset : static_cast<uint32_t>(t.size());
          for (uint32_t i = a; i < b && i < t.size(); ++i)
            if (!Invisible(t[i])) gone += static_cast<wchar_t>(t[i]);
          if (p != ch.to.para) gone += L' ';
        }
        if (gone.size() > 200) gone = gone.substr(0, 200) + L"…";
        ch.text = std::wstring(r.kind == Kind::MoveFrom ? L"Moved away" : L"Deleted") + by +
                  (gone.empty() ? std::wstring() : L": “" + gone + L"”");
      } else {
        ch.text = std::wstring(r.kind == Kind::MoveTo ? L"Moved here" : L"Inserted") + by;
      }
      changes_.push_back(std::move(ch));
    }
  }

}

void DocView::VisibleText(uint32_t para, std::wstring& text, std::vector<uint32_t>& map) const {
  const std::u16string& s = doc_->Para(para).text;
  for (const auto& run : doc_->Runs(para)) {
    if (run.fieldCode || Deleted(run.revision)) continue;
    if (run.run && doc_->ResolvedRun(para, run.run).vanish) continue;
    for (uint32_t i = run.start; i < run.end && i < s.size(); ++i) {
      char16_t c = s[i];
      if (Invisible(c)) continue;
      if (c == 0x000B) c = u'\n';
      else if (c == 0x2011) c = u'-';
      else if (c == 0x00A0) c = u' ';
      else if (c == 0x000C || c == 0x000E) continue;
      text += static_cast<wchar_t>(c);
      map.push_back(i);
    }
  }
}

bool DocView::TakeNewPages() {
  const size_t n = pagination_->PageCount();
  if (n == 0) {
    // Nothing laid out (an empty document): one empty page of the first section's size.
    if (!sizes_.empty()) return false;
    fw::doc::SectionProps props;
    if (!doc_->Sections().empty()) props = doc_->Sections().front().props;
    double w = props.pageWidth.Value() / 20.0, h = props.pageHeight.Value() / 20.0;
    if (props.landscape && w < h) std::swap(w, h);
    sizes_.push_back({w, h});
    placeholder_ = true;
    return true;
  }
  if (placeholder_) {
    sizes_.clear();
    placeholder_ = false;
  }
  // After an edit, pages are laid out again: their sizes may change, and once the layout is done
  // there may be fewer. Until then the old ones hold their places, so the view doesn't jump.
  bool changed = false;
  for (size_t i = 0; i < n && i < sizes_.size(); ++i) {
    const layout::Page& p = pagination_->GetPage(i);
    if (sizes_[i].w != p.width || sizes_[i].h != p.height) {
      sizes_[i] = {p.width, p.height};
      changed = true;
    }
  }
  if (pagination_->Done() && sizes_.size() > n) {
    sizes_.resize(n);
    changed = true;
  }
  if (sizes_.size() >= n && !changed) return false;
  const bool first = sizes_.empty();
  for (size_t i = sizes_.size(); i < n; ++i) {
    const layout::Page& p = pagination_->GetPage(i);
    sizes_.push_back({p.width, p.height});
  }
  if (!first && VpW() > 0) {
    Anchor a = CaptureAnchor(0, 0);
    RecomputeScale();
    Relayout(&a);
  }
  return true;
}

void DocView::ShowNotices() {
  noticesShown_ = true;
  // Someone else editing the file matters most: changes can only be saved as a copy.
  if (std::wstring inUse = file_.InUseMessage(); !inUse.empty()) {
    host_.ViewMessage(BannerKind::Warning, inUse);
    return;
  }
  // Content that isn't drawn comes first: the banner is one line, and a long font list is cut.
  std::wstring text;
  bool equations = false, ink = false;
  for (const auto& d : doc_->Drawings()) {
    equations |= d.kind == fw::doc::DrawingKind::Equation;
    ink |= d.kind == fw::doc::DrawingKind::ContentPart;
  }
  if (equations) text = L"Equations";
  if (ink) text += text.empty() ? L"Ink drawings" : L" and ink drawings";
  if (!text.empty()) text += L" aren't shown yet.";
  const auto& subs = fonts_->Substitutions();
  if (!subs.empty()) {
    std::wstring list;
    for (size_t i = 0; i < subs.size() && i < 3; ++i) {
      if (i) list += L", ";
      list += subs[i].requested + L" (shown in " + subs[i].used + L")";
    }
    if (subs.size() > 3) list += L" and " + std::to_wstring(subs.size() - 3) + L" more";
    if (!text.empty()) text += L" ";
    text += (subs.size() == 1 ? L"A font in this document isn't installed: " : L"Fonts in this document aren't installed: ") +
            list + L". Lines may break differently than in Word.";
  }
  if (!text.empty()) host_.ViewMessage(BannerKind::Info, text);
}

// ---------------------------------------------------------------------------
// Coordinates

draw::Matrix DocView::PageToScreen(int page) const {
  RECT r = PageRect(page);
  const double w = sizes_[page].w;
  const float s = static_cast<float>((r.right - r.left) / std::max(1.0, w));
  return draw::Matrix::Scale(s, s) * draw::Matrix::Translate(static_cast<float>(r.left), static_cast<float>(r.top));
}

bool DocView::ToPagePoint(int page, int x, int y, float& px, float& py) const {
  if (page < 0 || page >= static_cast<int>(sizes_.size())) return false;
  RECT r = PageRect(page);
  const double s = (r.right - r.left) / std::max(1.0, sizes_[page].w);
  px = static_cast<float>((x - r.left) / s);
  py = static_cast<float>((y - r.top) / s);
  return true;
}

D2D1_RECT_F DocView::PageRectToScreen(int page, const layout::Rect& rc) const {
  draw::Matrix m = PageToScreen(page);
  draw::Point a = m.Apply({rc.left, rc.top}), b = m.Apply({rc.right, rc.bottom});
  return RectF(std::min(a.x, b.x), std::min(a.y, b.y), std::max(a.x, b.x), std::max(a.y, b.y));
}

void DocView::OnPresentationChanged(bool on) {
  if (!on) return;
  ClearSearch();
  if (editor_) anchor_ = focus_;  // the caret stays where it was
  else anchor_ = focus_ = {};
}

// ---------------------------------------------------------------------------
// Painting

void DocView::Draw(Canvas& c, ID2D1DeviceContext* dc) {
  const Palette& p = theme::P();
  const bool inverted = host_.ViewInverted();
  D2D1_RECT_F vpr = RectF(vp_);
  c.PushClip(vpr);
  c.Fill(vpr, presentation_ ? Rgb(0x000000) : inverted ? p.docBgInverted : p.docBg);
  if (!Ready()) {
    c.PopClip();
    return;
  }
  StepScroll();
  int first, last;
  VisiblePages(first, last);
  for (int i = first; i <= last; ++i) DrawPage(c, dc, i, inverted);
  DrawSquiggles(c, first, last);
  DrawRanges(c, first, last);
  DrawCaret(c, first, last);
  if (!presentation_) DrawPictureFrame(c, first, last);
  if (!presentation_) DrawScrollbars(c);
  c.PopClip();
}

void DocView::DrawPage(Canvas& c, ID2D1DeviceContext* dc, int page, bool inverted) {
  const Palette& p = theme::P();
  D2D1_RECT_F rf = RectF(PageRect(page));
  D2D1_RECT_F vis = Intersect(rf, RectF(vp_));
  if (Empty(vis)) return;
  if (!presentation_) c.SoftShadow(rf, 1.0f * ui_, 5.0f * ui_, Alpha(p.pageShadow, 0.35f), 1.0f * ui_);
  c.Fill(rf, inverted ? p.paperInverted : p.paper);
  if (placeholder_ || page >= static_cast<int>(pagination_->PageCount())) return;
  c.PushClip(vis);
  {
    draw::D2DPainter painter(dc);
    painter.Transform(PageToScreen(page));
    auto drawing = [this](draw::Painter& p, const layout::DrawingBox& box) { DrawDrawing(p, box); };
    if (inverted) {
      InvertingPainter dark(painter);
      pagination_->Paint(dark, page, drawing);
    } else {
      pagination_->Paint(painter, page, drawing);
    }
  }
  c.PopClip();
}

void DocView::DrawDrawing(draw::Painter& painter, const layout::DrawingBox& box) {
  if (box.drawing >= doc_->Drawings().size()) return;
  if (box.drawing >= drawings_.size()) drawings_.resize(doc_->Drawings().size());  // inserted
  DrawingSlot& slot = drawings_[box.drawing];
  const fw::doc::Drawing& d = doc_->Drawings()[box.drawing];
  if (slot.tried && slot.version != d.version) slot = DrawingSlot{};  // changed (D3)
  if (!slot.tried) {
    slot.tried = true;
    slot.version = d.version;
    // The model gives what is inside the w:drawing, w:pict or w:object; wrap it back in one, so
    // fwdraw sees VML shapes beside their shape types.
    const char* wrapper = d.kind == fw::doc::DrawingKind::DrawingML ? "w:drawing"
                          : d.kind == fw::doc::DrawingKind::Vml     ? "w:pict"
                          : d.kind == fw::doc::DrawingKind::Object  ? "w:object"
                                                                    : nullptr;
    if (wrapper && d.part < doc_->Parts().size()) {
      std::string xml = std::string("<") + wrapper +
                        " xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">" +
                        doc_->DrawingXml(d) + "</" + wrapper + ">";
      slot.xml = std::make_unique<fw::pkg::XmlDom>();
      if (slot.xml->Parse(xml).Ok()) {
        slot.resources = std::make_unique<draw::PackageResources>(doc_->Package(), doc_->Parts()[d.part].name, theme_,
                                                                   colorMap_, images_);
        slot.drawing = d.kind == fw::doc::DrawingKind::DrawingML
                           ? draw::Drawing::FromWordDrawing(slot.xml->Root(), *slot.resources)
                           : draw::Drawing::FromVml(slot.xml->Root(), *slot.resources);
      }
    }
  }
  const draw::Rect frame{box.rect.left, box.rect.top, box.rect.right, box.rect.bottom};
  if (slot.drawing) {
    if (!slot.drawing->IsHidden()) slot.drawing->Draw(painter, frame);
    return;
  }
  // Not something fwdraw draws (an equation, ink): a light frame where it goes.
  draw::Pen pen;
  pen.brush = draw::Brush::Solid({0.75f, 0.75f, 0.75f, 1});
  pen.width = 0.75f;
  painter.StrokePath(draw::Path::FromRect(frame), pen);
}

void DocView::DrawRanges(Canvas& c, int first, int last) {
  const Palette& p = theme::P();
  const int pages = static_cast<int>(pagination_->PageCount());
  auto fill = [&](const std::vector<std::pair<size_t, layout::Rect>>& rects, D2D1_COLOR_F color, bool round) {
    for (const auto& [page, r] : rects) {
      const int pg = static_cast<int>(page);
      if (pg < first || pg > last || pg >= pages) continue;
      D2D1_RECT_F s = PageRectToScreen(pg, r);
      if (round) c.FillRound(Inflate(s, 1.5f * ui_, 1.0f * ui_), 2 * ui_, color);
      else c.Fill(s, color);
    }
  };
  // Comments, changes and matches are measured once pagination is done: measuring a range lays
  // out every page up to it, which painting mustn't wait for.
  const bool done = pagination_->Done();
  // Comments: their text is marked, as Word marks it.
  if (!presentation_) {
    for (auto& cm : comments_) {
      if (!cm.measured && done) {
        cm.rects = pagination_->Ranges(cm.from, cm.to);
        cm.measured = true;
      }
      fill(cm.rects, Alpha(Rgb(0xF2C744), 0.30f), false);
    }
  }
  // Tracked changes: inserted text underlined, a mark where text was deleted, and a bar in the
  // left margin beside each.
  if (!presentation_) {
    const float u = std::max(1.0f, ui_);
    auto bar = [&](int pg, float top, float bottom, D2D1_COLOR_F color) {
      const layout::Page& page = pagination_->GetPage(pg);
      const float x = std::max(4.0f, page.body.left - 14.0f);
      D2D1_RECT_F s = PageRectToScreen(pg, {x, top, x + 1, bottom});
      c.Fill(RectF(s.left, s.top, s.left + u, s.bottom), color);
    };
    for (auto& ch : changes_) {
      if (!ch.measured && done) {
        if (ch.deletion) ch.at = pagination_->CaretAt(ch.from);
        else ch.rects = pagination_->Ranges(ch.from, ch.to);
        ch.measured = true;
      }
      const D2D1_COLOR_F color = ChangeColor(ch.author);
      if (ch.deletion) {
        if (!ch.at) continue;
        const int pg = static_cast<int>(ch.at->page);
        if (pg < first || pg > last || pg >= pages) continue;
        D2D1_RECT_F s = PageRectToScreen(pg, {ch.at->x, ch.at->top, ch.at->x, ch.at->bottom});
        c.Fill(RectF(s.left - u, s.top, s.left + u, s.bottom), color);
        c.Fill(RectF(s.left - 3 * u, s.top, s.left + 3 * u, s.top + u), color);
        bar(pg, ch.at->top, ch.at->bottom, color);
        continue;
      }
      for (const auto& [page, r] : ch.rects) {
        const int pg = static_cast<int>(page);
        if (pg < first || pg > last || pg >= pages) continue;
        D2D1_RECT_F s = PageRectToScreen(pg, r);
        c.Fill(RectF(s.left, s.bottom - u, s.right, s.bottom), color);
        bar(pg, r.top, r.bottom, color);
      }
    }
  }
  for (size_t i = 0; i < matches_.size(); ++i) {
    Match& m = matches_[i];
    if (!m.measured && (done || static_cast<int>(i) == current_)) {
      m.rects = pagination_->Ranges(m.from, m.to);
      m.measured = true;
    }
    fill(m.rects, static_cast<int>(i) == current_ ? p.searchCurrent : p.searchHit, true);
  }
  if (HasSelection()) {
    TextPos a = std::min(anchor_, focus_), b = std::max(anchor_, focus_);
    if (!(selFrom_ == a) || !(selTo_ == b) || selRevision_ != pagination_->Revision()) {
      selRects_ = pagination_->Ranges(a, b);
      selFrom_ = a;
      selTo_ = b;
      selRevision_ = pagination_->Revision();
    }
    fill(selRects_, p.selection, false);
  }
}

// ---------------------------------------------------------------------------
// Background work

bool DocView::HasIdleWork() const {
  return Ready() && (!pagination_->Done() || !noticesShown_ || HasProofWork() || annotationsStale_ ||
                     (!placeholder_ && sizes_.size() < pagination_->PageCount()) ||
                     (format_ && DocFormat::FontsPending()));
}

bool DocView::IdleWork(ID2D1DeviceContext*, double deadlineMs) {
  if (!Ready()) return false;
  bool repaint = false;
  if (!pagination_->Done()) {
    double ms = std::clamp(deadlineMs - anim::Now(), 1.0, 12.0);
    pagination_->Continue(ms);
    // Done now: the comments, changes and matches can be measured.
    if (pagination_->Done()) repaint = true;
  }
  // Spelling, and words for the status bar (DocProof.cpp).
  repaint |= ProofIdle(deadlineMs);
  // Pages laid out here, or by finding or scrolling to a position (which lays out up to it).
  if (TakeNewPages()) {
    repaint = true;
    host_.ViewStateChanged();
  }
  if (pagination_->Revision() != revision_) {
    revision_ = pagination_->Revision();
    for (auto& m : matches_) m.measured = false;
    for (auto& cm : comments_) cm.measured = false;
    for (auto& ch : changes_) ch.measured = false;
    repaint = true;
  }
  if (!noticesShown_ && pagination_->Done()) ShowNotices();
  // Comments, changes and links an edit moved: found again once typing pauses.
  if (annotationsStale_ && pagination_->Done()) {
    annotationsStale_ = false;
    CollectAnnotations();
    repaint = true;
  }
  // The font box's list, once the pages are there.
  if (format_ && pagination_->Done() && DocFormat::FontsPending()) DocFormat::ReadFonts();
  return repaint;
}

// ---------------------------------------------------------------------------
// Input

std::optional<DocView::TextPos> DocView::PosAt(int x, int y, bool nearest) const {
  int page = PageAtPoint(x, y);
  if (page < 0 && nearest) page = NearestPage(x, y);
  if (page < 0 || placeholder_ || page >= static_cast<int>(pagination_->PageCount())) return std::nullopt;
  float px, py;
  if (!ToPagePoint(page, x, y, px, py)) return std::nullopt;
  return pagination_->HitTest(page, px, py);
}

DocView::Hit DocView::HitAt(int x, int y) {
  Hit hit;
  auto pos = PosAt(x, y, false);
  if (!pos) return hit;
  int page = PageAtPoint(x, y);
  float px, py;
  ToPagePoint(page, x, y, px, py);
  // The character under the point: the one before or after the nearest caret position.
  std::optional<uint32_t> under;
  // Measuring a character lays out the pages up to it, so while pagination runs (a few seconds at
  // most) the nearest character will do.
  const uint32_t size = static_cast<uint32_t>(doc_->Para(pos->para).text.size());
  if (!pagination_->Done() && size > 0) under = std::min(pos->offset, size - 1);
  for (uint32_t off : {pos->offset, pos->offset > 0 ? pos->offset - 1 : pos->offset}) {
    if (under) break;
    if (off >= doc_->Para(pos->para).text.size()) continue;
    for (const auto& [pg, r] : pagination_->Ranges({pos->para, off}, {pos->para, off + 1})) {
      if (static_cast<int>(pg) == page && px >= r.left - 0.5f && px < r.right + 0.5f && py >= r.top && py < r.bottom) {
        under = off;
        break;
      }
    }
    if (under) break;
  }
  if (!under) return hit;
  const TextPos at{pos->para, *under};
  for (const auto& run : doc_->Runs(at.para)) {
    if (at.offset < run.start || at.offset >= run.end || !run.hyperlink) continue;
    hit.link = Wide(doc_->HyperlinkTarget(run.hyperlink));
    break;
  }
  if (hit.link.empty()) {
    for (const auto& l : links_)
      if (!(at < l.from) && at < l.to) hit.link = l.target;
  }
  for (size_t i = 0; i < comments_.size(); ++i)
    if (!(at < comments_[i].from) && at < comments_[i].to) hit.comment = static_cast<int>(i);
  for (size_t i = 0; i < changes_.size(); ++i)
    if (!changes_[i].deletion && !(at < changes_[i].from) && at < changes_[i].to) hit.change = static_cast<int>(i);
  return hit;
}

void DocView::OnMouseDown(int x, int y, int button, UINT mods, bool doubleClick) {
  mouseX_ = x;
  mouseY_ = y;
  if (!Ready()) return;
  BeginPress(x, y);
  if (button == 2) {  // the middle button pans
    drag_ = Drag::Pan;
    return;
  }
  if (ScrollbarMouseDown(x, y, button)) return;
  if (PictureMouseDown(x, y, button, mods, doubleClick)) return;  // a right click selects it too
  if (button == 1) return;  // a right click keeps the selection for the context menu
  Hit hit = HitAt(x, y);
  if (!hit.link.empty() && !(mods & kModShift)) {
    pendingLink_ = hit.link;
    drag_ = Drag::Link;
    return;
  }
  if (presentation_) {
    if (x < (vp_.left + vp_.right) / 2 - VpW() / 4) PrevPage();
    else NextPage();
    return;
  }
  std::optional<TextPos> pos = PosAt(x, y, false);
  if (!pos && Editing() && PageAtPoint(x, y) >= 0) pos = PosAt(x, y, true);  // in a margin: the nearest place
  if (pos) {
    const bool triple = !doubleClick && anim::Now() - lastDoubleClick_ < GetDoubleClickTime() &&
                        std::abs(x - lastDoubleX_) + std::abs(y - lastDoubleY_) < 6 * ui_;
    if (doubleClick) {
      WordAt(*pos, anchor_, focus_);
      drag_ = Drag::None;
      lastDoubleClick_ = anim::Now();
      lastDoubleX_ = x;
      lastDoubleY_ = y;
    } else if (triple) {  // the paragraph
      anchor_ = {pos->para, 0};
      focus_ = {pos->para, static_cast<uint32_t>(doc_->Para(pos->para).text.size())};
      drag_ = Drag::None;
      lastDoubleClick_ = -1e9;
    } else if ((mods & kModShift) && (HasSelection() || Editing()) && StoryOf(*pos) == StoryOf(anchor_)) {
      focus_ = *pos;
      drag_ = Drag::Select;
    } else {
      anchor_ = focus_ = *pos;
      drag_ = Drag::Select;
    }
    if (Editing()) {
      goalX_ = -1;
      editor_->BreakTyping();
      CaretMoved(false);
    }
    host_.ViewInvalidate();
    return;
  }
  if (HasSelection()) {
    anchor_ = focus_ = {};
    host_.ViewInvalidate();
  }
  drag_ = Drag::Pan;
}

void DocView::OnMouseMove(int x, int y, UINT mods) {
  mouseX_ = x;
  mouseY_ = y;
  if (!Ready()) return;
  if (PictureMouseMove(x, y, mods)) return;
  if (ScrollDragMove(x, y)) return;
  if (drag_ == Drag::Link) {
    if (std::abs(x - downX_) + std::abs(y - downY_) > 6 * ui_) drag_ = Drag::Pan;
    return;
  }
  if (drag_ == Drag::Select) {
    if (y < vp_.top) ScrollBy(0, (y - vp_.top) * 0.5, false);
    else if (y > vp_.bottom) ScrollBy(0, (y - vp_.bottom) * 0.5, false);
    if (auto pos = PosAt(x, y, true); pos && StoryOf(*pos) == StoryOf(anchor_) && !(*pos == focus_)) {
      focus_ = *pos;
      host_.ViewInvalidate();
    }
    return;
  }
  UpdateScrollbarHover(x, y);
}

void DocView::OnMouseUp(int x, int y, int, UINT) {
  if (!Ready()) return;
  if (PictureMouseUp(x, y)) return;
  Drag d = drag_;
  drag_ = Drag::None;
  if (d == Drag::Link) {
    FollowLink(pendingLink_);
  } else if (d == Drag::Select && !Editing() && !HasSelection() && std::abs(x - downX_) + std::abs(y - downY_) < 3) {
    anchor_ = focus_ = {};  // a plain click just clears the selection (when editing, it leaves the caret there)
    host_.ViewInvalidate();
  } else if (d == Drag::Select && Editing()) {
    CaretMoved(false);
  }
  if (d == Drag::Select && format_ && format_->PainterOn()) format_->SelectionMade();
}

void DocView::OnMouseLeave() {
  mouseX_ = mouseY_ = -1;
  ScrollbarMouseLeave();
  host_.ViewInvalidate();
}

bool DocView::OnKeyDown(UINT vk, UINT mods) {
  if (PictureKey(vk, mods)) return true;
  if (vk == VK_ESCAPE && format_ && format_->PainterOn()) {
    format_->CancelPainter();
    host_.ViewInvalidate();
    return true;
  }
  if (vk == VK_TAB && !(mods & (kModCtrl | kModAlt)) && Editing() && ListTab((mods & kModShift) != 0)) return true;
  if (Editing() && pagination_->PageCount() > 0 && EditKey(vk, mods)) return true;
  if (!Ready() || pagination_->PageCount() == 0) return NavigationKey(vk, mods);
  if (vk == VK_ESCAPE && HasSelection()) {
    anchor_ = focus_ = {};
    host_.ViewInvalidate();
    return true;
  }
  return NavigationKey(vk, mods);
}

HCURSOR DocView::CursorAt(int x, int y) {
  static HCURSOR arrow = LoadCursor(nullptr, IDC_ARROW), ibeam = LoadCursor(nullptr, IDC_IBEAM),
                 hand = LoadCursor(nullptr, IDC_HAND), move = LoadCursor(nullptr, IDC_SIZEALL);
  if (!Ready()) return arrow;
  if (drag_ == Drag::Pan) return move;
  if (drag_ == Drag::Select) return ibeam;
  if (OverScrollbar(x, y)) return arrow;
  if (HCURSOR c = PictureCursor(x, y)) return c;
  if (!HitAt(x, y).link.empty()) return hand;
  if (presentation_) return arrow;
  return PosAt(x, y, false) ? ibeam : arrow;
}

std::wstring DocView::TooltipAt(int x, int y) {
  if (!Ready() || drag_ != Drag::None) return {};
  Hit hit = HitAt(x, y);
  if (!hit.link.empty()) {
    if (hit.link[0] == L'#') return L"Go to “" + hit.link.substr(1) + L"”";
    return hit.link;
  }
  std::wstring tip;
  if (hit.comment >= 0) tip = comments_[hit.comment].text;
  if (hit.change >= 0) tip += (tip.empty() ? L"" : L"\n") + changes_[hit.change].text;
  if (!tip.empty()) return tip;
  // A deletion's mark, near its caret.
  int page = PageAtPoint(x, y);
  float px, py;
  if (page >= 0 && ToPagePoint(page, x, y, px, py)) {
    for (const auto& ch : changes_) {
      if (ch.deletion && ch.at && static_cast<int>(ch.at->page) == page && std::abs(px - ch.at->x) < 3 &&
          py >= ch.at->top && py < ch.at->bottom)
        return ch.text;
    }
  }
  return {};
}

void DocView::FollowLink(const std::wstring& link) {
  if (link.empty()) return;
  if (link[0] == L'#') {
    const std::string name = fw::WideToUtf8(link.substr(1));
    for (const auto& pair : doc_->Pairs()) {
      if (pair.kind != fw::pkg::RefKind::BookmarkId || pair.openPos.para == fw::doc::kNone) continue;
      if (pair.open.AttrOr(fw::doc::Name("name")) != name) continue;
      TextPos at{pair.openPos.para, pair.openPos.offset};
      ScrollToPos(at, {at.para, at.offset + 1});
      return;
    }
    host_.ViewMessage(BannerKind::Warning, L"The place this link points to isn't in the document.");
    return;
  }
  if (!util::OpenExternalLink(link))
    host_.ViewMessage(BannerKind::Warning, L"This link was blocked because it doesn’t point to a website.");
}

// ---------------------------------------------------------------------------
// Text

void DocView::WordAt(TextPos pos, TextPos& from, TextPos& to) const {
  std::wstring t;
  std::vector<uint32_t> map;
  VisibleText(pos.para, t, map);
  from = to = pos;
  if (map.empty()) return;
  size_t i = std::lower_bound(map.begin(), map.end(), pos.offset) - map.begin();
  if (i >= map.size()) i = map.size() - 1;
  if (!WordChar(t[i]) && i > 0 && WordChar(t[i - 1])) --i;
  size_t a = i, b = i + 1;
  if (WordChar(t[i])) {
    while (a > 0 && WordChar(t[a - 1])) --a;
    while (b < t.size() && WordChar(t[b])) ++b;
  }
  while (b < t.size() && t[b] == L' ') ++b;  // with the space after it, as Word selects words
  from = {pos.para, map[a]};
  to = {pos.para, b < map.size() ? map[b] : map.back() + 1};
}

std::wstring DocView::TextOf(TextPos from, TextPos to, size_t limit) const {
  if (to < from) std::swap(from, to);
  const uint32_t story = StoryOf(from);
  std::wstring out;
  for (uint32_t p = from.para; p <= to.para && p < doc_->Paragraphs().size(); ++p) {
    if (doc_->Para(p).story != story) continue;
    std::wstring t;
    std::vector<uint32_t> map;
    VisibleText(p, t, map);
    if (p != from.para) out += L"\r\n";
    for (size_t i = 0; i < t.size(); ++i) {
      if (p == from.para && map[i] < from.offset) continue;
      if (p == to.para && map[i] >= to.offset) break;
      out += t[i] == L'\n' ? std::wstring(L"\r\n") : std::wstring(1, t[i]);
    }
    if (out.size() > limit) return out.substr(0, limit);
  }
  return out;
}

void DocView::Copy() {
  if (!Ready() || !HasSelection()) return;
  if (PutOnClipboard()) host_.ViewMessage(BannerKind::Success, L"Copied");
}

bool DocView::PutOnClipboard() {
  if (SelectedPicture() != fw::doc::kNone && CopyPicture()) return true;
  const fw::doc::Pos a{std::min(anchor_, focus_).para, std::min(anchor_, focus_).offset};
  const fw::doc::Pos b{std::max(anchor_, focus_).para, std::max(anchor_, focus_).offset};
  const fw::doc::edit::Clip clip = fw::doc::edit::CopyClip(*doc_, a, b, clipSource_);
  const std::u16string text = clip.PlainText();
  if (text.empty()) return false;
  util::SetClipboardText(std::wstring(text.begin(), text.end()), fw::doc::edit::ToRtf(clip), fw::doc::edit::ToHtml(clip));
  return true;
}

std::wstring DocView::SelectedText() {
  if (!Ready() || !HasSelection()) return {};
  return TextOf(anchor_, focus_, 1000);
}

void DocView::SelectAll() {
  if (!Ready()) return;
  std::vector<uint32_t> paras = doc_->StoryParagraphs(doc_->MainStory());
  if (paras.empty()) return;
  anchor_ = {paras.front(), 0};
  focus_ = {paras.back(), static_cast<uint32_t>(doc_->Para(paras.back()).text.size())};
  host_.ViewInvalidate();
}

// ---------------------------------------------------------------------------
// Find

void DocView::StartSearch(const std::wstring& query, bool matchCase) {
  if (!Ready()) return;
  ClearSearch();
  query_ = query;
  matchCase_ = matchCase;
  if (query.empty()) return;
  auto lower = [](std::wstring s) {
    if (!s.empty())
      LCMapStringEx(LOCALE_NAME_USER_DEFAULT, LCMAP_LOWERCASE, s.data(), static_cast<int>(s.size()), s.data(),
                    static_cast<int>(s.size()), nullptr, nullptr, 0);
    return s;
  };
  const std::wstring q = matchCase ? query : lower(query);
  // The main text first, then headers, footers, notes and text boxes, as Word searches them.
  std::vector<uint32_t> stories{doc_->MainStory()};
  for (uint32_t s = 0; s < doc_->Stories().size(); ++s)
    if (s != doc_->MainStory() && doc_->Stories()[s].kind != fw::doc::StoryKind::Comment) stories.push_back(s);
  for (uint32_t story : stories) {
    for (uint32_t p : doc_->StoryParagraphs(story)) {
      std::wstring t;
      std::vector<uint32_t> map;
      VisibleText(p, t, map);
      if (t.size() < q.size()) continue;
      const std::wstring hay = matchCase ? t : lower(t);
      for (size_t at = hay.find(q); at != std::wstring::npos; at = hay.find(q, at + q.size())) {
        Match m;
        m.from = {p, map[at]};
        m.to = {p, map[at + q.size() - 1] + 1};
        matches_.push_back(std::move(m));
      }
    }
  }
  // The first match from the current page on.
  current_ = matches_.empty() ? -1 : 0;
  for (size_t i = 0; i < matches_.size(); ++i) {
    auto page = pagination_->PageOf(matches_[i].from);
    if (page && static_cast<int>(*page) >= currentPage_) {
      current_ = static_cast<int>(i);
      break;
    }
    if (!page) break;
  }
  TakeNewPages();
  if (current_ >= 0) ScrollToPos(matches_[current_].from, matches_[current_].to);
  host_.ViewInvalidate();
  host_.ViewStateChanged();
}

void DocView::ClearSearch() {
  matches_.clear();
  current_ = -1;
  host_.ViewInvalidate();
}

void DocView::SearchStep(bool forward) {
  if (matches_.empty()) return;
  const int n = static_cast<int>(matches_.size());
  current_ = current_ < 0 ? 0 : (current_ + (forward ? 1 : n - 1)) % n;
  ScrollToPos(matches_[current_].from, matches_[current_].to);
  host_.ViewStateChanged();
  host_.ViewInvalidate();
}

void DocView::ScrollToPos(TextPos from, TextPos to) {
  auto rects = pagination_->Ranges(from, to);
  TakeNewPages();
  if (rects.empty()) {
    if (auto page = pagination_->PageOf(from)) {
      TakeNewPages();
      GoToPage(static_cast<int>(*page));
    }
    return;
  }
  const int page = static_cast<int>(rects.front().first);
  if (page >= PageCount()) return;
  if (viewMode_ == fw::ViewMode::Single && layout_.rowOfPage[page] != singleRow_) {
    singleRow_ = layout_.rowOfPage[page];
    sy_ = tsy_ = 0;
  }
  ScrollScreenRectIntoView(PageRectToScreen(page, rects.front().second), true);
}

// ---------------------------------------------------------------------------
// File

SaveFormat DocView::SaveAsFormat() const {
  if (fw::EndsWithNoCase(path_, L".docm")) return {L"Word macro-enabled document", L"*.docm", L"docm"};
  if (fw::EndsWithNoCase(path_, L".dotx")) return {L"Word template", L"*.dotx", L"dotx"};
  if (fw::EndsWithNoCase(path_, L".dotm")) return {L"Word macro-enabled template", L"*.dotm", L"dotm"};
  return {L"Word document", L"*.docx", L"docx"};
}

bool DocView::Save(const std::wstring& path, std::wstring& error) {
  if (!doc_) return false;
  if (!file_.Save(*doc_, path, error)) return false;
  if (editor_) editor_->MarkSaved();  // undo back to here is clean
  path_ = file_.Path();
  host_.ViewStateChanged();
  return true;
}

std::wstring DocView::SuggestedSavePath() const { return doc_ ? file_.SuggestedPath(*doc_) : path_; }

void DocView::Autosave() {
  if (doc_) file_.Autosave(*doc_);
}

void DocView::EditForTesting() {
  if (!doc_) return;
  const std::vector<uint32_t> paras = doc_->StoryParagraphs(doc_->MainStory());
  if (editor_ && editor_->CanEdit() && !paras.empty() &&
      editor_->InsertText({paras.front(), editor_->ParaLength(paras.front())}, u" (edited)"))
    return;
  // A document the editor refuses: marked edited all the same, for the dot and the prompt.
  if (const fw::doc::Part* main = doc_->FindPart(fw::doc::PartKind::Document))
    NoteEdit(static_cast<uint32_t>(main - doc_->Parts().data()));
}

void DocView::NoteEdit(uint32_t part) {
  const bool first = !file_.Dirty();
  file_.Edited(part);
  if (first) host_.ViewStateChanged();  // the tab's dot, Save, the autosave timer
}

PrintOutcome DocView::Print(HWND owner, const std::function<void(int, int)>& progress, std::wstring& message) {
  pagination_->LayOutAll();
  TakeNewPages();
  const int pageCount = static_cast<int>(pagination_->PageCount());
  if (pageCount == 0) {
    message = L"This document has nothing to print.";
    return PrintOutcome::Failed;
  }
  PRINTPAGERANGE ranges[16] = {};
  PRINTDLGEXW pd{sizeof(pd)};
  pd.hwndOwner = owner;
  pd.Flags = PD_NOSELECTION | PD_USEDEVMODECOPIESANDCOLLATE;
  pd.nMaxPageRanges = 16;
  pd.lpPageRanges = ranges;
  pd.nMinPage = 1;
  pd.nMaxPage = static_cast<DWORD>(pageCount);
  pd.nCopies = 1;
  pd.nStartPage = START_PAGE_GENERAL;
  HRESULT hr = PrintDlgExW(&pd);
  auto cleanup = [&]() {
    if (pd.hDC) DeleteDC(pd.hDC);
    if (pd.hDevMode) GlobalFree(pd.hDevMode);
    if (pd.hDevNames) GlobalFree(pd.hDevNames);
  };
  if (FAILED(hr) || pd.dwResultAction != PD_RESULT_PRINT || !pd.hDevNames) {
    cleanup();
    if (FAILED(hr)) {
      message = L"The print dialog couldn’t be opened. Is a printer installed?";
      return PrintOutcome::Failed;
    }
    return PrintOutcome::Cancelled;
  }
  std::vector<int> toPrint;
  if (pd.Flags & PD_PAGENUMS) {
    for (DWORD i = 0; i < pd.nPageRanges; ++i)
      for (DWORD p = ranges[i].nFromPage; p <= ranges[i].nToPage && p >= 1 && p <= static_cast<DWORD>(pageCount); ++p)
        toPrint.push_back(static_cast<int>(p) - 1);
  } else if (pd.Flags & PD_CURRENTPAGE) {
    toPrint.push_back(std::min(currentPage_, pageCount - 1));
  } else {
    for (int i = 0; i < pageCount; ++i) toPrint.push_back(i);
  }
  std::wstring printer;
  if (auto* names = static_cast<DEVNAMES*>(GlobalLock(pd.hDevNames))) {
    printer = reinterpret_cast<const wchar_t*>(names) + names->wDeviceOffset;
    GlobalUnlock(pd.hDevNames);
  }
  const DEVMODEW* devmode = pd.hDevMode ? static_cast<const DEVMODEW*>(GlobalLock(pd.hDevMode)) : nullptr;
  PrintOutcome outcome = PrintPages(printer, devmode, toPrint, nullptr, progress, message);
  if (devmode) GlobalUnlock(pd.hDevMode);
  cleanup();
  return outcome;
}

PrintOutcome DocView::PrintToFile(const std::wstring& path, std::wstring& message) {
  pagination_->LayOutAll();
  TakeNewPages();
  std::vector<int> pages(pagination_->PageCount());
  for (size_t i = 0; i < pages.size(); ++i) pages[i] = static_cast<int>(i);
  Microsoft::WRL::ComPtr<IStream> stream;
  if (FAILED(SHCreateStreamOnFileEx(path.c_str(), STGM_CREATE | STGM_WRITE | STGM_SHARE_DENY_WRITE,
                                    FILE_ATTRIBUTE_NORMAL, TRUE, nullptr, &stream))) {
    message = L"Couldn't create " + path;
    return PrintOutcome::Failed;
  }
  return PrintPages(L"Microsoft Print to PDF", nullptr, pages, stream.Get(), nullptr, message);
}

PrintOutcome DocView::PrintPages(const std::wstring& printer, const DEVMODEW* devmode, const std::vector<int>& pages,
                                 IStream* output, const std::function<void(int, int)>& progress,
                                 std::wstring& message) {
  if (pages.empty()) {
    message = L"This document has nothing to print.";
    return PrintOutcome::Failed;
  }
  auto drawing = [this](draw::Painter& p, const layout::DrawingBox& box) { DrawDrawing(p, box); };
  draw::PrintJob job;
  HRESULT hr = job.Start(gpu::D2DDevice(), printer, devmode, Title(), output);
  int done = 0;
  for (size_t i = 0; SUCCEEDED(hr) && i < pages.size(); ++i) {
    if (progress) progress(done, static_cast<int>(pages.size()));
    const layout::Page& page = pagination_->GetPage(pages[i]);
    hr = job.AddPage(page.width, page.height,
                     [&](draw::Painter& painter) { pagination_->Paint(painter, pages[i], drawing); });
    if (SUCCEEDED(hr)) ++done;
  }
  if (SUCCEEDED(hr)) hr = job.Finish();
  if (FAILED(hr)) {
    message = L"Printing stopped because the printer reported an error.";
    return PrintOutcome::Failed;
  }
  switch (job.Wait(120000)) {
    case draw::PrintJob::Outcome::Canceled: message = L"Printing was canceled."; return PrintOutcome::Cancelled;
    case draw::PrintJob::Outcome::Failed:
      message = L"Printing stopped because the printer reported an error.";
      return PrintOutcome::Failed;
    default: break;
  }
  message = done == 1 ? L"Sent 1 page to the printer" : L"Sent " + std::to_wstring(done) + L" pages to the printer";
  return PrintOutcome::Printed;
}

// ---------------------------------------------------------------------------
// Commands

namespace {

struct CommandName {
  int cmd;
  const wchar_t* label;
  const wchar_t* keys;
};

constexpr CommandName kCommandNames[] = {
    {CmdGoToPage, L"Go to page", L"Ctrl+G"},
    {CmdNextPage, L"Next page", L"Page Down"},
    {CmdPrevPage, L"Previous page", L"Page Up"},
    {CmdFirstPage, L"First page", L"Ctrl+Home"},
    {CmdLastPage, L"Last page", L"Ctrl+End"},
    {CmdZoomIn, L"Zoom in", L"Ctrl+="},
    {CmdZoomOut, L"Zoom out", L"Ctrl+−"},
    {CmdZoomAuto, L"Zoom automatically", L"Ctrl+0"},
    {CmdZoomActual, L"Actual size", L"Ctrl+1"},
    {CmdZoomFitPage, L"Fit page", L"Ctrl+2"},
    {CmdZoomFitWidth, L"Fit width", L"Ctrl+3"},
    {CmdViewSingle, L"Single page", L"Ctrl+6"},
    {CmdViewContinuous, L"Continuous pages", L"Ctrl+7"},
    {CmdViewFacing, L"Two pages", L"Ctrl+8"},
    {CmdPresentation, L"Present", L"F5"},
    {CmdToggleInvert, L"Night reading: dark pages", L""},
    {CmdCut, L"Cut", L"Ctrl+X"},
    {CmdCopy, L"Copy", L"Ctrl+C"},
    {CmdPaste, L"Paste", L"Ctrl+V"},
    {CmdUndo, L"Undo", L"Ctrl+Z"},
    {CmdRedo, L"Redo", L"Ctrl+Y"},
    {CmdSelectAll, L"Select all", L"Ctrl+A"},
    {CmdSpelling, L"Spelling: next error", L"F7"},
    {CmdWordCount, L"Word count", L"Ctrl+Shift+G"},
    {CmdLanguage, L"Language…", L""},
    {CmdInsertPicture, L"Insert picture…", L""},
    {CmdPictureInline, L"Picture: in line with text", L""},
    {CmdPictureSquare, L"Picture: square wrapping", L""},
    {CmdPictureTight, L"Picture: tight wrapping", L""},
    {CmdPictureTopBottom, L"Picture: top and bottom", L""},
    {CmdPictureBehind, L"Picture: behind text", L""},
    {CmdPictureInFront, L"Picture: in front of text", L""},
    {CmdPictureCrop, L"Crop picture", L""},
    {CmdPictureRotate, L"Rotate picture 90°", L""},
    {CmdPictureReset, L"Reset picture size", L""},
    {CmdPicturePanel, L"Picture size, rotation and alt text…", L""},
};

}  // namespace

int DocView::Shortcut(UINT vk, UINT mods) {
  const bool ctrl = (mods & kModCtrl) != 0;
  if ((mods & kModAlt) && !ctrl && (vk == VK_LEFT || vk == VK_RIGHT)) return vk == VK_LEFT ? CmdPrevPage : CmdNextPage;
  if (ctrl && !(mods & kModShift)) {
    switch (vk) {
      case 'G': return CmdGoToPage;
      case VK_OEM_PLUS:
      case VK_ADD: return CmdZoomIn;
      case VK_OEM_MINUS:
      case VK_SUBTRACT: return CmdZoomOut;
      case '0':
      case VK_NUMPAD0: return CmdZoomAuto;
      case '1': return CmdZoomActual;
      case '2': return CmdZoomFitPage;
      case '3': return CmdZoomFitWidth;
      case '6': return CmdViewSingle;
      case '7': return CmdViewContinuous;
      case '8': return CmdViewFacing;
    }
  }
  if (ctrl && (mods & kModShift) && !(mods & kModAlt) && vk == 'G') return CmdWordCount;
  if (vk == VK_F5) return CmdPresentation;
  if (vk == VK_F7 && !(mods & (kModCtrl | kModShift | kModAlt))) return CmdSpelling;
  return 0;
}

int DocView::ShortcutFor(UINT vk, UINT mods) const {
  // Word's keys first in a document being edited: Ctrl+1 is single spacing there, not actual size.
  if (format_)
    if (int cmd = DocFormat::Shortcut(vk, mods)) return cmd;
  return Shortcut(vk, mods);
}

CommandState DocView::QueryCommand(int cmd) {
  if (format_ && DocFormat::Owns(cmd)) return format_->Query(cmd);
  CommandState s;
  if (auto picture = QueryPicture(cmd)) s = *picture;
  switch (cmd) {
    case CmdSave: s = {Ready() && (Modified() || path_.empty()), false}; break;
    case CmdPrint: s = {Ready(), false}; break;
    case CmdCopy: s = {Ready() && HasSelection(), false}; break;
    case CmdCut: s = {Editing() && HasSelection(), false}; break;
    case CmdPaste: s = {Editing() && (IsClipboardFormatAvailable(CF_UNICODETEXT) || HasPictureOnClipboard()), false}; break;
    case CmdUndo: s = {Editing() && editor_->CanUndo(), false}; break;
    case CmdRedo: s = {Editing() && editor_->CanRedo(), false}; break;
    case CmdSelectAll: s = {Ready(), false}; break;
    case CmdSpelling:
    case CmdWordCount: s = {Ready(), false}; break;
    case CmdLanguage: s = {Editing(), false}; break;
    case CmdRotateLeft:
    case CmdRotateRight:
    case CmdToggleCover:
    case CmdHighlight:
    case CmdSelectAllOnPage:
      s.visible = false;
      break;
    default:
      if (cmd < CmdInsertPicture || cmd > CmdPicturePanel) s = PagedView::QueryCommand(cmd);
      break;
  }
  for (const CommandName& n : kCommandNames) {
    if (n.cmd == cmd) {
      s.label = n.label;
      s.shortcut = n.keys;
    }
  }
  // Word's keys win in a document being edited: Ctrl+=, Ctrl+0, Ctrl+1 and Ctrl+2 format there.
  if (format_ && (cmd == CmdZoomIn || cmd == CmdZoomAuto || cmd == CmdZoomActual || cmd == CmdZoomFitPage))
    s.shortcut.clear();
  // "Undo typing", "Redo new paragraph".
  // (The names are copies: one each, so their iterators are of the same string.)
  if ((cmd == CmdUndo || cmd == CmdRedo) && s.enabled) {
    const std::u16string name = cmd == CmdUndo ? editor_->UndoName() : editor_->RedoName();
    s.label = (cmd == CmdUndo ? L"Undo " : L"Redo ") + fw::ToLower(std::wstring(name.begin(), name.end()));
  }
  return s;
}

std::vector<int> DocView::Commands() {
  std::vector<int> ids;
  for (const CommandName& n : kCommandNames)
    if (editor_ || (n.cmd != CmdCut && n.cmd != CmdPaste && n.cmd != CmdUndo && n.cmd != CmdRedo)) ids.push_back(n.cmd);
  if (format_)
    for (int cmd : DocFormat::CommandIds()) ids.push_back(cmd);
  return ids;
}

bool DocView::Execute(int cmd) {
  if (format_ && DocFormat::Owns(cmd)) {
    // Ctrl+Shift+V pastes the format Ctrl+Shift+C copied, as in Word; with none, the text alone.
    if (cmd == CmdPasteFormat && !format_->Carrying()) Paste(false);
    else format_->Execute(cmd);
    return true;
  }
  if (ExecuteProof(cmd)) return true;
  if (ExecutePicture(cmd)) return true;
  switch (cmd) {
    case CmdCopy: Copy(); return true;
    case CmdCut: Cut(); return true;
    case CmdPaste: Paste(); return true;
    case CmdUndo: Undo(); return true;
    case CmdRedo: Redo(); return true;
    case CmdSelectAll: SelectAll(); return true;
    case CmdRotateLeft:
    case CmdRotateRight:
    case CmdToggleCover: return true;  // not for documents
  }
  return PagedView::Execute(cmd);
}

void DocView::BuildToolbar(ToolbarModel& m, const ToolbarContext& ctx) {
  auto item = [](int cmd, wchar_t glyph, const wchar_t* tooltip = L"") {
    ToolItem t;
    t.cmd = cmd;
    t.glyph = glyph;
    t.tooltip = tooltip;
    return t;
  };
  ToolItem save = item(CmdSave, icon::Save, editor_ || path_.empty() ? L"Save (Ctrl+S)"
                                                                     : L"This document can't be edited; Save as makes a copy");
  save.enabled = QueryCommand(CmdSave).enabled;
  save.joinNext = true;
  ToolItem saveAs = item(CmdSaveMenu, icon::ChevronDown, L"Save as… (Ctrl+Shift+S)");
  saveAs.width = 18;
  saveAs.glyphSize = 9;
  ToolItem print = item(CmdPrint, icon::Print);
  m.left = {save, saveAs, print};
  if (Editing()) {
    // Undo and redo, then formatting; the priorities say what a narrow window drops first (Toolbar.h).
    auto query = [&](int cmd, wchar_t glyph, int priority) {
      CommandState q = QueryCommand(cmd);
      ToolItem t = item(cmd, glyph);
      t.tooltip = q.Tooltip();
      t.enabled = q.enabled;
      t.priority = priority;
      return t;
    };
    ToolItem undo = query(CmdUndo, icon::Undo, 8);
    undo.sepBefore = true;
    m.left.push_back(undo);
    m.left.push_back(query(CmdRedo, icon::Redo, 7));
    format_->AddToolbar(m);
    AddPictureToolbar(m);
  }
  SyncPicturePanel();

  m.pager = !Editing();  // when editing, the status bar has the page and the zoom
  m.page = currentPage_;
  m.pageCount = PageCount();
  m.zoomText = fw::FormatPercent(Zoom());

  wchar_t viewGlyph = viewMode_ == fw::ViewMode::Single   ? icon::ViewSingle
                      : viewMode_ == fw::ViewMode::Facing ? icon::ViewFacing
                                                          : icon::ViewContinuous;
  ToolItem view = item(CmdViewMenu, viewGlyph, L"Page layout");
  view.width = 52;
  view.dropdown = true;
  view.priority = 1;
  ToolItem find = item(CmdFind, icon::Search);
  find.checked = ctx.searchOpen;
  find.toggle = true;
  find.priority = 5;
  find.sepBefore = true;
  ToolItem night = item(CmdToggleInvert, icon::QuietHours, L"Night reading: dark pages");
  night.checked = ctx.inverted;
  night.toggle = true;
  night.priority = 3;
  ToolItem full = item(CmdFullScreen, ctx.fullscreen ? icon::BackToWindow : icon::FullScreen);
  full.priority = 2;
  m.right = {view, find, night, full};
}

void DocView::FillContextMenu(HMENU m, int x, int y) {
  using menu::AppendItem;
  using menu::AppendSeparator;
  FillSpellingMenu(m, x, y);  // a spelling error's suggestions first, as Word has them
  FillPictureMenu(m);         // and a selected picture's
  if (Editing()) {
    AppendItem(m, CmdCut, L"Cut\tCtrl+X", HasSelection());
    AppendItem(m, CmdCopy, L"Copy\tCtrl+C", HasSelection());
    AppendItem(m, CmdPaste, L"Paste\tCtrl+V", QueryCommand(CmdPaste).enabled);
    AppendSeparator(m);
    HMENU language = CreatePopupMenu();
    FillLanguageMenu(language);
    menu::AppendSubmenu(m, language, L"Language", GetMenuItemCount(language) > 0);
    AppendSeparator(m);
  } else if (HasSelection()) {
    AppendItem(m, CmdCopy, L"Copy\tCtrl+C");
    AppendSeparator(m);
  }
  AppendItem(m, CmdSelectAll, L"Select all\tCtrl+A");
  AppendSeparator(m);
  AppendItem(m, CmdZoomIn, format_ ? L"Zoom in" : L"Zoom in\tCtrl+=");
  AppendItem(m, CmdZoomOut, L"Zoom out\tCtrl+−");
  AppendItem(m, CmdZoomFitWidth, L"Fit width\tCtrl+3");
  AppendItem(m, CmdZoomFitPage, format_ ? L"Fit page" : L"Fit page\tCtrl+2");
  AppendSeparator(m);
  AppendItem(m, CmdFind, L"Find…\tCtrl+F");
  AppendItem(m, CmdPrint, L"Print…\tCtrl+P");
}

void DocView::BuildStatusBar(StatusBarModel& m) {
  std::wstring pages = L"Page " + std::to_wstring(currentPage_ + 1) + L" of " + std::to_wstring(PageCount());
  if (!pagination_->Done()) pages += L"…";
  m.segments = {pages};
  ProofStatus(m);  // words and language
  m.zoom = true;
  m.zoomValue = Zoom();
  m.onZoom = [this](double z) { SetZoom(z); };
}

// ---------------------------------------------------------------------------
// Formatting

DocFormat::Target DocView::FormatTarget() const {
  DocFormat::Target t;
  if (!Editing()) return t;
  TextPos a = std::min(anchor_, focus_), b = std::max(anchor_, focus_);
  const auto& paras = doc_->Paragraphs();
  if (a.para >= paras.size() || b.para >= paras.size()) return t;
  a.offset = std::min<uint32_t>(a.offset, static_cast<uint32_t>(paras[a.para].text.size()));
  b.offset = std::min<uint32_t>(b.offset, static_cast<uint32_t>(paras[b.para].text.size()));
  t.any = true;
  t.sel = {{a.para, a.offset}, {b.para, b.offset}};
  t.chars = t.sel;
  if (a == b) {
    // A caret inside a word formats the word, as Word does (its space after stays as it is).
    TextPos from, to;
    WordAt(a, from, to);
    const std::u16string& text = paras[a.para].text;
    while (to.offset > from.offset && to.offset <= text.size() && text[to.offset - 1] == u' ') --to.offset;
    if (from < a && a < to) t.chars = {{from.para, from.offset}, {to.para, to.offset}};
  }
  return t;
}

void DocView::AfterFormat(const DocFormat::Done& done) {
  // OnEdited has told the layout what changed: the caret's page is laid out again now.
  if (done.caret) anchor_ = focus_ = {done.caret->para, done.caret->offset};
  goalX_ = -1;
  if (auto c = pagination_->CaretAt(focus_)) pagination_->LayOutPages(c->page + 2);
  TakeNewPages();
  CaretMoved(done.caret.has_value());
  if (!done.message.empty()) host_.ViewMessage(BannerKind::Info, done.message);
  host_.ViewStateChanged();
  host_.ViewInvalidate();
}

bool DocView::ListTab(bool back) {
  if (!format_) return false;
  DocFormat::Target t = FormatTarget();
  if (!t.any) return false;
  // At the start of a list item, or across items: the level. Elsewhere Tab is the editor's.
  const bool across = t.sel.from.para != t.sel.to.para;
  if (!across && t.sel.from.offset != 0) return false;
  const std::string_view numId = doc_->ResolvedPara(t.sel.from.para).numId;
  if (numId.empty() || numId == "0") return false;
  Execute(back ? CmdOutdent : CmdIndent);
  return true;
}
