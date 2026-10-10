#include "RenderCache.h"

#include <fpdf_formfill.h>
#include <fpdf_progressive.h>

#include <algorithm>
#include <cmath>

#include "Anim.h"
#include "Document.h"

namespace {

constexpr int kRenderFlags = FPDF_ANNOT;
constexpr int kPreviewMax = 320;  // longest side of preview bitmaps, in pixels

struct Pause : IFSDK_PAUSE {
  double deadline = 0;
};

FPDF_BOOL NeedToPause(IFSDK_PAUSE* p) { return anim::Now() >= static_cast<Pause*>(p)->deadline; }

}  // namespace

RenderCache::RenderCache(Document& doc) : doc_(doc) {}

RenderCache::~RenderCache() { AbortJob(); }

void RenderCache::BeginFrame() {
  ++frame_;
  wants_.clear();
}

ID2D1Bitmap* RenderCache::Find(const Key& k, bool* stale) {
  auto it = tiles_.find(k);
  if (it == tiles_.end() || !it->second.bitmap) return nullptr;
  it->second.lastFrame = frame_;
  if (stale) *stale = it->second.stale;
  return it->second.bitmap.Get();
}

void RenderCache::Want(const Key& k, PageGeom geom, int priority) {
  auto it = tiles_.find(k);
  if (it != tiles_.end()) {
    it->second.lastFrame = frame_;
    it->second.geom = geom;
    if (it->second.bitmap && !it->second.stale) {
      if (it->second.dirty) wants_.push_back({k, geom, priority, false});  // handled by RenderDirtyNow
      return;
    }
  }
  wants_.push_back({k, geom, priority, false});
}

ID2D1Bitmap* RenderCache::Preview(int page, int rotation, int* scaleKey) {
  auto it = previews_.find({page, rotation});
  if (it == previews_.end() || !it->second.bitmap) return nullptr;
  it->second.lastFrame = frame_;
  if (scaleKey) *scaleKey = previewScale_[{page, rotation}];
  return it->second.bitmap.Get();
}

void RenderCache::WantPreview(int page, int rotation, PageGeom geomAtScale, int priority) {
  auto it = previews_.find({page, rotation});
  if (it != previews_.end()) {
    it->second.lastFrame = frame_;
    if (it->second.bitmap && !it->second.stale) return;
  }
  Key k;
  k.page = page;
  k.rotation = rotation;
  k.scaleKey = -1;
  wants_.push_back({k, geomAtScale, priority, true});
}

void RenderCache::ForEachOtherScale(int page, int rotation, int scaleKey,
                                    const std::function<void(const Key&, ID2D1Bitmap*)>& fn) {
  Key lo;
  lo.page = page;
  lo.rotation = rotation;
  lo.scaleKey = INT_MIN;
  for (auto it = tiles_.lower_bound(lo); it != tiles_.end(); ++it) {
    const Key& k = it->first;
    if (k.page != page || k.rotation != rotation) break;
    if (k.scaleKey == scaleKey || !it->second.bitmap) continue;
    fn(k, it->second.bitmap.Get());
  }
}

bool RenderCache::HasWork() const {
  if (job_.active) return true;
  for (const auto& w : wants_) {
    if (w.preview) {
      auto it = previews_.find({w.key.page, w.key.rotation});
      if (it == previews_.end() || !it->second.bitmap || it->second.stale) return true;
    } else {
      auto it = tiles_.find(w.key);
      if (it == tiles_.end() || !it->second.bitmap || it->second.stale) return true;
    }
  }
  return false;
}

