// Shapes, text boxes and watermarks in the document tab (plan §6.2, milestone D4). Insert puts a
// shape at the caret's paragraph, selected; it's then moved, resized and turned with the picture
// frame's handles (DocPictures.cpp), changed in the Shape panel, and typing goes into its text. The
// Watermark panel puts text or a picture behind every page, or takes it off. The changes are
// fw::doc::edit::Shapes' and Watermark's, each one undo step.
#include <windows.h>
// After windows.h:
#include <dwrite.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>

#include "Commands.h"
#include "Util.h"
#include "core/Strings.h"
#include "ui/Gfx.h"
#include "ui/Icons.h"
#include "ui/Theme.h"
#include "word/DocView.h"
#include "word/ShapePanels.h"

using namespace gfx;
namespace edit = fw::doc::edit;
namespace layout = fw::doc::layout;
using fw::doc::kNone;
using Microsoft::WRL::ComPtr;

namespace {

constexpr double kEmuPerPt = 12700;
constexpr size_t kMaxPictureFile = 64u << 20;

std::wstring Wide(const std::u16string& s) { return std::wstring(s.begin(), s.end()); }
fw::Emu Emu(double pt) { return fw::Emu(std::llround(pt * kEmuPerPt)); }
layout::TextPos ToText(fw::doc::Pos p) { return {p.para, p.offset}; }

uint32_t Rgb(const D2D1_COLOR_F& c) {
  auto v = [](float x) { return static_cast<uint32_t>(std::lround(std::clamp(x, 0.0f, 1.0f) * 255)); };
  return (v(c.r) << 16) | (v(c.g) << 8) | v(c.b);
}
D2D1_COLOR_F Color(uint32_t rgb) {
  return {((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, 1.0f};
}

// The text's width over its height (one em) in `font`, measured with DirectWrite; 0 when it can't be.
double TextAspect(const std::u16string& text, const std::string& font) {
  IDWriteFactory* f = DWrite();
  if (!f || text.empty()) return 0;
  const std::wstring family = fw::Utf8ToWide(font), w(text.begin(), text.end());
  ComPtr<IDWriteTextFormat> format;
  ComPtr<IDWriteTextLayout> lay;
  DWRITE_TEXT_METRICS m{};
  if (FAILED(f->CreateTextFormat(family.c_str(), nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                 DWRITE_FONT_STRETCH_NORMAL, 100.0f, L"", &format)) ||
      FAILED(f->CreateTextLayout(w.c_str(), static_cast<UINT32>(w.size()), format.Get(), 1e6f, 1e3f, &lay)) ||
      FAILED(lay->GetMetrics(&m)))
    return 0;
  return m.widthIncludingTrailingWhitespace / 100.0;
}

}  // namespace

// ---------------------------------------------------------------------------
// Which shape

const edit::ShapeInfo& DocView::ShapeInfoOf(uint32_t drawing) const {
  const uint32_t version = drawing < doc_->Drawings().size() ? doc_->Drawings()[drawing].version : 0;
  const uint64_t key = (static_cast<uint64_t>(drawing) << 32) | version;
  if (shapeKey_ != key || shapeEdits_ != edits_) {
    shapeKey_ = key;
    shapeEdits_ = edits_;
    shapeInfo_ = shapes_ ? shapes_->Info(drawing) : edit::ShapeInfo{};
  }
  return shapeInfo_;
}

bool DocView::Movable(uint32_t drawing) const {
  return drawing != kNone && (PictureInfoOf(drawing).picture || ShapeInfoOf(drawing).shape);
}

std::u16string DocView::WhyNotMovable(uint32_t drawing) const {
  // The shape tools say why for shapes; for anything they don't take, the picture tools do.
  const std::u16string& why = ShapeInfoOf(drawing).why;
  if (!why.empty() && why.rfind(u"Only shapes", 0) != 0) return why;
  return PictureInfoOf(drawing).why;
}

// ---------------------------------------------------------------------------
// Inserting, and the shape's text

void DocView::InsertShape(edit::ShapeKind kind) {
  if (!Editing() || !shapes_ || placeholder_) return;
  TextPos at = std::min(anchor_, focus_);
  // From a text box's text, it goes beside the text box, as in Word.
  for (int depth = 0; depth < 8 && at.para < doc_->Paragraphs().size(); ++depth) {
    const uint32_t story = doc_->Para(at.para).story;
    const uint32_t owner = story < doc_->Stories().size() ? doc_->Stories()[story].drawing : kNone;
    if (owner >= doc_->Drawings().size() || doc_->Drawings()[owner].pos.para == kNone) break;
    at = ToText(doc_->Drawings()[owner].pos);
  }
  // On the page where its paragraph starts (it's anchored there), below the caret's line there.
  const auto caret = pagination_->CaretAt({at.para, 0});
  if (!caret || caret->page >= sizes_.size()) {
    host_.ViewMessage(BannerKind::Info, L"Put the caret where the shape goes on a page.");
    return;
  }
  using K = edit::ShapeKind;
  double w = 144, h = 90;  // points
  switch (kind) {
    case K::TextBox: w = 180, h = 72; break;
    case K::Oval: w = h = 108; break;
    case K::Line:
    case K::Arrow: h = 0; break;
    case K::Callout: w = 144, h = 72; break;
    default: break;
  }
  const fw::PageSizePt page = sizes_[caret->page];
  const double margin = 18;
  w = std::min(w, std::max(36.0, page.w - 2 * margin));
  double x = static_cast<double>(caret->x), y = static_cast<double>(caret->bottom) + 6;
  // Not right over one put there before: a step down and to the right.
  for (int tries = 0; tries < 20; ++tries) {
    bool taken = false;
    for (const layout::DrawingBox& box : pagination_->GetPage(caret->page).drawings)
      taken = taken || (std::fabs(box.rect.left - x) < 2 && std::fabs(box.rect.top - y) < 2);
    if (!taken) break;
    x += 18;
    y += 18;
  }
  x = std::clamp(x, margin, std::max(margin, page.w - margin - w));
  y = std::clamp(y, margin, std::max(margin, page.h - margin - h));
  if (!ApplyPicture(shapes_->Insert(at.para, kind, Emu(x), Emu(y), Emu(w), Emu(h)))) return;
  if (kind == K::TextBox || kind == K::Callout) EnterShapeText(false);  // ready to type in, as in Word
}

bool DocView::EnterShapeText(bool add) {
  const uint32_t d = SelectedPicture();
  if (d == kNone || !shapes_ || PictureInfoOf(d).picture || !ShapeInfoOf(d).shape) return false;
  uint32_t story = shapes_->TextStory(d);
  if (story == kNone) {
    if (!add || !Apply(shapes_->AddText(d))) return false;
    story = shapes_->TextStory(d);
  }
  if (story == kNone) return false;
  const std::vector<uint32_t> paras = doc_->StoryParagraphs(story);
  if (paras.empty()) return false;
  const uint32_t last = paras.back();
  anchor_ = focus_ = {last, static_cast<uint32_t>(doc_->Para(last).text.size())};
  goalX_ = -1;
  editor_->BreakTyping();
  caretRevision_ = UINT32_MAX;
  host_.ViewInvalidate();
  host_.ViewStateChanged();
  return true;
}

bool DocView::TypeIntoShape(std::u16string_view text) {
  const uint32_t d = SelectedPicture();
  if (d == kNone || !shapes_ || !Editing() || PictureInfoOf(d).picture || !ShapeInfoOf(d).shape || ShapeInfoOf(d).line)
    return false;
  if (EnterShapeText(true) && !text.empty()) Apply(editor_->InsertText({focus_.para, focus_.offset}, text));
  return true;  // the shape stays when its text can't be had
}

bool DocView::PickPictureFile(std::vector<uint8_t>& bytes, const wchar_t* title) {
  ComPtr<IFileOpenDialog> dlg;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return false;
  COMDLG_FILTERSPEC filters[] = {{L"Pictures", L"*.png;*.jpg;*.jpeg;*.jpe;*.jfif;*.gif;*.bmp;*.dib;*.tif;*.tiff"},
                                 {L"All files", L"*.*"}};
  dlg->SetFileTypes(2, filters);
  dlg->SetTitle(title);
  DWORD opts = 0;
  dlg->GetOptions(&opts);
  dlg->SetOptions(opts | FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM);
  ComPtr<IShellItem> item;
  PWSTR path = nullptr;
  if (FAILED(dlg->Show(host_.ViewHwnd())) || FAILED(dlg->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
    return false;
  const std::wstring file = path;
  CoTaskMemFree(path);
  std::string data;
  if (!util::ReadFileBytes(file, data, kMaxPictureFile)) {
    host_.ViewMessage(BannerKind::Warning, L"“" + fw::FileNameOf(file) + L"” couldn't be read, or is too big to add.");
    return false;
  }
  bytes.assign(data.begin(), data.end());
  return true;
}

// ---------------------------------------------------------------------------
// Commands

std::optional<CommandState> DocView::QueryShape(int cmd) {
  if (cmd < CmdInsertTextBox || cmd > CmdWatermarkRemove) return std::nullopt;
  CommandState s;
  s.enabled = Editing() && shapes_ != nullptr;
  const uint32_t d = s.enabled ? SelectedPicture() : kNone;
  const bool shape = d != kNone && !PictureInfoOf(d).picture && ShapeInfoOf(d).shape;
  switch (cmd) {
    case CmdShapeAddText: s.enabled = shape && !ShapeInfoOf(d).text && !ShapeInfoOf(d).line; break;
    case CmdShapeEditText: s.enabled = shape && ShapeInfoOf(d).text; break;
    case CmdShapePanel: s.checked = shapePanel_ && host_.ViewShownPanel() == shapePanel_.get(); break;
    case CmdWatermarkPanel: s.checked = watermarkPanel_ && host_.ViewShownPanel() == watermarkPanel_.get(); break;
    default: break;
  }
  return s;
}

bool DocView::ExecuteShape(int cmd) {
  if (cmd < CmdInsertTextBox || cmd > CmdWatermarkRemove) return false;
  if (!Editing() || !shapes_) return true;
  if (cmd <= CmdInsertCallout) {
    InsertShape(static_cast<edit::ShapeKind>(cmd - CmdInsertTextBox));
    return true;
  }
  auto selected = [this] {
    const uint32_t d = SelectedPicture();
    return d != kNone && ShapeInfoOf(d).shape ? d : kNone;
  };
  switch (cmd) {
    case CmdShapeAddText:
    case CmdShapeEditText:
      if (selected() != kNone) EnterShapeText(cmd == CmdShapeAddText);
      break;
    case CmdShapePanel: {
      if (!shapePanel_) {
        ShapePanel::Actions a;
        a.wrap = [this](edit::Wrap w) { SetPictureWrap(w); };
        a.size = [this, selected](double w, double h) {
          if (uint32_t d = selected(); d != kNone) ApplyPicture(shapes_->Resize(d, Emu(w), Emu(h)));
        };
        a.rotate = [this, selected](double deg) {
          if (uint32_t d = selected(); d != kNone) ApplyPicture(shapes_->Rotate(d, deg));
        };
        a.fill = [this, selected](std::optional<uint32_t> rgb) {
          if (uint32_t d = selected(); d != kNone) ApplyPicture(shapes_->SetFill(d, rgb));
        };
        a.outline = [this, selected](std::optional<uint32_t> rgb, double points) {
          if (uint32_t d = selected(); d != kNone) ApplyPicture(shapes_->SetOutline(d, rgb, points));
        };
        a.addText = [this] { Execute(CmdShapeAddText); };
        shapePanel_ = std::make_unique<ShapePanel>(std::move(a));
      }
      const bool open = host_.ViewShownPanel() == shapePanel_.get();
      host_.ViewShowPanel(open ? nullptr : shapePanel_.get());
      SyncShapePanels();
      break;
    }
    case CmdWatermarkPanel: {
      if (!watermarkPanel_) {
        WatermarkPanel::Actions a;
        // The headers change, not the text: the caret stays.
        a.text = [this](const edit::TextWatermark& mark) {
          edit::TextWatermark m = mark;
          m.aspect = TextAspect(m.text, m.font);
          ApplyPageChange(edit::SetTextWatermark(*editor_, m), false);
        };
        a.picture = [this](bool washout) {
          std::vector<uint8_t> bytes;
          if (PickPictureFile(bytes, L"Watermark picture")) ApplyPageChange(edit::SetPictureWatermark(*editor_, bytes, washout), false);
          watermarkShown_ = UINT64_MAX;  // shows what there is, chosen or not
          SyncShapePanels();
        };
        a.remove = [this] { Execute(CmdWatermarkRemove); };
        watermarkPanel_ = std::make_unique<WatermarkPanel>(std::move(a));
      }
      const bool open = host_.ViewShownPanel() == watermarkPanel_.get();
      host_.ViewShowPanel(open ? nullptr : watermarkPanel_.get());
      SyncShapePanels();
      break;
    }
    case CmdWatermarkRemove:
      if (edit::FindWatermark(*doc_).kind != edit::WatermarkInfo::Kind::None)
        ApplyPageChange(edit::RemoveWatermark(*editor_), false);
      break;
  }
  return true;
}

void DocView::SyncShapePanels() {
  if (shapePanel_ && host_.ViewShownPanel() == shapePanel_.get()) {
    const uint32_t d = SelectedPicture();
    const uint64_t key = d == kNone ? UINT64_MAX - 1 : (static_cast<uint64_t>(edits_) << 32) | d;
    if (key != shapeShown_) {
      shapeShown_ = key;
      if (d == kNone) shapePanel_->Show(std::nullopt, L"Select a shape or text box to change how it looks.");
      else if (!ShapeInfoOf(d).shape) shapePanel_->Show(std::nullopt, Wide(ShapeInfoOf(d).why));
      else shapePanel_->Show(ShapeInfoOf(d));
    }
  } else {
    shapeShown_ = UINT64_MAX;  // shown again when it opens
  }
  if (watermarkPanel_ && host_.ViewShownPanel() == watermarkPanel_.get()) {
    if (watermarkShown_ != edits_) {
      watermarkShown_ = edits_;
      watermarkPanel_->Show(edit::FindWatermark(*doc_), Editing());
    }
  } else {
    watermarkShown_ = UINT64_MAX;
  }
}

// ---------------------------------------------------------------------------
// The panels

namespace {
constexpr float kLabelW = 96;  // DIPs
constexpr float kRow = 44;
}  // namespace

std::vector<Control*> RowsPanel::Layout(const D2D1_RECT_F& body, float ui) {
  ui_ = ui;
  body_ = body;
  headings_.clear();
  std::vector<Control*> controls;
  if (!shown_) return controls;
  const float u = ui, labelX = body.left + 20 * u, cx = body.left + (20 + kLabelW) * u, right = body.right - 20 * u;
  float y = body.top + 4 * u;
  for (auto& [label, c] : rows_) {
    if (!c) {
      if (!headings_.empty()) y += 12 * u;
      headings_.push_back({label, RectF(labelX, y, right, y + 24 * u)});
      y += 32 * u;
      continue;
    }
    if (!c->visible) continue;
    c->rc = RectF(std::round(cx), std::round(y), std::round(right), std::round(y + 32 * u));
    controls.push_back(c);
    y += kRow * u;
  }
  return controls;
}

void RowsPanel::Draw(Canvas& c) {
  const Palette& p = theme::P();
  const float u = ui_;
  if (!shown_) {
    c.TextWrapped(why_, RectF(body_.left + 20 * u, body_.top + 8 * u, body_.right - 20 * u, body_.top + 200 * u), p.textSecondary,
                  13 * u, 6);
    return;
  }
  for (const auto& [text, rc] : headings_) c.Text(text, rc, p.text, 13 * u, DWRITE_FONT_WEIGHT_SEMI_BOLD);
  for (const auto& [label, control] : rows_) {
    if (!control || !control->visible || label.empty()) continue;
    c.Text(label, RectF(control->rc.left - kLabelW * u, control->rc.top, control->rc.left - 8 * u, control->rc.bottom),
           p.textSecondary, 13 * u);
  }
}

// ---- Shape

ShapePanel::ShapePanel(Actions actions) : actions_(std::move(actions)) {
  wchar_t measure[4] = {};
  metric_ = !(GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_IMEASURE, measure, 4) > 0 && measure[0] == L'1');
  wrap_.items = {L"In line with text", L"Square", L"Tight", L"Top and bottom", L"Behind text", L"In front of text"};
  wrap_.label = L"Wrap text";
  wrap_.onChange = [this](int i) {
    if (i >= 0 && i < 6 && actions_.wrap) actions_.wrap(static_cast<fw::doc::edit::Wrap>(i));
  };
  for (SpinBox* s : {&width_, &height_}) {
    s->decimals = 2;
    s->step = metric_ ? 0.1 : 0.05;
    s->minValue = 0;
    s->maxValue = metric_ ? 55.88 : 22;
    s->unit = metric_ ? L" cm" : L"″";
  }
  width_.label = L"Width";
  height_.label = L"Height";
  width_.onChange = [this](double v) {
    if (actions_.size) actions_.size(FromUnit(v), FromUnit(height_.value));
  };
  height_.onChange = [this](double v) {
    if (actions_.size) actions_.size(FromUnit(width_.value), FromUnit(v));
  };
  rotation_.minValue = 0;
  rotation_.maxValue = 359;
  rotation_.step = 15;
  rotation_.unit = L"°";
  rotation_.label = L"Rotation";
  rotation_.onChange = [this](double v) {
    if (actions_.rotate) actions_.rotate(v);
  };
  fill_.label = L"Fill";
  fill_.automaticLabel = L"No fill";
  fill_.onChange = [this](D2D1_COLOR_F c, bool none) {
    if (actions_.fill) actions_.fill(none ? std::nullopt : std::optional<uint32_t>(Rgb(c)));
  };
  outline_.label = L"Outline";
  outline_.automaticLabel = L"No outline";
  outline_.onChange = [this](D2D1_COLOR_F c, bool none) {
    if (actions_.outline) actions_.outline(none ? std::nullopt : std::optional<uint32_t>(Rgb(c)), 0);
  };
  weight_.decimals = 2;
  weight_.minValue = 0.25;
  weight_.maxValue = 20;
  weight_.step = 0.25;
  weight_.unit = L" pt";
  weight_.label = L"Outline weight";
  weight_.onChange = [this](double v) {
    // A weight on its own keeps the color it has (or black, when it had none Featherweight can read).
    if (actions_.outline) actions_.outline(outlineColor_ ? outlineColor_ : std::optional<uint32_t>(0), v);
  };
  addText_.text = L"Add text";
  addText_.glyph = icon::Add;
  addText_.label = L"Add text";
  addText_.onInvoke = [this] {
    if (actions_.addText) actions_.addText();
  };
  rows_ = {{L"Layout", nullptr},     {L"Wrap text", &wrap_}, {L"Size", nullptr},         {L"Width", &width_},
           {L"Height", &height_},    {L"Rotation", &rotation_}, {L"Look", nullptr},     {L"Fill", &fill_},
           {L"Outline", &outline_},  {L"Weight", &weight_}, {L"", &addText_}};
}

double ShapePanel::ToUnit(int64_t emu) const { return emu / (metric_ ? 360000.0 : 914400.0); }
double ShapePanel::FromUnit(double v) const { return v * (metric_ ? 72 / 2.54 : 72.0); }

void ShapePanel::Show(const std::optional<fw::doc::edit::ShapeInfo>& info, const std::wstring& why) {
  shown_ = info.has_value();
  why_ = why;
  for (auto& [label, c] : rows_)
    if (c) c->visible = shown_;
  if (!info) return;
  line_ = info->line;
  text_ = info->text;
  fill_.visible = !line_;
  addText_.visible = !line_ && !text_;
  wrap_.selected = static_cast<int>(info->wrap);
  width_.SetValue(ToUnit(info->cx.Value()));
  height_.SetValue(ToUnit(info->cy.Value()));
  rotation_.SetValue(std::round(info->rotation));
  fill_.automatic = info->noFill;
  fill_.color = info->fill ? Color(*info->fill) : D2D1_COLOR_F{1, 1, 1, 1};
  outline_.automatic = info->noOutline;
  outline_.color = info->outline ? Color(*info->outline) : D2D1_COLOR_F{0, 0, 0, 1};
  outlineColor_ = info->outline;
  weight_.SetValue(info->outlineWidth > 0 ? info->outlineWidth : 0.75);
}

std::vector<Control*> ShapePanel::Layout(const D2D1_RECT_F& body, float ui) {
  std::vector<Control*> controls = RowsPanel::Layout(body, ui);
  for (Control* c : controls)
    if (c != &wrap_) c->rc.right = std::min(c->rc.right, c->rc.left + std::round(140 * ui));
  return controls;
}

// ---- Watermark

WatermarkPanel::WatermarkPanel(Actions actions) : actions_(std::move(actions)) {
  kindBox_.items = {L"None", L"Text", L"Picture"};
  kindBox_.label = L"Watermark";
  kindBox_.onChange = [this](int i) {
    if (i == 0 && actions_.remove) actions_.remove();
    if (i == 1) ApplyText();
    if (i == 2 && actions_.picture) actions_.picture(washOut_);
  };
  text_.items = {L"DRAFT", L"CONFIDENTIAL", L"DO NOT COPY", L"SAMPLE", L"URGENT", L"ASAP"};
  text_.autoComplete = false;
  text_.label = L"Text";
  text_.onCommit = [this](const std::wstring&, int) { ApplyText(); };
  font_.items = {L"Calibri", L"Arial", L"Times New Roman", L"Segoe UI", L"Georgia", L"Verdana"};
  font_.autoComplete = true;
  font_.label = L"Font";
  font_.onCommit = [this](const std::wstring&, int) { ApplyText(); };
  color_.label = L"Color";
  color_.color = Color(0xC0C0C0);
  color_.onChange = [this](D2D1_COLOR_F, bool) { ApplyText(); };
  layout_.items = {L"Diagonal", L"Horizontal"};
  layout_.label = L"Layout";
  layout_.selected = 0;
  layout_.onChange = [this](int) { ApplyText(); };
  look_.items = {L"Semitransparent", L"Solid"};
  look_.label = L"Look";
  look_.selected = 0;
  look_.onChange = [this](int) { ApplyText(); };
  picture_.text = L"Choose picture…";
  picture_.glyph = icon::Picture;
  picture_.label = L"Choose picture";
  picture_.onInvoke = [this] {
    if (actions_.picture) actions_.picture(washOut_);
  };
  washout_.items = {L"Washout", L"Original colors"};
  washout_.label = L"Picture look";
  washout_.selected = 0;
  washout_.onChange = [this](int i) { washOut_ = i == 0; };
  remove_.text = L"Remove watermark";
  remove_.glyph = icon::Delete;
  remove_.label = L"Remove watermark";
  remove_.onInvoke = [this] {
    if (actions_.remove) actions_.remove();
  };
  rows_ = {{L"Watermark", nullptr}, {L"Kind", &kindBox_}, {L"Text", &text_},     {L"Font", &font_},
           {L"Color", &color_},     {L"Layout", &layout_}, {L"Look", &look_},    {L"Picture", &washout_},
           {L"", &picture_},        {L"", &remove_}};
}

fw::doc::edit::TextWatermark WatermarkPanel::Mark() const {
  fw::doc::edit::TextWatermark m;
  const std::wstring& text = text_.Text();
  if (!text.empty()) m.text.assign(text.begin(), text.end());
  if (!font_.Text().empty()) m.font = fw::WideToUtf8(font_.Text());
  m.color = color_.automatic ? 0xC0C0C0 : Rgb(color_.color);
  m.diagonal = layout_.selected != 1;
  m.semitransparent = look_.selected != 1;
  return m;
}

void WatermarkPanel::ApplyText() {
  if (actions_.text) actions_.text(Mark());
}

void WatermarkPanel::Show(const fw::doc::edit::WatermarkInfo& info, bool editable) {
  using Kind = fw::doc::edit::WatermarkInfo::Kind;
  shown_ = editable;
  why_ = editable ? L"" : L"This document can't be changed.";
  kind_ = info.kind;
  kindBox_.selected = static_cast<int>(info.kind);
  if (info.kind == Kind::Text) {
    text_.SetText(Wide(info.text));
    if (!info.font.empty()) font_.SetText(fw::Utf8ToWide(info.font));
    color_.color = Color(info.color);
    color_.automatic = false;
    layout_.selected = info.diagonal ? 0 : 1;
    look_.selected = info.semitransparent ? 0 : 1;
  } else if (text_.Text().empty()) {
    text_.SetText(L"DRAFT");
    font_.SetText(L"Calibri");
  }
  if (info.kind == Kind::Picture) {
    washout_.selected = info.washout ? 0 : 1;
    washOut_ = info.washout;
  }
  for (auto& [label, c] : rows_)
    if (c) c->visible = shown_;
  if (!shown_) return;
  for (Control* c : {static_cast<Control*>(&text_), static_cast<Control*>(&font_), static_cast<Control*>(&color_),
                     static_cast<Control*>(&layout_), static_cast<Control*>(&look_)})
    c->visible = info.kind == Kind::Text;
  washout_.visible = picture_.visible = info.kind == Kind::Picture;
  remove_.visible = info.kind != Kind::None;
}

std::vector<Control*> WatermarkPanel::Layout(const D2D1_RECT_F& body, float ui) {
  std::vector<Control*> controls = RowsPanel::Layout(body, ui);
  for (Control* c : controls)
    if (c == &picture_ || c == &remove_) c->rc.right = std::min(c->rc.right, c->rc.left + std::round(180 * ui));
  return controls;
}
