// Pictures in the document tab (plan §6.2 "Pictures", milestone D3): insert from a file, the
// clipboard or a drop; click to select; drag to move, the handles to resize and turn; crop mode;
// how text wraps; the Picture panel for size, rotation and alt text. The changes are
// fw::doc::edit::Pictures', each one undo step. A selected picture is a selection of its one
// character, so typing over it, Delete, Cut and Copy work as for text.
#include <windows.h>
// After windows.h:
#include <shellapi.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwctype>

#include "Commands.h"
#include "Util.h"
#include "core/Strings.h"
#include "draw/Images.h"
#include "ui/Gfx.h"
#include "ui/Icons.h"
#include "ui/Menu.h"
#include "ui/Theme.h"
#include "word/DocView.h"
#include "word/PicturePanel.h"

using namespace gfx;
namespace edit = fw::doc::edit;
namespace layout = fw::doc::layout;
using fw::doc::kNone;
using Microsoft::WRL::ComPtr;

namespace {

constexpr int kRotateHandle = 8;
constexpr double kEmuPerPt = 12700;
constexpr size_t kMaxPictureFile = 64u << 20;  // a picture file bigger than this isn't read

fw::doc::Pos ToDoc(layout::TextPos p) { return {p.para, p.offset}; }
layout::TextPos ToText(fw::doc::Pos p) { return {p.para, p.offset}; }

bool IsPictureFile(const std::wstring& path) {
  std::wstring ext = fw::ToLower(path.substr(path.find_last_of(L'.') == std::wstring::npos ? path.size() : path.find_last_of(L'.')));
  for (const wchar_t* e : {L".png", L".jpg", L".jpeg", L".jpe", L".jfif", L".gif", L".bmp", L".dib", L".tif", L".tiff", L".svg"})
    if (ext == e) return true;
  return false;
}

std::wstring Wide(const std::u16string& s) { return std::wstring(s.begin(), s.end()); }

// Clipboard data in `format` as bytes; empty when there's none.
std::vector<uint8_t> ClipboardData(UINT format) {
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

// A packed DIB (CF_DIB, CF_DIBV5) as a BMP file, which WIC reads.
std::vector<uint8_t> DibToBmp(const std::vector<uint8_t>& dib) {
  if (dib.size() < sizeof(BITMAPINFOHEADER)) return {};
  BITMAPINFOHEADER h;
  std::memcpy(&h, dib.data(), sizeof(h));
  if (h.biSize < sizeof(BITMAPINFOHEADER) || h.biSize > dib.size()) return {};
  // The color table and masks come after the header.
  uint32_t colors = h.biClrUsed ? h.biClrUsed : (h.biBitCount <= 8 ? 1u << h.biBitCount : 0);
  uint32_t masks = h.biSize == sizeof(BITMAPINFOHEADER) && h.biCompression == BI_BITFIELDS ? 12 : 0;
  uint64_t offset = 14ull + h.biSize + masks + colors * 4ull;
  if (offset > dib.size() + 14ull) return {};
  std::vector<uint8_t> bmp(14 + dib.size());
  BITMAPFILEHEADER f{};
  f.bfType = 0x4D42;
  f.bfSize = static_cast<DWORD>(bmp.size());
  f.bfOffBits = static_cast<DWORD>(offset);
  std::memcpy(bmp.data(), &f, 14);
  std::memcpy(bmp.data() + 14, dib.data(), dib.size());
  return bmp;
}

// A picture's pixels as a packed 32-bit DIB (bottom-up, straight alpha), for CF_DIB.
std::vector<uint8_t> BitmapToDib(const draw::Bitmap& b) {
  BITMAPINFOHEADER h{};
  h.biSize = sizeof(h);
  h.biWidth = b.Width();
  h.biHeight = b.Height();
  h.biPlanes = 1;
  h.biBitCount = 32;
  h.biCompression = BI_RGB;
  std::vector<uint8_t> out(sizeof(h) + static_cast<size_t>(b.Stride()) * b.Height());
  std::memcpy(out.data(), &h, sizeof(h));
  for (int y = 0; y < b.Height(); ++y) {
    const uint8_t* src = b.Pixels() + static_cast<size_t>(y) * b.Stride();
    uint8_t* dst = out.data() + sizeof(h) + static_cast<size_t>(b.Height() - 1 - y) * b.Stride();
    for (int x = 0; x < b.Width(); ++x) {
      const uint8_t a = src[x * 4 + 3];
      for (int k = 0; k < 3; ++k) dst[x * 4 + k] = a ? static_cast<uint8_t>(std::min(255, src[x * 4 + k] * 255 / a)) : 0;
      dst[x * 4 + 3] = a;
    }
  }
  return out;
}

void PutClipboard(UINT format, const std::vector<uint8_t>& bytes) {
  if (bytes.empty()) return;
  HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes.size());
  if (!h) return;
  if (void* p = GlobalLock(h)) {
    std::memcpy(p, bytes.data(), bytes.size());
    GlobalUnlock(h);
    if (SetClipboardData(format, h)) return;
  }
  GlobalFree(h);
}

double Snap(double degrees) { return std::round(degrees / 15.0) * 15.0; }

}  // namespace

// ---------------------------------------------------------------------------
// Which picture, and where

uint32_t DocView::SelectedPicture() const {
  if (!doc_ || !Ready() || placeholder_) return kNone;
  const TextPos a = std::min(anchor_, focus_), b = std::max(anchor_, focus_);
  if (a.para != b.para || b.offset != a.offset + 1 || a.para >= doc_->Paragraphs().size()) return kNone;
  const std::u16string& text = doc_->Para(a.para).text;
  if (a.offset >= text.size() || text[a.offset] != 0xFFFC) return kNone;
  const auto& drawings = doc_->Drawings();
  for (uint32_t i = 0; i < drawings.size(); ++i)
    if (drawings[i].pos.para == a.para && drawings[i].pos.offset == a.offset) return i;
  return kNone;
}

