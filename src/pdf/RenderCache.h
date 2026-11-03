#pragma once

#include <d2d1_1.h>
#include <fpdfview.h>
#include <wrl/client.h>

#include <functional>
#include <map>
#include <tuple>
#include <vector>

using Microsoft::WRL::ComPtr;

class Document;

// Renders pages as 512x512 tiles with PDFium's progressive renderer and keeps them in an LRU cache.
// All work happens on the UI thread in small time slices.
class RenderCache {
 public:
  static constexpr int kTile = 512;

  struct Key {
    int page = 0;
    int rotation = 0;
    int scaleKey = 0;  // see ScaleKey()
    int ty = 0, tx = 0;
    bool operator<(const Key& o) const {
      return std::tie(page, rotation, scaleKey, ty, tx) < std::tie(o.page, o.rotation, o.scaleKey, o.ty, o.tx);
    }
    bool operator==(const Key& o) const {
      return page == o.page && rotation == o.rotation && scaleKey == o.scaleKey && ty == o.ty && tx == o.tx;
    }
  };
  struct PageGeom {
    int w = 0, h = 0;  // page size in device pixels at the tile's scale
  };

  explicit RenderCache(Document& doc);
  ~RenderCache();

  static int ScaleKey(double scale) { return static_cast<int>(scale * 4096.0 + 0.5); }
  static double ScaleOf(int scaleKey) { return scaleKey / 4096.0; }

  void BeginFrame();
  // Returns the tile's bitmap if cached. `stale` is set when its content is outdated.
  ID2D1Bitmap* Find(const Key& k, bool* stale = nullptr);
  void Want(const Key& k, PageGeom geom, int priority);
  // A small whole-page bitmap used while tiles are still rendering.
  ID2D1Bitmap* Preview(int page, int rotation, int* scaleKey);
  void WantPreview(int page, int rotation, PageGeom geomAtScale, int priority);
  // Calls `fn` for cached tiles of a page at other scales (for stretched placeholders).
  void ForEachOtherScale(int page, int rotation, int scaleKey,
                         const std::function<void(const Key&, ID2D1Bitmap*)>& fn);

  bool HasWork() const;
  // Renders until `deadlineMs` (anim::Now() clock). Returns true when a new tile became ready.
  bool Work(ID2D1DeviceContext* dc, double deadlineMs);

  // Marks part of a page (PDF page coordinates) as changed, e.g. after typing in a form field.
  void MarkDirty(int page, double left, double top, double right, double bottom);
  // Re-renders dirty parts of tiles wanted this frame immediately. Returns true if anything changed.
  bool RenderDirtyNow(ID2D1DeviceContext* dc);
  void InvalidatePage(int page);
  void InvalidateAll();
  void Clear();
  // Frees the full-resolution tiles but keeps the small previews (used for background tabs).
  void DropTiles();
  void ReleaseDeviceResources();
  void CancelPage(int page);
  int ActivePage() const { return job_.active ? job_.key.page : -1; }

 private:
  struct Tile {
    ComPtr<ID2D1Bitmap> bitmap;
    int w = 0, h = 0;
    uint64_t lastFrame = 0;
    bool stale = false;
    bool dirty = false;
    int dl = 0, dt = 0, dr = 0, db = 0;  // dirty rect in tile pixels
    PageGeom geom;
  };
  struct Want_ {
    Key key;
    PageGeom geom;
    int priority;
    bool preview;
  };
  struct Job {
    bool active = false;
    Key key;
    PageGeom geom;
    bool preview = false;
    FPDF_BITMAP bitmap = nullptr;
    std::vector<unsigned char> pixels;
    int w = 0, h = 0, stride = 0;
    FPDF_PAGE page = nullptr;
  };

  void StartJob(const Want_& w);
  void FinishJob(ID2D1DeviceContext* dc);
  void AbortJob();
  void Evict();
  bool RenderRegion(FPDF_PAGE page, const Key& key, PageGeom geom, int x, int y, int w, int h,
                    std::vector<unsigned char>& out, int& stride);
  ComPtr<ID2D1Bitmap> MakeBitmap(ID2D1DeviceContext* dc, const unsigned char* pixels, int w, int h, int stride);

  Document& doc_;
  std::map<Key, Tile> tiles_;
  std::map<std::pair<int, int>, Tile> previews_;  // (page, rotation)
  std::map<std::pair<int, int>, int> previewScale_;
  std::vector<Want_> wants_;
  Job job_;
  uint64_t frame_ = 0;
  size_t bytes_ = 0;
  size_t budget_ = 192u << 20;
};
