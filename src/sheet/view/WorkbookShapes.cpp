// Pictures and shapes in the grid (S3, s3-data): Insert > Pictures (files, or a picture pasted from
// the clipboard; stored once), Insert > Shapes (rectangle, rounded rectangle, oval, line, arrow,
// text box) at the active cell, and the Format panel (fill, outline, text, alt text) for the
// selected object. Selecting, moving, resizing and deleting are s3-charts' (WorkbookCharts.cpp);
// the edits are the Editor's (InsertPicture, InsertShape, SetObjectXml).

#include <windows.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwctype>

#include "Commands.h"
#include "Util.h"
#include "core/Strings.h"
#include "draw/Images.h"
#include "sheet/data/Shapes.h"
#include "sheet/view/WorkbookFormat.h"
#include "sheet/view/WorkbookView.h"
#include "ui/Gfx.h"
#include "ui/Theme.h"
#include "ui/Toolbar.h"

using Microsoft::WRL::ComPtr;
using namespace gfx;

namespace fw::sheet::view {

using data::ObjectFormat;
using data::ShapeKind;

namespace {

constexpr size_t kMaxPictureFile = 64u << 20;

struct NamedColor {
  const wchar_t* name;
  uint32_t rgb;
};
// Office's standard colors, with black, white and gray.
constexpr NamedColor kColors[] = {{L"Black", 0x000000},       {L"White", 0xFFFFFF},      {L"Gray", 0x7F7F7F},
                                  {L"Dark red", 0xC00000},    {L"Red", 0xFF0000},        {L"Orange", 0xFFC000},
                                  {L"Yellow", 0xFFFF00},      {L"Light green", 0x92D050}, {L"Green", 0x00B050},
                                  {L"Light blue", 0x00B0F0},  {L"Blue", 0x0070C0},       {L"Dark blue", 0x002060},
                                  {L"Purple", 0x7030A0}};
// Excel's outline weights, in points.
constexpr double kWeights[] = {0.25, 0.5, 0.75, 1, 1.5, 2.25, 3, 4.5, 6};
constexpr const wchar_t* kWeightNames[] = {L"¼ pt", L"½ pt", L"¾ pt", L"1 pt", L"1½ pt", L"2¼ pt", L"3 pt", L"4½ pt", L"6 pt"};

bool IsPictureFile(const std::wstring& path) {
  const size_t dot = path.find_last_of(L'.');
  const std::wstring ext = fw::ToLower(dot == std::wstring::npos ? std::wstring() : path.substr(dot));
  for (const wchar_t* e : {L".png", L".jpg", L".jpeg", L".jpe", L".jfif", L".gif", L".bmp", L".dib"})
    if (ext == e) return true;
  return false;
}

// Clipboard data in `format`; empty when there's none.
std::vector<uint8_t> ClipboardBytes(UINT format) {
  std::vector<uint8_t> out;
  if (!IsClipboardFormatAvailable(format) || !OpenClipboard(nullptr)) return out;
  if (HANDLE h = GetClipboardData(format)) {
    if (const void* p = GlobalLock(h)) {
      const size_t n = GlobalSize(h);
      if (n <= kMaxPictureFile) out.assign(static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + n);
      GlobalUnlock(h);
    }
  }
  CloseClipboard();
  return out;
}

// A packed DIB (CF_DIB, CF_DIBV5) as a PNG, as Excel stores a pasted bitmap: read by Windows' own
// BMP decoder, written by its PNG encoder.
std::string DibToPng(const std::vector<uint8_t>& dib) {
  if (dib.size() < sizeof(BITMAPINFOHEADER)) return {};
  BITMAPINFOHEADER h;
  std::memcpy(&h, dib.data(), sizeof(h));
  if (h.biSize < sizeof(BITMAPINFOHEADER) || h.biSize > dib.size()) return {};
  const uint32_t colors = h.biClrUsed ? h.biClrUsed : (h.biBitCount <= 8 ? 1u << h.biBitCount : 0);
  const uint32_t masks = h.biSize == sizeof(BITMAPINFOHEADER) && h.biCompression == BI_BITFIELDS ? 12 : 0;
  const uint64_t offset = 14ull + h.biSize + masks + colors * 4ull;
  if (offset > dib.size() + 14ull) return {};
  std::vector<uint8_t> bmp(14 + dib.size());
  BITMAPFILEHEADER f{};
  f.bfType = 0x4D42;
  f.bfSize = static_cast<DWORD>(bmp.size());
  f.bfOffBits = static_cast<DWORD>(offset);
  std::memcpy(bmp.data(), &f, 14);
  std::memcpy(bmp.data() + 14, dib.data(), dib.size());
  IWICImagingFactory* wic = gfx::WIC();
  ComPtr<IWICStream> in;
  ComPtr<IWICBitmapDecoder> decoder;
  ComPtr<IWICBitmapFrameDecode> frame;
  if (!wic || FAILED(wic->CreateStream(&in)) || FAILED(in->InitializeFromMemory(bmp.data(), static_cast<DWORD>(bmp.size()))) ||
      FAILED(CoCreateInstance(CLSID_WICBmpDecoder, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&decoder))) ||
      FAILED(decoder->Initialize(in.Get(), WICDecodeMetadataCacheOnDemand)) || FAILED(decoder->GetFrame(0, &frame)))
    return {};
  UINT w = 0, ht = 0;
  if (FAILED(frame->GetSize(&w, &ht)) || !w || !ht || uint64_t{w} * ht > (1ull << 26)) return {};
  ComPtr<IStream> out;
  ComPtr<IWICBitmapEncoder> encoder;
  ComPtr<IWICBitmapFrameEncode> page;
  ComPtr<IWICFormatConverter> converter;
  WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
  if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &out)) || FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
      FAILED(encoder->Initialize(out.Get(), WICBitmapEncoderNoCache)) || FAILED(encoder->CreateNewFrame(&page, nullptr)) ||
      FAILED(page->Initialize(nullptr)) || FAILED(page->SetSize(w, ht)) || FAILED(page->SetPixelFormat(&format)) ||
      FAILED(wic->CreateFormatConverter(&converter)) ||
      FAILED(converter->Initialize(frame.Get(), format, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)) ||
      FAILED(page->WriteSource(converter.Get(), nullptr)) || FAILED(page->Commit()) || FAILED(encoder->Commit()))
    return {};
  HGLOBAL g = nullptr;
  STATSTG stat{};
  if (FAILED(GetHGlobalFromStream(out.Get(), &g)) || FAILED(out->Stat(&stat, STATFLAG_NONAME))) return {};
  std::string png;
  if (const void* p = GlobalLock(g)) {
    png.assign(static_cast<const char*>(p), static_cast<size_t>(stat.cbSize.QuadPart));
    GlobalUnlock(g);
  }
  return png;
}

}  // namespace