std::optional<DocView::PicBox> DocView::BoxOf(uint32_t drawing) const {
  if (!pagination_ || drawing >= doc_->Drawings().size()) return std::nullopt;
  const fw::doc::Drawing& d = doc_->Drawings()[drawing];
  if (d.pos.para == kNone) return std::nullopt;
  const auto page = pagination_->PageOf(ToText(d.pos));
  if (!page) return std::nullopt;
  const size_t count = pagination_->PageCount();
  for (size_t p : {*page, *page + 1, *page > 0 ? *page - 1 : *page}) {
    if (p >= count) continue;
    for (const layout::DrawingBox& box : pagination_->GetPage(p).drawings)
      if (box.drawing == drawing) return PicBox{static_cast<int>(p), box.rect};
  }
  return std::nullopt;
}

uint32_t DocView::PictureAt(int x, int y) const {
  const int page = PageAtPoint(x, y);
  if (page < 0 || placeholder_ || page >= static_cast<int>(pagination_->PageCount())) return kNone;
  float px, py;
  if (!ToPagePoint(page, x, y, px, py)) return kNone;
  const auto& boxes = pagination_->GetPage(page).drawings;
  for (auto it = boxes.rbegin(); it != boxes.rend(); ++it) {  // the last drawn is on top
    const layout::Rect& r = it->rect;
    if (px < r.left || px >= r.right || py < r.top || py >= r.bottom) continue;
    if (it->drawing >= doc_->Drawings().size() || doc_->Drawings()[it->drawing].pos.para == kNone) continue;
    if (it->behindText) {
      // Text over a picture behind it wins: the picture is picked where there's no line.
      if (auto pos = pagination_->HitTest(page, px, py))
        if (auto line = pagination_->LineAt(*pos); line && static_cast<int>(line->page) == page && px >= line->box.left &&
                                                   px < line->box.right && py >= line->box.top && py < line->box.bottom)
          continue;
    }
    return it->drawing;
  }
  return kNone;
}

const edit::PictureInfo& DocView::PictureInfoOf(uint32_t drawing) const {
  const uint32_t version = drawing < doc_->Drawings().size() ? doc_->Drawings()[drawing].version : 0;
  const uint64_t key = (static_cast<uint64_t>(drawing) << 32) | version;
  if (infoKey_ != key || infoEdits_ != edits_) {
    infoKey_ = key;
    infoEdits_ = edits_;
    info_ = pictures_ ? pictures_->Info(drawing) : edit::PictureInfo{};
  }
  return info_;
}

// The handles' centers on screen: 0 top left, clockwise to 7 left; 8 the turn handle above.
static void HandleCenters(const D2D1_RECT_F& r, float ui, float out[9][2]) {
  const float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
  const float pts[9][2] = {{r.left, r.top},     {cx, r.top},      {r.right, r.top},  {r.right, cy},          {r.right, r.bottom},
                           {cx, r.bottom},      {r.left, r.bottom}, {r.left, cy},    {cx, r.top - 22 * ui}};
  std::memcpy(out, pts, sizeof(pts));
}

int DocView::HandleAt(int x, int y) const {
  const uint32_t d = SelectedPicture();
  if (d == kNone || !PictureInfoOf(d).picture) return -1;
  auto box = BoxOf(d);
  if (!box) return -1;
  float c[9][2];
  HandleCenters(PageRectToScreen(box->page, box->rect), ui_, c);
  const float reach = 7 * ui_;
  for (int h = cropMode_ ? 7 : 8; h >= 0; --h)
    if (std::fabs(x - c[h][0]) <= reach && std::fabs(y - c[h][1]) <= reach) return h;
  return -1;
}

void DocView::SelectDrawing(uint32_t drawing) {
  const fw::doc::Pos p = doc_->Drawings()[drawing].pos;
  anchor_ = ToText(p);
  focus_ = {p.para, p.offset + 1};
  goalX_ = -1;
  if (editor_) editor_->BreakTyping();
  host_.ViewInvalidate();
  host_.ViewStateChanged();
}

bool DocView::ApplyPicture(const edit::Result& r) {
  if (!Apply(r)) return false;
  // The picture stays selected (Result::caret is its character).
  const TextPos at = ToText(r.caret);
  if (at.para < doc_->Paragraphs().size() && at.offset < doc_->Para(at.para).text.size() &&
      doc_->Para(at.para).text[at.offset] == 0xFFFC) {
    anchor_ = at;
    focus_ = {at.para, at.offset + 1};
  }
  host_.ViewInvalidate();
  return true;
}

// ---------------------------------------------------------------------------
// Drawing the frame

