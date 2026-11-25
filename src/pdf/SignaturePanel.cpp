// The Signature panel (F1, f1-pdf): see SignaturePanel.h.
#include "pdf/SignaturePanel.h"

#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>

#include "Util.h"
#include "draw/Images.h"
#include "pdf/PdfText.h"
#include "ui/Theme.h"

using namespace gfx;
using Microsoft::WRL::ComPtr;

std::wstring PdfView::SignaturePanel::Title() const { return T(L"Signature"); }

std::vector<Control*> PdfView::SignaturePanel::Layout(const D2D1_RECT_F& body, float ui) {
  body_ = body;
  ui_ = ui;
  const float u = ui, l = body.left + 16 * u, r = body.right - 16 * u;
  pad_ = RectF(l, body.top + 12 * u, r, body.top + 12 * u + 150 * u);
  const float row = pad_.bottom + 10 * u, half = (r - l - 8 * u) / 2;
  buttons_[kClear] = RectF(l, row, l + half, row + 32 * u);
  buttons_[kPicture] = RectF(r - half, row, r, row + 32 * u);
  buttons_[kPlace] = RectF(l, row + 42 * u, r, row + 78 * u);
  hint_ = RectF(l, row + 88 * u, r, body.bottom - 8 * u);
  return {};
}

pdfsign::Ink PdfView::SignaturePanel::Ink() const {
  if (!picture_.empty()) {
    pdfsign::Ink ink;
    ink.bgra = picture_;
    ink.width = pictureW_;
    ink.height = pictureH_;
    return ink;
  }
  return pdfsign::InkFromPoints(strokes_);
}

bool PdfView::SignaturePanel::Enabled(int b) const {
  if (b == kClear) return !Empty();
  if (b == kPlace) return !Empty() && v_.Ready() && v_.PageToolsBlocked().empty();
  return true;
}

int PdfView::SignaturePanel::ButtonAt(float x, float y) const {
  for (int b = 0; b < kButtonCount; ++b)
    if (Contains(buttons_[b], x, y)) return b;
  return -1;
}