// ---- The Format panel: the selected object's fill, outline, text and alt text.

class WorkbookView::ShapePanel : public PanelContent {
 public:
  explicit ShapePanel(WorkbookView& view) : v_(view) {
    fill_.label = L"Fill";
    line_.label = L"Outline";
    weight_.label = L"Outline weight";
    fill_.items = {L"Automatic", L"No fill"};
    line_.items = {L"Automatic", L"No line"};
    for (const NamedColor& c : kColors) fill_.items.push_back(c.name), line_.items.push_back(c.name);
    weight_.items = {L"Automatic"};
    for (const wchar_t* w : kWeightNames) weight_.items.push_back(w);
    fill_.onChange = [this](int i) { Paint(data::kFormatFill, i); };
    line_.onChange = [this](int i) { Paint(data::kFormatLine, i); };
    weight_.onChange = [this](int i) {
      Apply(data::kFormatLine, [&](ObjectFormat& f) {
        f.lineWidth = i > 0 ? kWeights[i - 1] : 0;
        if (f.line == ObjectFormat::Paint::Automatic && i > 0) f.line = ObjectFormat::Paint::Solid, f.lineRgb = 0x000000;
      });
    };
    textBox_.autoComplete = alt_.autoComplete = false;
    textBox_.onCommit = [this](const std::wstring& t, int) { Apply(data::kFormatText, [&](ObjectFormat& f) { f.text = WideToUtf8(t); }); };
    alt_.onCommit = [this](const std::wstring& t, int) { Apply(data::kFormatAltText, [&](ObjectFormat& f) { f.altText = WideToUtf8(t); }); };
  }