void DocView::DrawPictureFrame(Canvas& c, int first, int last) {
  const Palette& p = theme::P();
  const float u = ui_;
  auto handle = [&](float x, float y, bool round) {
    const D2D1_RECT_F h = RectF(x - 4 * u, y - 4 * u, x + 4 * u, y + 4 * u);
    if (round) {
      c.FillEllipse(x, y, 5 * u, 5 * u, p.paper);
      c.StrokeRound(Inflate(h, 1 * u, 1 * u), 5 * u, p.accent, 1.5f * u);
    } else {
      c.Fill(h, p.paper);
      c.Stroke(h, p.accent, 1.2f * u);
    }
  };
  // While dragging: where it goes.
  if (picDrag_.kind != PictureDrag::None && picDrag_.kind != PictureDrag::Pending) {
    const int page = picDrag_.kind == PictureDrag::Move ? picDrag_.dropPage : picDrag_.page;
    if (page < first || page > last) return;
    if (picDrag_.kind == PictureDrag::Rotate) {
      const layout::Rect& r = picDrag_.start;
      const float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
      const double t = picDrag_.angle * 3.14159265358979 / 180;
      const float cs = static_cast<float>(std::cos(t)), sn = static_cast<float>(std::sin(t));
      const float hx = (r.right - r.left) / 2, hy = (r.bottom - r.top) / 2;
      const float corners[4][2] = {{-hx, -hy}, {hx, -hy}, {hx, hy}, {-hx, hy}};
      draw::Matrix m = PageToScreen(page);
      for (int k = 0; k < 4; ++k) {
        const float* a = corners[k];
        const float* b = corners[(k + 1) % 4];
        draw::Point pa = m.Apply({cx + a[0] * cs - a[1] * sn, cy + a[0] * sn + a[1] * cs});
        draw::Point pb = m.Apply({cx + b[0] * cs - b[1] * sn, cy + b[0] * sn + b[1] * cs});
        c.Line(pa.x, pa.y, pb.x, pb.y, p.accent, 1.5f * u);
      }
      return;
    }
    if (picDrag_.kind == PictureDrag::Crop) c.Stroke(PageRectToScreen(page, picDrag_.full), Alpha(p.accent, 0.4f), 1 * u);
    c.Stroke(PageRectToScreen(page, picDrag_.now), p.accent, 1.5f * u);
    return;
  }
  const uint32_t d = SelectedPicture();
  if (d == kNone) return;
  auto box = BoxOf(d);
  if (!box || box->page < first || box->page > last) return;
  const D2D1_RECT_F r = PageRectToScreen(box->page, box->rect);
  c.Stroke(r, p.accent, 1 * u);
  if (!PictureInfoOf(d).picture || !Editing()) return;
  float h[9][2];
  HandleCenters(r, u, h);
  if (cropMode_) {
    // Crop handles: bars along the edges and corners.
    const float t = 3 * u, l = 10 * u;
    for (int k = 0; k < 8; ++k) {
      const float x = h[k][0], y = h[k][1];
      const bool corner = k % 2 == 0;
      if (corner || k == 1 || k == 5) c.Fill(RectF(x - (corner ? (k == 0 || k == 6 ? 0 : l) : l / 2), y - t / 2, x + (corner ? (k == 0 || k == 6 ? l : 0) : l / 2), y + t / 2), p.text);
      if (corner || k == 3 || k == 7) c.Fill(RectF(x - t / 2, y - (corner ? (k < 3 ? 0 : l) : l / 2), x + t / 2, y + (corner ? (k < 3 ? l : 0) : l / 2)), p.text);
    }
    return;
  }
  c.Line(h[1][0], h[1][1], h[8][0], h[8][1], p.accent, 1 * u);
  for (int k = 0; k < 8; ++k) handle(h[k][0], h[k][1], false);
  handle(h[8][0], h[8][1], true);
}

// ---------------------------------------------------------------------------
// The pointer

bool DocView::PictureMouseDown(int x, int y, int button, UINT mods, bool doubleClick) {
  if (!Editing() || presentation_ || !pictures_) return false;
  const uint32_t selected = SelectedPicture();
  if (button == 0 && selected != kNone) {
    const int h = HandleAt(x, y);
    if (h >= 0) {
      auto box = BoxOf(selected);
      if (!box) return false;
      picDrag_ = {};
      picDrag_.kind = h == kRotateHandle ? PictureDrag::Rotate : cropMode_ ? PictureDrag::Crop : PictureDrag::Resize;
      picDrag_.drawing = selected;
      picDrag_.handle = h;
      picDrag_.page = picDrag_.dropPage = box->page;
      picDrag_.start = picDrag_.now = box->rect;
      ToPagePoint(box->page, x, y, picDrag_.grabX, picDrag_.grabY);
      picDrag_.startCrop = picDrag_.crop = PictureInfoOf(selected).crop;
      picDrag_.angle = PictureInfoOf(selected).rotation;
      // The whole picture, crop included, for crop mode.
      const edit::Crop& k = picDrag_.startCrop;
      const float w = box->rect.right - box->rect.left, ht = box->rect.bottom - box->rect.top;
      const float fw = static_cast<float>(w / std::max(0.01, 1 - k.left - k.right));
      const float fh = static_cast<float>(ht / std::max(0.01, 1 - k.top - k.bottom));
      picDrag_.full = {box->rect.left - static_cast<float>(k.left) * fw, box->rect.top - static_cast<float>(k.top) * fh,
                       box->rect.left - static_cast<float>(k.left) * fw + fw, box->rect.top - static_cast<float>(k.top) * fh + fh};
      return true;
    }
  }
  const uint32_t d = PictureAt(x, y);
  if (d == kNone) {
    if (cropMode_) {
      cropMode_ = false;
      host_.ViewInvalidate();
    }
    return false;
  }
  if (d != selected) cropMode_ = false;
  SelectDrawing(d);
  if (doubleClick) {
    Execute(CmdPicturePanel);
    return true;
  }
  if (button == 0) {
    if (auto box = BoxOf(d)) {
      picDrag_ = {};
      picDrag_.kind = PictureDrag::Pending;
      picDrag_.drawing = d;
      picDrag_.page = picDrag_.dropPage = box->page;
      picDrag_.start = picDrag_.now = box->rect;
      ToPagePoint(box->page, x, y, picDrag_.grabX, picDrag_.grabY);
    }
  }
  (void)mods;
  return true;
}