void PdfView::SignaturePanel::Draw(Canvas& c) {
  const Palette& p = theme::P();
  const float u = ui_;
  // The pad: paper, with a line to sign on.
  c.FillRound(pad_, 6 * u, Rgb(0xFFFFFF));
  c.StrokeRound(pad_, 6 * u, p.inputBorder, std::max(1.0f, u));
  const float lineY = pad_.bottom - 34 * u;
  c.Line(pad_.left + 16 * u, lineY, pad_.right - 16 * u, lineY, Rgb(0xB0B4BC), std::max(1.0f, u));
  c.Text(L"×", RectF(pad_.left + 16 * u, lineY - 22 * u, pad_.left + 34 * u, lineY), Rgb(0x8A8F99), 15 * u);
  if (!picture_.empty()) {
    if (!pictureBmp_ || pictureDc_ != c.DC()) {
      pictureDc_ = c.DC();
      pictureBmp_.Reset();
      D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
          D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
      c.DC()->CreateBitmap(D2D1::SizeU(pictureW_, pictureH_), picture_.data(), pictureW_ * 4, props, &pictureBmp_);
    }
    if (pictureBmp_) {
      const D2D1_RECT_F room = Inflate(pad_, -10 * u, -10 * u);
      const float k = std::min(Width(room) / pictureW_, Height(room) / pictureH_);
      const float w = pictureW_ * k, h = pictureH_ * k, cx = (room.left + room.right) / 2, cy = (room.top + room.bottom) / 2;
      c.DC()->DrawBitmap(pictureBmp_.Get(), RectF(cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2), 1.0f,
                         D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }
  } else if (strokes_.empty()) {
    c.Text(T(L"Sign here with a pen or the mouse"), RectF(pad_.left, pad_.top + 20 * u, pad_.right, pad_.top + 44 * u),
           Rgb(0x8A8F99), 13 * u, DWRITE_FONT_WEIGHT_REGULAR, Align::Center);
  }
  const D2D1_COLOR_F ink = Rgb(0x1A2F80);
  for (const auto& s : strokes_) {
    for (size_t i = 1; i < s.size(); ++i)
      c.Line(pad_.left + s[i - 1].first * u, pad_.top + s[i - 1].second * u, pad_.left + s[i].first * u,
             pad_.top + s[i].second * u, ink, 2.2f * u);
    if (s.size() == 1) c.FillEllipse(pad_.left + s[0].first * u, pad_.top + s[0].second * u, 1.4f * u, 1.4f * u, ink);
  }
  // The buttons: Clear and Picture…, then Place, the main one.
  const std::wstring labels[kButtonCount] = {T(L"Clear"), T(L"Picture…"), T(L"Place on a page")};
  for (int b = 0; b < kButtonCount; ++b) {
    const bool on = Enabled(b), primary = b == kPlace;
    D2D1_COLOR_F fill = primary ? (on ? p.accent : p.hover) : p.inputBg;
    if (on && b == pressed_) fill = primary ? p.accentPressed : p.pressed;
    else if (on && b == hover_) fill = primary ? p.accentHover : p.hover;
    c.FillRound(buttons_[b], 4 * u, fill);
    if (!primary) c.StrokeRound(buttons_[b], 4 * u, p.inputBorder, std::max(1.0f, u));
    c.Text(labels[b], buttons_[b], primary ? (on ? p.onAccent : p.textDisabled) : (on ? p.text : p.textDisabled),
           13 * u, primary ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_REGULAR, Align::Center);
  }
  const std::wstring hint = v_.PlacingSignature()
                                ? T(L"Click on a page where the signature goes. Esc stops.")
                                : T(L"Placed as a stamp that other PDF readers show and print. It isn't a certificate "
                                    L"signature: for one, use Sign, Sign with a certificate.");
  c.TextWrapped(hint, hint_, v_.PlacingSignature() ? p.accentText : p.textSecondary, 12 * u, 5);
}

bool PdfView::SignaturePanel::OnMouseDown(float x, float y, bool) {
  if (Contains(pad_, x, y)) {
    if (!picture_.empty()) {
      picture_.clear();
      pictureBmp_.Reset();
    }
    strokes_.push_back({{(x - pad_.left) / ui_, (y - pad_.top) / ui_}});
    drawing_ = true;
    return true;
  }
  pressed_ = ButtonAt(x, y);
  if (pressed_ >= 0 && !Enabled(pressed_)) pressed_ = -1;
  return pressed_ >= 0;
}

bool PdfView::SignaturePanel::OnMouseMove(float x, float y) {
  if (drawing_ && !strokes_.empty()) {
    const float px = std::clamp(x, pad_.left, pad_.right), py = std::clamp(y, pad_.top, pad_.bottom);
    const std::pair<float, float> pt{(px - pad_.left) / ui_, (py - pad_.top) / ui_};
    const auto& last = strokes_.back().back();
    if (std::hypot(pt.first - last.first, pt.second - last.second) < 1.0f) return false;
    strokes_.back().push_back(pt);
    return true;
  }
  const int b = ButtonAt(x, y);
  if (b == hover_) return false;
  hover_ = b;
  return true;
}

void PdfView::SignaturePanel::OnMouseUp(float x, float y) {
  if (drawing_) {
    drawing_ = false;
    v_.host_.ViewInvalidate();
    return;
  }
  const int b = pressed_;
  pressed_ = -1;
  if (b >= 0 && ButtonAt(x, y) == b) Press(b);
  v_.host_.ViewInvalidate();
}

void PdfView::SignaturePanel::Press(int b) {
  switch (b) {
    case kClear:
      strokes_.clear();
      picture_.clear();
      pictureBmp_.Reset();
      v_.StopPlacingSignature();
      break;
    case kPicture: PickPicture(); break;
    case kPlace: v_.StartPlacingSignature(); break;
  }
}

bool PdfView::SignaturePanel::PickPicture() {
  ComPtr<IFileOpenDialog> dlg;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return false;
  const std::wstring name = T(L"Pictures");
  const COMDLG_FILTERSPEC types[] = {{name.c_str(), L"*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tif;*.tiff"}};
  dlg->SetFileTypes(1, types);
  const std::wstring title = T(L"A picture of your signature");
  dlg->SetTitle(title.c_str());
  ComPtr<IShellItem> item;
  PWSTR file = nullptr;
  if (FAILED(dlg->Show(v_.host_.ViewHwnd())) || FAILED(dlg->GetResult(&item)) ||
      FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &file)))
    return false;
  const std::wstring path = file;
  CoTaskMemFree(file);
  std::string bytes;
  std::shared_ptr<draw::Image> image;
  if (util::ReadFileBytes(path, bytes, size_t{32} << 20))
    image = draw::Image::Open(std::vector<uint8_t>(bytes.begin(), bytes.end()));
  std::shared_ptr<const draw::Bitmap> px = image ? image->Pixels(1200, 600) : nullptr;
  if (!px || px->Width() <= 0 || px->Height() <= 0) {
    v_.host_.ViewMessage(BannerKind::Error, T(L"That picture couldn't be read."));
    return false;
  }
  // At most 1200 pixels wide: a signature needs no more, and the PDF stays small.
  std::shared_ptr<const draw::Bitmap> fit = px;
  if (px->Width() > 1200 || px->Height() > 600) {
    const double k = std::min(1200.0 / px->Width(), 600.0 / px->Height());
    fit = draw::BoxResample(*px, std::max(1, static_cast<int>(px->Width() * k)),
                            std::max(1, static_cast<int>(px->Height() * k)));
  }
  if (!fit) return false;
  picture_.assign(fit->Pixels(), fit->Pixels() + static_cast<size_t>(fit->Stride()) * fit->Height());
  pictureW_ = fit->Width();
  pictureH_ = fit->Height();
  pictureBmp_.Reset();
  strokes_.clear();
  return true;
}

std::wstring PdfView::SignaturePanel::TooltipAt(float x, float y) {
  switch (ButtonAt(x, y)) {
    case kClear: return T(L"Start again");
    case kPicture: return T(L"Use a picture of your signature (a photo or a scan)");
    case kPlace: return T(L"Then click on a page where it goes");
    default: return {};
  }
}

void PdfView::SignaturePanel::DrawForTesting() {
  strokes_.clear();
  picture_.clear();
  std::vector<std::pair<float, float>> loop;
  for (int i = 0; i <= 120; ++i) {
    const float t = i / 120.0f;
    loop.emplace_back(20 + 240 * t, 80 - 30 * std::sin(t * 18.0f) * (1 - t * 0.5f));
  }
  strokes_.push_back(loop);
  strokes_.push_back({{60.f, 110.f}, {140.f, 104.f}, {230.f, 112.f}});
}