  std::wstring Title() const override { return name_.empty() ? L"Format object" : L"Format " + name_; }

  std::vector<Control*> Layout(const D2D1_RECT_F& body, float ui) override {
    body_ = body;
    ui_ = ui;
    Place();
    return {&fill_, &line_, &weight_, &textBox_, &alt_};
  }

  void Draw(Canvas& c) override {
    for (const auto& [text, rc] : labels_) c.Text(text, rc, theme::P().textSecondary, 12 * ui_);
  }

  // The selected object's format into the controls.
  void Open() {
    const ObjectFormat f = Read();
    name_ = Utf8ToWide(f.name);
    auto index = [](ObjectFormat::Paint p, uint32_t rgb) {
      if (p == ObjectFormat::Paint::Automatic) return 0;
      if (p == ObjectFormat::Paint::None) return 1;
      for (size_t i = 0; i < std::size(kColors); ++i)
        if (kColors[i].rgb == rgb) return static_cast<int>(i) + 2;
      return -1;  // a color not offered: none shown
    };
    fill_.selected = index(f.fill, f.fillRgb);
    line_.selected = index(f.line, f.lineRgb);
    weight_.selected = 0;
    for (size_t i = 0; i < std::size(kWeights); ++i)
      if (std::abs(kWeights[i] - f.lineWidth) < 0.01) weight_.selected = static_cast<int>(i) + 1;
    textBox_.SetText(Utf8ToWide(f.text));
    alt_.SetText(Utf8ToWide(f.altText));
    fill_.enabled = f.kind == ObjectFormat::Kind::Shape;
    line_.enabled = weight_.enabled = f.kind != ObjectFormat::Kind::Other;
    textBox_.enabled = f.hasText;
    Place();
  }

  // One property set from the script (object:fill,...): the same path as the controls.
  bool Set(uint32_t what, const std::function<void(ObjectFormat&)>& change) { return Apply(what, change); }

 private:
  ObjectFormat Read() const {
    const std::string d = v_.Ed().SheetDrawing(static_cast<int>(v_.sheet_), false);
    return d.empty() || !v_.SelectedObject() ? ObjectFormat{} : data::ReadObjectFormat(v_.Ed().AnchorXml(d, v_.object_));
  }

  void Paint(uint32_t what, int i) {
    if (i < 0) return;
    Apply(what, [&](ObjectFormat& f) {
      const ObjectFormat::Paint p = i == 0 ? ObjectFormat::Paint::Automatic : i == 1 ? ObjectFormat::Paint::None : ObjectFormat::Paint::Solid;
      const uint32_t rgb = i >= 2 ? kColors[i - 2].rgb : 0;
      if (what == data::kFormatFill) f.fill = p, f.fillRgb = rgb;
      else f.line = p, f.lineRgb = rgb;
    });
  }