bool DocView::PictureMouseMove(int x, int y, UINT mods) {
  PictureDrag& g = picDrag_;
  if (g.kind == PictureDrag::None) return false;
  if (g.kind == PictureDrag::Pending) {
    if (std::abs(x - downX_) + std::abs(y - downY_) < 4 * ui_) return true;
    if (!PictureInfoOf(g.drawing).picture) {
      g.kind = PictureDrag::None;
      if (!PictureInfoOf(g.drawing).why.empty()) host_.ViewMessage(BannerKind::Info, Wide(PictureInfoOf(g.drawing).why));
      return true;
    }
    g.kind = PictureDrag::Move;
  }
  g.shift = (mods & kModShift) != 0;
  float px, py;
  if (g.kind == PictureDrag::Move) {
    int page = PageAtPoint(x, y);
    if (page < 0) page = NearestPage(x, y);
    if (page < 0) page = g.page;
    ToPagePoint(page, x, y, px, py);
    const float w = g.start.right - g.start.left, h = g.start.bottom - g.start.top;
    g.dropPage = page;
    g.now = {px - (g.grabX - g.start.left), py - (g.grabY - g.start.top), 0, 0};
    g.now.right = g.now.left + w;
    g.now.bottom = g.now.top + h;
    host_.ViewInvalidate();
    return true;
  }
  ToPagePoint(g.page, x, y, px, py);
  const float dx = px - g.grabX, dy = py - g.grabY;
  if (g.kind == PictureDrag::Rotate) {
    const float cx = (g.start.left + g.start.right) / 2, cy = (g.start.top + g.start.bottom) / 2;
    double a = std::atan2(py - cy, px - cx) * 180 / 3.14159265358979 + 90;
    if (g.shift) a = Snap(a);
    g.angle = std::fmod(a + 720, 360);
    host_.ViewInvalidate();
    return true;
  }
  layout::Rect r = g.start;
  const int h = g.handle;
  const bool left = h == 0 || h == 6 || h == 7, right = h == 2 || h == 3 || h == 4;
  const bool top = h == 0 || h == 1 || h == 2, bottom = h == 4 || h == 5 || h == 6;
  if (left) r.left += dx;
  if (right) r.right += dx;
  if (top) r.top += dy;
  if (bottom) r.bottom += dy;
  const float minSide = 4;
  if (g.kind == PictureDrag::Crop) {
    // Within the whole picture, leaving some of it.
    r.left = std::clamp(r.left, g.full.left, r.right - minSide);
    r.right = std::clamp(r.right, r.left + minSide, g.full.right);
    r.top = std::clamp(r.top, g.full.top, r.bottom - minSide);
    r.bottom = std::clamp(r.bottom, r.top + minSide, g.full.bottom);
    const double fw = g.full.right - g.full.left, fh = g.full.bottom - g.full.top;
    g.crop = {(r.left - g.full.left) / fw, (r.top - g.full.top) / fh, (g.full.right - r.right) / fw, (g.full.bottom - r.bottom) / fh};
    g.now = r;
    host_.ViewInvalidate();
    return true;
  }
  if (r.right - r.left < minSide) (left ? r.left : r.right) = left ? r.right - minSide : r.left + minSide;
  if (r.bottom - r.top < minSide) (top ? r.top : r.bottom) = top ? r.bottom - minSide : r.top + minSide;
  // Corners keep the picture's shape (Shift frees them, or keeps them for a picture that isn't locked).
  const bool corner = h % 2 == 0;
  if (corner && (PictureInfoOf(g.drawing).lockAspect != g.shift)) {
    const float w0 = g.start.right - g.start.left, h0 = g.start.bottom - g.start.top;
    const float k = std::max((r.right - r.left) / w0, (r.bottom - r.top) / h0);
    const float w = w0 * k, ht = h0 * k;
    if (left) r.left = r.right - w;
    else r.right = r.left + w;
    if (top) r.top = r.bottom - ht;
    else r.bottom = r.top + ht;
  }
  g.now = r;
  host_.ViewInvalidate();
  return true;
}

bool DocView::PictureMouseUp(int x, int y) {
  PictureDrag g = picDrag_;
  picDrag_ = {};
  if (g.kind == PictureDrag::None) return false;
  host_.ViewInvalidate();
  if (g.kind == PictureDrag::Pending) return true;  // a click: selected
  const fw::doc::Drawing& d = doc_->Drawings()[g.drawing];
  const bool floating = d.anchored;
  auto emu = [](float pt) { return fw::Emu(std::llround(pt * kEmuPerPt)); };
  switch (g.kind) {
    case PictureDrag::Move: {
      if (!floating) {
        auto to = PosAt(x, y, true);
        if (!to || StoryOf(*to) != doc_->Para(d.pos.para).story) return true;
        ApplyPicture(pictures_->MoveInline(g.drawing, ToDoc(*to)));
        return true;
      }
      // Held to the page where it was dropped, anchored to the paragraph there.
      std::optional<fw::doc::Pos> anchor;
      if (auto to = PosAt(x, y, true); to && StoryOf(*to) == doc_->Para(d.pos.para).story) anchor = fw::doc::Pos{to->para, 0};
      ApplyPicture(pictures_->MoveFloating(g.drawing, emu(g.now.left), emu(g.now.top), anchor));
      return true;
    }
    case PictureDrag::Resize: {
      const fw::Emu cx = emu(g.now.right - g.now.left), cy = emu(g.now.bottom - g.now.top);
      const bool moved = g.now.left != g.start.left || g.now.top != g.start.top;
      editor_->BeginGroup(u"Resize picture");
      edit::Result r = pictures_->Resize(g.drawing, cx, cy);
      if (r && floating && moved) r = pictures_->MoveFloating(g.drawing, emu(g.now.left), emu(g.now.top));
      editor_->EndGroup();
      ApplyPicture(r);
      return true;
    }
    case PictureDrag::Rotate:
      ApplyPicture(pictures_->Rotate(g.drawing, g.angle));
      return true;
    case PictureDrag::Crop: {
      const bool moved = g.now.left != g.start.left || g.now.top != g.start.top;
      editor_->BeginGroup(u"Crop picture");
      edit::Result r = pictures_->SetCrop(g.drawing, g.crop);
      if (r && floating && moved) r = pictures_->MoveFloating(g.drawing, emu(g.now.left), emu(g.now.top));
      editor_->EndGroup();
      ApplyPicture(r);
      return true;
    }
    default: return true;
  }
}

