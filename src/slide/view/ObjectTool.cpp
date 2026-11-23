// Objects on the slide (P2, p2-objects): see ObjectTool.h. The pointer and keys, the drag's two
// bitmaps, frames, handles and guides, and the edits through the deck's editor.

#include "slide/view/ObjectTool.h"

#include <commdlg.h>
#include <shellapi.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <set>

#include "Commands.h"
#include "Util.h"
#include "core/Strings.h"
#include "draw/D2DPainter.h"
#include "draw/Geometry.h"
#include "draw/Images.h"
#include "slide/objects/Arrange.h"
#include "slide/objects/ChartPart.h"
#include "slide/objects/ObjectXml.h"
#include "slide/render/SlideRender.h"
#include "slide/view/Deck.h"
#include "ui/Gfx.h"
#include "ui/Icons.h"
#include "ui/Theme.h"

#if __has_include("slide/edit/Edit.h")
#include "slide/edit/Edit.h"
#define FW_SLIDE_EDIT 1
#else
#define FW_SLIDE_EDIT 0
#endif

using Microsoft::WRL::ComPtr;
using namespace gfx;
namespace ob = fw::slide::objects;

namespace fw::slide::view {

namespace {

constexpr float kDragStart = 4;       // pixels before a press becomes a drag
constexpr float kHandleReach = 6;     // DIPs around a handle that grab it
constexpr float kRotateOffset = 22;   // DIPs: the turn handle above the top edge (as documents')
constexpr float kNudge = 6;           // points: an arrow key (PowerPoint's grid, 1/12 inch)
constexpr float kFineNudge = 1;       // points: Ctrl+arrow
constexpr float kGrid = 6;            // points: snapping when no guide matches
constexpr UINT kMaxLayer = 4096;      // pixels: larger slides drag with outlines only
constexpr size_t kMaxPictureFile = 64u << 20;
const D2D1_COLOR_F kGuideColor = Rgb(0xE8603C);  // smart guides, as PowerPoint's orange-red

std::u16string U16(const wchar_t* s) { return std::u16string(reinterpret_cast<const char16_t*>(s)); }
std::wstring W(const std::u16string& s) { return std::wstring(s.begin(), s.end()); }

// Drawing filters for the drag's layers.
class SkipFilter : public render::DrawFilter {
 public:
  explicit SkipFilter(std::set<uint32_t> skip) : skip_(std::move(skip)) {}
  bool Shape(uint32_t id) override { return id == 0 || !skip_.count(id); }

 private:
  std::set<uint32_t> skip_;
};

class OnlyFilter : public render::DrawFilter {
 public:
  explicit OnlyFilter(std::set<uint32_t> keep) : keep_(std::move(keep)) {}
  bool Shape(uint32_t id) override { return id != 0 && keep_.count(id); }
  bool Background() override { return false; }