  bool Apply(uint32_t what, const std::function<void(ObjectFormat&)>& change) {
    if (!v_.SelectedObject()) return false;
    const int sheet = static_cast<int>(v_.sheet_);
    const std::string d = v_.Ed().SheetDrawing(sheet, false);
    const std::string anchor = d.empty() ? std::string() : v_.Ed().AnchorXml(d, v_.object_);
    ObjectFormat f = data::ReadObjectFormat(anchor);
    change(f);
    const std::string xml = data::FormatObject(anchor, f, what);
    const char16_t* step = what == data::kFormatAltText ? u"Alt Text" : what == data::kFormatText ? u"Edit Text" : u"Format Object";
    if (xml.empty() || v_.EditRefused(v_.Ed().SetObjectXml(sheet, v_.object_, xml, step))) return false;
    v_.host_.ViewInvalidate();
    Open();
    return true;
  }

  void Place() {
    const float u = ui_, left = body_.left + 16 * u, right = body_.right - 16 * u;
    float y = body_.top + 10 * u;
    labels_.clear();
    auto row = [&](const wchar_t* label, Control& c) {
      labels_.push_back({label, RectF(left, y, right, y + 16 * u)});
      y += 20 * u;
      c.rc = RectF(left, y, right, y + 30 * u);
      y += 42 * u;
    };
    row(L"Fill", fill_);
    row(L"Outline", line_);
    row(L"Outline weight", weight_);
    row(L"Text", textBox_);
    row(L"Alt text", alt_);
    v_.host_.ViewInvalidate();
  }

  WorkbookView& v_;
  D2D1_RECT_F body_{};
  float ui_ = 1;
  std::wstring name_;
  Dropdown fill_, line_, weight_;
  ComboBox textBox_, alt_;
  std::vector<std::pair<std::wstring, D2D1_RECT_F>> labels_;
};

// ---- Inserting

draw::Rect WorkbookView::NewObjectRect(double width, double height) const {
  // At the active cell's top left, as Excel puts a picture or a clicked shape.
  const float x = static_cast<float>(static_cast<double>(grid_.ColLeft(active_.col)) * kPtPerPx);
  const float y = static_cast<float>(static_cast<double>(grid_.RowTop(active_.row)) * kPtPerPx);
  return {x, y, x + static_cast<float>(width), y + static_cast<float>(height)};
}

void WorkbookView::SelectNewObject(uint32_t id) {
  object_ = id;
  ribbonInserted_ = id;  // the ribbon shows its Format tab (R1)
  objectSheet_ = sheet_;
  host_.ViewInvalidate();
  host_.ViewStateChanged();
}

bool WorkbookView::InsertPictureBytes(const std::string& bytes, const std::string& altText) {
  if (!Ready() || preview_ || ChartSheet()) return false;
  if (editor_.Active() && !CommitEdit(false)) return false;
  std::string ext, type;
  std::shared_ptr<draw::Image> image;
  if (data::PictureType(bytes, ext, type)) image = draw::Image::Open(std::vector<uint8_t>(bytes.begin(), bytes.end()));
  if (!image) {
    host_.ViewMessage(BannerKind::Warning, L"That isn't a picture Featherweight adds: PNG, JPEG, GIF or BMP.");
    return false;
  }
  // Its own size: its pixels at the resolution it records (96 dpi when none), as Excel inserts it.
  const draw::Size n = image->NaturalSize();
  uint32_t id = 0;
  if (EditRefused(Ed().InsertPicture(static_cast<int>(sheet_), bytes, PlaceFor(NewObjectRect(std::max(1.0f, n.width), std::max(1.0f, n.height))),
                                     altText, &id)))
    return false;
  SelectNewObject(id);
  return true;
}

bool WorkbookView::InsertShapeOf(data::ShapeKind kind, const std::string& text) {
  if (!Ready() || preview_ || ChartSheet()) return false;
  if (editor_.Active() && !CommitEdit(false)) return false;
  // An inch, as Excel's shape put in with a click; a line or an arrow an inch across, flat; a text
  // box a line high.
  const bool line = kind == ShapeKind::Line || kind == ShapeKind::Arrow;
  const draw::Rect r = NewObjectRect(kind == ShapeKind::TextBox ? 108 : 72, kind == ShapeKind::TextBox ? 28 : line ? 0 : 72);
  uint32_t id = 0;
  if (EditRefused(Ed().InsertShape(static_cast<int>(sheet_), kind, PlaceFor(r), text, &id))) return false;
  SelectNewObject(id);
  return true;
}