HCURSOR DocView::PictureCursor(int x, int y) const {
  static HCURSOR move = LoadCursor(nullptr, IDC_SIZEALL), nwse = LoadCursor(nullptr, IDC_SIZENWSE),
                 nesw = LoadCursor(nullptr, IDC_SIZENESW), we = LoadCursor(nullptr, IDC_SIZEWE),
                 ns = LoadCursor(nullptr, IDC_SIZENS), cross = LoadCursor(nullptr, IDC_CROSS);
  if (!Editing() || presentation_ || !pictures_) return nullptr;
  if (picDrag_.kind == PictureDrag::Move) return move;
  const int h = picDrag_.kind == PictureDrag::Resize || picDrag_.kind == PictureDrag::Crop ? picDrag_.handle
                : picDrag_.kind == PictureDrag::Rotate                                       ? kRotateHandle
                                                                                             : HandleAt(x, y);
  switch (h) {
    case 0:
    case 4: return nwse;
    case 2:
    case 6: return nesw;
    case 1:
    case 5: return ns;
    case 3:
    case 7: return we;
    case kRotateHandle: return cross;
  }
  const uint32_t d = SelectedPicture();
  if (d != kNone && PictureAt(x, y) == d && PictureInfoOf(d).picture) return move;
  return nullptr;
}

bool DocView::PictureKey(UINT vk, UINT mods) {
  if (picDrag_.kind != PictureDrag::None && vk == VK_ESCAPE) {
    picDrag_ = {};
    host_.ViewInvalidate();
    return true;
  }
  if (!cropMode_) return false;
  if (vk == VK_ESCAPE || vk == VK_RETURN) {
    cropMode_ = false;
    host_.ViewInvalidate();
    return true;
  }
  (void)mods;
  return false;
}

// ---------------------------------------------------------------------------
// Inserting

fw::Emu DocView::ColumnWidthAt(TextPos pos) {
  size_t section = 0;
  if (auto c = pagination_->CaretAt(pos); c && c->page < pagination_->PageCount()) section = pagination_->GetPage(c->page).section;
  const auto& sections = doc_->Sections();
  if (section >= sections.size()) return fw::Emu(0);
  const fw::doc::SectionProps& s = sections[section].props;
  const int64_t twips = static_cast<int64_t>(s.pageWidth.Value()) - s.marginStart.Value() - s.marginEnd.Value() - s.gutter.Value();
  return fw::Emu(std::max<int64_t>(0, twips) * 635);
}

bool DocView::InsertPictureBytes(const std::vector<uint8_t>& bytes, std::u16string_view name, std::optional<TextPos> at) {
  if (!Editing() || !pictures_) return false;
  if (!edit::Pictures::Accepts(bytes)) {
    host_.ViewMessage(BannerKind::Warning, L"That isn't a picture Featherweight adds: PNG, JPEG, GIF, BMP, TIFF or SVG.");
    return false;
  }
  const bool replace = !at && HasSelection();
  const TextPos where = at ? *at : std::min(anchor_, focus_);
  editor_->BeginGroup(u"Insert picture");
  edit::Result r;
  if (replace) r = editor_->Delete(SelFrom(), SelTo());
  bool deleted = replace && r;
  if (r) r = pictures_->Insert(ToDoc(where), bytes, ColumnWidthAt(where), name);
  editor_->EndGroup();
  if (!r && deleted) editor_->Undo();  // the selection comes back
  return ApplyPicture(r);
}

bool DocView::InsertPictureFiles(const std::vector<std::wstring>& files, int x, int y) {
  if (!Editing()) return false;
  std::optional<TextPos> at;
  if (x >= 0) {
    at = PosAt(x, y, true);
    if (!at) return false;
  }
  bool any = false;
  for (const std::wstring& path : files) {
    if (!IsPictureFile(path)) continue;
    std::string data;
    if (!util::ReadFileBytes(path, data, kMaxPictureFile)) {
      host_.ViewMessage(BannerKind::Warning, L"“" + fw::FileNameOf(path) + L"” couldn't be read, or is too big to add.");
      continue;
    }
    const std::wstring name = fw::FileNameOf(path);
    if (!InsertPictureBytes(std::vector<uint8_t>(data.begin(), data.end()), std::u16string(name.begin(), name.end()), at)) break;
    // The next one goes after it.
    at = TextPos{std::min(anchor_, focus_).para, std::min(anchor_, focus_).offset + 1};
    any = true;
  }
  return any;
}

bool DocView::PictureFiles(const std::vector<std::wstring>& files) {
  return !files.empty() && std::all_of(files.begin(), files.end(), IsPictureFile);
}