void RenderCache::StartJob(const Want_& w) {
  job_ = Job{};
  FPDF_PAGE page = doc_.Page(w.key.page);
  if (!page) {
    // Unrenderable page: store a blank tile so we don't retry forever.
    return;
  }
  job_.active = true;
  job_.key = w.key;
  job_.geom = w.geom;
  job_.preview = w.preview;
  job_.page = page;

  int x = 0, y = 0, tw, th, sizeW = w.geom.w, sizeH = w.geom.h;
  if (w.preview) {
    double f = static_cast<double>(kPreviewMax) / std::max(w.geom.w, w.geom.h);
    if (f > 1) f = 1;
    sizeW = std::max(1, static_cast<int>(std::lround(w.geom.w * f)));
    sizeH = std::max(1, static_cast<int>(std::lround(w.geom.h * f)));
    tw = sizeW;
    th = sizeH;
    job_.geom = {sizeW, sizeH};
  } else {
    x = w.key.tx * kTile;
    y = w.key.ty * kTile;
    tw = std::min(kTile, w.geom.w - x);
    th = std::min(kTile, w.geom.h - y);
  }
  if (tw <= 0 || th <= 0) {
    job_.active = false;
    return;
  }
  job_.w = tw;
  job_.h = th;
  job_.stride = tw * 4;
  job_.pixels.assign(static_cast<size_t>(job_.stride) * th, 0xFF);
  job_.bitmap = FPDFBitmap_CreateEx(tw, th, FPDFBitmap_BGRx, job_.pixels.data(), job_.stride);
  FPDFBitmap_FillRect(job_.bitmap, 0, 0, tw, th, 0xFFFFFFFF);

  Pause pause;
  pause.version = 1;
  pause.NeedToPauseNow = NeedToPause;
  pause.deadline = anim::Now() + 4;
  int status = FPDF_RenderPageBitmap_Start(job_.bitmap, page, -x, -y, sizeW, sizeH, w.key.rotation, kRenderFlags,
                                           &pause);
  if (status == FPDF_RENDER_FAILED) status = FPDF_RENDER_DONE;  // keep whatever was drawn
  (void)status;
}

bool RenderCache::Work(ID2D1DeviceContext* dc, double deadlineMs) {
  bool produced = false;
  while (anim::Now() < deadlineMs) {
    if (!job_.active) {
      // Pick the most important outstanding request.
      const Want_* best = nullptr;
      for (const auto& w : wants_) {
        bool needed;
        if (w.preview) {
          auto it = previews_.find({w.key.page, w.key.rotation});
          needed = it == previews_.end() || !it->second.bitmap || it->second.stale;
        } else {
          auto it = tiles_.find(w.key);
          needed = it == tiles_.end() || !it->second.bitmap || it->second.stale;
        }
        if (needed && (!best || w.priority < best->priority)) best = &w;
      }
      if (!best) break;
      Want_ w = *best;
      StartJob(w);
      if (!job_.active) {
        // Could not render this page; remember an empty placeholder to avoid a loop.
        Tile& t = w.preview ? previews_[{w.key.page, w.key.rotation}] : tiles_[w.key];
        t.stale = false;
        t.lastFrame = frame_;
        wants_.erase(std::remove_if(wants_.begin(), wants_.end(),
                                    [&](const Want_& x) { return x.preview == w.preview && x.key == w.key; }),
                     wants_.end());
        continue;
      }
    }
    Pause pause;
    pause.version = 1;
    pause.NeedToPauseNow = NeedToPause;
    pause.deadline = deadlineMs;
    int status = FPDF_RenderPage_Continue(job_.page, &pause);
    if (status == FPDF_RENDER_TOBECONTINUED) break;
    FinishJob(dc);
    produced = true;
  }
  return produced;
}