// ---- Sizes kept

std::vector<WorkbookView::HeldSize> WorkbookView::HeldSizes() {
  std::vector<HeldSize> out;
  const SheetObjects* objects = Ready() && !ChartSheet() ? Objects() : nullptr;
  if (!objects) return out;
  const std::vector<SheetObjects::Object> all = objects->Objects();
  if (all.empty()) return out;
  const std::string drawing = Ed().SheetDrawing(static_cast<int>(sheet_), false);
  if (drawing.empty()) return out;
  for (const SheetObjects::Object& o : all) {
    const std::string a = Ed().AnchorXml(drawing, o.id);
    const std::string_view start = std::string_view(a).substr(0, a.find('>'));  // its start tag
    if (start.find("twoCellAnchor") == std::string_view::npos ||
        (start.find("editAs=\"oneCell\"") == std::string_view::npos && start.find("editAs='oneCell'") == std::string_view::npos))
      continue;
    out.push_back({o.id, o.rect.Width(), o.rect.Height()});
  }
  return out;
}

edit::Result WorkbookView::KeepSizes(const std::vector<HeldSize>& held) {
  if (held.empty()) return {};
  // The grid as the change left it (the group's end lays it out again).
  grid_.Build(Current(), book_->GetStyles(), *measure_);
  PlaceObjects();
  const SheetObjects* objects = Objects();
  if (!objects) return {};
  const std::vector<SheetObjects::Object> all = objects->Objects();
  for (const HeldSize& h : held)
    for (const SheetObjects::Object& o : all)
      if (o.id == h.id && (std::abs(o.rect.Width() - h.width) > 0.05f || std::abs(o.rect.Height() - h.height) > 0.05f)) {
        const draw::Rect r{o.rect.left, o.rect.top, o.rect.left + h.width, o.rect.top + h.height};
        if (edit::Result res = Ed().MoveObject(static_cast<int>(sheet_), o.id, PlaceFor(r)); !res) return res;
      }
  return {};
}

bool WorkbookView::AspectLocked(uint32_t id, const draw::Rect& start) {
  AspectSeen& s = aspectSeen_;
  if (s.id == id && s.start.left == start.left && s.start.top == start.top && s.start.right == start.right &&
      s.start.bottom == start.bottom)
    return s.locked;
  s = {id, start, false};
  const std::string drawing = Ready() && !ChartSheet() ? Ed().SheetDrawing(static_cast<int>(sheet_), false) : std::string();
  if (drawing.empty()) return false;
  // The object's own locks (xdr:cNvPicPr/a:picLocks, cNvSpPr/a:spLocks...), the first one's.
  const std::string a = Ed().AnchorXml(drawing, id);
  const size_t at = a.find("noChangeAspect=");
  s.locked = at != std::string::npos && at + 16 < a.size() && (a[at + 16] == '1' || a.compare(at + 16, 4, "true") == 0);
  return s.locked;
}

bool WorkbookView::InsertPictureFiles(const std::vector<std::wstring>& files) {
  bool any = false;
  for (const std::wstring& path : files) {
    std::string bytes;
    if (!IsPictureFile(path) || !util::ReadFileBytes(path, bytes, kMaxPictureFile)) {
      host_.ViewMessage(BannerKind::Warning, L"“" + fw::FileNameOf(path) + L"” couldn't be read, or isn't a picture Featherweight adds.");
      continue;
    }
    if (!InsertPictureBytes(bytes, "")) break;
    any = true;
  }
  return any;
}