void DocView::InsertPictureDialog() {
  if (!Editing()) return;
  ComPtr<IFileOpenDialog> dlg;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
  COMDLG_FILTERSPEC filters[] = {{L"Pictures", L"*.png;*.jpg;*.jpeg;*.jpe;*.jfif;*.gif;*.bmp;*.dib;*.tif;*.tiff;*.svg"},
                                 {L"All files", L"*.*"}};
  dlg->SetFileTypes(2, filters);
  dlg->SetTitle(L"Insert picture");
  dlg->SetOkButtonLabel(L"Insert");
  DWORD opts = 0;
  dlg->GetOptions(&opts);
  dlg->SetOptions(opts | FOS_ALLOWMULTISELECT | FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM);
  if (FAILED(dlg->Show(host_.ViewHwnd()))) return;
  ComPtr<IShellItemArray> items;
  if (FAILED(dlg->GetResults(&items))) return;
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
  std::vector<std::wstring> pictures;
  for (const auto& f : files)
    if (IsPictureFile(f)) pictures.push_back(f);
  if (pictures.empty() && !files.empty()) {
    host_.ViewMessage(BannerKind::Warning, L"Pick a PNG, JPEG, GIF, BMP, TIFF or SVG picture.");
    return;
  }
  InsertPictureFiles(pictures);
}

bool DocView::PastePicture() {
  // Text comes first: a picture is pasted only when the clipboard has no text (a picture copied
  // from Word or a browser comes with HTML or RTF for it, but no text).
  static const UINT png = RegisterClipboardFormatW(L"PNG"), jfif = RegisterClipboardFormatW(L"JFIF");
  if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
    const std::wstring text = util::GetClipboardText();
    if (std::any_of(text.begin(), text.end(), [](wchar_t ch) { return !std::iswspace(ch) && ch != 0xFFFC; })) return false;
  }
  std::vector<uint8_t> bytes = ClipboardData(png);
  if (bytes.empty()) bytes = ClipboardData(jfif);
  if (bytes.empty()) bytes = DibToBmp(ClipboardData(CF_DIBV5));
  if (bytes.empty()) bytes = DibToBmp(ClipboardData(CF_DIB));
  if (!bytes.empty()) {
    InsertPictureBytes(bytes, u"", std::nullopt);
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
    if (PictureFiles(files)) {
      InsertPictureFiles(files);
      return true;
    }
  }
  return false;
}

bool DocView::HasPictureOnClipboard() const {
  static const UINT png = RegisterClipboardFormatW(L"PNG");
  return IsClipboardFormatAvailable(png) || IsClipboardFormatAvailable(CF_DIB) || IsClipboardFormatAvailable(CF_HDROP);
}

bool DocView::CopyPicture() {
  const uint32_t d = SelectedPicture();
  if (d == kNone || !pictures_) return false;
  const std::string xml = doc_->DrawingXml(doc_->Drawings()[d]);
  // The picture's part: its blip's relationship id.
  const size_t at = xml.find(":embed=\"");
  if (at == std::string::npos) return false;
  const size_t end = xml.find('"', at + 8);
  std::vector<uint8_t> bytes;
  if (end == std::string::npos || !doc_->ReadTarget(doc_->Drawings()[d].part, xml.substr(at + 8, end - at - 8), bytes).Ok())
    return false;
  std::shared_ptr<draw::Image> image = draw::Image::Open(bytes);
  std::shared_ptr<const draw::Bitmap> px = image ? image->Pixels() : nullptr;
  if (!OpenClipboard(host_.ViewHwnd())) return false;
  EmptyClipboard();
  // PNG and JPEG as they are (Word and browsers take them), and a bitmap for everything else.
  static const UINT png = RegisterClipboardFormatW(L"PNG"), jfif = RegisterClipboardFormatW(L"JFIF");
  if (image && image->Format() == draw::ImageFormat::Png) PutClipboard(png, bytes);
  if (image && image->Format() == draw::ImageFormat::Jpeg) PutClipboard(jfif, bytes);
  if (px) PutClipboard(CF_DIB, BitmapToDib(*px));
  CloseClipboard();
  return true;
}

// ---------------------------------------------------------------------------
// Commands

std::optional<CommandState> DocView::QueryPicture(int cmd) {
  if (cmd < CmdInsertPicture || cmd > CmdPicturePanel) return std::nullopt;
  CommandState s;
  if (cmd == CmdInsertPicture) {
    s.enabled = Editing();
    return s;
  }
  const uint32_t d = SelectedPicture();
  const bool on = Editing() && d != kNone && PictureInfoOf(d).picture;
  s.enabled = on;
  if (!on) return s;
  const edit::Wrap w = PictureInfoOf(d).wrap;
  switch (cmd) {
    case CmdPictureInline: s.checked = w == edit::Wrap::Inline; break;
    case CmdPictureSquare: s.checked = w == edit::Wrap::Square; break;
    case CmdPictureTight: s.checked = w == edit::Wrap::Tight; break;
    case CmdPictureTopBottom: s.checked = w == edit::Wrap::TopAndBottom; break;
    case CmdPictureBehind: s.checked = w == edit::Wrap::Behind; break;
    case CmdPictureInFront: s.checked = w == edit::Wrap::InFront; break;
    case CmdPictureCrop: s.checked = cropMode_; break;
    case CmdPicturePanel: s.checked = picturePanel_ && host_.ViewShownPanel() == picturePanel_.get(); break;
  }
  return s;
}

void DocView::SetPictureWrap(edit::Wrap wrap) {
  const uint32_t d = SelectedPicture();
  if (d == kNone) return;
  // A picture that starts floating stays where it is on its page.
  fw::Emu x(0), y(0);
  if (auto box = BoxOf(d)) {
    x = fw::Emu(std::llround(box->rect.left * kEmuPerPt));
    y = fw::Emu(std::llround(box->rect.top * kEmuPerPt));
  }
  ApplyPicture(pictures_->SetWrap(d, wrap, x, y));
}