 private:
  std::set<uint32_t> keep_;
};

D2D1_MATRIX_3X2_F ToD2D(const draw::Matrix& m) { return {m.m11, m.m12, m.m21, m.m22, m.dx, m.dy}; }

bool IsPictureName(const std::wstring& path) {
  const size_t dot = path.find_last_of(L'.');
  const std::wstring ext = fw::ToLower(dot == std::wstring::npos ? std::wstring() : path.substr(dot));
  for (const wchar_t* e : {L".png", L".jpg", L".jpeg", L".jpe", L".jfif", L".gif", L".bmp", L".dib", L".tif", L".tiff",
                           L".emf", L".wmf", L".svg"})
    if (ext == e) return true;
  return false;
}

// Whether a text body holds any characters (a:p/a:r/a:t, a:p/a:fld/a:t). A shape with an empty one
// moves when dragged, as one without text does (PowerPoint's); a double-click puts the caret in it.
bool HasText(pugi::xml_node body) {
  for (pugi::xml_node para : body.children())
    for (pugi::xml_node run : para.children())
      for (pugi::xml_node t : run.children()) {
        const char* name = t.name();
        const char* colon = std::strrchr(name, ':');
        if (std::strcmp(colon ? colon + 1 : name, "t") == 0 && *t.child_value()) return true;
      }
  return false;
}

bool EmptyText(const ob::ObjectInfo& o) {
  return o.shape && o.shape->textNode && !o.placeholder && !o.shape->textBox && !HasText(o.shape->textNode);
}

std::vector<uint8_t> ReadFileBytes(const std::wstring& path) {
  std::string bytes;
  if (!util::ReadFileBytes(path, bytes, kMaxPictureFile)) return {};
  return std::vector<uint8_t>(bytes.begin(), bytes.end());
}

// Test runs (--snapshot) keep a clipboard of their own (util::UsePrivateClipboard): copied objects
// stay in the process there, valid until anything else is copied.
struct PrivateObjects {
  unsigned long sequence = 0;
  std::string bytes;
};
PrivateObjects& Private() {
  static PrivateObjects p;
  return p;
}

std::vector<uint8_t> ClipboardBytes(UINT format) {
  std::vector<uint8_t> out;
  if (util::UsingPrivateClipboard()) return out;  // never the user's clipboard in a test run
  if (!format || !IsClipboardFormatAvailable(format) || !OpenClipboard(nullptr)) return out;
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

// A packed DIB from the clipboard as a BMP file (the editor stores pictures by their own bytes).
std::vector<uint8_t> DibToBmp(const std::vector<uint8_t>& dib) {
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
  return bmp;
}

UINT ObjectsFormat() {
  static const UINT f = RegisterClipboardFormatW(L"Featherweight Slide Objects");
  return f;
}

void PutString(std::string& out, const std::string& s) {
  out += std::to_string(s.size());
  out += '\n';
  out += s;
}
bool GetString(std::string_view& in, std::string& s) {
  const size_t nl = in.find('\n');
  if (nl == std::string_view::npos || nl > 12) return false;
  size_t n = 0;
  for (char c : in.substr(0, nl)) {
    if (c < '0' || c > '9') return false;
    n = n * 10 + static_cast<size_t>(c - '0');
  }
  in.remove_prefix(nl + 1);
  if (n > in.size()) return false;
  s.assign(in.substr(0, n));
  in.remove_prefix(n);
  return true;
}

#if FW_SLIDE_EDIT
// The clipboard's own format: the editor's ShapeClip (the shapes and the parts they use), flattened.
// The deck it came from counts only within this process (links to its slides work only there).
using Clip = fw::slide::edit::ShapeClip;

void PutRels(std::string& out, const std::vector<Clip::Part::Rel>& rels) {
  PutString(out, std::to_string(rels.size()));
  for (const Clip::Part::Rel& r : rels) {
    PutString(out, r.id);
    PutString(out, r.type);
    PutString(out, r.target);
    PutString(out, r.external ? "1" : "0");
  }
}

bool GetCount(std::string_view& in, size_t& n) {
  std::string s;
  if (!GetString(in, s) || s.empty() || s.size() > 6) return false;
  n = static_cast<size_t>(std::strtoul(s.c_str(), nullptr, 10));
  return n <= 100000;
}

bool GetRels(std::string_view& in, std::vector<Clip::Part::Rel>& rels) {
  size_t n = 0;
  if (!GetCount(in, n)) return false;
  rels.resize(n);
  for (Clip::Part::Rel& r : rels) {
    std::string ext;
    if (!GetString(in, r.id) || !GetString(in, r.type) || !GetString(in, r.target) || !GetString(in, ext)) return false;
    r.external = ext == "1";
  }
  return true;
}

std::string Encode(const Clip& c) {
  std::string out = "FWSO2\n";
  PutString(out, std::to_string(GetCurrentProcessId()) + ":" + std::to_string(reinterpret_cast<uintptr_t>(c.deck)));
  PutString(out, std::to_string(c.shapes.size()));
  for (const std::string& x : c.shapes) PutString(out, x);
  PutRels(out, c.rels);
  PutString(out, std::to_string(c.parts.size()));
  for (const Clip::Part& p : c.parts) {
    PutString(out, p.name);
    PutString(out, p.contentType);
    PutString(out, std::string(p.bytes.begin(), p.bytes.end()));
    PutRels(out, p.rels);
  }
  return out;
}

bool Decode(std::string_view in, Clip& c) {
  if (in.substr(0, 6) != "FWSO2\n") return false;
  in.remove_prefix(6);
  std::string origin;
  if (!GetString(in, origin)) return false;
  const std::string self = std::to_string(GetCurrentProcessId()) + ":";
  if (origin.rfind(self, 0) == 0)
    c.deck = reinterpret_cast<const void*>(static_cast<uintptr_t>(std::strtoull(origin.c_str() + self.size(), nullptr, 10)));
  size_t n = 0;
  if (!GetCount(in, n)) return false;
  c.shapes.resize(n);
  for (std::string& x : c.shapes)
    if (!GetString(in, x)) return false;
  if (!GetRels(in, c.rels) || !GetCount(in, n)) return false;
  c.parts.resize(n);
  for (Clip::Part& p : c.parts) {
    std::string bytes;
    if (!GetString(in, p.name) || !GetString(in, p.contentType) || !GetString(in, bytes) || !GetRels(in, p.rels)) return false;
    p.bytes.assign(bytes.begin(), bytes.end());
  }
  return !c.shapes.empty();
}
#endif

struct TableSize {
  int columns, rows;
};
constexpr TableSize kTables[] = {{2, 2}, {3, 2}, {3, 3}, {4, 3}, {4, 4}, {5, 4}, {5, 5}, {6, 6}};
constexpr const wchar_t* kChartNames[] = {L"Column", L"Bar", L"Line", L"Pie"};

struct ArrangeItem {
  int cmd;
  const wchar_t* label;
  const wchar_t* keys;
};
constexpr ArrangeItem kArrange[] = {
    {CmdBringToFront, L"Bring to Front", L"Ctrl+Shift+]"},
    {CmdBringForward, L"Bring Forward", L"Ctrl+]"},
    {CmdSendBackward, L"Send Backward", L"Ctrl+["},
    {CmdSendToBack, L"Send to Back", L"Ctrl+Shift+["},
    {CmdGroupObjects, L"Group", L"Ctrl+G"},
    {CmdUngroupObjects, L"Ungroup", L"Ctrl+Shift+G"},
    {CmdObjAlignLeft, L"Align Left", L""},
    {CmdObjAlignCenter, L"Align Center", L""},
    {CmdObjAlignRight, L"Align Right", L""},
    {CmdObjAlignTop, L"Align Top", L""},
    {CmdObjAlignMiddle, L"Align Middle", L""},
    {CmdObjAlignBottom, L"Align Bottom", L""},
    {CmdDistributeHorizontally, L"Distribute Horizontally", L""},
    {CmdDistributeVertically, L"Distribute Vertically", L""},
    {CmdAlignToSlide, L"Align to Slide", L""},
    {CmdRotateRight90, L"Rotate Right 90°", L""},
    {CmdRotateLeft90, L"Rotate Left 90°", L""},
    {CmdFlipVertical, L"Flip Vertical", L""},
    {CmdFlipHorizontal, L"Flip Horizontal", L""},
};

struct NamedCommand {
  int cmd;
  const wchar_t* label;
  const wchar_t* keys;
};
constexpr NamedCommand kOther[] = {
    {CmdInsertShape, L"Insert shape", L""},
    {CmdObjectFill, L"Shape fill", L""},
    {CmdObjectOutline, L"Shape outline", L""},
    {CmdObjectOutlineWeight, L"Outline weight", L""},
    {CmdObjectShadow, L"Shadow", L""},
    {CmdTableRowAbove, L"Insert row above", L""},
    {CmdTableRowBelow, L"Insert row below", L""},
    {CmdTableColumnLeft, L"Insert column left", L""},
    {CmdTableColumnRight, L"Insert column right", L""},
    {CmdTableDeleteRow, L"Delete row", L""},
    {CmdTableDeleteColumn, L"Delete column", L""},
    {CmdTableMergeCells, L"Merge cells", L""},
    {CmdTableSplitCell, L"Split cells", L""},
    {CmdDuplicateObjects, L"Duplicate", L"Ctrl+D"},
    {CmdArrangeMenu, L"Arrange", L""},
};

constexpr int kTableCmds[] = {CmdTableRowAbove,    CmdTableRowBelow,     CmdTableColumnLeft,
                               CmdTableColumnRight, CmdTableDeleteRow,    CmdTableDeleteColumn,
                               CmdTableMergeCells,  CmdTableSplitCell};

constexpr float kWeights[] ={0.25f, 0.5f, 0.75f, 1, 1.5f, 2.25f, 3, 4.5f, 6};
constexpr const wchar_t* kWeightNames[] = {L"¼ pt", L"½ pt", L"¾ pt", L"1 pt", L"1½ pt", L"2¼ pt", L"3 pt", L"4½ pt", L"6 pt"};
struct Dash {
  const char* preset;
  const wchar_t* name;
};
constexpr Dash kDashes[] = {{"solid", L"Solid"}, {"sysDot", L"Round dot"}, {"sysDash", L"Square dot"},
                            {"dash", L"Dash"},   {"dashDot", L"Dash dot"},  {"lgDash", L"Long dash"}};

}  // namespace

ObjectTool::ObjectTool(Host host) : host_(std::move(host)) {}
ObjectTool::~ObjectTool() = default;

// ---------------------------------------------------------------------------
// The map, coordinates and handles

const ob::ObjectMap& ObjectTool::Map() {
  const size_t slide = host_.current();
  Deck* deck = host_.deck();
  Presentation* model = deck ? deck->Model() : nullptr;
  if (!model || slide >= model->SlideCount()) {
    map_.Clear();
    mapSlide_ = SIZE_MAX;
    return map_;
  }
  if (mapDirty_ || mapSlide_ != slide) {
    if (mapSlide_ != slide) {
      selection_.clear();
      level_ = 0;
      cropMode_ = false;
      textShape_ = 0;
    }
    map_.Build(*model->GetSlide(slide));
    mapSlide_ = slide;
    mapDirty_ = false;
    // What's gone leaves the selection.
    selection_.erase(std::remove_if(selection_.begin(), selection_.end(), [&](uint32_t id) { return !map_.Find(id); }),
                     selection_.end());
    if (level_ && !map_.Find(level_)) level_ = 0;
  }
  return map_;
}

void ObjectTool::SlideChanged() {
  mapDirty_ = true;
  DropLayers();
  guides_.clear();
}

float ObjectTool::Scale() const {
  Deck* deck = host_.deck();
  const D2D1_RECT_F rc = host_.slideRect();
  const float w = deck ? deck->SlideSize().width : 0;
  return w > 0 ? (rc.right - rc.left) / w : 1;
}

draw::Point ObjectTool::ToSlide(float x, float y) const {
  const D2D1_RECT_F rc = host_.slideRect();
  const float k = Scale();
  return {(x - rc.left) / k, (y - rc.top) / k};
}

D2D1_POINT_2F ObjectTool::ToScreen(draw::Point p) const {
  const D2D1_RECT_F rc = host_.slideRect();
  const float k = Scale();
  return {rc.left + p.x * k, rc.top + p.y * k};
}

float ObjectTool::Tolerance() const { return kHandleReach * host_.ui() / Scale(); }
float ObjectTool::RotateOffset() const { return kRotateOffset * host_.ui() / Scale(); }

ob::HandleSet ObjectTool::HandlesOf(const ob::ObjectInfo& o) const {
  ob::HandleSet s;
  s.line = o.line;
  s.rotate = !o.line && o.kind != ob::ObjectKind::GraphicFrame;
  s.lockAspect = o.lockAspect;
  return s;
}

ob::Handle ObjectTool::HandleAt(float x, float y, uint32_t* id) const {
  const draw::Point p = ToSlide(x, y);
  const float tol = Tolerance(), rot = RotateOffset();
  // Topmost selected first.
  for (auto it = selection_.rbegin(); it != selection_.rend(); ++it) {
    const ob::ObjectInfo* o = map_.Find(*it);
    if (!o) continue;
    ob::HandleSet set = HandlesOf(*o);
    if (cropMode_) set.rotate = false;
    if (selection_.size() > 1) set.rotate = false;
    const ob::Handle h = ob::HitHandle(o->frame, set, p, tol, rot);
    if (h != ob::Handle::None && h != ob::Handle::Body) {
      if (id) *id = o->id;
      return h;
    }
  }
  return ob::Handle::None;
}

// ---------------------------------------------------------------------------
// The pointer

bool ObjectTool::WantsPress(float x, float y) const {
  if (HandleAt(x, y, nullptr) != ob::Handle::None) return true;
  const draw::Point p = ToSlide(x, y);
  const uint32_t id = map_.HitTest(p, level_, Tolerance() / 2);
  if (!id) return false;
  const ob::ObjectInfo* o = map_.Find(id);
  if (!o || !o->shape) return false;
  // Objects without text are the objects' (an empty text body too, except a placeholder's: that
  // takes the caret); text bodies are text editing's, except at their edge.
  const bool text = (o->shape->textNode && !EmptyText(*o)) || o->shape->kind == fw::slide::Shape::Kind::Table;
  if (!text || o->kind == ob::ObjectKind::Group) return true;
  if (std::find(selection_.begin(), selection_.end(), id) != selection_.end()) {
    const float edge = Tolerance() * 0.75f;
    ob::Frame inner = o->frame;
    inner.rect = {inner.rect.left + edge, inner.rect.top + edge, inner.rect.right - edge, inner.rect.bottom - edge};
    return !ob::HitFrame(inner, false, p, 0);
  }
  return false;
}

bool ObjectTool::MouseDown(float x, float y, UINT mods, bool doubleClick) {
  if (!Editable(false)) return false;  // a deck shown, not edited: the tab pans and follows links
  Map();
  const draw::Point p = ToSlide(x, y);
  drag_ = DragState();
  drag_.downX = x;
  drag_.downY = y;
  drag_.from = drag_.at = p;
  guides_.clear();
  const bool shift = (mods & kModShift) != 0, ctrl = (mods & kModCtrl) != 0;
  // A handle of the selection.
  uint32_t hid = 0;
  const ob::Handle h = HandleAt(x, y, &hid);
  if (h != ob::Handle::None) {
    drag_.kind = DragKind::Pending;
    drag_.handle = h;
    drag_.grabbed = hid;
    return true;
  }
  if (cropMode_ && !doubleClick) cropMode_ = false;
  uint32_t id = map_.HitTest(p, level_, Tolerance() / 2);
  if (!id && level_) {  // outside the group being edited: back to the slide's level
    level_ = 0;
    id = map_.HitTest(p, 0, Tolerance() / 2);
  }
  if (doubleClick && id) {
    const ob::ObjectInfo* o = map_.Find(id);
    if (o && o->kind == ob::ObjectKind::Group) {  // into the group
      const uint32_t deep = map_.HitDeepest(p, Tolerance() / 2);
      level_ = id;
      id = deep ? map_.AtLevel(deep, level_) : 0;
      if (id) selection_ = {id};
      host_.invalidate();
      return id != 0;
    }
    if (o && o->kind == ob::ObjectKind::Picture) {
      selection_ = {id};
      cropMode_ = !cropMode_;
      host_.invalidate();
      return true;
    }
    if (o && EmptyText(*o)) return false;  // text editing's: the caret in the empty shape
  }
  if (!id) {
    // The empty slide: a marquee (Shift/Ctrl add to the selection).
    if (!shift && !ctrl) selection_.clear();
    drag_.kind = DragKind::Marquee;
    drag_.additive = shift || ctrl;
    drag_.marquee = {p.x, p.y, p.x, p.y};
    textShape_ = 0;
    host_.invalidate();
    return true;
  }
  const bool selected = std::find(selection_.begin(), selection_.end(), id) != selection_.end();
  if (shift || ctrl) {
    if (selected && !ctrl) {
      selection_.erase(std::remove(selection_.begin(), selection_.end(), id), selection_.end());
      host_.invalidate();
      return true;
    }
    if (!selected) selection_.push_back(id);
  } else if (!selected) {
    selection_ = {id};
  }
  textShape_ = 0;
  drag_.kind = DragKind::Pending;
  drag_.handle = ob::Handle::Body;
  drag_.grabbed = id;
  drag_.copy = ctrl && selected;
  host_.invalidate();
  return true;
}

void ObjectTool::MouseMove(float x, float y, UINT mods) {
  if (drag_.kind == DragKind::None) return;
  drag_.at = ToSlide(x, y);
  if (drag_.kind == DragKind::Pending) {
    if (std::hypot(x - drag_.downX, y - drag_.downY) < kDragStart * host_.ui()) return;
    StartDrag(mods);
  }
  UpdateDrag(mods);
  host_.invalidate();
}

void ObjectTool::MouseUp(float x, float y, UINT mods) {
  if (drag_.kind == DragKind::None) return;
  drag_.at = ToSlide(x, y);
  if (drag_.kind == DragKind::Pending) {
    // A click on a selected object of several: it alone (unless a modifier kept the others).
    if (drag_.handle == ob::Handle::Body && !(mods & (kModShift | kModCtrl)) && selection_.size() > 1)
      selection_ = {drag_.grabbed};
    drag_ = DragState();
    host_.invalidate();
    return;
  }
  FinishDrag(mods);
  drag_ = DragState();
  guides_.clear();
  DropLayers();
  host_.invalidate();
}

void ObjectTool::StartDrag(UINT mods) {
  (void)mods;
  const ob::Handle h = drag_.handle;
  if (h == ob::Handle::Body) {
    drag_.kind = DragKind::Move;
    if (cropMode_) drag_.kind = DragKind::Move;
  } else if (h == ob::Handle::Rotate) {
    drag_.kind = DragKind::Rotate;
  } else if (h == ob::Handle::Start || h == ob::Handle::End) {
    drag_.kind = DragKind::LineEnd;
  } else {
    drag_.kind = cropMode_ ? DragKind::Crop : DragKind::Resize;
  }
  if (drag_.kind == DragKind::Crop) drag_.cropStart = {};
  // The objects that move: the selection (a handle drags only its own object when cropping,
  // turning several or moving a line's end).
  const bool own = drag_.kind == DragKind::Crop || drag_.kind == DragKind::LineEnd;
  for (uint32_t id : selection_) {
    if (own && id != drag_.grabbed) continue;
    if (const ob::ObjectInfo* o = map_.Find(id)) drag_.objects.push_back({id, o->frame, o->frame});
  }
#if FW_SLIDE_EDIT
  if (drag_.kind == DragKind::Crop) {
    Deck* deck = host_.deck();
    if (fw::slide::edit::Editor* ed = deck ? deck->Editor() : nullptr) {
      pugi::xml_document doc;
      const std::string xml = ed->ShapeXml(host_.current(), drag_.grabbed);
      if (doc.load_string(xml.c_str())) drag_.cropStart = ob::ReadCrop(doc.document_element());
    }
  }
#endif
  layersDue_ = true;
}

std::vector<draw::Rect> ObjectTool::OtherBoxes() const {
  std::vector<draw::Rect> out;
  for (const ob::ObjectInfo* o : map_.Children(level_)) {
    bool moving = false;
    for (const Moving& m : drag_.objects) moving = moving || map_.Within(m.id, o->id) || o->id == m.id;
    if (!moving) out.push_back(o->frame.Bounds());
  }
  return out;
}

ob::SnapSettings ObjectTool::Snapping(UINT mods) const {
  ob::SnapSettings s;
  s.tolerance = 5 * host_.ui() / Scale();
  s.grid = kGrid;
  if (mods & kModAlt) {  // Alt: free
    s.slide = s.objects = s.spacing = false;
    s.grid = 0;
  }
  return s;
}

void ObjectTool::UpdateDrag(UINT mods) {
  const bool shift = (mods & kModShift) != 0, ctrl = (mods & kModCtrl) != 0;
  Deck* deck = host_.deck();
  const draw::Size slide = deck ? deck->SlideSize() : draw::Size{};
  float dx = drag_.at.x - drag_.from.x, dy = drag_.at.y - drag_.from.y;
  guides_.clear();
  switch (drag_.kind) {
    case DragKind::Marquee:
      drag_.marquee = ob::Normalized(drag_.from, drag_.at);
      return;
    case DragKind::Move: {
      if (shift) (std::fabs(dx) >= std::fabs(dy) ? dy : dx) = 0;  // straight across or down
      draw::Rect box{};
      for (size_t i = 0; i < drag_.objects.size(); ++i)
        box = i ? ob::Union(box, drag_.objects[i].start.Bounds()) : drag_.objects[i].start.Bounds();
      const ob::Snap snap = ob::SnapMove(ob::Offset(box, dx, dy), OtherBoxes(), slide, Snapping(mods));
      if (!shift || dx != 0) dx += snap.dx;
      if (!shift || dy != 0) dy += snap.dy;
      guides_ = snap.guides;
      for (Moving& m : drag_.objects) {
        m.now = m.start;
        m.now.rect = ob::Offset(m.start.rect, dx, dy);
      }
      drag_.copy = drag_.copy || ctrl;
      return;
    }
    case DragKind::Rotate:
      for (Moving& m : drag_.objects) {
        // Each turns about its own centre by the grabbed object's angle.
        const ob::ObjectInfo* g = map_.Find(drag_.grabbed);
        const ob::Frame grabbed = g ? g->frame : m.start;
        const ob::Frame turned = ob::Rotate(grabbed, drag_.from, drag_.at, shift);
        m.now = m.start;
        m.now.rotation = ob::NormalizeDegrees(m.start.rotation + (turned.rotation - grabbed.rotation));
        if (shift) m.now.rotation = ob::NormalizeDegrees(std::round(m.now.rotation / 15) * 15);
      }
      return;
    case DragKind::LineEnd:
      for (Moving& m : drag_.objects) m.now = ob::MoveLineEnd(m.start, drag_.handle, drag_.at, shift);
      return;
    case DragKind::Resize:
    case DragKind::Crop: {
      const ob::ObjectInfo* g = map_.Find(drag_.grabbed);
      for (Moving& m : drag_.objects) {
        const ob::ObjectInfo* o = map_.Find(m.id);
        ob::ResizeOptions opt;
        opt.keepAspect = drag_.kind == DragKind::Resize && (shift || (o && o->lockAspect && ob::IsCorner(drag_.handle)));
        opt.fromCentre = ctrl;
        opt.minSize = 1;
        // Another selected object's own handle in the same place, moved as far along its own axes.
        draw::Point from = drag_.from, to = drag_.at;
        if (g && m.id != drag_.grabbed) {
          const draw::Point a = ob::HandlePoint(g->frame, drag_.handle, 0), b = ob::HandlePoint(m.start, drag_.handle, 0);
          from = {from.x - a.x + b.x, from.y - a.y + b.y};
          to = {to.x - a.x + b.x, to.y - a.y + b.y};
        }
        m.now = ob::Resize(m.start, drag_.handle, from, to, opt);
        if (drag_.kind == DragKind::Crop) {  // cropping never flips
          m.now.flipH = m.start.flipH;
          m.now.flipV = m.start.flipV;
        }
      }
      // A single unturned object snaps its dragged edges.
      if (drag_.objects.size() == 1 && drag_.objects[0].start.rotation == 0 && !shift) {
        Moving& m = drag_.objects[0];
        const int i = static_cast<int>(drag_.handle);
        ob::Edges e;
        e.left = i == 0 || i == 6 || i == 7;
        e.right = i == 2 || i == 3 || i == 4;
        e.top = i == 0 || i == 1 || i == 2;
        e.bottom = i == 4 || i == 5 || i == 6;
        if (m.now.flipH) std::swap(e.left, e.right);
        if (m.now.flipV != m.start.flipV) std::swap(e.top, e.bottom);
        const ob::Snap snap = ob::SnapResize(m.now.rect, e, OtherBoxes(), slide, Snapping(mods));
        if (e.left) m.now.rect.left += snap.dx;
        if (e.right) m.now.rect.right += snap.dx;
        if (e.top) m.now.rect.top += snap.dy;
        if (e.bottom) m.now.rect.bottom += snap.dy;
        guides_ = snap.guides;
      }
      return;
    }
    default: return;
  }
}

void ObjectTool::FinishDrag(UINT mods) {
  (void)mods;
  switch (drag_.kind) {
    case DragKind::Marquee: {
      std::vector<uint32_t> in = map_.InMarquee(drag_.marquee, level_);
      if (!drag_.additive) selection_.clear();
      for (uint32_t id : in)
        if (std::find(selection_.begin(), selection_.end(), id) == selection_.end()) selection_.push_back(id);
      return;
    }
    case DragKind::Move: {
      const float dx = drag_.objects.empty() ? 0 : drag_.objects[0].now.rect.left - drag_.objects[0].start.rect.left;
      const float dy = drag_.objects.empty() ? 0 : drag_.objects[0].now.rect.top - drag_.objects[0].start.rect.top;
      if (dx == 0 && dy == 0) return;
      if (drag_.copy) DuplicateSelection(dx, dy);
      else CommitFrames(drag_.objects, u"Move");
      return;
    }
    case DragKind::Resize:
    case DragKind::LineEnd: CommitFrames(drag_.objects, u"Resize"); return;
    case DragKind::Rotate: CommitFrames(drag_.objects, u"Rotate"); return;
    case DragKind::Crop: {
#if FW_SLIDE_EDIT
      if (drag_.objects.empty() || !Editable()) return;
      Deck* deck = host_.deck();
      fw::slide::edit::Editor* ed = deck->Editor();
      const Moving& m = drag_.objects[0];
      const ob::Crop c = ob::CropForFrame(drag_.cropStart, m.start.rect, m.now.rect);
      fw::slide::edit::EditGroup group(*ed, u"Crop Picture");
      fw::slide::edit::Crop ec{c.left, c.top, c.right, c.bottom};
      if (!ed->SetCrop(host_.current(), m.id, ec)) return;
      if (!CommitFrames(drag_.objects, u"Crop Picture")) return;
      group.done = true;
      Edited();
#endif
      return;
    }
    default: return;
  }
}

// ---------------------------------------------------------------------------
// Keys

int ObjectTool::Shortcut(UINT vk, UINT mods) const {
  const bool ctrl = (mods & kModCtrl) != 0, shift = (mods & kModShift) != 0, alt = (mods & kModAlt) != 0;
  if (!ctrl || alt) return 0;
  // The deck's one undo, also while text is edited (typing is the editor's steps too; text editing
  // reads its text back after an undo).
  if (vk == 'Z') return shift ? CmdRedo : CmdUndo;
  if (vk == 'Y') return shift ? 0 : CmdRedo;
  if (textEditing_) return 0;
  switch (vk) {
    case 'G': return shift ? CmdUngroupObjects : CmdGroupObjects;
    case VK_OEM_6: return shift ? CmdBringToFront : CmdBringForward;   // ]
    case VK_OEM_4: return shift ? CmdSendToBack : CmdSendBackward;     // [
    case 'D': return !shift && !selection_.empty() ? CmdDuplicateObjects : 0;
  }
  return 0;
}

bool ObjectTool::KeyDown(UINT vk, UINT mods) {
  if (textEditing_ || drag_.kind != DragKind::None) {
    if (vk == VK_ESCAPE && drag_.kind != DragKind::None) {  // the drag is called off
      drag_ = DragState();
      guides_.clear();
      DropLayers();
      host_.invalidate();
      return true;
    }
    return false;
  }
  Map();
  const bool ctrl = (mods & kModCtrl) != 0, shift = (mods & kModShift) != 0, alt = (mods & kModAlt) != 0;
  switch (vk) {
    case VK_TAB:
      if (ctrl || alt) return false;
      if (const uint32_t next = map_.Next(selection_.empty() ? 0 : selection_.back(), level_, shift)) {
        selection_ = {next};
        cropMode_ = false;
        host_.invalidate();
      }
      return true;
    case VK_ESCAPE:
      if (cropMode_) {
        cropMode_ = false;
      } else if (level_) {
        selection_ = {level_};
        level_ = map_.Find(level_) ? map_.Find(level_)->parent : 0;
      } else if (!selection_.empty()) {
        selection_.clear();
      } else {
        return false;
      }
      host_.invalidate();
      return true;
    case VK_DELETE:
    case VK_BACK:
      if (selection_.empty() || ctrl || alt) return false;
      DeleteSelection();
      return true;
    case VK_LEFT:
    case VK_RIGHT:
    case VK_UP:
    case VK_DOWN: {
      if (selection_.empty() || alt) return false;
      const float step = ctrl ? kFineNudge : kNudge;
      const float dx = vk == VK_LEFT ? -step : vk == VK_RIGHT ? step : 0;
      const float dy = vk == VK_UP ? -step : vk == VK_DOWN ? step : 0;
      Nudge(dx, dy);
      return true;
    }
    default: return false;
  }
}

HCURSOR ObjectTool::Cursor(float x, float y) const {
  static HCURSOR all = LoadCursor(nullptr, IDC_SIZEALL), nwse = LoadCursor(nullptr, IDC_SIZENWSE),
                 nesw = LoadCursor(nullptr, IDC_SIZENESW), we = LoadCursor(nullptr, IDC_SIZEWE),
                 ns = LoadCursor(nullptr, IDC_SIZENS), cross = LoadCursor(nullptr, IDC_CROSS);
  if (drag_.kind == DragKind::Move) return all;
  if (drag_.kind == DragKind::Rotate || drag_.kind == DragKind::Marquee) return cross;
  uint32_t id = 0;
  ob::Handle h = drag_.kind == DragKind::Resize || drag_.kind == DragKind::Crop || drag_.kind == DragKind::LineEnd
                     ? drag_.handle
                     : HandleAt(x, y, &id);
  if (h == ob::Handle::Rotate) return cross;
  if (h == ob::Handle::Start || h == ob::Handle::End) return cross;
  if (ob::IsSizing(h)) {
    // The handle's direction on screen, the object's turn included, to the nearest of four cursors.
    const ob::ObjectInfo* o = map_.Find(id ? id : drag_.grabbed);
    const float turn = o ? o->frame.rotation : 0;
    const int steps = static_cast<int>(std::lround(turn / 45));
    const int dir = (static_cast<int>(h) + steps) % 8;  // 0 nw, 1 n, 2 ne, 3 e...
    switch (dir % 4) {
      case 0: return nwse;
      case 1: return ns;
      case 2: return nesw;
      default: return we;
    }
  }
  if (!selection_.empty() && !textEditing_) {
    const draw::Point p = ToSlide(x, y);
    const uint32_t hit = map_.HitTest(p, level_, Tolerance() / 2);
    if (hit && std::find(selection_.begin(), selection_.end(), hit) != selection_.end() && WantsPress(x, y)) return all;
  }
  return nullptr;
}

std::wstring ObjectTool::Tooltip(float x, float y) const {
  uint32_t id = 0;
  if (HandleAt(x, y, &id) == ob::Handle::Rotate) return L"Drag to turn; Shift: in 15° steps";
  return {};
}

// ---------------------------------------------------------------------------
// Drawing

void ObjectTool::DropLayers() {
  under_.Reset();
  layer_.Reset();
  layersDue_ = false;
}

void ObjectTool::ReleaseDeviceResources() {
  DropLayers();
  ctx_.Reset();
}

void ObjectTool::MakeLayers(ID2D1DeviceContext* dc) {
  layersDue_ = false;
  Deck* deck = host_.deck();
  if (!deck || drag_.objects.empty()) return;
  const D2D1_RECT_F rc = host_.slideRect();
  const UINT w = static_cast<UINT>(std::lround(rc.right - rc.left)), h = static_cast<UINT>(std::lround(rc.bottom - rc.top));
  if (w == 0 || h == 0 || w > kMaxLayer || h > kMaxLayer) return;
  if (!ctx_) {
    ComPtr<ID2D1Device> device;
    dc->GetDevice(&device);
    if (!device || FAILED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &ctx_))) return;
  }
  std::set<uint32_t> moving, drawn;
  for (const Moving& m : drag_.objects) moving.insert(m.id);
  // What the moving layer draws: the moving objects, what's in them, and the groups they're in.
  for (const ob::ObjectInfo& o : map_.Objects()) {
    for (uint32_t id : moving)
      if (map_.Within(o.id, id) || map_.Within(id, o.id)) drawn.insert(o.id);
  }
  const draw::Size s = deck->SlideSize();
  const size_t slide = host_.current();
  auto render = [&](render::DrawFilter& filter, bool opaque) -> ComPtr<ID2D1Bitmap1> {
    ComPtr<ID2D1Bitmap1> bmp;
    const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    if (FAILED(ctx_->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0, props, &bmp))) return nullptr;
    ctx_->SetTarget(bmp.Get());
    ctx_->BeginDraw();
    ctx_->Clear(opaque ? D2D1::ColorF(1, 1, 1, 1) : D2D1::ColorF(0, 0, 0, 0));
    ctx_->SetTransform(D2D1::Matrix3x2F::Scale(w / s.width, h / s.height));
    {
      draw::D2DPainter painter(ctx_.Get());
      painter.UsePdfLikeDrawing();  // as the slide view draws it, so text doesn't change look while dragged
      deck->PaintFiltered(slide, painter, &filter);
    }
    ctx_->SetTransform(D2D1::Matrix3x2F::Identity());
    const HRESULT hr = ctx_->EndDraw();
    ctx_->SetTarget(nullptr);
    return SUCCEEDED(hr) ? bmp : nullptr;
  };
  SkipFilter skip(moving);
  OnlyFilter only(drawn);
  under_ = render(skip, true);
  layer_ = render(only, false);
  layersRect_ = rc;
}