void WorkbookView::InsertPictureDialog() {
  ComPtr<IFileOpenDialog> dlg;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
  COMDLG_FILTERSPEC filters[] = {{L"Pictures", L"*.png;*.jpg;*.jpeg;*.jpe;*.jfif;*.gif;*.bmp;*.dib"}, {L"All files", L"*.*"}};
  dlg->SetFileTypes(2, filters);
  dlg->SetTitle(L"Insert picture");
  dlg->SetOkButtonLabel(L"Insert");
  DWORD opts = 0;
  dlg->GetOptions(&opts);
  dlg->SetOptions(opts | FOS_ALLOWMULTISELECT | FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM);
  ComPtr<IShellItemArray> items;
  if (FAILED(dlg->Show(host_.ViewHwnd())) || FAILED(dlg->GetResults(&items))) return;
  DWORD count = 0;
  items->GetCount(&count);
  std::vector<std::wstring> files;
  for (DWORD i = 0; i < count; ++i) {
    ComPtr<IShellItem> item;
    PWSTR path = nullptr;
    if (SUCCEEDED(items->GetItemAt(i, &item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
      files.push_back(path);
      CoTaskMemFree(path);
    }
  }
  InsertPictureFiles(files);
}

bool WorkbookView::PastePicture() {
  // Cells first: this workbook's own copy, or any text (cells copied from Excel come with a
  // picture of themselves too). A picture copied from a browser or a paint program has none.
  if (clip_ && util::ClipboardSequence() == clip_->seq) return false;
  if (util::UsingPrivateClipboard()) return false;  // a test run: never the user's clipboard
  if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
    const std::wstring text = util::GetClipboardText();
    if (std::any_of(text.begin(), text.end(), [](wchar_t c) { return !std::iswspace(c) && c != 0xFFFC; })) return false;
  }
  static const UINT png = RegisterClipboardFormatW(L"PNG"), jfif = RegisterClipboardFormatW(L"JFIF");
  std::vector<uint8_t> raw = ClipboardBytes(png);
  if (raw.empty()) raw = ClipboardBytes(jfif);
  std::string bytes(raw.begin(), raw.end());
  if (bytes.empty()) bytes = DibToPng(ClipboardBytes(CF_DIBV5));
  if (bytes.empty()) bytes = DibToPng(ClipboardBytes(CF_DIB));
  if (!bytes.empty()) {
    InsertPictureBytes(bytes, "");
    return true;
  }
  if (IsClipboardFormatAvailable(CF_HDROP) && OpenClipboard(nullptr)) {
    std::vector<std::wstring> files;
    if (HDROP drop = static_cast<HDROP>(GetClipboardData(CF_HDROP))) {
      const UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
      for (UINT i = 0; i < n && i < 64; ++i) {
        wchar_t path[MAX_PATH * 2];
        if (DragQueryFileW(drop, i, path, static_cast<UINT>(std::size(path)))) files.push_back(path);
      }
    }
    CloseClipboard();
    if (!files.empty() && std::all_of(files.begin(), files.end(), IsPictureFile)) {
      InsertPictureFiles(files);
      return true;
    }
  }
  return false;
}

void WorkbookView::ShowShapePanel() {
  if (!SelectedObject()) return;
  if (!shapePanel_) shapePanel_ = std::make_shared<ShapePanel>(*this);
  static_cast<ShapePanel*>(shapePanel_.get())->Open();
  host_.ViewShowPanel(shapePanel_.get());
}

// ---- Commands

bool WorkbookView::ShapesCommandState(int cmd, CommandState& s) {
  if (cmd < CmdObjectsBox || cmd > CmdFormatObject) return false;
  static const wchar_t* const kNames[] = {L"Insert",     L"Pictures…", L"Rectangle", L"Rounded rectangle", L"Oval",
                                          L"Line",       L"Arrow",     L"Text box",  L"Format object…"};
  s.label = kNames[cmd - CmdObjectsBox];
  s.enabled = Ready() && !preview_ && !ChartSheet() && (Current().Kind() == SheetKind::Worksheet || !Current().PartName().empty());
  if (cmd == CmdFormatObject) s.enabled = s.enabled && SelectedObject().has_value();
  return true;
}

bool WorkbookView::ShapesCommand(int cmd) {
  CommandState s;
  if (!ShapesCommandState(cmd, s)) return false;
  if (!s.enabled) return true;
  static constexpr ShapeKind kKinds[] = {ShapeKind::Rectangle, ShapeKind::RoundedRectangle, ShapeKind::Oval,
                                         ShapeKind::Line,      ShapeKind::Arrow,            ShapeKind::TextBox};
  if (cmd == CmdObjectPicture) InsertPictureDialog();
  else if (cmd >= CmdObjectRectangle && cmd <= CmdObjectTextBox) InsertShapeOf(kKinds[cmd - CmdObjectRectangle], "");
  else if (cmd == CmdFormatObject) ShowShapePanel();
  return true;
}

bool WorkbookView::ShapesScriptOp(const std::wstring& arg) {
  // object:picture,<path>; object:<rect|roundrect|oval|line|arrow|textbox>[,<text>]; on the selected
  // object: object:fill,<RRGGBB|none|auto>; object:outline,<RRGGBB|none|auto>[,<points>];
  // object:text,<text>; object:alt,<text>; object:panel (the Format panel).
  const size_t comma = arg.find(L',');
  const std::wstring what = arg.substr(0, comma), rest = comma == std::wstring::npos ? std::wstring() : arg.substr(comma + 1);
  static const std::pair<const wchar_t*, ShapeKind> kKinds[] = {{L"rect", ShapeKind::Rectangle}, {L"roundrect", ShapeKind::RoundedRectangle},
                                                                 {L"oval", ShapeKind::Oval},      {L"line", ShapeKind::Line},
                                                                 {L"arrow", ShapeKind::Arrow},    {L"textbox", ShapeKind::TextBox}};
  if (what == L"picture") return InsertPictureFiles({rest});
  if (what == L"panel") return ShapesCommand(CmdFormatObject), shapePanel_ != nullptr;
  for (const auto& [name, kind] : kKinds)
    if (what == name) return InsertShapeOf(kind, WideToUtf8(rest));
  if (!SelectedObject()) return false;
  if (!shapePanel_) shapePanel_ = std::make_shared<ShapePanel>(*this);
  auto* p = static_cast<ShapePanel*>(shapePanel_.get());
  auto paint = [](const std::wstring& v, ObjectFormat::Paint& paint, uint32_t& rgb) {
    const std::wstring c = v.substr(0, v.find(L','));
    if (c == L"auto") paint = ObjectFormat::Paint::Automatic;
    else if (c == L"none") paint = ObjectFormat::Paint::None;
    else paint = ObjectFormat::Paint::Solid, rgb = static_cast<uint32_t>(std::wcstoul(c.c_str(), nullptr, 16)) & 0xFFFFFF;
  };
  if (what == L"fill") return p->Set(data::kFormatFill, [&](ObjectFormat& f) { paint(rest, f.fill, f.fillRgb); });
  if (what == L"outline")
    return p->Set(data::kFormatLine, [&](ObjectFormat& f) {
      paint(rest, f.line, f.lineRgb);
      if (const size_t c = rest.find(L','); c != std::wstring::npos) f.lineWidth = _wtof(rest.c_str() + c + 1);
    });
  if (what == L"text") return p->Set(data::kFormatText, [&](ObjectFormat& f) { f.text = WideToUtf8(rest); });
  if (what == L"alt") return p->Set(data::kFormatAltText, [&](ObjectFormat& f) { f.altText = WideToUtf8(rest); });
  return false;
}

}  // namespace fw::sheet::view