void RenderCache::FinishJob(ID2D1DeviceContext* dc) {
  if (!job_.active) return;
  int x = job_.preview ? 0 : job_.key.tx * kTile;
  int y = job_.preview ? 0 : job_.key.ty * kTile;
  if (doc_.Form()) {
    FPDF_FFLDraw(doc_.Form(), job_.bitmap, job_.page, -x, -y, job_.geom.w, job_.geom.h, job_.key.rotation,
                 kRenderFlags);
  }
  FPDF_RenderPage_Close(job_.page);
  FPDFBitmap_Destroy(job_.bitmap);
  job_.bitmap = nullptr;

  Tile t;
  t.bitmap = MakeBitmap(dc, job_.pixels.data(), job_.w, job_.h, job_.stride);
  t.w = job_.w;
  t.h = job_.h;
  t.lastFrame = frame_;
  t.geom = job_.geom;
  if (job_.preview) {
    auto key = std::make_pair(job_.key.page, job_.key.rotation);
    auto old = previews_.find(key);
    if (old != previews_.end()) bytes_ -= static_cast<size_t>(old->second.w) * old->second.h * 4;
    previews_[key] = t;
    previewScale_[key] = job_.geom.w;  // width in pixels; used to compute the stretch factor
  } else {
    auto old = tiles_.find(job_.key);
    if (old != tiles_.end()) bytes_ -= static_cast<size_t>(old->second.w) * old->second.h * 4;
    tiles_[job_.key] = t;
  }
  bytes_ += static_cast<size_t>(t.w) * t.h * 4;
  job_.active = false;
  job_.pixels.clear();
  job_.pixels.shrink_to_fit();
  Evict();
}

void RenderCache::AbortJob() {
  if (!job_.active) return;
  FPDF_RenderPage_Close(job_.page);
  if (job_.bitmap) FPDFBitmap_Destroy(job_.bitmap);
  job_ = Job{};
}

void RenderCache::CancelPage(int page) {
  if (job_.active && job_.key.page == page) AbortJob();
}

ComPtr<ID2D1Bitmap> RenderCache::MakeBitmap(ID2D1DeviceContext* dc, const unsigned char* pixels, int w, int h,
                                            int stride) {
  ComPtr<ID2D1Bitmap> bmp;
  if (!dc) return bmp;
  D2D1_BITMAP_PROPERTIES props =
      D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.f, 96.f);
  dc->CreateBitmap(D2D1::SizeU(w, h), pixels, stride, props, &bmp);
  return bmp;
}

bool RenderCache::RenderRegion(FPDF_PAGE page, const Key& key, PageGeom geom, int x, int y, int w, int h,
                               std::vector<unsigned char>& out, int& stride) {
  stride = w * 4;
  out.assign(static_cast<size_t>(stride) * h, 0xFF);
  FPDF_BITMAP bmp = FPDFBitmap_CreateEx(w, h, FPDFBitmap_BGRx, out.data(), stride);
  if (!bmp) return false;
  FPDFBitmap_FillRect(bmp, 0, 0, w, h, 0xFFFFFFFF);
  FPDF_RenderPageBitmap(bmp, page, -x, -y, geom.w, geom.h, key.rotation, kRenderFlags);
  if (doc_.Form()) FPDF_FFLDraw(doc_.Form(), bmp, page, -x, -y, geom.w, geom.h, key.rotation, kRenderFlags);
  FPDFBitmap_Destroy(bmp);
  return true;
}

void RenderCache::MarkDirty(int page, double left, double top, double right, double bottom) {
  FPDF_PAGE p = doc_.PageIfLoaded(page);
  if (!p) return;
  for (auto it = tiles_.begin(); it != tiles_.end();) {
    const Key& k = it->first;
    Tile& t = it->second;
    if (k.page != page || !t.bitmap) {
      ++it;
      continue;
    }
    // Convert the page rectangle into this tile's pixel space.
    int x1, y1, x2, y2;
    FPDF_PageToDevice(p, 0, 0, t.geom.w, t.geom.h, k.rotation, left, top, &x1, &y1);
    FPDF_PageToDevice(p, 0, 0, t.geom.w, t.geom.h, k.rotation, right, bottom, &x2, &y2);
    int l = std::min(x1, x2) - 2 - k.tx * kTile, r = std::max(x1, x2) + 2 - k.tx * kTile;
    int tp = std::min(y1, y2) - 2 - k.ty * kTile, b = std::max(y1, y2) + 2 - k.ty * kTile;
    l = std::max(l, 0);
    tp = std::max(tp, 0);
    r = std::min(r, t.w);
    b = std::min(b, t.h);
    if (l >= r || tp >= b) {
      ++it;
      continue;
    }
    if (t.dirty) {
      t.dl = std::min(t.dl, l);
      t.dt = std::min(t.dt, tp);
      t.dr = std::max(t.dr, r);
      t.db = std::max(t.db, b);
    } else {
      t.dirty = true;
      t.dl = l;
      t.dt = tp;
      t.dr = r;
      t.db = b;
    }
    ++it;
  }
  for (auto& [key, prev] : previews_)
    if (key.first == page) prev.stale = true;
}