void ObjectTool::Draw(Canvas& c, ID2D1DeviceContext* dc) {
  Map();
  const bool live = drag_.kind == DragKind::Move || drag_.kind == DragKind::Resize || drag_.kind == DragKind::Rotate ||
                    drag_.kind == DragKind::LineEnd || drag_.kind == DragKind::Crop;
  if (live && layersDue_) MakeLayers(dc);
  const D2D1_RECT_F rc = host_.slideRect();
  if (live && under_ && layer_ && rc.left == layersRect_.left && rc.top == layersRect_.top) {
    dc->DrawBitmap(under_.Get(), rc, 1, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
    // The moving layer: one transform for all when they move; one object turned or resized gets
    // its own; several turned or resized show a faint copy where they were, and outlines.
    const float k = Scale();
    const draw::Matrix toScreen = draw::Matrix::Scale(k, k) * draw::Matrix::Translate(rc.left, rc.top);
    const draw::Matrix fromScreen = toScreen.Inverse();
    draw::Matrix slideMove;
    bool single = true;
    float opacity = 1;
    if (drag_.kind == DragKind::Move && !drag_.objects.empty()) {
      const Moving& m = drag_.objects[0];
      slideMove = draw::Matrix::Translate(m.now.rect.left - m.start.rect.left, m.now.rect.top - m.start.rect.top);
      if (drag_.copy) {  // a copy: the originals stay
        dc->DrawBitmap(layer_.Get(), rc, 1, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
      }
    } else if (drag_.objects.size() == 1 && drag_.kind != DragKind::Crop) {
      const Moving& m = drag_.objects[0];
      const float w0 = m.start.rect.Width(), h0 = m.start.rect.Height();
      const float sx = w0 > 0 ? m.now.rect.Width() / w0 : 1, sy = h0 > 0 ? m.now.rect.Height() / h0 : 1;
      slideMove = m.start.Transform().Inverse() * draw::Matrix::Scale(sx, sy) * m.now.Transform();
    } else {
      single = false;
      opacity = 0.35f;
    }
    D2D1_MATRIX_3X2_F old;
    dc->GetTransform(&old);
    if (single) dc->SetTransform(ToD2D(fromScreen * slideMove * toScreen) * old);
    dc->DrawBitmap(layer_.Get(), rc, opacity, D2D1_INTERPOLATION_MODE_LINEAR);
    dc->SetTransform(old);
  }
  DrawFrames(c);
  DrawGuides(c);
  if (drag_.kind == DragKind::Marquee) {
    const D2D1_POINT_2F a = ToScreen({drag_.marquee.left, drag_.marquee.top}),
                        b = ToScreen({drag_.marquee.right, drag_.marquee.bottom});
    const Palette& p = theme::P();
    const D2D1_RECT_F r = RectF(a.x, a.y, b.x, b.y);
    c.Fill(r, Alpha(p.accent, 0.12f));
    c.Stroke(r, p.accent, 1 * host_.ui());
  }
}

void ObjectTool::DrawFrames(Canvas& c) {
  if (selection_.empty() && !level_) return;
  const Palette& p = theme::P();
  const float u = host_.ui();
  auto line = [&](draw::Point a, draw::Point b, D2D1_COLOR_F col, float w) {
    const D2D1_POINT_2F pa = ToScreen(a), pb = ToScreen(b);
    c.Line(pa.x, pa.y, pb.x, pb.y, col, w);
  };
  auto outline = [&](const ob::Frame& f, D2D1_COLOR_F col, float w) {
    const auto k = f.Corners();
    for (int i = 0; i < 4; ++i) line(k[i], k[(i + 1) % 4], col, w);
  };
  auto handle = [&](draw::Point at, bool round) {
    const D2D1_POINT_2F s = ToScreen(at);
    const D2D1_RECT_F h = RectF(s.x - 4 * u, s.y - 4 * u, s.x + 4 * u, s.y + 4 * u);
    if (round) {
      c.FillEllipse(s.x, s.y, 5 * u, 5 * u, p.paper);
      c.StrokeRound(Inflate(h, 1 * u, 1 * u), 5 * u, p.accent, 1.5f * u);
    } else {
      c.Fill(h, p.paper);
      c.Stroke(h, p.accent, 1.2f * u);
    }
  };
  // The group being edited: a faint frame around it.
  if (level_)
    if (const ob::ObjectInfo* g = map_.Find(level_)) outline(g->frame, Alpha(p.accent, 0.5f), 1 * u);
  const bool dragging = drag_.kind == DragKind::Move || drag_.kind == DragKind::Resize || drag_.kind == DragKind::Rotate ||
                        drag_.kind == DragKind::LineEnd || drag_.kind == DragKind::Crop;
  if (dragging) {
    for (const Moving& m : drag_.objects) {
      const ob::ObjectInfo* o = map_.Find(m.id);
      if (o && o->line) {
        const auto e = m.now.LineEnds();
        line(e[0], e[1], p.accent, 1 * u);
      } else {
        outline(m.now, p.accent, 1 * u);
      }
      if (drag_.kind == DragKind::Crop) outline(m.start, Alpha(p.accent, 0.4f), 1 * u);
    }
    return;
  }
  const float rot = RotateOffset();
  for (uint32_t id : selection_) {
    const ob::ObjectInfo* o = map_.Find(id);
    if (!o) continue;
    const ob::HandleSet set = HandlesOf(*o);
    if (set.line) {
      handle(ob::HandlePoint(o->frame, ob::Handle::Start, 0), false);
      handle(ob::HandlePoint(o->frame, ob::Handle::End, 0), false);
      continue;
    }
    outline(o->frame, p.accent, (id == textShape_ ? 1.5f : 1) * u);
    if (cropMode_ && o->kind == ob::ObjectKind::Picture) {
      // Crop handles: thick black bars at the corners and edges.
      for (int i = 0; i < 8; ++i) {
        const D2D1_POINT_2F s = ToScreen(ob::HandlePoint(o->frame, static_cast<ob::Handle>(i), 0));
        c.Fill(RectF(s.x - 5 * u, s.y - 5 * u, s.x + 5 * u, s.y + 5 * u), p.text);
      }
      continue;
    }
    if (set.rotate && selection_.size() == 1) {
      line(ob::HandlePoint(o->frame, ob::Handle::Top, 0), ob::HandlePoint(o->frame, ob::Handle::Rotate, rot), p.accent, 1 * u);
      handle(ob::HandlePoint(o->frame, ob::Handle::Rotate, rot), true);
    }
    for (int i = 0; i < 8; ++i) handle(ob::HandlePoint(o->frame, static_cast<ob::Handle>(i), 0), false);
  }
}

void ObjectTool::DrawGuides(Canvas& c) {
  if (guides_.empty()) return;
  const float u = host_.ui();
  for (const ob::Guide& g : guides_) {
    switch (g.kind) {
      case ob::Guide::Kind::Vertical: {
        const D2D1_POINT_2F a = ToScreen({g.at, g.from}), b = ToScreen({g.at, g.to});
        c.Line(a.x, a.y, b.x, b.y, kGuideColor, 1 * u);
        break;
      }
      case ob::Guide::Kind::Horizontal: {
        const D2D1_POINT_2F a = ToScreen({g.from, g.at}), b = ToScreen({g.to, g.at});
        c.Line(a.x, a.y, b.x, b.y, kGuideColor, 1 * u);
        break;
      }
      case ob::Guide::Kind::GapX:
      case ob::Guide::Kind::GapY: {
        const bool x = g.kind == ob::Guide::Kind::GapX;
        const D2D1_POINT_2F a = ToScreen(x ? draw::Point{g.from, g.at} : draw::Point{g.at, g.from});
        const D2D1_POINT_2F b = ToScreen(x ? draw::Point{g.to, g.at} : draw::Point{g.at, g.to});
        c.Line(a.x, a.y, b.x, b.y, kGuideColor, 1 * u);
        // Small ends, as PowerPoint marks equal gaps.
        const float t = 4 * u;
        if (x) {
          c.Line(a.x, a.y - t, a.x, a.y + t, kGuideColor, 1 * u);
          c.Line(b.x, b.y - t, b.x, b.y + t, kGuideColor, 1 * u);
        } else {
          c.Line(a.x - t, a.y, a.x + t, a.y, kGuideColor, 1 * u);
          c.Line(b.x - t, b.y, b.x + t, b.y, kGuideColor, 1 * u);
        }
        break;
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Selection

void ObjectTool::Select(const std::vector<uint32_t>& ids) {
  Map();
  selection_.clear();
  for (uint32_t id : ids)
    if (map_.Find(id)) selection_.push_back(id);
  if (!selection_.empty()) {
    const ob::ObjectInfo* o = map_.Find(selection_[0]);
    level_ = o ? o->parent : 0;
  }
  cropMode_ = false;
  host_.invalidate();
}

void ObjectTool::SelectForText(uint32_t id) {
  Select({id});
  textShape_ = id;
}

void ObjectTool::ClearSelection() {
  selection_.clear();
  textShape_ = 0;
  cropMode_ = false;
  level_ = 0;
  host_.invalidate();
}

// ---------------------------------------------------------------------------
// Edits

void ObjectTool::Refused(const std::wstring& why) const {
  if (host_.refused) host_.refused(why);
}

void ObjectTool::Edited() {
  // The editor's change listener (registered in Editable) has dropped what was drawn.
  mapDirty_ = true;
  host_.invalidate();
}

bool ObjectTool::Editable(bool say) const {
#if FW_SLIDE_EDIT
  Deck* deck = host_.deck();
  fw::slide::edit::Editor* ed = deck ? deck->Editor() : nullptr;
  std::u16string why;
  if (ed && ed->CanEdit(&why)) {
    if (!listening_) {
      const_cast<ObjectTool*>(this)->listening_ = true;
      ObjectTool* self = const_cast<ObjectTool*>(this);
      // Edits, undo and redo, whoever made them: the map is read again, and what the tab drew of
      // the changed slides goes.
      ed->OnChange([self](const fw::slide::edit::Changes& c) {
        self->SlideChanged();
        if (!self->host_.slideEdited) return;
        if (c.allSlides || c.slideList) self->host_.slideEdited(SIZE_MAX);
        else
          for (const fw::slide::edit::SlideChange& sc : c.slides)
            if (sc.index >= 0) self->host_.slideEdited(static_cast<size_t>(sc.index));
      });
    }
    return true;
  }
  if (say) Refused(why.empty() ? L"This presentation can't be edited." : W(why));
  return false;
#else
  if (say) Refused(L"Editing presentations isn't available in this build yet.");
  return false;
#endif
}

bool ObjectTool::CommitFrames(const std::vector<Moving>& moved, const std::u16string& step) {
#if FW_SLIDE_EDIT
  if (moved.empty() || !Editable()) return false;
  Deck* deck = host_.deck();
  fw::slide::edit::Editor* ed = deck->Editor();
  std::vector<std::pair<uint32_t, fw::slide::edit::Transform>> list;
  for (const Moving& m : moved) {
    const ob::ObjectInfo* o = map_.Find(m.id);
    if (!o) continue;
    ob::Frame local;
    if (m.now.rotation == m.start.rotation && m.now.rect.Width() == m.start.rect.Width() &&
        m.now.rect.Height() == m.start.rect.Height() && m.now.flipH == m.start.flipH && m.now.flipV == m.start.flipV) {
      // A move: the size stays exactly as written.
      const draw::Point d = ob::DeltaInParent({m.now.rect.left - m.start.rect.left, m.now.rect.top - m.start.rect.top},
                                              o->parentToSlide);
      local = o->local;
      local.rect = ob::Offset(local.rect, d.x, d.y);
    } else {
      local = o->parent ? ob::FrameInParent(m.now, o->parentToSlide) : m.now;
    }
    fw::slide::edit::Transform t;
    t.bounds = local.rect;
    t.rotation = local.rotation;
    t.flipH = local.flipH;
    t.flipV = local.flipV;
    list.push_back({m.id, t});
  }
  if (list.empty()) return false;
  fw::slide::edit::EditGroup group(*ed, step);
  const fw::slide::edit::Result r = ed->SetTransforms(host_.current(), list);
  if (!r) {
    Refused(W(r.message));
    return false;
  }
  group.done = true;
  Edited();
  return true;
#else
  (void)moved;
  (void)step;
  Editable();
  return false;
#endif
}

bool ObjectTool::Nudge(float dx, float dy) {
  Map();
  std::vector<Moving> moved;
  for (uint32_t id : selection_)
    if (const ob::ObjectInfo* o = map_.Find(id)) {
      Moving m{id, o->frame, o->frame};
      m.now.rect = ob::Offset(o->frame.rect, dx, dy);
      moved.push_back(m);
    }
  return CommitFrames(moved, u"Nudge");
}

bool ObjectTool::DeleteSelection() {
#if FW_SLIDE_EDIT
  if (selection_.empty() || !Editable()) return false;
  const fw::slide::edit::Result r = host_.deck()->Editor()->DeleteShapes(host_.current(), selection_);
  if (!r) {
    Refused(W(r.message));
    return false;
  }
  selection_.clear();
  cropMode_ = false;
  Edited();
  return true;
#else
  return Editable();
#endif
}

bool ObjectTool::Arrange(int cmd) {
#if FW_SLIDE_EDIT
  if (selection_.empty() || !Editable()) return false;
  using Z = fw::slide::edit::ZOrder;
  const Z z = cmd == CmdBringToFront ? Z::BringToFront
              : cmd == CmdBringForward ? Z::BringForward
              : cmd == CmdSendBackward ? Z::SendBackward
                                       : Z::SendToBack;
  const fw::slide::edit::Result r = host_.deck()->Editor()->Arrange(host_.current(), selection_, z);
  if (!r) {
    Refused(W(r.message));
    return false;
  }
  Edited();
  return true;
#else
  (void)cmd;
  return Editable();
#endif
}

bool ObjectTool::AlignOrDistribute(int cmd) {
  Map();
  Deck* deck = host_.deck();
  if (selection_.empty() || !deck) return false;
  std::vector<draw::Rect> boxes;
  std::vector<Moving> moved;
  for (uint32_t id : selection_)
    if (const ob::ObjectInfo* o = map_.Find(id)) {
      boxes.push_back(o->frame.Bounds());
      moved.push_back({id, o->frame, o->frame});
    }
  const bool toSlide = alignToSlide_ || boxes.size() == 1;
  std::vector<draw::Point> d;
  if (cmd == CmdDistributeHorizontally || cmd == CmdDistributeVertically) {
    d = ob::Distribute(boxes, cmd == CmdDistributeHorizontally, alignToSlide_ || boxes.size() < 3, deck->SlideSize());
  } else {
    static constexpr ob::AlignTo kHow[] = {ob::AlignTo::Left, ob::AlignTo::Centre, ob::AlignTo::Right,
                                           ob::AlignTo::Top,  ob::AlignTo::Middle, ob::AlignTo::Bottom};
    d = ob::Align(boxes, kHow[cmd - CmdObjAlignLeft], toSlide, deck->SlideSize());
  }
  std::vector<Moving> changed;
  for (size_t i = 0; i < moved.size(); ++i)
    if (std::fabs(d[i].x) > 0.001f || std::fabs(d[i].y) > 0.001f) {
      moved[i].now.rect = ob::Offset(moved[i].start.rect, d[i].x, d[i].y);
      changed.push_back(moved[i]);
    }
  if (changed.empty()) return true;
  return CommitFrames(changed, cmd >= CmdDistributeHorizontally ? u"Distribute" : u"Align");
}

bool ObjectTool::GroupSelection() {
#if FW_SLIDE_EDIT
  if (selection_.size() < 2 || !Editable()) return false;
  uint32_t group = 0;
  const fw::slide::edit::Result r = host_.deck()->Editor()->Group(host_.current(), selection_, &group);
  if (!r) {
    Refused(W(r.message));
    return false;
  }
  Edited();
  Map();
  selection_ = {group};
  return true;
#else
  return Editable();
#endif
}

bool ObjectTool::UngroupSelection() {
#if FW_SLIDE_EDIT
  if (!Editable()) return false;
  Map();
  std::vector<uint32_t> groups, children;
  for (uint32_t id : selection_)
    if (const ob::ObjectInfo* o = map_.Find(id); o && o->kind == ob::ObjectKind::Group) groups.push_back(id);
  if (groups.empty()) return false;
  fw::slide::edit::Editor* ed = host_.deck()->Editor();
  fw::slide::edit::EditGroup step(*ed, u"Ungroup");
  for (uint32_t g : groups) {
    std::vector<uint32_t> ids;
    const fw::slide::edit::Result r = ed->Ungroup(host_.current(), g, &ids);
    if (!r) {
      Refused(W(r.message));
      return false;
    }
    children.insert(children.end(), ids.begin(), ids.end());
  }
  step.done = true;
  Edited();
  Map();
  selection_ = children;
  return true;
#else
  return Editable();
#endif
}

bool ObjectTool::Turn(float degrees) {
  Map();
  std::vector<Moving> moved;
  for (uint32_t id : selection_)
    if (const ob::ObjectInfo* o = map_.Find(id); o && !o->line && o->kind != ob::ObjectKind::GraphicFrame) {
      Moving m{id, o->frame, o->frame};
      m.now.rotation = ob::NormalizeDegrees(o->frame.rotation + degrees);
      moved.push_back(m);
    }
  return CommitFrames(moved, u"Rotate");
}

bool ObjectTool::Flip(bool horizontal) {
  Map();
  std::vector<Moving> moved;
  for (uint32_t id : selection_)
    if (const ob::ObjectInfo* o = map_.Find(id); o && o->kind != ob::ObjectKind::GraphicFrame) {
      Moving m{id, o->frame, o->frame};
      // Flipping a turned object mirrors its turn too, as PowerPoint does.
      if (horizontal) m.now.flipH = !m.now.flipH;
      else m.now.flipV = !m.now.flipV;
      m.now.rotation = ob::NormalizeDegrees(-o->frame.rotation);
      moved.push_back(m);
    }
  return CommitFrames(moved, horizontal ? u"Flip Horizontal" : u"Flip Vertical");
}

bool ObjectTool::Copy(bool cut) {
#if FW_SLIDE_EDIT
  if (selection_.empty()) return false;
  Deck* deck = host_.deck();
  fw::slide::edit::Editor* ed = deck ? deck->Editor() : nullptr;
  if (!ed) return false;
  // In the slide's order, bottom to top, so pasting keeps it.
  std::vector<uint32_t> ids;
  for (const ob::ObjectInfo& o : Map().Objects())
    if (std::find(selection_.begin(), selection_.end(), o.id) != selection_.end()) ids.push_back(o.id);
  const Clip clip = ed->CopyShapes(host_.current(), ids);
  if (clip.Empty()) return false;
  std::string data = Encode(clip);
  if (util::UsingPrivateClipboard()) {
    util::SetClipboardText(std::wstring());  // replaces whatever was copied before, as EmptyClipboard does
    Private() = {util::ClipboardSequence(), std::move(data)};
    if (cut) DeleteSelection();
    return true;
  }
  if (!OpenClipboard(host_.window ? host_.window() : nullptr)) return false;
  EmptyClipboard();
  if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, data.size())) {
    if (void* p = GlobalLock(h)) {
      std::memcpy(p, data.data(), data.size());
      GlobalUnlock(h);
      if (!SetClipboardData(ObjectsFormat(), h)) GlobalFree(h);
    } else {
      GlobalFree(h);
    }
  }
  CloseClipboard();
  if (cut) DeleteSelection();
  return true;
#else
  (void)cut;
  return false;
#endif
}

bool ObjectTool::Paste() {
#if FW_SLIDE_EDIT
  if (!Editable()) return false;
  Deck* deck = host_.deck();
  fw::slide::edit::Editor* ed = deck->Editor();
  const size_t slide = host_.current();
  const draw::Size size = deck->SlideSize();
  // Objects copied in Featherweight (any slide, any deck): the editor pastes them with their parts.
  std::vector<uint8_t> own;
  if (!util::UsingPrivateClipboard()) own = ClipboardBytes(ObjectsFormat());
  else if (Private().sequence == util::ClipboardSequence()) own.assign(Private().bytes.begin(), Private().bytes.end());
  Clip clip;
  if (!own.empty() && Decode(std::string_view(reinterpret_cast<const char*>(own.data()), own.size()), clip)) {
    std::vector<uint32_t> pasted;
    const fw::slide::edit::Result r = ed->PasteShapes(slide, clip, &pasted);
    if (!r) {
      Refused(W(r.message));
      return false;
    }
    Edited();
    Map();
    selection_ = pasted;
    level_ = 0;
    return true;
  }
  // Pictures: files, a PNG, or a bitmap.
  if (!util::UsingPrivateClipboard() && IsClipboardFormatAvailable(CF_HDROP) && OpenClipboard(nullptr)) {
    std::vector<std::wstring> files;
    if (HDROP drop = static_cast<HDROP>(GetClipboardData(CF_HDROP))) {
      const UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
      for (UINT i = 0; i < n && i < 64; ++i) {
        wchar_t path[MAX_PATH];
        if (DragQueryFileW(drop, i, path, MAX_PATH) && IsPictureName(path)) files.push_back(path);
      }
    }
    CloseClipboard();
    if (!files.empty()) return InsertPictures(files, {size.width / 2, size.height / 2}, true);
  }
  static const UINT png = RegisterClipboardFormatW(L"PNG");
  std::vector<uint8_t> bytes = ClipboardBytes(png);
  if (bytes.empty()) bytes = DibToBmp(ClipboardBytes(CF_DIB));
  if (!bytes.empty()) return InsertPictureBytes(std::move(bytes), {size.width / 2, size.height / 2}, true);
  // Text: a new text box with it.
  if (const std::wstring t = util::GetClipboardText(); !t.empty()) {
    uint32_t id = 0;
    const draw::Rect r{size.width / 2 - 180, size.height / 2 - 20, size.width / 2 + 180, size.height / 2 + 20};
    const fw::slide::edit::Result res = ed->InsertTextBox(slide, r, t, &id);
    if (!res) {
      Refused(W(res.message));
      return false;
    }
    Edited();
    Map();
    selection_ = {id};
    return true;
  }
  return false;
#else
  return Editable();
#endif
}

bool ObjectTool::DuplicateSelection(float dx, float dy) {
#if FW_SLIDE_EDIT
  if (selection_.empty() || !Editable()) return false;
  Map();
  fw::slide::edit::Editor* ed = host_.deck()->Editor();
  const size_t slide = host_.current();
  fw::slide::edit::EditGroup group(*ed, u"Duplicate");
  std::vector<uint32_t> made;
  for (const ob::ObjectInfo& o : map_.Objects()) {
    if (std::find(selection_.begin(), selection_.end(), o.id) == selection_.end()) continue;
    pugi::xml_document doc;
    const std::string xml = ed->ShapeXml(slide, o.id);
    if (!doc.load_string(xml.c_str())) continue;
    pugi::xml_node el = doc.document_element();
    const draw::Point d = ob::DeltaInParent({dx, dy}, o.parentToSlide);
    ob::Xfrm x;
    if (!ob::ReadXfrm(el, x)) x = ob::FromFrame(o.local, x);
    x.x += ob::ToEmu(d.x);
    x.y += ob::ToEmu(d.y);
    ob::WriteXfrm(el, x);
    uint32_t id = 0;
    const fw::slide::edit::Result r = ed->InsertShape(slide, ob::ToXml(el), &id, -1);
    if (!r) {
      Refused(W(r.message));
      return false;
    }
    made.push_back(id);
  }
  group.done = true;
  Edited();
  Map();
  selection_ = made;
  level_ = 0;
  return true;
#else
  (void)dx;
  (void)dy;
  return Editable();
#endif
}

bool ObjectTool::InsertPictures(const std::vector<std::wstring>& files, draw::Point at, bool centred) {
  bool any = false;
  for (const std::wstring& f : files) {
    std::vector<uint8_t> bytes = ReadFileBytes(f);
    if (bytes.empty()) {
      Refused(L"Featherweight couldn't read " + f + L".");
      continue;
    }
    any = InsertPictureBytes(std::move(bytes), at, centred) || any;
    at.x += 18, at.y += 18;  // several: each a little further
  }
  return any;
}

bool ObjectTool::InsertPictureBytes(std::vector<uint8_t> bytes, draw::Point at, bool centred) {
#if FW_SLIDE_EDIT
  if (!Editable()) return false;
  Deck* deck = host_.deck();
  std::string error;
  std::shared_ptr<draw::Image> image = draw::Image::Open(bytes, draw::ImageLimits::Default(), &error);
  if (!image) {
    Refused(L"That isn't a picture Featherweight can insert.");
    return false;
  }
  const draw::Size slide = deck->SlideSize();
  draw::Rect r = ob::PlacePicture(image->NaturalSize(), slide);
  if (!centred) r = ob::Offset(r, at.x - (r.left + r.right) / 2, at.y - (r.top + r.bottom) / 2);
  uint32_t id = 0;
  const fw::slide::edit::Result res = deck->Editor()->InsertPicture(host_.current(), std::move(bytes), r, L"", &id);
  if (!res) {
    Refused(W(res.message));
    return false;
  }
  Edited();
  Map();
  selection_ = {id};
  level_ = 0;
  return true;
#else
  (void)bytes;
  (void)at;
  (void)centred;
  return Editable();
#endif
}

bool ObjectTool::InsertShape(int index) {
  const auto& gallery = ob::ShapeGallery();
  Deck* deck = host_.deck();
  if (index < 0 || index >= static_cast<int>(gallery.size()) || !deck) return false;
  const draw::Size s = deck->SlideSize();
  const std::string preset = gallery[static_cast<size_t>(index)].preset;
  // As PowerPoint without a drag: 2 x 1⅓ inches centred; a line 2 inches across.
  const draw::Rect r = ob::IsLinePreset(preset)
                           ? draw::Rect{s.width / 2 - 72, s.height / 2, s.width / 2 + 72, s.height / 2}
                           : draw::Rect{s.width / 2 - 72, s.height / 2 - 48, s.width / 2 + 72, s.height / 2 + 48};
  return InsertPreset(preset, r);
}

bool ObjectTool::InsertPreset(const std::string& preset, const draw::Rect& box) {
#if FW_SLIDE_EDIT
  if (!Editable()) return false;
  fw::slide::edit::Editor* ed = host_.deck()->Editor();
  uint32_t id = 0;
  fw::slide::edit::Result r;
  if (ob::IsLinePreset(preset)) {
    // A line: a connector, as PowerPoint draws it (the editor gives it an id of its own).
    const std::string xml = ob::NewObjectXml([&](pugi::xml_node tree) {
      ob::Xfrm x;
      x.x = ob::ToEmu(box.left), x.y = ob::ToEmu(box.top), x.cx = ob::ToEmu(box.Width()), x.cy = ob::ToEmu(box.Height());
      return ob::AppendLine(tree, 2, x, preset == "straightConnector1");
    });
    r = ed->InsertShape(host_.current(), xml, &id, -1);
  } else {
    r = ed->InsertAutoShape(host_.current(), preset, box, &id);
  }
  if (!r) {
    Refused(W(r.message));
    return false;
  }
  Edited();
  Map();
  selection_ = {id};
  level_ = 0;
  return true;
#else
  (void)preset;
  (void)box;
  return Editable();
#endif
}

bool ObjectTool::InsertTextBox() {
  Deck* deck = host_.deck();
  if (!deck) return false;
  const draw::Size s = deck->SlideSize();
  return InsertTextBox(draw::Rect{s.width / 2 - 120, s.height / 2 - 18, s.width / 2 + 120, s.height / 2 + 18});
}

bool ObjectTool::InsertTextBox(const draw::Rect& r) {
#if FW_SLIDE_EDIT
  if (!Editable()) return false;
  uint32_t id = 0;
  const fw::slide::edit::Result res = host_.deck()->Editor()->InsertTextBox(host_.current(), r, L"", &id);
  if (!res) {
    Refused(W(res.message));
    return false;
  }
  Edited();
  Map();
  selection_ = {id};
  level_ = 0;
  if (host_.beginText) host_.beginText(id);  // the caret in it, as PowerPoint does
  return true;
#else
  (void)r;
  return Editable();
#endif
}

bool ObjectTool::InsertTable(int rows, int columns) {
#if FW_SLIDE_EDIT
  if (!Editable()) return false;
  Deck* deck = host_.deck();
  const draw::Size s = deck->SlideSize();
  // As PowerPoint: 640 points wide on a wide slide (less on a narrow one), rows 29.2 points.
  const float w = std::min(640.f, s.width - 96), h = 29.2f * static_cast<float>(rows);
  const draw::Rect r{(s.width - w) / 2, (s.height - h) / 2, (s.width + w) / 2, (s.height + h) / 2};
  uint32_t id = 0;
  const fw::slide::edit::Result res = deck->Editor()->InsertTable(host_.current(), rows, columns, r, &id);
  if (!res) {
    Refused(W(res.message));
    return false;
  }
  Edited();
  Map();
  selection_ = {id};
  level_ = 0;
  if (host_.beginText) host_.beginText(id);  // the caret in the first cell
  return true;
#else
  (void)rows;
  (void)columns;
  return Editable();
#endif
}

bool ObjectTool::InsertChart(int type) {
#if FW_SLIDE_EDIT
  if (!Editable()) return false;
  Deck* deck = host_.deck();
  fw::slide::edit::Editor* ed = deck->Editor();
  const size_t slide = host_.current();
  const draw::Size s = deck->SlideSize();
  const ob::ChartData data = ob::DefaultChartData();
  const ob::ChartType kind = static_cast<ob::ChartType>(std::clamp(type, 0, 3));
  fw::slide::edit::EditGroup group(*ed, u"Insert Chart");
  const std::string chartPart = ed->NewPartName("/ppt/charts/chart", ".xml");
  std::string chartXml = ob::ChartXml(kind, data, "rId1");
  const std::string relId = ed->AddPart(ed->SlidePart(slide), ob::kChartRelType, chartPart, ob::kChartContentType,
                                        std::vector<uint8_t>(chartXml.begin(), chartXml.end()));
  if (relId.empty()) {
    Refused(L"The chart couldn't be added to this slide.");
    return false;
  }
  const std::string wbPart = ed->NewPartName("/ppt/embeddings/Microsoft_Excel_Worksheet", ".xlsx");
  const std::string wbId = ed->AddPart(chartPart, ob::kPackageRelType, wbPart, ob::kXlsxContentType, ob::ChartWorkbook(data));
  if (wbId.empty()) {
    Refused(L"The chart's data couldn't be added.");
    return false;
  }
  if (wbId != "rId1") {
    chartXml = ob::ChartXml(kind, data, wbId);
    ed->WritePart(chartPart, std::vector<uint8_t>(chartXml.begin(), chartXml.end()));
  }
  // As PowerPoint: 640 x 360 points on a wide slide, centred.
  const float w = std::min(640.f, s.width - 96), h = std::min(360.f, s.height - 96);
  const std::string frame = ob::NewObjectXml([&](pugi::xml_node tree) {
    ob::Xfrm x;
    x.x = ob::ToEmu((s.width - w) / 2), x.y = ob::ToEmu((s.height - h) / 2), x.cx = ob::ToEmu(w), x.cy = ob::ToEmu(h);
    return ob::AppendChart(tree, 2, relId, x);
  });
  uint32_t id = 0;
  const fw::slide::edit::Result r = ed->InsertShape(slide, frame, &id, -1);
  if (!r) {
    Refused(W(r.message));
    return false;
  }
  group.done = true;
  Edited();
  Map();
  selection_ = {id};
  level_ = 0;
  return true;
#else
  (void)type;
  return Editable();
#endif
}

uint32_t ObjectTool::SelectedTable() const {
  const ob::ObjectInfo* o = SelectedShape() ? map_.Find(SelectedShape()) : nullptr;
  return o && o->shape && o->shape->kind == fw::slide::Shape::Kind::Table ? o->id : 0;
}

bool ObjectTool::TableEdit(int cmd, const int* cells) {
#if FW_SLIDE_EDIT
  Map();
  const uint32_t id = SelectedTable();
  if (!id || !Editable()) return false;
  fw::slide::edit::Editor* ed = host_.deck()->Editor();
  const size_t slide = host_.current();
  // The cells chosen (text editing's caret cell or range, or a script's); without any, rows and
  // columns go at the table's ends and come off them, as before a cell is chosen in PowerPoint.
  int r0 = -1, c0 = -1, r1 = -1, c1 = -1;
  if (cells) r0 = cells[0], c0 = cells[1], r1 = cells[2], c1 = cells[3];
  else if (!host_.tableCells || !host_.tableCells(id, r0, c0, r1, c1)) r0 = -1;
  if (r0 >= 0) {
    if (r0 > r1) std::swap(r0, r1);
    if (c0 > c1) std::swap(c0, c1);
  } else if (cmd != CmdTableMergeCells && cmd != CmdTableSplitCell) {
    pugi::xml_document doc;
    if (!doc.load_string(ed->ShapeXml(slide, id).c_str())) return false;
    const int rows = ob::TableRows(doc.document_element()), cols = ob::TableColumns(doc.document_element());
    const bool start = cmd == CmdTableRowAbove || cmd == CmdTableColumnLeft;
    r0 = r1 = start ? 0 : rows - 1;
    c0 = c1 = start ? 0 : cols - 1;
  }
  fw::slide::edit::Result r;
  switch (cmd) {
    case CmdTableRowAbove: r = ed->InsertTableRows(slide, id, r0, 1); break;
    case CmdTableRowBelow: r = ed->InsertTableRows(slide, id, r1 + 1, 1); break;
    case CmdTableColumnLeft: r = ed->InsertTableColumns(slide, id, c0, 1); break;
    case CmdTableColumnRight: r = ed->InsertTableColumns(slide, id, c1 + 1, 1); break;
    case CmdTableDeleteRow: r = ed->DeleteTableRows(slide, id, r0, r1 - r0 + 1); break;
    case CmdTableDeleteColumn: r = ed->DeleteTableColumns(slide, id, c0, c1 - c0 + 1); break;
    case CmdTableMergeCells:
      if (r0 < 0 || (r0 == r1 && c0 == c1)) {
        Refused(L"Choose two or more cells to merge first.");
        return false;
      }
      r = ed->MergeTableCells(slide, id, r0, c0, r1, c1);
      break;
    case CmdTableSplitCell:
      if (r0 < 0) {
        Refused(L"Click in the merged cell to split first.");
        return false;
      }
      r = ed->SplitTableCell(slide, id, r0, c0);
      break;
    default: return false;
  }
  if (!r) {
    Refused(W(r.message));
    return false;
  }
  Edited();
  return true;
#else
  (void)cmd;
  (void)cells;
  return Editable();
#endif
}

namespace {

// Changes each selected object's element with `change` (one of ObjectXml's), as one step.
template <class F>
bool Restyle(Deck* deck, size_t slide, const std::vector<uint32_t>& ids, const std::u16string& step, F&& change,
             std::wstring& why) {
#if FW_SLIDE_EDIT
  fw::slide::edit::Editor* ed = deck ? deck->Editor() : nullptr;
  if (!ed) return false;
  fw::slide::edit::EditGroup group(*ed, step);
  bool any = false;
  for (uint32_t id : ids) {
    pugi::xml_document doc;
    const std::string xml = ed->ShapeXml(slide, id);
    if (!doc.load_string(xml.c_str())) continue;
    pugi::xml_node el = doc.document_element();
    // A group: its children are changed instead.
    std::vector<pugi::xml_node> targets;
    if (ob::KindOf(el) == ob::ObjectKind::Group) {
      std::vector<pugi::xml_node> stack{el};
      while (!stack.empty()) {
        pugi::xml_node n = stack.back();
        stack.pop_back();
        for (pugi::xml_node c = n.first_child(); c; c = c.next_sibling()) {
          const ob::ObjectKind k = ob::KindOf(c);
          if (k == ob::ObjectKind::Group) stack.push_back(c);
          else if (k != ob::ObjectKind::Other) targets.push_back(c);
        }
      }
    } else {
      targets.push_back(el);
    }
    bool changed = false;
    for (pugi::xml_node t : targets) changed = change(t) || changed;
    if (!changed) continue;
    const fw::slide::edit::Result r = ed->SetShapeXml(slide, id, ob::ToXml(el), step);
    if (!r) {
      why = W(r.message);
      return false;
    }
    any = true;
  }
  group.done = any;
  return any;
#else
  (void)deck, (void)slide, (void)ids, (void)step, (void)change, (void)why;
  return false;
#endif
}

}  // namespace

bool ObjectTool::SetFillColor(std::optional<uint32_t> rgb, bool none) {
  if (selection_.empty() || !Editable()) return false;
  ob::Paint p;
  p.kind = none ? ob::Paint::Kind::None : rgb ? ob::Paint::Kind::Solid : ob::Paint::Kind::Automatic;
  p.rgb = rgb.value_or(0);
  std::wstring why;
  const bool ok = Restyle(host_.deck(), host_.current(), selection_, u"Shape Fill",
                          [&](pugi::xml_node n) { return ob::SetFill(n, p); }, why);
  if (!ok && !why.empty()) Refused(why);
  if (ok) Edited();
  return ok;
}

bool ObjectTool::SetLineColor(std::optional<uint32_t> rgb, bool none) {
  if (selection_.empty() || !Editable()) return false;
  ob::Paint p;
  p.kind = none ? ob::Paint::Kind::None : rgb ? ob::Paint::Kind::Solid : ob::Paint::Kind::Automatic;
  p.rgb = rgb.value_or(0);
  std::wstring why;
  const bool ok = Restyle(host_.deck(), host_.current(), selection_, u"Shape Outline",
                          [&](pugi::xml_node n) { return ob::SetLine(n, p); }, why);
  if (!ok && !why.empty()) Refused(why);
  if (ok) Edited();
  return ok;
}

bool ObjectTool::SetLineWidth(float points) {
  if (selection_.empty() || !Editable()) return false;
  std::wstring why;
  const bool ok = Restyle(host_.deck(), host_.current(), selection_, u"Shape Outline",
                          [&](pugi::xml_node n) { return ob::SetLineWidth(n, points); }, why);
  if (!ok && !why.empty()) Refused(why);
  if (ok) Edited();
  return ok;
}

bool ObjectTool::SetShadow(bool on) {
  if (selection_.empty() || !Editable()) return false;
  std::wstring why;
  const bool ok = Restyle(host_.deck(), host_.current(), selection_, u"Shadow",
                          [&](pugi::xml_node n) { return ob::SetShadow(n, on); }, why);
  if (!ok && !why.empty()) Refused(why);
  if (ok) Edited();
  return ok;
}

bool ObjectTool::ToggleCrop() {
  const ob::ObjectInfo* o = SelectedShape() ? Map().Find(SelectedShape()) : nullptr;
  if (!o || o->kind != ob::ObjectKind::Picture) return false;
  cropMode_ = !cropMode_;
  host_.invalidate();
  return true;
}

// ---------------------------------------------------------------------------
// Pictures dropped

bool ObjectTool::TakesPictures(const std::vector<std::wstring>& files) const {
  if (files.empty() || !Editable(false)) return false;
  for (const std::wstring& f : files)
    if (!IsPictureName(f)) return false;
  return true;
}

void ObjectTool::DropPictures(const std::vector<std::wstring>& files, float x, float y) {
  Deck* deck = host_.deck();
  if (!deck) return;
  const bool centred = x < 0 || y < 0;
  InsertPictures(files, centred ? draw::Point{} : ToSlide(x, y), centred);
}

// ---------------------------------------------------------------------------
// Commands

std::vector<int> ObjectTool::Commands() const {
  std::vector<int> out;
  for (const ArrangeItem& a : kArrange) out.push_back(a.cmd);
  for (const NamedCommand& n : kOther)
    if (n.cmd != CmdArrangeMenu && n.cmd != CmdInsertShape) out.push_back(n.cmd);
  return out;
}

bool ObjectTool::Query(int cmd, CommandState& s) {
  Map();
  const bool editable = Editable(false);
  const bool any = !selection_.empty() && !textEditing_;
  size_t groups = 0, pictures = 0, tables = 0, turnable = 0;
  for (uint32_t id : selection_)
    if (const ob::ObjectInfo* o = map_.Find(id)) {
      groups += o->kind == ob::ObjectKind::Group;
      pictures += o->kind == ob::ObjectKind::Picture;
      tables += o->shape && o->shape->kind == fw::slide::Shape::Kind::Table;
      turnable += !o->line && o->kind != ob::ObjectKind::GraphicFrame;
    }
  bool mine = true;
  switch (cmd) {
    case CmdGroupObjects: s.enabled = editable && any && selection_.size() >= 2; break;
    case CmdUngroupObjects: s.enabled = editable && any && groups > 0; break;
    case CmdBringToFront:
    case CmdBringForward:
    case CmdSendBackward:
    case CmdSendToBack:
    case CmdDuplicateObjects:
    case CmdObjectFill:
    case CmdObjectOutline:
    case CmdObjectOutlineWeight:
    case CmdObjectShadow: s.enabled = editable && any; break;
    case CmdObjAlignLeft:
    case CmdObjAlignCenter:
    case CmdObjAlignRight:
    case CmdObjAlignTop:
    case CmdObjAlignMiddle:
    case CmdObjAlignBottom: s.enabled = editable && any; break;
    case CmdDistributeHorizontally:
    case CmdDistributeVertically: s.enabled = editable && any && (selection_.size() >= 3 || alignToSlide_); break;
    case CmdAlignToSlide:
      s.enabled = editable;
      s.checked = alignToSlide_;
      break;
    case CmdRotateRight90:
    case CmdRotateLeft90:
    case CmdFlipHorizontal:
    case CmdFlipVertical: s.enabled = editable && any && turnable > 0; break;
    case CmdPictureCrop:
      s.enabled = editable && any && selection_.size() == 1 && pictures == 1;
      s.checked = cropMode_;
      break;
    case CmdTableRowAbove:
    case CmdTableRowBelow:
    case CmdTableColumnLeft:
    case CmdTableColumnRight:
    case CmdTableDeleteRow:
    case CmdTableDeleteColumn:
    case CmdTableLayoutMenu: s.enabled = editable && selection_.size() == 1 && tables == 1; break;
    case CmdTableMergeCells:
    case CmdTableSplitCell: {
      // Text editing's chosen cells: split needs one, merge two or more.
      int r0 = -1, c0 = -1, r1 = -1, c1 = -1;
      const bool cells = editable && selection_.size() == 1 && tables == 1 && host_.tableCells &&
                         host_.tableCells(selection_[0], r0, c0, r1, c1);
      s.enabled = cells && (cmd == CmdTableSplitCell || r0 != r1 || c0 != c1);
      break;
    }
    case CmdArrangeMenu:
    case CmdInsertShape:
    case CmdInsertTextBox:
    case CmdInsertPicture:
    case CmdInsertChart:
    case CmdChartColumn:
    case CmdChartBar:
    case CmdChartLine:
    case CmdChartPie:
    case CmdInsertRectangle:
    case CmdInsertRoundedRectangle:
    case CmdInsertOval:
    case CmdInsertLine:
    case CmdInsertArrow:
    case CmdInsertCallout: s.enabled = editable && !textEditing_; break;
    case CmdDeleteObject:
      s.enabled = editable && any;
      s.label = L"Delete";
      break;
    case CmdCut:
    case CmdCopy:
      if (!any) return false;  // the slide's own copy (P1), or text editing's
      s.enabled = cmd == CmdCopy || editable;
      break;
    case CmdPaste:
      if (textEditing_) return false;
      s.enabled = editable;
      break;
    case CmdSelectAll:
      if (textEditing_) return false;
      s.enabled = !map_.Children(level_).empty();
      break;
#if FW_SLIDE_EDIT
    case CmdUndo:
    case CmdRedo: {
      if (!editable) return false;
      fw::slide::edit::Editor* ed = host_.deck()->Editor();
      s.enabled = cmd == CmdUndo ? ed->CanUndo() : ed->CanRedo();
      const std::u16string name = cmd == CmdUndo ? ed->UndoName() : ed->RedoName();
      s.label = (cmd == CmdUndo ? L"Undo " : L"Redo ") + W(name);
      return true;
    }
#endif
    default: mine = false;
  }
  if (!mine) return false;
  for (const ArrangeItem& a : kArrange)
    if (a.cmd == cmd) s.label = a.label, s.shortcut = a.keys;
  for (const NamedCommand& n : kOther)
    if (n.cmd == cmd) s.label = n.label, s.shortcut = n.keys;
  return true;
}

bool ObjectTool::Execute(int cmd) {
  Map();
  switch (cmd) {
    case CmdGroupObjects: return GroupSelection();
    case CmdUngroupObjects: return UngroupSelection();
    case CmdBringToFront:
    case CmdBringForward:
    case CmdSendBackward:
    case CmdSendToBack: return Arrange(cmd);
    case CmdObjAlignLeft:
    case CmdObjAlignCenter:
    case CmdObjAlignRight:
    case CmdObjAlignTop:
    case CmdObjAlignMiddle:
    case CmdObjAlignBottom:
    case CmdDistributeHorizontally:
    case CmdDistributeVertically: return AlignOrDistribute(cmd);
    case CmdAlignToSlide:
      alignToSlide_ = !alignToSlide_;
      return true;
    case CmdRotateRight90: return Turn(90);
    case CmdRotateLeft90: return Turn(-90);
    case CmdFlipHorizontal: return Flip(true);
    case CmdFlipVertical: return Flip(false);
    case CmdPictureCrop: return ToggleCrop();
    case CmdObjectShadow: return SetShadow(true);
    case CmdDuplicateObjects: return DuplicateSelection(18, 18);
    case CmdDeleteObject: return DeleteSelection();
    case CmdTableRowAbove:
    case CmdTableRowBelow:
    case CmdTableColumnLeft:
    case CmdTableColumnRight:
    case CmdTableDeleteRow:
    case CmdTableDeleteColumn:
    case CmdTableMergeCells:
    case CmdTableSplitCell: return TableEdit(cmd);
    case CmdInsertTextBox: return InsertTextBox();
    case CmdInsertShape:
    case CmdInsertRectangle: return InsertShape(2);
    case CmdInsertRoundedRectangle: return InsertShape(3);
    case CmdInsertOval: return InsertShape(5);
    case CmdInsertLine: return InsertShape(0);
    case CmdInsertArrow: return InsertShape(1);
    case CmdInsertCallout: return InsertShape(static_cast<int>(ob::ShapeGallery().size()) - 3);
    case CmdInsertChart:
    case CmdChartColumn: return InsertChart(0);
    case CmdChartBar: return InsertChart(1);
    case CmdChartLine: return InsertChart(2);
    case CmdChartPie: return InsertChart(3);
    case CmdInsertPicture: {
      if (!Editable()) return false;
      const std::vector<std::wstring> files = PickPictureFiles(host_.window ? host_.window() : nullptr, true);
      if (files.empty()) return true;
      const draw::Size s = host_.deck()->SlideSize();
      return InsertPictures(files, {s.width / 2, s.height / 2}, true);
    }
    case CmdCut:
    case CmdCopy:
      if (selection_.empty() || textEditing_) return false;
      return Copy(cmd == CmdCut);
    case CmdPaste:
      if (textEditing_) return false;
      return Paste();
#if FW_SLIDE_EDIT
    case CmdUndo:
    case CmdRedo: {
      if (!Editable(false)) return false;
      fw::slide::edit::Editor* ed = host_.deck()->Editor();
      const fw::slide::edit::Result r = cmd == CmdUndo ? ed->Undo() : ed->Redo();
      if (!r) Refused(W(r.message));
      Edited();
      return true;
    }
#endif
    case CmdSelectAll:
      if (textEditing_) return false;
      selection_.clear();
      for (const ob::ObjectInfo* o : map_.Children(level_)) selection_.push_back(o->id);
      host_.invalidate();
      return true;
    default: return false;
  }
}

void ObjectTool::AddTools(ToolbarModel& m) {
  if (!Editable(false)) return;
  Map();
  auto button = [](int cmd, wchar_t glyph, const wchar_t* tip, int priority) {
    ToolItem t;
    t.cmd = cmd;
    t.glyph = glyph;
    t.tooltip = tip;
    t.priority = priority;
    return t;
  };
  ToolItem picture = button(CmdInsertPicture, icon::Picture, L"Insert pictures", 7);
  picture.sepBefore = true;
  m.left.push_back(picture);
  ToolItem shapes;
  shapes.kind = ToolKind::Dropdown;
  shapes.cmd = CmdInsertShape;
  shapes.text = L"Shapes";
  shapes.fitText = true;
  shapes.width = 70;
  shapes.tooltip = L"Insert a shape";
  shapes.priority = 6;
  shapes.maxRows = 16;
  for (const ob::ShapePreset& p : ob::ShapeGallery()) shapes.choices.push_back(fw::Utf8ToWide(p.name));
  shapes.onChange = [this](const ToolValue& v) { InsertShape(v.index); };
  m.left.push_back(shapes);
  m.left.push_back(button(CmdInsertTextBox, icon::FontCase, L"Insert a text box", 5));
  ToolItem table;
  table.kind = ToolKind::Dropdown;
  table.cmd = CmdInsertTable;
  table.glyph = icon::Table;
  table.text = L"Table";
  table.fitText = true;
  table.width = 64;
  table.tooltip = L"Insert a table (columns × rows)";
  table.priority = 5;
  for (const TableSize& t : kTables) table.choices.push_back(std::to_wstring(t.columns) + L" × " + std::to_wstring(t.rows));
  table.onChange = [this](const ToolValue& v) {
    if (v.index >= 0 && v.index < static_cast<int>(std::size(kTables))) InsertTable(kTables[v.index].rows, kTables[v.index].columns);
  };
  m.left.push_back(table);
  ToolItem chart;
  chart.kind = ToolKind::Dropdown;
  chart.cmd = CmdInsertChart;
  chart.glyph = icon::Report;
  chart.text = L"Chart";
  chart.fitText = true;
  chart.width = 64;
  chart.tooltip = L"Insert a chart";
  chart.priority = 5;
  for (const wchar_t* n : kChartNames) chart.choices.push_back(n);
  chart.onChange = [this](const ToolValue& v) {
    if (v.index >= 0 && v.index < 4) InsertChart(v.index);
  };
  m.left.push_back(chart);
  // A table selected, or a cell of it being edited: its rows, columns and cells (PowerPoint's
  // Layout tab), at the cells chosen.
  if (Map(), SelectedTable()) {
    ToolItem layout;
    layout.kind = ToolKind::Dropdown;
    layout.cmd = CmdTableLayoutMenu;
    layout.glyph = icon::Table;
    layout.text = L"Table layout";
    layout.fitText = true;
    layout.width = 96;
    layout.tooltip = L"Insert and delete rows and columns, merge and split cells";
    layout.priority = 6;
    layout.sepBefore = true;
    for (int cmd : kTableCmds) {
      CommandState s;
      Query(cmd, s);
      layout.choices.push_back(s.label);
    }
    layout.onChange = [this](const ToolValue& v) {
      if (v.index >= 0 && v.index < static_cast<int>(std::size(kTableCmds))) Execute(kTableCmds[v.index]);
    };
    m.left.push_back(layout);
  }
  if (selection_.empty() || textEditing_) return;
  // The selection's tools.
  ToolItem arrange;
  arrange.kind = ToolKind::Dropdown;
  arrange.cmd = CmdArrangeMenu;
  arrange.text = L"Arrange";
  arrange.fitText = true;
  arrange.width = 72;
  arrange.tooltip = L"Order, group, align and turn the selected objects";
  arrange.priority = 7;
  arrange.sepBefore = true;
  arrange.maxRows = 20;
  for (const ArrangeItem& a : kArrange) {
    CommandState s;
    Query(a.cmd, s);
    std::wstring label = (a.cmd == CmdAlignToSlide && alignToSlide_ ? L"✓ " : L"") + std::wstring(a.label);
    if (*a.keys) label += std::wstring(L"\t") + a.keys;
    arrange.choices.push_back(label);
  }
  arrange.onChange = [this](const ToolValue& v) {
    if (v.index >= 0 && v.index < static_cast<int>(std::size(kArrange))) Execute(kArrange[v.index].cmd);
  };
  m.left.push_back(arrange);
  ToolItem fill;
  fill.kind = ToolKind::ColorPicker;
  fill.cmd = CmdObjectFill;
  fill.glyph = icon::Color;
  fill.tooltip = L"Shape fill";
  fill.color = Rgb(0x4472C4);  // Office's accent 1
  fill.automaticLabel = L"No fill";
  fill.automaticColor = D2D1::ColorF(0, 0, 0, 0);
  fill.priority = 6;
  fill.onChange = [this](const ToolValue& v) {
    if (v.automatic) SetFillColor(std::nullopt, true);
    else SetFillColor((static_cast<uint32_t>(std::lround(v.color.r * 255)) << 16) | (static_cast<uint32_t>(std::lround(v.color.g * 255)) << 8) |
                          static_cast<uint32_t>(std::lround(v.color.b * 255)), false);
  };
  m.left.push_back(fill);
  ToolItem outline = fill;
  outline.cmd = CmdObjectOutline;
  outline.glyph = icon::Brush;
  outline.tooltip = L"Shape outline";
  outline.automaticLabel = L"No outline";
  outline.onChange = [this](const ToolValue& v) {
    if (v.automatic) SetLineColor(std::nullopt, true);
    else SetLineColor((static_cast<uint32_t>(std::lround(v.color.r * 255)) << 16) | (static_cast<uint32_t>(std::lround(v.color.g * 255)) << 8) |
                          static_cast<uint32_t>(std::lround(v.color.b * 255)), false);
  };
  m.left.push_back(outline);
  ToolItem weight;
  weight.kind = ToolKind::Dropdown;
  weight.cmd = CmdObjectOutlineWeight;
  weight.text = L"Weight";
  weight.fitText = true;
  weight.width = 64;
  weight.tooltip = L"Outline weight and dashes, shadow";
  weight.priority = 4;
  for (const wchar_t* n : kWeightNames) weight.choices.push_back(n);
  for (const Dash& d : kDashes) weight.choices.push_back(std::wstring(L"Dashes: ") + d.name);
  weight.choices.push_back(L"Shadow");
  weight.choices.push_back(L"No shadow");
  weight.onChange = [this](const ToolValue& v) {
    const int nw = static_cast<int>(std::size(kWeights)), nd = static_cast<int>(std::size(kDashes));
    if (v.index < 0) return;
    if (v.index < nw) {
      SetLineWidth(kWeights[v.index]);
    } else if (v.index < nw + nd) {
      const char* preset = kDashes[v.index - nw].preset;
      std::wstring why;
      const bool ok = Restyle(host_.deck(), host_.current(), selection_, u"Shape Outline",
                              [&](pugi::xml_node n) { return ob::SetLineDash(n, preset); }, why);
      if (!ok && !why.empty()) Refused(why);
      if (ok) Edited();
    } else {
      SetShadow(v.index == nw + nd);
    }
  };
  m.left.push_back(weight);
  size_t pictures = 0;
  for (uint32_t id : selection_)
    if (const ob::ObjectInfo* o = map_.Find(id)) pictures += o->kind == ob::ObjectKind::Picture;
  if (selection_.size() == 1 && pictures == 1) {
    ToolItem crop = button(CmdPictureCrop, icon::Crop, L"Crop the picture (drag its edges)", 6);
    crop.toggle = true;
    crop.checked = cropMode_;
    m.left.push_back(crop);
  }
}

namespace {

// "x,y,w,h" in points.
bool ParseBox(const std::wstring& text, draw::Rect& r) {
  float v[4];
  size_t at = 0;
  for (int i = 0; i < 4; ++i) {
    const size_t end = text.find(L',', at);
    if ((end == std::wstring::npos) != (i == 3)) return false;
    const std::wstring part = text.substr(at, end == std::wstring::npos ? std::wstring::npos : end - at);
    wchar_t* stop = nullptr;
    v[i] = std::wcstof(part.c_str(), &stop);
    if (part.empty() || (stop && *stop)) return false;
    at = end + 1;
  }
  r = {v[0], v[1], v[0] + std::max(0.f, v[2]), v[1] + std::max(0.f, v[3])};
  return true;
}

}  // namespace

bool ObjectTool::ScriptOp(const std::wstring& op, const std::wstring& arg) {
  if (op != L"insert" && op != L"shapefill" && op != L"tablemerge" && op != L"tablesplit") return false;
  Map();
  auto fail = [&](const wchar_t* why) {
    fwprintf(stderr, L"%ls:%ls: %ls\n", op.c_str(), arg.c_str(), why);
    return true;
  };
  if (op == L"tablemerge" || op == L"tablesplit") {
    int c[4] = {-1, -1, -1, -1};
    const int n = swscanf_s(arg.c_str(), L"%d,%d,%d,%d", &c[0], &c[1], &c[2], &c[3]);
    if (n != (op == L"tablemerge" ? 4 : 2) || c[0] < 0 || c[1] < 0) return fail(L"not cells row,col[,row,col]");
    if (n == 2) c[2] = c[0], c[3] = c[1];
    if (!SelectedTable()) return fail(L"no table selected");
    return TableEdit(op == L"tablemerge" ? CmdTableMergeCells : CmdTableSplitCell, c) ? true : fail(L"not done");
  }
  if (op == L"shapefill") {
    if (arg.size() != 6 || !std::all_of(arg.begin(), arg.end(), [](wchar_t c) { return std::iswxdigit(c) != 0; }))
      return fail(L"not a color RRGGBB");
    const uint32_t rgb = static_cast<uint32_t>(std::wcstoul(arg.c_str(), nullptr, 16));
    if (selection_.empty()) return fail(L"nothing selected");
    return SetFillColor(rgb, false) ? true : fail(L"not done");
  }
  const size_t bar = arg.find(L'|');
  const std::wstring what = arg.substr(0, bar), rest = bar == std::wstring::npos ? std::wstring() : arg.substr(bar + 1);
  if (what == L"table") {
    const size_t x = rest.find_first_of(L"xX");
    const int rows = x == std::wstring::npos ? 0 : _wtoi(rest.substr(0, x).c_str());
    const int cols = x == std::wstring::npos ? 0 : _wtoi(rest.substr(x + 1).c_str());
    if (rows < 1 || cols < 1 || rows > 75 || cols > 75) return fail(L"table|<rows>x<columns>, 1 to 75 each");
    return InsertTable(rows, cols) ? true : fail(L"not done");
  }
  if (what == L"chart") {
    for (int i = 0; i < 4; ++i)
      if (fw::ToLower(rest) == fw::ToLower(kChartNames[i])) return InsertChart(i) ? true : fail(L"not done");
    return fail(L"chart|column, bar, line or pie");
  }
  if (what == L"shape") {
    const size_t b2 = rest.find(L'|');
    draw::Rect r;
    if (b2 == std::wstring::npos || !ParseBox(rest.substr(b2 + 1), r)) return fail(L"shape|<preset>|x,y,w,h");
    const std::string preset = fw::WideToUtf8(rest.substr(0, b2));
    if (!ob::IsLinePreset(preset) && !draw::Geometry::Preset(preset)) return fail(L"no such preset");
    return InsertPreset(preset, r) ? true : fail(L"not done");
  }
  if (what == L"textbox") {
    draw::Rect r;
    if (!ParseBox(rest, r)) return fail(L"textbox|x,y,w,h");
    return InsertTextBox(r) ? true : fail(L"not done");
  }
  return fail(L"insert:table, chart, shape or textbox");
}

std::vector<std::wstring> PickPictureFiles(HWND owner, bool several) {
  std::vector<wchar_t> buf(32768, L'\0');
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof ofn;
  ofn.hwndOwner = owner;
  ofn.lpstrFilter = L"Pictures\0*.png;*.jpg;*.jpeg;*.jfif;*.gif;*.bmp;*.dib;*.tif;*.tiff;*.emf;*.wmf;*.svg\0All files\0*.*\0";
  ofn.lpstrFile = buf.data();
  ofn.nMaxFile = static_cast<DWORD>(buf.size());
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR | (several ? OFN_ALLOWMULTISELECT : 0);
  std::vector<std::wstring> files;
  if (!GetOpenFileNameW(&ofn)) return files;
  const std::wstring first(buf.data());
  const wchar_t* p = buf.data() + first.size() + 1;
  if (!several || !*p) {
    files.push_back(first);
    return files;
  }
  for (; *p; p += std::wcslen(p) + 1) files.push_back(first + L"\\" + p);
  return files;
}

void ObjectTool::ContextMenu(HMENU menu, float x, float y) {
  Map();
  if (textEditing_) return;
  const draw::Point p = ToSlide(x, y);
  const uint32_t id = map_.HitTest(p, level_, Tolerance() / 2);
  if (id && std::find(selection_.begin(), selection_.end(), id) == selection_.end()) {
    selection_ = {id};
    host_.invalidate();
  }
  auto add = [&](int cmd) {
    CommandState s;
    if (!Query(cmd, s)) return;
    std::wstring label = s.label.empty() ? L"" : s.label;
    if (cmd == CmdCut) label = L"Cut";
    if (cmd == CmdCopy) label = L"Copy";
    if (cmd == CmdPaste) label = L"Paste";
    if (!s.shortcut.empty()) label += L"\t" + s.shortcut;
    AppendMenuW(menu, MF_STRING | (s.enabled ? 0 : MF_GRAYED) | (s.checked ? MF_CHECKED : 0), cmd, label.c_str());
  };
  if (selection_.empty()) {
    add(CmdPaste);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    return;
  }
  for (int cmd : {CmdCut, CmdCopy, CmdPaste}) add(cmd);
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  for (int cmd : {CmdGroupObjects, CmdUngroupObjects, CmdBringToFront, CmdSendToBack}) add(cmd);
  if (SelectedShape())
    if (const ob::ObjectInfo* o = map_.Find(SelectedShape()); o && o->kind == ob::ObjectKind::Picture) add(CmdPictureCrop);
  add(CmdDeleteObject);
  TableContextMenu(menu);
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
}

void ObjectTool::TableContextMenu(HMENU menu) {
  Map();
  if (!SelectedTable() || !Editable(false)) return;
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  for (int cmd : kTableCmds) {
    CommandState s;
    Query(cmd, s);
    AppendMenuW(menu, MF_STRING | (s.enabled ? 0 : MF_GRAYED), cmd, s.label.c_str());
  }
}

}  // namespace fw::slide::view