bool DocView::ExecutePicture(int cmd) {
  if (cmd < CmdInsertPicture || cmd > CmdPicturePanel) return false;
  if (!Editing() || !pictures_) return true;
  if (cmd == CmdInsertPicture) {
    InsertPictureDialog();
    return true;
  }
  const uint32_t d = SelectedPicture();
  if (d == kNone) return true;
  if (!PictureInfoOf(d).picture) {
    if (!PictureInfoOf(d).why.empty()) host_.ViewMessage(BannerKind::Info, Wide(PictureInfoOf(d).why));
    return true;
  }
  switch (cmd) {
    case CmdPictureInline: SetPictureWrap(edit::Wrap::Inline); break;
    case CmdPictureSquare: SetPictureWrap(edit::Wrap::Square); break;
    case CmdPictureTight: SetPictureWrap(edit::Wrap::Tight); break;
    case CmdPictureTopBottom: SetPictureWrap(edit::Wrap::TopAndBottom); break;
    case CmdPictureBehind: SetPictureWrap(edit::Wrap::Behind); break;
    case CmdPictureInFront: SetPictureWrap(edit::Wrap::InFront); break;
    case CmdPictureCrop:
      cropMode_ = !cropMode_;
      host_.ViewInvalidate();
      break;
    case CmdPictureRotate: ApplyPicture(pictures_->Rotate(d, PictureInfoOf(d).rotation + 90)); break;
    case CmdPictureReset: ApplyPicture(pictures_->ResetSize(d)); break;
    case CmdPicturePanel: {
      if (!picturePanel_) {
        PicturePanel::Actions a;
        a.wrap = [this](edit::Wrap w) { SetPictureWrap(w); };
        a.size = [this](double w, double h) {
          if (uint32_t p = SelectedPicture(); p != kNone)
            ApplyPicture(pictures_->Resize(p, fw::Emu(std::llround(w * kEmuPerPt)), fw::Emu(std::llround(h * kEmuPerPt))));
        };
        a.rotate = [this](double deg) {
          if (uint32_t p = SelectedPicture(); p != kNone) ApplyPicture(pictures_->Rotate(p, deg));
        };
        a.altText = [this](const std::wstring& text) {
          if (uint32_t p = SelectedPicture(); p != kNone)
            ApplyPicture(pictures_->SetAltText(p, std::u16string(text.begin(), text.end())));
        };
        a.crop = [this] { Execute(CmdPictureCrop); };
        a.reset = [this] { Execute(CmdPictureReset); };
        picturePanel_ = std::make_unique<PicturePanel>(std::move(a));
      }
      const bool open = host_.ViewShownPanel() == picturePanel_.get();
      host_.ViewShowPanel(open ? nullptr : picturePanel_.get());
      SyncPicturePanel();
      break;
    }
  }
  return true;
}

void DocView::SyncPicturePanel() {
  if (!picturePanel_ || host_.ViewShownPanel() != picturePanel_.get()) {
    panelShown_ = UINT64_MAX;  // shown again when it opens
    return;
  }
  // Every edit counts (undo too): the drawing and the edits so far say what it shows.
  const uint32_t d = SelectedPicture();
  const uint64_t key = d == kNone ? UINT64_MAX - 1 : (static_cast<uint64_t>(edits_) << 32) | d;
  if (key == panelShown_) return;
  panelShown_ = key;
  if (d == kNone) {
    picturePanel_->Show(std::nullopt, L"Select a picture to change its size, rotation and alt text.");
  } else if (!PictureInfoOf(d).picture) {
    picturePanel_->Show(std::nullopt, Wide(PictureInfoOf(d).why));
  } else {
    picturePanel_->Show(PictureInfoOf(d));
  }
}

void DocView::AddPictureToolbar(ToolbarModel& m) {
  const uint32_t d = SelectedPicture();
  if (d == kNone || !PictureInfoOf(d).picture) return;
  const edit::Wrap w = PictureInfoOf(d).wrap;
  ToolItem wrap;
  wrap.kind = ToolKind::Dropdown;
  wrap.cmd = CmdPictureWrapBox;
  wrap.width = 148;
  wrap.priority = 9;
  wrap.glyph = icon::Picture;
  wrap.tooltip = L"Wrap text: how text flows around the picture";
  wrap.sepBefore = true;
  static constexpr int kWraps[] = {CmdPictureInline, CmdPictureSquare, CmdPictureTight, CmdPictureTopBottom, CmdPictureBehind, CmdPictureInFront};
  wrap.choices = {L"In line with text", L"Square", L"Tight", L"Top and bottom", L"Behind text", L"In front of text"};
  wrap.choice = static_cast<int>(w);
  wrap.onChange = [this](const ToolValue& v) {
    if (v.index >= 0 && v.index < 6) Execute(kWraps[v.index]);
  };
  m.left.push_back(wrap);
  ToolItem crop;
  crop.cmd = CmdPictureCrop;
  crop.glyph = icon::Crop;
  crop.toggle = true;
  crop.checked = cropMode_;
  crop.priority = 9;
  crop.tooltip = L"Crop: drag the picture's edges (Esc when done)";
  m.left.push_back(crop);
  ToolItem rotate;
  rotate.cmd = CmdPictureRotate;
  rotate.glyph = icon::Rotate;
  rotate.priority = 8;
  rotate.tooltip = L"Rotate 90°";
  m.left.push_back(rotate);
  ToolItem panel;
  panel.cmd = CmdPicturePanel;
  panel.glyph = icon::Settings;
  panel.toggle = true;
  panel.checked = picturePanel_ && host_.ViewShownPanel() == picturePanel_.get();
  panel.priority = 8;
  panel.tooltip = L"Picture: size, rotation and alt text";
  m.left.push_back(panel);
}