bool RenderCache::RenderDirtyNow(ID2D1DeviceContext* dc) {
  bool changed = false;
  for (auto it = tiles_.begin(); it != tiles_.end();) {
    Tile& t = it->second;
    if (!t.dirty) {
      ++it;
      continue;
    }
    const Key& k = it->first;
    bool wanted = t.lastFrame == frame_;
    if (!wanted) {
      // Not on screen: just drop it, it will be re-rendered when needed.
      bytes_ -= static_cast<size_t>(t.w) * t.h * 4;
      it = tiles_.erase(it);
      continue;
    }
    if (job_.active && job_.key.page == k.page) AbortJob();
    FPDF_PAGE page = doc_.Page(k.page);
    std::vector<unsigned char> px;
    int stride = 0;
    int w = t.dr - t.dl, h = t.db - t.dt;
    if (page && w > 0 && h > 0 &&
        RenderRegion(page, k, t.geom, k.tx * kTile + t.dl, k.ty * kTile + t.dt, w, h, px, stride)) {
      D2D1_RECT_U dst = D2D1::RectU(t.dl, t.dt, t.dr, t.db);
      t.bitmap->CopyFromMemory(&dst, px.data(), stride);
      changed = true;
    }
    t.dirty = false;
    ++it;
  }
  (void)dc;
  return changed;
}

void RenderCache::InvalidatePage(int page) {
  if (job_.active && job_.key.page == page) AbortJob();
  for (auto& [k, t] : tiles_)
    if (k.page == page) t.stale = true;
  for (auto& [k, t] : previews_)
    if (k.first == page) t.stale = true;
}

void RenderCache::InvalidateAll() {
  AbortJob();
  for (auto& [k, t] : tiles_) t.stale = true;
  for (auto& [k, t] : previews_) t.stale = true;
}

void RenderCache::Clear() {
  AbortJob();
  tiles_.clear();
  previews_.clear();
  previewScale_.clear();
  bytes_ = 0;
}

void RenderCache::DropTiles() {
  AbortJob();
  for (const auto& [k, t] : tiles_) bytes_ -= static_cast<size_t>(t.w) * t.h * 4;
  tiles_.clear();
}

void RenderCache::ReleaseDeviceResources() { Clear(); }

void RenderCache::Evict() {
  if (bytes_ <= budget_) return;
  std::vector<std::pair<uint64_t, Key>> order;
  order.reserve(tiles_.size());
  for (const auto& [k, t] : tiles_)
    if (t.lastFrame != frame_) order.emplace_back(t.lastFrame, k);
  std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  for (const auto& [frame, k] : order) {
    if (bytes_ <= budget_ * 3 / 4) break;
    auto it = tiles_.find(k);
    bytes_ -= static_cast<size_t>(it->second.w) * it->second.h * 4;
    tiles_.erase(it);
  }
  // Previews are small; cap their number.
  if (previews_.size() > 400) {
    std::vector<std::pair<uint64_t, std::pair<int, int>>> po;
    for (const auto& [k, t] : previews_)
      if (t.lastFrame != frame_) po.emplace_back(t.lastFrame, k);
    std::sort(po.begin(), po.end());
    for (size_t i = 0; i < po.size() && previews_.size() > 300; ++i) {
      auto it = previews_.find(po[i].second);
      bytes_ -= static_cast<size_t>(it->second.w) * it->second.h * 4;
      previews_.erase(it);
      previewScale_.erase(po[i].second);
    }
  }
}