void DocView::FillPictureMenu(HMENU m) {
  using menu::AppendItem;
  using menu::AppendSeparator;
  const uint32_t d = SelectedPicture();
  if (d == kNone || !Editing()) return;
  if (!PictureInfoOf(d).picture) return;
  const edit::Wrap w = PictureInfoOf(d).wrap;
  HMENU wrap = CreatePopupMenu();
  AppendItem(wrap, CmdPictureInline, L"In line with text", true, w == edit::Wrap::Inline, true);
  AppendItem(wrap, CmdPictureSquare, L"Square", true, w == edit::Wrap::Square, true);
  AppendItem(wrap, CmdPictureTight, L"Tight", true, w == edit::Wrap::Tight, true);
  AppendItem(wrap, CmdPictureTopBottom, L"Top and bottom", true, w == edit::Wrap::TopAndBottom, true);
  AppendItem(wrap, CmdPictureBehind, L"Behind text", true, w == edit::Wrap::Behind, true);
  AppendItem(wrap, CmdPictureInFront, L"In front of text", true, w == edit::Wrap::InFront, true);
  menu::AppendSubmenu(m, wrap, L"Wrap text");
  AppendItem(m, CmdPictureCrop, L"Crop", true, cropMode_);
  AppendItem(m, CmdPictureRotate, L"Rotate 90°");
  AppendItem(m, CmdPictureReset, L"Reset size");
  AppendItem(m, CmdPicturePanel, L"Size, rotation and alt text…");
  AppendSeparator(m);
}

// ---------------------------------------------------------------------------
// The Picture panel

namespace {
constexpr float kLabelW = 96;  // DIPs
constexpr float kRow = 44;
}  // namespace

PicturePanel::PicturePanel(Actions actions) : actions_(std::move(actions)) {
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
    s->minValue = metric_ ? 0.03 : 0.01;
    s->maxValue = metric_ ? 55.88 : 22;
    s->unit = metric_ ? L" cm" : L"″";
  }
  width_.label = L"Width";
  height_.label = L"Height";
  width_.onChange = [this](double v) {
    const double w = FromUnit(v), h = lockAspect_ ? w * aspect_ : FromUnit(height_.value);
    if (actions_.size) actions_.size(w, h);
  };
  height_.onChange = [this](double v) {
    const double h = FromUnit(v), w = lockAspect_ ? h / std::max(1e-6, aspect_) : FromUnit(width_.value);
    if (actions_.size) actions_.size(w, h);
  };
  rotation_.minValue = 0;
  rotation_.maxValue = 359;
  rotation_.step = 15;
  rotation_.unit = L"°";
  rotation_.label = L"Rotation";
  rotation_.onChange = [this](double v) {
    if (actions_.rotate) actions_.rotate(v);
  };
  alt_.autoComplete = false;
  alt_.label = L"Alt text";
  alt_.tooltip = L"What the picture shows, for people who can't see it";
  alt_.onCommit = [this](const std::wstring& text, int) {
    if (actions_.altText) actions_.altText(text);
  };
  crop_.text = L"Crop";
  crop_.glyph = icon::Crop;
  crop_.label = L"Crop";
  crop_.onInvoke = [this] {
    if (actions_.crop) actions_.crop();
  };
  reset_.text = L"Reset size";
  reset_.glyph = icon::Undo;
  reset_.label = L"Reset size";
  reset_.onInvoke = [this] {
    if (actions_.reset) actions_.reset();
  };
  rows_ = {{L"Layout", nullptr}, {L"Wrap text", &wrap_},  {L"Size", nullptr},    {L"Width", &width_},
           {L"Height", &height_}, {L"Rotation", &rotation_}, {L"", &crop_},  {L"", &reset_},
           {L"Alt text", nullptr}, {L"Description", &alt_}};
}

double PicturePanel::ToUnit(int64_t emu) const { return emu / (metric_ ? 360000.0 : 914400.0); }
double PicturePanel::FromUnit(double v) const { return v * (metric_ ? 72 / 2.54 : 72.0); }

void PicturePanel::Show(const std::optional<fw::doc::edit::PictureInfo>& info, const std::wstring& why) {
  shown_ = info.has_value();
  why_ = why;
  for (auto& [label, c] : rows_)
    if (c) c->visible = shown_;
  if (!info) return;
  wrap_.selected = static_cast<int>(info->wrap);
  lockAspect_ = info->lockAspect;
  aspect_ = info->cx.Value() > 0 ? static_cast<double>(info->cy.Value()) / info->cx.Value() : 1;
  width_.SetValue(ToUnit(info->cx.Value()));
  height_.SetValue(ToUnit(info->cy.Value()));
  rotation_.SetValue(std::round(info->rotation));
  alt_.SetText(std::wstring(info->altText.begin(), info->altText.end()));
}

std::vector<Control*> PicturePanel::Layout(const D2D1_RECT_F& body, float ui) {
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
    float w = right - cx;
    if (c == &width_ || c == &height_ || c == &rotation_) w = std::min(w, 132 * u);
    if (c == &crop_ || c == &reset_) w = std::min(w, 132 * u);
    c->rc = RectF(std::round(cx), std::round(y), std::round(cx + w), std::round(y + 32 * u));
    controls.push_back(c);
    y += kRow * u;
  }
  return controls;
}

void PicturePanel::Draw(Canvas& c) {
  const Palette& p = theme::P();
  const float u = ui_;
  if (!shown_) {
    c.TextWrapped(why_, RectF(body_.left + 20 * u, body_.top + 8 * u, body_.right - 20 * u, body_.top + 200 * u), p.textSecondary,
                  13 * u, 6);
    return;
  }
  for (const auto& [text, rc] : headings_) c.Text(text, rc, p.text, 13 * u, DWRITE_FONT_WEIGHT_SEMI_BOLD);
  for (const auto& [label, control] : rows_) {
    if (!control || label.empty()) continue;
    c.Text(label, RectF(control->rc.left - kLabelW * u, control->rc.top, control->rc.left - 8 * u, control->rc.bottom),
           p.textSecondary, 13 * u);
  }
}
